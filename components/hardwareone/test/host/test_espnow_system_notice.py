#!/usr/bin/env python3
"""Run the record a peer's restart leaves and the ESP-NOW text-queue drain.

ESP-NOW 1.0 step 8 retired the BOOT frame (31): a paired peer's restart is
seen in its heartbeat's boot counter (System_ESPNow_PeerRestartCore.h) and
v4h_heartbeat files it with espnowFilePeerRestart.

Compiled part: extracts LogMessageType and TextQueueEntry from System_ESPNow.h
and, from System_ESPNow.cpp, espnowFilePeerRestart and the espnow_task
text-queue drain ("// 11. Drain text message queue"), and runs them against
recording doubles for history, the console, the device names and the event
bus. A restart is printed as "[ESP-NOW] <name> is online (boot #N)" and stored
as one attributed MSG_SYSTEM_EVENT record "Online (boot #N)" with no group id;
the name is the registry name, else the MAC text; chat is printed and stored as
before, and the drain parses nothing.

Source part: EspNowState has no deviceName copy and nothing refers to one;
v4h_text files every frame as chat; v4h_heartbeat files a restart only when a
known counter moved forward (restartSeen), and nothing names the BOOT frame.

--source-dir replays other production sources (for example the tree before the
fix), which compile with the same doubles and fail the checks that name the
defect.
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
    sys.path.insert(0, str(HERE))
    from test_web_batch_handlers import extract_block

    def read(name):
        overlay = a.source_dir / name if a.source_dir else None
        return (overlay if overlay and overlay.exists() else a.component / name).read_text()

    source = read('System_ESPNow.cpp')
    header = read('System_ESPNow.h')

    def definition(text, marker, what):
        count = text.count(marker)
        if count != 1:
            sys.exit(f'FAIL extraction: {what} marker {marker!r} found {count} times, expected 1')
        return extract_block(text, marker)

    # ---- Source checks ------------------------------------------------------
    checks = 0
    failed = 0

    def check(name, ok):
        nonlocal checks, failed
        checks += 1
        if ok:
            print(f'ok   {name}')
        else:
            failed += 1
            print(f'FAIL {name}')

    # Both EspNowState definitions (ESP-NOW on, and the stub for builds without it)
    # are in the header; no other type there has a String deviceName member.
    check('B2 EspNowState keeps no copy of the device name',
          header.count('struct EspNowState {') == 2 and
          re.search(r'^\s*String\s+deviceName\s*;', header, re.MULTILINE) is None)
    check('B3 nothing refers to a gEspNow->deviceName copy',
          'gEspNow->deviceName' not in source and 'gEspNow->deviceName' not in header)
    # The constructor's initializer list must not name the removed member either: no
    # host test compiles the enabled header, and GCC rejects the stray initializer.
    # Every EspNowState constructor with an initializer list: the list runs from the
    # colon to the body's opening brace, which stands alone on its line (members
    # such as devices{} carry braces of their own).
    ctors = []
    for m in re.finditer(r'\bEspNowState\(\)\s*:', header):
        body = re.compile(r'^\s*\{\s*$', re.MULTILINE).search(header, m.end())
        ctors.append(header[m.end():body.start()] if body else '')
    check('B3b the EspNowState constructor does not initialize a deviceName member',
          bool(ctors) and all(re.search(r'\bdeviceName\s*\(', c) is None for c in ctors))
    text_handler = definition(source, 'static V4Verdict v4h_text(', 'System_ESPNow.cpp')
    check('B4 v4h_text files every frame as chat (BOOT 31 retired in 1.0 step 8)',
          'slot.msgType = MSG_TEXT;' in text_handler and 'MSG_SYSTEM_EVENT' not in text_handler)
    heartbeat = definition(source, 'static V4Verdict v4h_heartbeat(', 'System_ESPNow.cpp')
    check('B5 v4h_heartbeat files a restart only for a known counter that moved forward',
          'if (restarted) espnowFilePeerRestart(ctx.h->origin, hb.bootCounter, ctx.isSessionEncrypted);'
          in heartbeat and 'sessionPeerBootCounter(ctx.h->origin, hb.bootCounter, &restarted)' in heartbeat)
    check('B6 nothing names the BOOT frame or its JSON notice',
          all(name not in source and name not in header for name in
              ('ESPNOW_V4_TYPE_BOOT', 'MSG_TYPE_BOOT', 'buildBootNotification', 'meshSendBootToPeers',
               'formatEspNowSystemNotice')))

    # ---- Compiled part ------------------------------------------------------
    message_types = definition(header, 'enum LogMessageType', 'System_ESPNow.h') + ';'
    queue_entry = definition(header, 'struct TextQueueEntry', 'System_ESPNow.h') + ';'
    pieces = [definition(source, 'static void espnowFilePeerRestart(', 'System_ESPNow.cpp')]
    drain = extract_block(source, '// 11. Drain text message queue')
    assert source.count('// 11. Drain text message queue') == 1, 'one drain marker required'

    harness = (HERE / 'espnow_system_notice_harness.cpp').read_text()
    for tag, value in (('// INSERT_MESSAGE_TYPES', message_types),
                       ('// INSERT_TEXT_QUEUE_ENTRY', queue_entry),
                       ('// INSERT_PRODUCTION', '\n\n'.join(pieces)),
                       ('// INSERT_DRAIN', drain)):
        assert harness.count(tag) == 1, tag
        harness = harness.replace(tag, value)

    json_include = a.component.parent / 'hardwareone_libs' / 'ArduinoJson' / 'src'
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-system-notice-') as tmp:
        unit, exe = Path(tmp) / 'espnow_system_notice.cpp', Path(tmp) / 'espnow_system_notice'
        unit.write_text(harness)
        shutil.copy(a.component / 'test/host/web_batch_stubs/Arduino.h', Path(tmp) / 'Arduino.h')
        shutil.copy(a.component / 'System_MacText.h', Path(tmp) / 'System_MacText.h')
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic', '-Wno-unused-function',
               '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1', '-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0',
               '-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0', '-DARDUINOJSON_ENABLE_PROGMEM=0',
               '-I', tmp, '-I', str(json_include), str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                        '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True, timeout=240)
        result = subprocess.run([str(exe)], timeout=60)

    print(f'espnow_system_notice source checks: {checks} checks, {failed} failed')
    if failed or result.returncode:
        print('FAIL espnow_system_notice', file=sys.stderr)
        sys.exit(1)
    print('PASS espnow_system_notice')


if __name__ == '__main__':
    main()
