#!/usr/bin/env python3
"""Run the production ESP-NOW peer message store against host doubles.

The suite compiles the whole message-buffer section of System_ESPNow.cpp (slot
table, allocation on first use, the per-peer byte rings of
System_ESPNow_HistoryCore.h, chunked stores, merged reads, collapsed views,
seq-anchored head copies, delivery stamps, the status line;
logFileTransferEvent is left out) with the store's macros, types,
declarations, EspNowState members and their constructor items from
System_ESPNow.h, plus FreeRTOS mutex, allocation, heap, clock, settings and
logging doubles. Each case runs in its own process:

  ctor           the real constructor items on 0xA5 memory; nothing works and
                 nothing is allocated before peerHistoryStoreInit, which itself
                 allocates nothing, says one start line and latches once
  roundtrip      stores, merged reads, paging, chunking, collapse, reassembly,
                 delivery stamps, a 16 KB ring keeping a contiguous suffix
  growth         a ref taken before six new peers arrive still reads its record
  find_only      reads for unknown MACs create no slot and allocate nothing
  bounded        blocks allocated on first use (RequirePsram), line D said for
                 every allocation, 16 peers within the bound, the 17th refused with line B
                 once a minute, blocks never move, refs by seq (corrupted
                 offsets resolve by the walk, dropped records and bad slots
                 are refused)
  alloc          allocation failure: refused, line A once a minute with the
                 count, new attempts held off 5 s, no slot and no seq used
  alloc_scratch  the scratch fails with the first block: both refused and freed
  alloc_internal no PSRAM at runtime: 2 KB rings, RequireInternal, refused
                 without calling ps_alloc below the internal-heap floor
  budget         the total cap and the board maximum lower the ring; start line
  budget_bytes   a ring lowered to a size that is not whole KB (no PSRAM, 12
                 slots, 4 KB): the start and status lines say it in bytes
  slots_clamp    more mesh slots than MESH_PEER_MAX: capacity MESH_PEER_MAX
  view_clamp     a collapsed view asked for more than ESPNOW_HISTORY_VIEW_MAX
                 entries gets the newest ESPNOW_HISTORY_VIEW_MAX, not nothing
  time_order     pieces out of order: the conversation and all-peers views
                 are ordered by the head's time, not by when an entry opened
  install_race   another store installs the same MAC's block meanwhile: ours is
                 freed after unlock and both records land in the first
  install_full   other peers take every free slot meanwhile: ours is refused
                 with line B, and its block and scratch are freed
  seq_wrap       message numbers saturate at UINT32_MAX: refused, line E
  isolation      one peer's stores, chunks, wraps and stamps leave every other
                 block byte-identical
  damaged        a damaged header, RingState field or ring size: readers return
                 what is before the damage, clear that ring (line C once a
                 minute), and a store right after is visible
  names          the attribution parameter (and the old name guess) per record
  intake         an unterminated 300-byte buffer, a 52,000-byte chunked text
  paging         getAllMessages pages through three peers in seq order, each
                 record once; an idle poll decodes nothing
  delivery       states above 3 and msgId 0 change nothing; stamps per msgId
  lock_refusal   a refused or contended store lock changes nothing and is
                 never left held
  race           a storing thread races readers, including head copies of
                 stale refs (ASan/UBSan, or TSan with --thread-sanitize, which
                 runs only this case)

Every case also checks that no log line, allocation or free happens while the
store lock is held and that the lock is never taken twice by one thread.
Source contracts check that the readers (OLED, G2, web, the CLI) copy records
through the store API, hold no history pointers and name rows through
espnowHistoryDisplayName, and that ESP-NOW start allocates no history.

--source-dir <component-directory> overlays production sources. No radio,
display or filesystem is touched.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
CASES = ['ctor', 'roundtrip', 'growth', 'find_only', 'bounded', 'alloc', 'alloc_scratch', 'alloc_internal',
         'budget', 'budget_bytes', 'slots_clamp', 'view_clamp', 'time_order', 'install_race', 'install_full', 'seq_wrap', 'isolation', 'damaged', 'names', 'intake', 'paging', 'delivery',
         'lock_refusal', 'race']


def once(text, marker, where):
    count = text.count(marker)
    if count != 1:
        raise AssertionError(f'{where}: marker {marker!r} occurs {count} times, expected 1')
    return text.index(marker)


def build_unit(read, extract_block):
    header = read('System_ESPNow.h')
    source = read('System_ESPNow.cpp')

    # The history macros, as written: the PSRAM #if block, the no-PSRAM values,
    # the lock/say/retry intervals and the static_asserts that tie them to the core.
    defs_start = once(header, '#define ESPNOW_HISTORY_KB_DEFAULT_NOPSRAM', 'System_ESPNow.h')
    defs_end = once(header, '// Max length of a user text message', 'System_ESPNow.h')
    defines = header[defs_start:defs_end]
    type_blocks = []
    for marker in ('enum LogMessageType', 'enum class HistoryAttribution', 'struct ReceivedTextMessage',
                   'struct PeerMessageHistory'):
        once(header, marker, 'System_ESPNow.h')
        type_blocks.append(extract_block(header, marker) + ';')
    types = '\n\n'.join(type_blocks)
    decl_start = once(header, '// Per-device message buffer management', 'System_ESPNow.h')
    decl_end = once(header, "// Serialize one mesh peer's metadata", 'System_ESPNow.h')
    decls = header[decl_start:decl_end]
    state_start = once(header, '  // Per-device message history buffers (for web UI and OLED)', 'System_ESPNow.h')
    state_marker = 'uint32_t globalMessageSeqNum; // Global sequence number for all messages'
    state_end = once(header, state_marker, 'System_ESPNow.h') + len(state_marker)
    state = header[state_start:state_end]
    # The production constructor's items for exactly these members: a member the
    # constructor leaves out would be garbage on the device (placement new on
    # ps_alloc memory), so each must be there.
    members = re.findall(r'\b(\w+)(?:\[[^\]]*\])?;', re.sub(r'//[^\n]*', '', state))
    ctor_start = once(header, '  EspNowState() :', 'System_ESPNow.h')
    # The body starts at the first '{' that follows a ')' (items such as devices{} come before it).
    ctor_body = re.compile(r'\)\s*\{').search(header, ctor_start)
    ctor_list = header[ctor_start + len('  EspNowState() :'):ctor_body.start() + 1]
    items = {}
    for item in re.findall(r'\b(\w+)\s*(\([^()]*\)|\{\})', ctor_list):
        items[item[0]] = item[0] + item[1]
    missing = [m for m in members if m not in items]
    if missing:
        raise AssertionError(f'EspNowState() does not initialise {", ".join(missing)}')
    ctor = 'EspNowState() : ' + ', '.join(items[m] for m in members) + ' {}'

    section_start = once(source, '// Per-Device Message Buffer Management', 'System_ESPNow.cpp')
    section_end = source.index('#else // !ENABLE_ESPNOW', section_start)
    section = source[section_start:section_end]
    # logFileTransferEvent formats with String and posts to the event bus; it only
    # calls storeMessageInPeerHistory, which the cases drive directly.
    log_event = extract_block(section, 'void logFileTransferEvent(')
    once(section, log_event, 'message store section')
    section = section.replace(log_event, '// logFileTransferEvent: not compiled in this harness\n')

    required = ['struct PeerHistoryGuard', 'static void peerHistoryStoreInit(uint8_t settingKb)',
                'static PeerMessageHistory* peerHistorySlotLocked(', 'static PeerMessageHistory* peerHistoryInstallLocked(',
                'static bool historyAppendLocked(', 'static bool espnowStoreChunked(',
                'static bool peerHistoryStore(', 'bool storeMessageInPeerHistory(',
                'bool storeSentMessageInPeerHistory(', 'bool storeReceivedMessageChunked(',
                'void espnowUpdateSentDeliveryState(', 'int getPeerMessages(', 'int getAllMessages(',
                'int espnowCollapsedPeerMessages(', 'int espnowGetConversation(', 'int espnowCollapsedAllMessages(',
                'int espnowReassembleByReqId(', 'bool espnowCopyCollapsedHead(', 'int espnowHistoryMessageCount(',
                'void espnowHistoryStatusLine(']
    for marker in required:
        once(section, marker, 'message store section')
    init = 'static void storeInit(uint8_t kb = 0) { peerHistoryStoreInit(kb); }'

    harness = (HERE / 'espnow_message_store_harness.cpp').read_text()
    for tag, value in (('// INSERT_DEFINES', defines), ('// INSERT_TYPES', types),
                       ('// INSERT_DECLS', decls), ('// INSERT_STATE', state), ('// INSERT_CTOR', ctor),
                       ('// INSERT_STORE', section), ('// INSERT_INIT', init)):
        assert harness.count(tag) == 1, tag
        harness = harness.replace(tag, value)
    return harness


def source_contracts(read, extract_block, component):
    """The readers copy records out of the store; nothing outside it holds history pointers."""
    failures = []
    head_pointer = re.compile(r'(\.|->)head\b')
    readers = ('OLED_ESPNow.cpp', 'G2_Page_ESPNow.cpp', 'WebPage_ESPNow.cpp')
    for name in readers:
        text = read(name)
        if name != 'WebPage_ESPNow.cpp':
            match = head_pointer.search(text)
            if match:
                line = text.count('\n', 0, match.start()) + 1
                failures.append(f'{name}:{line} dereferences a CollapsedMsgRef live-ring pointer (head)')
            if 'espnowCopyCollapsedHead(' not in text:
                failures.append(f'{name} does not copy rows through espnowCopyCollapsedHead')
        for gone in ('senderName', 'senderMac', 'MESSAGES_PER_DEVICE'):
            if re.search(r'\b' + gone + r'\b', text):
                failures.append(f'{name} still uses {gone} (records carry no MAC or name now)')
        if 'espnowHistoryDisplayName(' not in text:
            failures.append(f'{name} does not name rows through espnowHistoryDisplayName')
    if 'oledEspNowValidateMessagePtr' in read('OLED_ESPNow.cpp') + read('OLED_ESPNow.h'):
        failures.append('OLED still validates live-ring pointers (oledEspNowValidateMessagePtr)')
    for path in sorted(list(component.glob('*.h')) + list(component.glob('*.cpp'))):
        text = read(path.name)
        if path.name not in ('System_ESPNow.cpp', 'System_ESPNow.h') and 'peerMessageHistories' in text:
            failures.append(f'{path.name} walks gEspNow->peerMessageHistories outside the store')
        if re.search(r'\bMESSAGES_PER_DEVICE\b|\bPEER_HISTORY_INITIAL_CAPACITY\b', text):
            failures.append(f'{path.name} uses the removed fixed history depth')
    header = read('System_ESPNow.h')
    if 'findOrCreatePeerHistory' in header:
        failures.append('System_ESPNow.h exports findOrCreatePeerHistory (slot creation outside the store lock)')
    if 'using CollapsedMsgRef = espnowhist::Ref;' not in header:
        failures.append('CollapsedMsgRef is not espnowhist::Ref')
    ref = extract_block(read('System_ESPNow_HistoryCore.h'), 'struct Ref ')
    if '*' in re.sub(r'//[^\n]*', '', ref):
        failures.append('espnowhist::Ref carries a pointer into the live history')
    source = read('System_ESPNow.cpp')
    if 'free(gEspNow->peerMessageHistories' in source:
        failures.append('System_ESPNow.cpp frees a published peer history block')
    if '!gEspNow->peerMessageHistories' in source:
        failures.append('System_ESPNow.cpp tests the inline slot array as a pointer')
    init = extract_block(source, 'static void peerHistoryStoreInit(uint8_t settingKb) ')
    if 'ps_alloc' in init:
        failures.append('peerHistoryStoreInit allocates (history must be allocated on first use)')
    resolver = extract_block(source, 'void espnowHistoryDisplayName(const ReceivedTextMessage& m, char* out, size_t cap) ')
    if 'GroupLockGuard' not in resolver or re.search(r'\bString\b', resolver):
        failures.append('espnowHistoryDisplayName must take GroupLockGuard and use no String')
    cli = extract_block(source, 'const char* cmd_espnow_messages(const String& argsInput) ')
    if 'espnowHistoryPageNames(msgs, n, names);' not in cli:
        failures.append('cmd_espnow_messages does not name rows through espnowHistoryPageNames')
    page = extract_block(source, 'static void espnowHistoryPageNames(')
    if 'espnowHistoryDisplayName(' not in page:
        failures.append('espnowHistoryPageNames does not name rows through espnowHistoryDisplayName')
    # espnowmessages json: the same keys, in the same order, from the copied
    # record; "name" is the page's looked-up name, never the MAC text directly.
    keys = re.findall(r'o\["(\w+)"\]\s*=\s*([^;]+);', cli)
    want_keys = ['seq', 'reqId', 'piece', 'of', 'mac', 'name', 'msg', 'enc', 'ts', 'type', 'sent', 'sendState']
    if [k for k, _ in keys] != want_keys:
        failures.append(f'espnowmessages json keys {[k for k, _ in keys]} != {want_keys}')
    values = dict(keys)
    for key, value in (('name', '(const char*)names[i]'), ('sendState', 'msgs[i].sendState'),
                       ('sent', 'msgs[i].isSent'), ('seq', '(unsigned long)msgs[i].seqNum'),
                       ('reqId', '(unsigned long)msgs[i].reqId'), ('piece', 'msgs[i].piece'),
                       ('of', 'msgs[i].pieceTotal')):
        if values.get(key, '').strip() != value:
            failures.append(f'espnowmessages json "{key}" is {values.get(key)!r}, expected {value!r}')
    # The espnowhistorykb setting reaches the store, and the command, the
    # settings row, the help text and the default agree (review 2026-11-18).
    if source.count('peerHistoryStoreInit(gSettings.espnowHistoryKb);') != 1:
        failures.append('ESP-NOW start does not latch the history size from gSettings.espnowHistoryKb')
    kb_max = re.search(r'#define ESPNOW_HISTORY_SETTING_KB_MAX (\d+)', header)
    kb_max = int(kb_max.group(1)) if kb_max else -1
    row = re.search(r'\{ "historyKB", SETTING_U8, &gSettings\.espnowHistoryKb, 0, 0, nullptr, 0, '
                    r'ESPNOW_HISTORY_SETTING_KB_MAX,\s*"[^"]*", nullptr, false, "mesh", "espnowhistorykb" \}', source)
    if not row:
        failures.append('the historyKB settings row is not U8 0..ESPNOW_HISTORY_SETTING_KB_MAX (default 0, '
                        'group mesh, command espnowhistorykb)')
    help_row = re.search(r'\{ "espnowhistorykb", "[^"]*\[<0-(\d+)>\][^"]*", true, cmd_espnow_historykb, '
                         r'"Usage: espnowhistorykb \[<0\.\.(\d+)>\]" \}', source)
    if not help_row or {int(help_row.group(1)), int(help_row.group(2))} != {kb_max}:
        failures.append(f'espnowhistorykb help/usage range does not match ESPNOW_HISTORY_SETTING_KB_MAX {kb_max}')
    cmd = extract_block(source, 'const char* cmd_espnow_historykb(const String& a) ')
    if ('espnowHistoryStatusLine(getDebugBuffer()' not in cmd or
            'handleSettingCommand(findEspNowEntry("historyKB"), a)' not in cmd):
        failures.append('espnowhistorykb: no argument must show the status line, an argument must set historyKB')
    # logFileTransferEvent is not compiled here: its record must be attributed
    # (file frames need pairing and a session), so rows show the peer's name.
    log_event = extract_block(source, 'void logFileTransferEvent(')
    if not re.search(r'storeMessageInPeerHistory\(peerMac, peerName, message, false, eventType, 0, 1, 1,\s*'
                     r'HistoryAttribution::Attributed\);', log_event):
        failures.append('logFileTransferEvent does not store its record as HistoryAttribution::Attributed')
    settings = read('System_Settings.h')
    if len(re.findall(r'\bespnowHistoryKb\(0\)', settings)) != 1:
        failures.append('Settings() does not default espnowHistoryKb to 0 (board default)')
    return failures


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    sanitizers = p.add_mutually_exclusive_group()
    sanitizers.add_argument('--sanitize', action='store_true', help='AddressSanitizer + UBSan')
    sanitizers.add_argument('--thread-sanitize', action='store_true',
                            help='ThreadSanitizer; runs only the race case')
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--source-dir', type=Path, help='production source overlay (e.g. the pre-fix tree)')
    a = p.parse_args()
    if not a.cxx:
        p.error('a host C++17 compiler is required')
    sys.path.insert(0, str(HERE))
    from test_web_batch_handlers import extract_block

    def read(name):
        overlay = a.source_dir / name if a.source_dir else None
        return (overlay if overlay and overlay.exists() else a.component / name).read_text()

    unit_text = build_unit(read, extract_block)
    failures = []
    cases = ['race'] if a.thread_sanitize else CASES
    env = dict(os.environ)
    if not (a.sanitize or a.thread_sanitize):
        # Freed memory is overwritten, so a stale read shows as wrong data even
        # without a sanitizer (macOS malloc / glibc).
        env.update(MallocScribble='1', MALLOC_PERTURB_='85')
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-msgstore-') as tmp:
        unit, exe = Path(tmp) / 'message_store.cpp', Path(tmp) / 'message_store'
        for name in ('System_MacText.h', 'System_ESPNow_HistoryCore.h'):
            (unit.parent / name).write_text(read(name))
        unit.write_text(unit_text)
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread', '-g', str(unit), '-o', str(exe)]
        if a.sanitize:
            # A UBSan report fails the case (exit code), like the other history suites.
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer']
        if a.thread_sanitize:
            cmd[1:1] = ['-fsanitize=thread', '-fno-omit-frame-pointer']
        subprocess.run(cmd, check=True, timeout=180)
        for case in cases:
            result = subprocess.run([str(exe), case], capture_output=True, text=True, timeout=120, env=env)
            sys.stdout.write(result.stdout)
            err = [line for line in result.stderr.splitlines() if 'malloc: enabling scribbling' not in line]
            if err:
                print('\n'.join(err[:60]), file=sys.stderr)
            if result.returncode:
                reason = f'exit {result.returncode}'
                for needle in ('heap-use-after-free', 'data race', 'SEGV', 'heap-buffer-overflow'):
                    if needle in result.stderr:
                        reason += f', {needle}'
                failures.append(f'case {case} ({reason})')
    if not a.thread_sanitize:
        for failure in source_contracts(read, extract_block, a.component):
            print('FAIL source contract:', failure, file=sys.stderr)
            failures.append(failure)
    if failures:
        print(f'{len(failures)} failure(s): ' + '; '.join(failures), file=sys.stderr)
        sys.exit(1)
    print(f'espnow_message_store: {len(cases)} case(s) and source contracts passed'
          if not a.thread_sanitize else 'espnow_message_store: race case clean under ThreadSanitizer')


if __name__ == '__main__':
    main()
