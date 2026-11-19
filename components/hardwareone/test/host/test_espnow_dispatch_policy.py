#!/usr/bin/env python3
"""Run the actual ESP-NOW V4 opcode table and dispatcher against spoofed-frame cases.

Extracts kV4HandlerTable, its flag constants, v4_dispatch_lookup,
v4_dispatch_table_try, espnowOriginIsKnown, v4h_text and the espnow_task
text-queue drain ("// 11. Drain text message queue") from System_ESPNow.cpp,
LogMessageType and TextQueueEntry from System_ESPNow.h, the opcode/flag enums
and header from System_ESPNow_Wire.h and V4RxCtx from System_ESPNow_RxCtx.h,
generates one recording double per other handler named in the table, and
compiles the harness once with ENABLE_BONDED_MODE 1 and once with 0. The cases
check that SUBSCRIBE_UPDATE needs a paired, session-encrypted frame, that
TIME_SYNC, SENSOR_STATUS, SENSOR_REQ, SENSOR_ENVELOPE, POWER_STATUS,
EVENT_PUSH and FORWARD_LINK need one too (keyed or keyless device, a signed broadcast is not
enough, no bond partner required), that
HEARTBEAT, TEXT and BOOT need an authenticated frame once the device has a
mesh passphrase (plaintext stays accepted on a keyless device), that plaintext
TEXT accepted on a keyless device is filed under its MAC rather than a paired
peer's name and posts no SYSEVT_TEXT_RX, that authenticated text keeps its
name, encryption flag, per-fragment records and single bus event, that opcode
150 has no row, that every zero-flag row is on an explicit allow-list, and
that every bond row reaches its handler only from the configured bond partner:
a second paired peer with a session is dropped with a WARN (one a minute),
bond mode off drops silently, the stored partner text is matched without a
parse when it is in the form bondconnect writes and through parseMacAddress
when it was typed by hand, and with bonded mode compiled out no bond row exists.
The two bond agreement rows (BOND_REQUEST, BOND_NOTICE) are the exception: any
paired peer reaches them over its session, bond mode on or off, and the drop
path tells the agreement whose bond frame was ignored (a paired peer that is
not the partner; never the partner or an unpaired source).
History, the event bus and the G2 push-kick are recording doubles; no radio or
transport is accessed.

`--component <dir>` or `--source-dir <overlay>` replays the pre-fix table.
extract_block is imported from the directory of this runner on purpose, not
from `<component>/test/host`: a component overlay (such as a saved base tree)
supplies production sources only, never a historical copy of the test code.
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
sys.path.insert(0, str(HERE))
from test_web_batch_handlers import extract_block  # noqa: E402

ZERO_FLAG_ALLOWED = ('KEY_EX_HELLO', 'KEY_EX_REPLY', 'KEY_EX_CONFIRM',
                     'SESSION_OPEN', 'SESSION_CONFIRM', 'SESSION_REKEY',
                     'TOPO_REQ', 'TOPO_START', 'TOPO_PEER', 'USER_SYNC')

# The rows of the table's ENABLE_BONDED_MODE block: accepted only from the
# configured bond partner. A row added to or removed from that block must be
# added here or removed, so it gets the partner cases.
BOND_OPCODES = ('BOND_HEARTBEAT', 'BOND_STREAM_CTRL', 'BOND_CAP_REQ', 'BOND_CAP_RESP',
                'BOND_SENSOR_DATA', 'BOND_POWER_STATUS', 'BOND_EVENT_PUSH', 'BOND_SENSOR_STATUS', 'SETTINGS_REQ',
                'SCHEMA_REQ',
                'BOND_STATUS_REQ', 'BOND_STATUS_RESP', 'MANIFEST_REQ')
# The rows of the table's second ENABLE_BONDED_MODE block, the bond agreement:
# accepted from any paired peer over its session (no REQ_BOND_PEER), because
# the receiver may not have the sender as its partner. A source from before
# them has no second block.
BOND_AGREEMENT_OPCODES = ('BOND_REQUEST', 'BOND_NOTICE')

# The receive table's minimum payload length per opcode (System_ESPNow_Wire.h,
# ESPNOW_V4_MINLEN_*), and the six exact-length handshake rows. These are the
# 1.0 values and they never rise: a frame of a 1.0 sender must stay accepted
# by every later receiver. A struct that grows after 1.0 keeps its row's value
# here; a test failure that asks for a larger number is the bug, not this table.
EXPECTED_MINLEN = {
    'KEY_EX_HELLO': 72, 'KEY_EX_REPLY': 72, 'KEY_EX_CONFIRM': 18,
    'SESSION_OPEN': 130, 'SESSION_CONFIRM': 146, 'SESSION_REKEY': 130,
    'SESSION_UNKNOWN': 12, 'TIME_SYNC': 16,
    'TEXT': 1, 'BOOT': 1, 'CMD': 1, 'CMD_RESP': 5, 'USER_SYNC': 1, 'STREAM': 6,
    'RELAY_DATA': 40, 'PEER_LIST': 4, 'HEARTBEAT': 44, 'PAIR_BEACON': 36,
    'PAIR_REQUEST': 40, 'PAIR_ACCEPT': 40, 'PAIR_REJECT': 40,
    'SENSOR_STATUS': 16, 'SENSOR_REQ': 20, 'SUBSCRIBE_UPDATE': 16, 'BOND_SENSOR_STATUS': 16,
    'SENSOR_ENVELOPE': 13, 'BOND_SENSOR_DATA': 13,
    'POWER_STATUS': 24, 'BOND_POWER_STATUS': 24, 'EVENT_PUSH': 128, 'BOND_EVENT_PUSH': 128,
    'FORWARD_LINK': 96, 'TOPO_REQ': 8, 'TOPO_START': 8, 'TOPO_PEER': 47,
    'METADATA_REQ': 0, 'BOND_CAP_REQ': 0, 'SETTINGS_REQ': 0, 'SCHEMA_REQ': 0, 'BOND_STATUS_REQ': 0,
    'MANIFEST_REQ': 0, 'METADATA_RESP': 202, 'METADATA_PUSH': 202,
    'FILE_START': 72, 'FILE_DATA': 3, 'FILE_END': 5, 'FILE_CANCEL': 5,
    'FS_LIST_REQ': 144, 'FS_LIST_REPLY': 140, 'FS_STAT_REQ': 140, 'FS_GET_REQ': 140, 'FS_GET_ACK': 140,
    'FS_STAT_REPLY': 164, 'BOND_HEARTBEAT': 24, 'BOND_STREAM_CTRL': 4, 'BOND_CAP_RESP': 84,
    'BOND_STATUS_RESP': 28, 'BOND_REQUEST': 8, 'BOND_NOTICE': 8,
}
EXACT_LENGTH = ('KEY_EX_HELLO', 'KEY_EX_REPLY', 'KEY_EX_CONFIRM', 'SESSION_OPEN', 'SESSION_CONFIRM', 'SESSION_REKEY')
ROW = r'\{\s*ESPNOW_V4_TYPE_(\w+)\s*,([^,]*),\s*(ESPNOW_V4_MINLEN_\w+)\s*,\s*(\w+)\s*\}'


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--source-dir', type=Path)
    a = p.parse_args()
    if not a.cxx:
        p.error('a host C++17 compiler is required')

    def read(name):
        overlay = a.source_dir / name if a.source_dir else None
        return (overlay if overlay and overlay.exists() else a.component / name).read_text()

    source = read('System_ESPNow.cpp')
    header = read('System_ESPNow.h')
    wire = read('System_ESPNow_Wire.h')
    rxctx = read('System_ESPNow_RxCtx.h')

    # Wire.h cannot be included on the host (it pulls the real build config and
    # its flexible array members fail -pedantic), so slice the three definitions.
    types = '\n'.join(extract_block(wire, marker) + ';' for marker in
                      ('enum EspNowV4Type', 'enum EspNowV4Flags',
                       'struct __attribute__((packed)) EspNowV4Header'))
    minlen_defines = re.findall(r'^#define ESPNOW_V4_MINLEN_\w+\b.*$', wire, re.MULTILINE)
    assert minlen_defines, 'System_ESPNow_Wire.h has no ESPNOW_V4_MINLEN_* constants (the table has no minLen column)'
    types += '\n' + '\n'.join(minlen_defines)
    ctx = extract_block(rxctx, 'struct V4RxCtx') + ';'
    # The policy slice carries every V4_OPC_FLAG_* constant, so the harness never
    # names one that the overlay may lack.
    policy = (source[source.index('using V4OpcodeHandler'):source.index('struct V4OpcodeEntry')]
              + extract_block(source, 'struct V4OpcodeEntry') + ';')
    table = extract_block(source, 'static const V4OpcodeEntry kV4HandlerTable[]') + ';'
    size = 'static constexpr size_t kV4HandlerTableSize = sizeof(kV4HandlerTable) / sizeof(kV4HandlerTable[0]);'
    lookup = extract_block(source, 'static const V4OpcodeEntry* v4_dispatch_lookup(')
    dispatch = extract_block(source, 'static bool v4_dispatch_table_try(')
    # The TEXT/BOOT receive path runs for real: the queue entry type, the
    # handler and the drain. The drain is an inline step of the espnow_task
    # loop, so its step comment is the marker and the harness wraps the block.
    for text, marker in ((header, 'enum LogMessageType'), (header, 'struct TextQueueEntry'),
                         (source, 'static bool espnowOriginIsKnown('),
                         (source, 'static void v4h_text('),
                         (source, '// 11. Drain text message queue')):
        assert text.count(marker) == 1, f'one definition required: {marker!r}'
    message_types = extract_block(header, 'enum LogMessageType') + ';'
    queue_entry = extract_block(header, 'struct TextQueueEntry') + ';'
    origin_known = extract_block(source, 'static bool espnowOriginIsKnown(')
    text_handler = extract_block(source, 'static void v4h_text(')
    # The BOOT row's handler (2026-11-18) runs for real when the tree has it:
    # it hands an authenticated notice from the peer itself to the session
    # rule (a recording double here; espnow_peer_restart_tests runs the rules)
    # and then files it through v4h_text.
    real_handlers = ['v4h_text']
    if 'static void v4h_boot(' in source:
        assert source.count('static void v4h_boot(') == 1, 'one definition required: v4h_boot'
        # The production rule header (peerRestartSignalScoped), dependency-free;
        # inlined because the unit is compiled alone.
        core = read('System_ESPNow_PeerRestartCore.h').replace('#pragma once', '')
        text_handler += ('\n\n' + core + '\n'
                         'static bool bootNoticeCounter(const uint8_t*, uint16_t, uint32_t* out) {\n'
                         '  *out = 7;\n  return true;\n}\n'
                         'static PeerBootVerdict sessionPeerBootNotice(const uint8_t*, bool, uint32_t) {\n'
                         '  ++gBootRuleCalls;\n  return static_cast<PeerBootVerdict>(gBootVerdict);\n}\n'
                         '[[maybe_unused]] static void sessionPeerBootSeen(const uint8_t*, uint32_t) {\n'
                         '  ++gBootSeenCalls;\n}\n'
                         + extract_block(source, 'static void v4h_boot('))
        real_handlers.append('v4h_boot')
    # SUBSCRIBE_UPDATE's handler runs for real: a longer frame and reserved
    # bytes that are not 0 still reach the subscription store (a double).
    if 'static void v4h_subscribe_update(' in source:
        text_handler += ('\n\n' + extract_block(wire, 'struct __attribute__((packed)) V4PayloadSubscribe') + ';\n'
                         'static bool peerIdentitySetSubscriptions(const uint8_t*, uint32_t mask) {\n'
                         '  ++gSubscribeCalls;\n  ++gHandlerCalls;\n  gSubscribeMask = mask;\n  return true;\n}\n'
                         + extract_block(source, 'static void v4h_subscribe_update('))
        real_handlers.append('v4h_subscribe_update')
    # The SESSION_UNKNOWN row's handler runs for real when the tree has it; the
    # rule behind it is a recording double (espnow_peer_restart_tests runs it).
    if 'static void v4h_session_unknown(' in source:
        assert source.count('static void v4h_session_unknown(') == 1, 'one definition required'
        text_handler += ('\n\n' + extract_block(wire, 'struct __attribute__((packed)) V4PayloadSessionUnknown')
                         + ';\n'
                         'static int sessionUnknownHintReceived(const uint8_t*, const V4PayloadSessionUnknown*) {\n'
                         '  ++gHintRuleCalls;\n  return 0;\n}\n'
                         + extract_block(source, 'static void v4h_session_unknown('))
        real_handlers.append('v4h_session_unknown')
    drain = 'static void drainTextQueueForTest() ' + extract_block(source, '// 11. Drain text message queue')
    # A drain that formats system notices calls formatEspNowSystemNotice;
    # test_espnow_system_notice.py runs the real one, a double does here.
    if 'static void formatEspNowSystemNotice(' in source:
        drain = ('#define EXT_RAM_BSS_ATTR\n'
                 'static void formatEspNowSystemNotice(const char*, size_t, const char*, char* history,\n'
                 '                                     size_t historyLen, char* line, size_t lineLen) {\n'
                 '  std::snprintf(history, historyLen, "notice");\n'
                 '  std::snprintf(line, lineLen, "notice");\n'
                 '}\n' + drain)

    # The bond partner predicate runs for real when the source has it (a tree
    # from before the partner gate has none; its dispatcher never calls one).
    bond_marker = 'bool isBondPeerMac(const uint8_t* mac)'
    bond_peer = extract_block(source, bond_marker) if bond_marker in source else ''
    bond_blocks = re.findall(r'#if ENABLE_BONDED_MODE\n(.*?)#endif', table, re.DOTALL)
    assert bond_blocks, 'kV4HandlerTable has no ENABLE_BONDED_MODE block'
    bond_rows = re.findall(r'\{\s*ESPNOW_V4_TYPE_(\w+)\s*,', bond_blocks[0])
    assert sorted(bond_rows) == sorted(BOND_OPCODES), f'bond rows changed, update BOND_OPCODES: {bond_rows}'
    bond_opcodes = '\n'.join(f'  ESPNOW_V4_TYPE_{name},' for name in BOND_OPCODES)
    assert len(bond_blocks) <= 2, 'a third ENABLE_BONDED_MODE block in kV4HandlerTable: add its rows to this test'
    agreement_rows = re.findall(r'\{\s*ESPNOW_V4_TYPE_(\w+)\s*,', bond_blocks[1]) if len(bond_blocks) == 2 else []
    assert not agreement_rows or sorted(agreement_rows) == sorted(BOND_AGREEMENT_OPCODES), \
        f'bond agreement rows changed, update BOND_AGREEMENT_OPCODES: {agreement_rows}'
    agreement_opcodes = '\n'.join(f'  ESPNOW_V4_TYPE_{name},' for name in agreement_rows)

    rows = re.findall(ROW, table)
    assert rows, 'no handler rows with a minLen column found in kV4HandlerTable'
    all_rows = re.findall(r'\{\s*ESPNOW_V4_TYPE_(\w+)\s*,', table)
    assert sorted(all_rows) == sorted(r[0] for r in rows), 'every kV4HandlerTable row has a minLen column'
    for name, flags, minlen, _handler in rows:
        assert minlen == f'ESPNOW_V4_MINLEN_{name}', f'the {name} row names its own minimum: {minlen}'
        assert name in EXPECTED_MINLEN, f'a new row {name}: add its 1.0 minimum to EXPECTED_MINLEN'
        exact = 'V4_OPC_LEN_EXACT' in flags
        assert exact == (name in EXACT_LENGTH), f'V4_OPC_LEN_EXACT is on exactly the six handshake rows ({name})'
    handlers = sorted(set(r[3] for r in rows))
    assert handlers, 'no handler rows found in kV4HandlerTable'
    bond_names = set(BOND_OPCODES) | set(BOND_AGREEMENT_OPCODES)
    def minlen_rows(names):
        return '\n'.join(f'  {{ ESPNOW_V4_TYPE_{n}, {EXPECTED_MINLEN[n]}, {"true" if n in EXACT_LENGTH else "false"} }},'
                          for n in names)
    present = [r[0] for r in rows]
    expected_minlen = (minlen_rows(n for n in present if n not in bond_names) + '\n#if ENABLE_BONDED_MODE\n' +
                       minlen_rows(n for n in present if n in bond_names) + '\n#endif')
    assert 'v4h_text' in handlers, 'the TEXT row must name v4h_text'
    stubs = '\n'.join(f'static void {name}(const V4RxCtx& ctx) {{ noteHandler("{name}", ctx); }}'
                      for name in handlers if name not in real_handlers)
    enum_block = extract_block(wire, 'enum EspNowV4Type')
    opcodes = re.findall(r'^\s*ESPNOW_V4_TYPE_(\w+)\s*=', enum_block, re.MULTILINE)
    assert opcodes, 'no opcode symbols found in enum EspNowV4Type'
    names = '\n'.join(f'  {{ ESPNOW_V4_TYPE_{name}, "{name}" }},' for name in opcodes)
    allowed = '\n'.join(f'  ESPNOW_V4_TYPE_{name},' for name in ZERO_FLAG_ALLOWED)

    harness = (HERE / 'espnow_dispatch_policy_harness.cpp').read_text()
    for tag, value in [('// INSERT_WIRE', types),
                       ('// INSERT_RXCTX', ctx),
                       ('// INSERT_MESSAGE_TYPES', message_types),
                       ('// INSERT_TEXT_QUEUE_ENTRY', queue_entry),
                       ('// INSERT_BOND_PEER', bond_peer),
                       ('// INSERT_BOND_OPCODES', bond_opcodes),
                       ('// INSERT_BOND_AGREEMENT_OPCODES', agreement_opcodes),
                       ('// INSERT_POLICY', policy),
                       ('// INSERT_ORIGIN_KNOWN', origin_known),
                       ('// INSERT_TEXT_HANDLER', text_handler),
                       ('// INSERT_HANDLER_STUBS', stubs),
                       ('// INSERT_TABLE', table + '\n' + size),
                       ('// INSERT_LOOKUP', lookup),
                       ('// INSERT_DISPATCH', dispatch),
                       ('// INSERT_DRAIN', drain),
                       ('// INSERT_OPCODE_NAMES', names),
                       ('// INSERT_ZERO_FLAG_ALLOWED', allowed),
                       ('// INSERT_EXPECTED_MINLEN', expected_minlen)]:
        assert harness.count(tag) == 1, tag
        harness = harness.replace(tag, value)

    failed = []
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-dispatch-') as tmp:
        unit = Path(tmp) / 'test.cpp'
        (unit.parent / 'System_MacText.h').write_text((HERE.parents[1] / 'System_MacText.h').read_text())
        unit.write_text(harness)
        for bond in (1, 0):
            exe = Path(tmp) / f'test_bond{bond}'
            cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
                   '-Wno-unused-function', '-Wno-unused-parameter',
                   f'-DENABLE_BONDED_MODE={bond}', str(unit), '-o', str(exe)]
            if a.sanitize:
                cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
            subprocess.run(cmd, check=True, timeout=120)
            print(f'== ENABLE_BONDED_MODE={bond}', flush=True)
            result = subprocess.run([str(exe)], timeout=60)
            if result.returncode:
                failed.append(bond)
    if failed:
        sys.exit('espnow_dispatch_policy: failures with ENABLE_BONDED_MODE='
                 + ', '.join(str(b) for b in failed))


if __name__ == '__main__':
    main()
