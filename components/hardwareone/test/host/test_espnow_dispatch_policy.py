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
SENSOR_STATUS, TEXT and BOOT need an authenticated frame once the device has a
mesh passphrase (plaintext stays accepted on a keyless device), that plaintext
TEXT accepted on a keyless device is filed under its MAC rather than a paired
peer's name and posts no SYSEVT_TEXT_RX, that authenticated text keeps its
name, encryption flag, per-fragment records and single bus event, that opcode
150 has no row, and that every zero-flag row is on an explicit allow-list.
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
                     'HEARTBEAT',
                     'TOPO_REQ', 'TOPO_START', 'TOPO_PEER', 'USER_SYNC')


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
    drain = 'static void drainTextQueueForTest() ' + extract_block(source, '// 11. Drain text message queue')

    handlers = sorted(set(re.findall(r'\{\s*ESPNOW_V4_TYPE_\w+\s*,[^,]*,\s*(\w+)\s*\}', table)))
    assert handlers, 'no handler rows found in kV4HandlerTable'
    assert 'v4h_text' in handlers, 'the TEXT row must name v4h_text'
    stubs = '\n'.join(f'static void {name}(const V4RxCtx& ctx) {{ noteHandler("{name}", ctx); }}'
                      for name in handlers if name != 'v4h_text')
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
                       ('// INSERT_POLICY', policy),
                       ('// INSERT_ORIGIN_KNOWN', origin_known),
                       ('// INSERT_TEXT_HANDLER', text_handler),
                       ('// INSERT_HANDLER_STUBS', stubs),
                       ('// INSERT_TABLE', table + '\n' + size),
                       ('// INSERT_LOOKUP', lookup),
                       ('// INSERT_DISPATCH', dispatch),
                       ('// INSERT_DRAIN', drain),
                       ('// INSERT_OPCODE_NAMES', names),
                       ('// INSERT_ZERO_FLAG_ALLOWED', allowed)]:
        assert harness.count(tag) == 1, tag
        harness = harness.replace(tag, value)

    failed = []
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-dispatch-') as tmp:
        unit = Path(tmp) / 'test.cpp'
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
