#!/usr/bin/env python3
"""Run the production ESP-NOW peer message store against host doubles.

The suite compiles the whole message-buffer section of System_ESPNow.cpp (slot
table, received/sent rings, chunked store, merged reads, collapsed views, copy
accessor, delivery stamps; logFileTransferEvent is left out) with the store's
types and declarations from System_ESPNow.h, plus FreeRTOS mutex, allocation,
clock and logging doubles. Each case runs in its own process:

  roundtrip     stores, merged reads, paging, chunking, collapse, reassembly,
                delivery stamps, ring wrap and the per-peer counts
  growth        a collapsed ref taken before six new peers arrive still reads
                its record (the history must not move under a reader)
  find_only     reads for unknown MACs create no slot and allocate nothing
  bounded       16 slots, the 17th peer refused, blocks never move, a ref whose
                ring index was reused is refused, block allocation failure
  lock_refusal  a refused or contended store lock changes nothing and is
                never left held
  race          a storing thread races readers (ASan/UBSan, or TSan with
                --thread-sanitize, which runs only this case)

Every case also checks that no log line is emitted while the store lock is
held and that the lock is never taken twice by one thread. Source contracts
check that the OLED and G2 screens copy records through
espnowCopyCollapsedHead instead of holding pointers into the history.

--source-dir <component-directory> overlays production sources (for example
the pre-fix tree) so the former use-after-free can be replayed. No radio,
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
CASES = ['roundtrip', 'growth', 'find_only', 'bounded', 'lock_refusal', 'race']


def once(text, marker, where):
    count = text.count(marker)
    if count != 1:
        raise AssertionError(f'{where}: marker {marker!r} occurs {count} times, expected 1')
    return text.index(marker)


def build_unit(read, extract_block):
    header = read('System_ESPNow.h')
    source = read('System_ESPNow.cpp')

    defines = '\n'.join(line for line in header.splitlines() if line.startswith('#define PEER_HISTORY_'))
    type_blocks = []
    for marker in ('enum LogMessageType', 'struct ReceivedTextMessage', 'struct PeerMessageHistory'):
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

    section_start = once(source, '// Per-Device Message Buffer Management', 'System_ESPNow.cpp')
    section_end = source.index('#else // !ENABLE_ESPNOW', section_start)
    section = source[section_start:section_end]
    # logFileTransferEvent formats with String and posts to the event bus; it only
    # calls storeMessageInPeerHistory, which the cases drive directly.
    log_event = extract_block(section, 'void logFileTransferEvent(')
    once(section, log_event, 'message store section')
    section = section.replace(log_event, '// logFileTransferEvent: not compiled in this harness\n')

    copy_api = 'bool espnowCopyCollapsedHead(' in decls
    required = ['static void espnowStoreInRing(', 'static void espnowStoreChunked(',
                'bool storeMessageInPeerHistory(', 'bool storeSentMessageInPeerHistory(',
                'bool storeReceivedMessageChunked(', 'void espnowUpdateSentDeliveryState(',
                'static int mergePeerRingsBySeq(', 'int getPeerMessages(', 'int getAllMessages(',
                'static int espnowCollapseRing(', 'int espnowCollapsedPeerMessages(',
                'int espnowGetConversation(', 'int espnowCollapsedAllMessages(',
                'int espnowReassembleByReqId(']
    if copy_api:
        required += ['struct PeerHistoryGuard', 'static size_t peerHistoryStoreInit(',
                     'static PeerMessageHistory* peerHistoryAllocateBlock(',
                     'static PeerMessageHistory* peerHistorySlotLocked(',
                     'bool espnowCopyCollapsedHead(', 'int espnowHistoryMessageCount(']
        init = 'static void storeInit() { (void)peerHistoryStoreInit(); }'
    else:
        required += ['static bool growPeerHistoryArray()', 'PeerMessageHistory* findOrCreatePeerHistory(']
        init_start = once(source, '    int initialCapacity = PEER_HISTORY_INITIAL_CAPACITY;', 'System_ESPNow.cpp')
        init_end = source.index('    size_t totalBytes = sizeof(EspNowState)', init_start)
        init = ('static void storeInit() {\n' + source[init_start:init_end] +
                '  (void)histSize;\n}')
    for marker in required:
        once(section, marker, 'message store section')

    harness = (HERE / 'espnow_message_store_harness.cpp').read_text()
    for tag, value in (('// INSERT_DEFINES', defines), ('// INSERT_TYPES', types),
                       ('// INSERT_DECLS', decls), ('// INSERT_STATE', state),
                       ('// INSERT_STORE', section), ('// INSERT_INIT', init)):
        assert harness.count(tag) == 1, tag
        harness = harness.replace(tag, value)
    return harness, copy_api


def source_contracts(read, extract_block):
    """The screens copy records out of the store; nothing outside it holds history pointers."""
    failures = []
    head_pointer = re.compile(r'(\.|->)head\b')
    for name in ('OLED_ESPNow.cpp', 'G2_Page_ESPNow.cpp'):
        text = read(name)
        if 'peerMessageHistories' in text:
            failures.append(f'{name} walks gEspNow->peerMessageHistories outside the store lock')
        match = head_pointer.search(text)
        if match:
            line = text.count('\n', 0, match.start()) + 1
            failures.append(f'{name}:{line} dereferences a CollapsedMsgRef live-ring pointer (head)')
        if 'oledEspNowValidateMessagePtr' in text:
            failures.append(f'{name} still validates live-ring pointers (oledEspNowValidateMessagePtr)')
        if 'espnowCopyCollapsedHead(' not in text:
            failures.append(f'{name} does not copy rows through espnowCopyCollapsedHead')
    if 'oledEspNowValidateMessagePtr' in read('OLED_ESPNow.h'):
        failures.append('OLED_ESPNow.h still declares oledEspNowValidateMessagePtr')
    header = read('System_ESPNow.h')
    if 'findOrCreatePeerHistory' in header:
        failures.append('System_ESPNow.h exports findOrCreatePeerHistory (slot creation outside the store lock)')
    ref = extract_block(header, 'struct CollapsedMsgRef')
    if '*' in re.sub(r'//[^\n]*', '', ref):
        failures.append('CollapsedMsgRef carries a pointer into the live history')
    source = read('System_ESPNow.cpp')
    if 'free(gEspNow->peerMessageHistories' in source:
        failures.append('System_ESPNow.cpp frees a published peer history array')
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

    unit_text, copy_api = build_unit(read, extract_block)
    failures = []
    cases = ['race'] if a.thread_sanitize else CASES
    env = dict(os.environ)
    if not (a.sanitize or a.thread_sanitize):
        # Freed memory is overwritten, so a stale read shows as wrong data even
        # without a sanitizer (macOS malloc / glibc).
        env.update(MallocScribble='1', MALLOC_PERTURB_='85')
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-msgstore-') as tmp:
        unit, exe = Path(tmp) / 'message_store.cpp', Path(tmp) / 'message_store'
        unit.write_text(unit_text)
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread', '-g',
               f'-DHW1_HISTORY_COPY_API={int(copy_api)}', str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
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
        for failure in source_contracts(read, extract_block):
            print('FAIL source contract:', failure, file=sys.stderr)
            failures.append(failure)
    if failures:
        print(f'{len(failures)} failure(s): ' + '; '.join(failures), file=sys.stderr)
        sys.exit(1)
    print(f'espnow_message_store: {len(cases)} case(s) and source contracts passed'
          if not a.thread_sanitize else 'espnow_message_store: race case clean under ThreadSanitizer')


if __name__ == '__main__':
    main()
