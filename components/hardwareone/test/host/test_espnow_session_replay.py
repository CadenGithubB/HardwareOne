#!/usr/bin/env python3
"""Run the production SESSION_FRAME wrap/unwrap, replay window and REKEY key
swap from System_ESPNow_Sessions.cpp against a scripted peer pair.

The production wire header and session record are used unchanged. Doubles: a
fake clock, counting log macros, a keyed AEAD double (binds key, nonce, AAD and
ciphertext) and a no-op pending-frame drain. Frames recorded under the previous
key epoch are replayed after a REKEY: they must be rejected and must never seed
or advance the new key's replay window, while a genuinely in-flight old-key
frame is still accepted once. No radio or hardware is accessed.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--source-dir', type=Path,
                   help='directory whose System_ESPNow_Sessions.{cpp,h} / System_ESPNow_Wire.h / '
                        'System_ESPNow_Deadline.h replace the component copies')
    a = p.parse_args()
    if not a.cxx:
        p.error('a host C++17 compiler is required')
    sys.path.insert(0, str(a.component / 'test/host'))
    from test_web_batch_handlers import extract_block

    def locate(name):
        overlay = a.source_dir / name if a.source_dir else None
        return overlay if overlay and overlay.exists() else a.component / name

    def read(name):
        return locate(name).read_text()

    # The session functions compare their deadlines through this header once
    # the tree has it; '' when neither the overlay nor the component does.
    deadline = locate('System_ESPNow_Deadline.h')
    deadline = deadline.read_text() if deadline.exists() else ''

    source = read('System_ESPNow_Sessions.cpp')
    header = read('System_ESPNow_Sessions.h')
    wire = read('System_ESPNow_Wire.h')
    # Real flag enum + packed 32-byte header with its static_assert.
    start = wire.index('enum EspNowV4Flags : uint16_t {')
    end = wire.index('static_assert(sizeof(EspNowV4Header) == 32')
    wire = wire[start:wire.index('\n', end) + 1]
    # Real lifecycle enum, rekey constants, SessionState and its size assertion.
    types = header[header.index('enum SessionStateLifecycle : uint8_t {'):
                   header.index('// One-time allocation of the gSessions table')]
    internal = '\n\n'.join(extract_block(source, marker) for marker in
                           ('void buildNonce(', 'bool replayCheckAndInsert('))
    production = '\n\n'.join(extract_block(source, marker) for marker in
                             ('void sessionClear(SessionState* s)',
                              'bool sessionWrapFrame(SessionState* s,',
                              'bool sessionUnwrapFrame(SessionState* s,',
                              'void sessionApplyRekeyedKeys(SessionState* s,',
                              'bool sessionMarkRekeyInitiated(SessionState* s,',
                              'void sessionAbortRekey(SessionState* s)',
                              'void sessionRekeyPrevKeysSweep(uint32_t nowMs)'))
    # An overlay supplies production code, never a historical copy of the test,
    # so the current regressions run against the pre-fix implementation.
    harness = (HERE / 'espnow_session_replay_harness.cpp').read_text()
    for tag, value in [('// INSERT_DEADLINE', deadline),
                       ('// INSERT_WIRE', wire), ('// INSERT_SESSION_TYPES', types),
                       ('// INSERT_INTERNAL', internal), ('// INSERT_PRODUCTION', production)]:
        assert harness.count(tag) == 1, tag
        harness = harness.replace(tag, value)
    has_prev_window = 'rxSeqHighWaterPrev' in types
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-session-replay-') as tmp:
        unit, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
        unit.write_text(harness)
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
               f'-DHW1_HAS_PREV_WINDOW={1 if has_prev_window else 0}', str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True, timeout=120)
        subprocess.run([str(exe)], check=True, timeout=60)


if __name__ == '__main__':
    main()
