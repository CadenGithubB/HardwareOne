#!/usr/bin/env python3
"""Run the production ESP-NOW history readers against a fake store.

History records carry no MAC-derived name: a reader names a row with
espnowHistoryDisplayName() and resolves references only through
espnowCopyCollapsedHead() (by sequence number, never by a raw ring offset).
This suite compiles the readers' history code as shipped, with the history
types and declarations taken from System_ESPNow.h, and runs it against a fake
store and resolver (espnow_history_readers_harness.cpp):

  web    handleEspNowMessages (WebPage_ESPNow.cpp) with its JSON helpers: the
         body is parsed here; every message has exactly the keys
         seq reqId piece of mac name msg enc ts type sent sendState, in that
         order; "name" is the peer's current name, the MAC text for an
         unattributed record and "" for a sent one; the ?mac= path and a
         rename between polls; each peer and kind resolved once per poll
  oled   oledEspNowRefreshMessages (OLED_ESPNow.cpp): asks for
         ESPNOW_HISTORY_VIEW_MAX refs, shows the newest 32, skips refs the
         store refuses (evicted), names and "name (k/n)" labels, sent rows'
         msgId and durable sendState, the empty state
  g2     chatWho / chatLogicalLine / chatEstBytes (G2_Page_ESPNow.cpp): labels,
         reassembly by the record's own peer MAC, the size estimate

Both PSRAM settings are built (CONFIG_SPIRAM 1: ESPNOW_HISTORY_VIEW_MAX 512 and
50 rows per poll; unset: 32 and 15). Source contracts check what the cases
cannot: both G2 views pass ESPNOW_HISTORY_VIEW_MAX as buffer size and maxN, a
G2 page build clears its name cache, and no reader names the removed record
fields or touches the history blocks.

--source-dir <directory> overlays production sources (for example a
System_ESPNow.h written from the frozen API before the store lands it).
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
CASES = ['web', 'oled', 'g2']
JSON_KEYS = ['seq', 'reqId', 'piece', 'of', 'mac', 'name', 'msg', 'enc', 'ts', 'type', 'sent', 'sendState']
READERS = ('WebPage_ESPNow.cpp', 'OLED_ESPNow.cpp', 'G2_Page_ESPNow.cpp')
API_FUNCTIONS = ('espnowGetConversation', 'espnowCollapsedAllMessages', 'espnowCopyCollapsedHead',
                 'espnowReassembleByReqId', 'espnowHistoryDisplayName', 'getPeerMessages', 'getAllMessages')


def once(text, marker, where):
    count = text.count(marker)
    if count != 1:
        raise AssertionError(f'{where}: marker {marker!r} occurs {count} times, expected 1')
    return text.index(marker)


def conditional(source, anchor, name):
    """The #if ... #endif block that holds `anchor` (one #if, one #else)."""
    at = source.index(anchor)
    begin = source.rindex('\n#if', 0, at) + 1
    end = source.index('#endif', at) + len('#endif')
    block = source[begin:end]
    assert block.count('#if') == 1 and block.count('#else') == 1, f'{name}: one #if/#else around {anchor!r}'
    return block


def declaration(header, function):
    """The one declaration of `function` in System_ESPNow.h (may span lines)."""
    found = re.findall(r'^[ \t]*(?:int|bool|void)[ \t]+' + function + r'\([^;{]*\);', header, re.M)
    assert len(found) == 1, f'System_ESPNow.h: {len(found)} declarations of {function}'
    return found[0].strip()


def build_unit(read, extract_block):
    header = read('System_ESPNow.h')
    web = read('WebPage_ESPNow.cpp')
    oled = read('OLED_ESPNow.cpp')
    g2 = read('G2_Page_ESPNow.cpp')

    api = []
    api += [line for line in header.splitlines() if line.startswith('#define ESPNOW_HISTORY_')]
    api.append(conditional(header, '#define ESPNOW_HISTORY_VIEW_MAX', 'System_ESPNow.h'))
    for marker in ('enum LogMessageType', 'struct ReceivedTextMessage'):
        once(header, marker, 'System_ESPNow.h')
        api.append(extract_block(header, marker) + ';')
    once(header, 'using CollapsedMsgRef = espnowhist::Ref;', 'System_ESPNow.h')
    api.append('using CollapsedMsgRef = espnowhist::Ref;')
    api += [declaration(header, f) for f in API_FUNCTIONS]

    web_parts = [extract_block(web, 'static inline esp_err_t webEspNowSendChunk('),
                 extract_block(web, 'static inline esp_err_t webEspNowSendChunkf('),
                 extract_block(web, 'static esp_err_t webEspNowSendJsonEscapedString('),
                 conditional(web, 'static constexpr int kWebMessagesBufCount = ', 'WebPage_ESPNow.cpp')]
    once(web, 'static ReceivedTextMessage* gWebMessagesBuf = nullptr;', 'WebPage_ESPNow.cpp')
    web_parts.append('static ReceivedTextMessage* gWebMessagesBuf = nullptr;')
    web_parts.append(extract_block(web, 'static esp_err_t handleEspNowMessages('))

    oled_part = extract_block(oled, 'void oledEspNowRefreshMessages() ')

    g2_parts = []
    for line in ('static constexpr int kChatCols = ', 'static EXT_RAM_BSS_ATTR char gChatReasm[',
                 'static EXT_RAM_BSS_ATTR espnowhist::NameCache gChatNames;'):
        at = once(g2, line, 'G2_Page_ESPNow.cpp')
        g2_parts.append(g2[at:g2.index('\n', at)])
    for marker in ('static const char* chatWho(', 'static void chatLogicalLine(',
                   'static size_t chatEstBytes('):
        g2_parts.append(extract_block(g2, marker))

    harness = (HERE / 'espnow_history_readers_harness.cpp').read_text()
    for tag, value in (('// INSERT_API', '\n'.join(api)), ('// INSERT_WEB', '\n\n'.join(web_parts)),
                       ('// INSERT_OLED', oled_part), ('// INSERT_G2', '\n\n'.join(g2_parts))):
        assert harness.count(tag) == 1, tag
        harness = harness.replace(tag, value)
    return harness


def check_web(stdout, psram):
    """Parse the three bodies the web case printed and check keys, order and names."""
    failures = []
    bodies = {}
    for line in stdout.splitlines():
        tag, _, body = line.partition(' ')
        if tag in ('WEB_ALL', 'WEB_PEER', 'WEB_RENAMED'):
            bodies[tag] = body
    if set(bodies) != {'WEB_ALL', 'WEB_PEER', 'WEB_RENAMED'}:
        return [f'web case printed {sorted(bodies)}']
    parsed = {}
    for tag, body in bodies.items():
        try:
            parsed[tag] = json.loads(body, object_pairs_hook=lambda pairs: pairs)
        except ValueError as e:
            failures.append(f'{tag}: not JSON ({e}): {body[:200]}')
    if failures:
        return failures
    want_top = ['messages', 'deliveries']
    rows = {}
    for tag, pairs in parsed.items():
        if [k for k, _ in pairs] != want_top:
            failures.append(f'{tag}: top-level keys {[k for k, _ in pairs]}')
            continue
        messages = pairs[0][1]
        rows[tag] = []
        for m in messages:
            keys = [k for k, _ in m]
            if keys != JSON_KEYS:
                failures.append(f'{tag}: message keys {keys}')
            rows[tag].append(dict(m))
        deliveries = pairs[1][1]
        if [dict(d) for d in deliveries] != [{'msgId': 42, 'state': 'delivered', 'mac': '01:02:03:04:05:06',
                                              'ageMs': 1000}]:
            failures.append(f'{tag}: deliveries {deliveries}')
    if failures:
        return failures
    all_rows = rows['WEB_ALL']
    cap = 50 if psram else 15
    if len(all_rows) != min(16, cap):
        failures.append(f'WEB_ALL: {len(all_rows)} rows, expected {min(16, cap)}')
    expect = [
        (1, 'AA', 'Alpha "one"', 'hi from A', False, True),
        (2, 'AA', '10:20:30:40:50:60', 'plain A', False, False),
        (3, 'AA', '', 'to A', True, True),
        (4, 'BB', 'Bravo', 'from B\nline', False, True),
        (5, 'BB', 'Bravo', 'part two', False, True),
        (6, 'AA', 'Alpha "one"', 'again A', False, True),
    ]
    mac = {'AA': '10:20:30:40:50:60', 'BB': 'AA:BB:CC:00:11:22'}
    for (seq, who, name, msg, sent, enc), row in zip(expect, all_rows):
        got = (row['seq'], row['mac'], row['name'], row['msg'], row['sent'], row['enc'])
        if got != (seq, mac[who], name, msg, sent, enc):
            failures.append(f'WEB_ALL seq {seq}: got {got}')
    a3 = all_rows[2]
    if (a3['reqId'], a3['piece'], a3['of'], a3['sendState'], a3['ts'], a3['type']) != (77, 1, 1, 1, 1003, 0):
        failures.append(f'WEB_ALL sent row fields {a3}')
    b4 = all_rows[3]
    if (b4['reqId'], b4['piece'], b4['of']) != (9, 1, 2):
        failures.append(f'WEB_ALL fragment fields {b4}')
    for i, row in enumerate(all_rows[6:]):
        if row['name'] != f'peer{i}' or row['seq'] != 10 + i:
            failures.append(f'WEB_ALL extra peer row {row}')
    if [(r['seq'], r['name']) for r in rows['WEB_PEER']] != [(4, 'Bravo'), (5, 'Bravo')]:
        failures.append(f"WEB_PEER rows {[(r['seq'], r['name']) for r in rows['WEB_PEER']]}")
    renamed = [(r['seq'], r['name']) for r in rows['WEB_RENAMED']][:3]
    if renamed != [(4, 'Bravo2'), (5, 'Bravo2'), (6, 'Alpha "one"')]:
        failures.append(f'WEB_RENAMED rows {renamed}')
    return failures


def source_contracts(read):
    failures = []
    g2 = read('G2_Page_ESPNow.cpp')
    for call in ('espnowCollapsedAllMessages(gInboxRefs, ESPNOW_HISTORY_VIEW_MAX)',
                 'espnowGetConversation(d.mac, gInboxRefs, ESPNOW_HISTORY_VIEW_MAX)',
                 'CollapsedMsgRef gInboxRefs[ESPNOW_HISTORY_VIEW_MAX];'):
        if call not in g2:
            failures.append(f'G2_Page_ESPNow.cpp lacks {call!r}')
    build = g2.index('static void chatBuildPages(int rc) {')
    if not g2[build:build + 200].split('{', 1)[1].lstrip().startswith('gChatNames.clear();'):
        failures.append('chatBuildPages does not start by clearing gChatNames')
    oled = read('OLED_ESPNow.cpp')
    if 'CollapsedMsgRef refs[ESPNOW_HISTORY_VIEW_MAX];' not in oled:
        failures.append('OLED_ESPNow.cpp ref buffer is not ESPNOW_HISTORY_VIEW_MAX')
    for name in READERS:
        code = re.sub(r'//[^\n]*', '', read(name))
        for word in ('MESSAGES_PER_DEVICE', 'senderName', 'senderMac', 'peerMessageHistories',
                     'PeerMessageHistory'):
            if re.search(r'\b' + word + r'\b', code):
                failures.append(f'{name} uses {word}')
        if 'espnowHistoryDisplayName(' not in code:
            failures.append(f'{name} does not name rows through espnowHistoryDisplayName')
        if name != 'WebPage_ESPNow.cpp' and 'espnowCopyCollapsedHead(' not in code:
            failures.append(f'{name} does not copy rows through espnowCopyCollapsedHead')
        if re.search(r'\.offset\b', code):
            failures.append(f'{name} reads a ref offset (refs resolve by seq in the store only)')
    return failures


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    p.add_argument('--sanitize', action='store_true', help='AddressSanitizer + UBSan')
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--source-dir', type=Path, help='production source overlay')
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
    passed = 0
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-readers-') as tmp:
        unit = Path(tmp) / 'readers.cpp'
        unit.write_text(unit_text)
        for psram in (True, False):
            exe = Path(tmp) / f'readers_{int(psram)}'
            cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', f'-I{a.component}',
                   str(unit), '-o', str(exe)]
            if a.source_dir:
                cmd.insert(1, f'-I{a.source_dir}')
            if psram:
                cmd.insert(1, '-DCONFIG_SPIRAM=1')
            if a.sanitize:
                cmd[1:1] = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                            '-fno-omit-frame-pointer']
            subprocess.run(cmd, check=True, timeout=180)
            for case in CASES:
                label = f'{case} ({"PSRAM" if psram else "no PSRAM"})'
                r = subprocess.run([str(exe), case], capture_output=True, text=True, timeout=60)
                for line in r.stdout.splitlines():
                    if line.startswith('CASE '):
                        print(line, '(PSRAM)' if psram else '(no PSRAM)')
                if r.stderr:
                    print(r.stderr[:4000], file=sys.stderr)
                case_failures = [] if r.returncode == 0 else [f'exit {r.returncode}']
                if case == 'web' and r.returncode == 0:
                    case_failures += check_web(r.stdout, psram)
                for f in case_failures:
                    failures.append(f'{label}: {f}')
                if not case_failures:
                    passed += 1
    for f in source_contracts(read):
        failures.append(f'source contract: {f}')
    for f in failures:
        print('FAIL', f, file=sys.stderr)
    total = 2 * len(CASES)
    print(f'espnow_history_readers: {passed}/{total} case runs passed, '
          f'{"source contracts passed" if not any(f.startswith("source") for f in failures) else "source contracts FAILED"}')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
