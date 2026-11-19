#!/usr/bin/env python3
"""espnowsessions output sized to the debug buffer.

Runs the production cmd_espnow_sessions (System_ESPNow.cpp) over a session
table double with 0 to 16 ACTIVE sessions, every capability bit set and the
largest numbers: the JSON form (docs/APP_JSON_CONTRACT.md) is always whole JSON
inside the 4096-byte debug buffer, the sessions it cannot hold left out and
counted in "more"; the text form always ends in a NUL and says how many
sessions it did not show. Fails on a tree that writes into 1024 bytes (4 such
sessions overflowed it, and serializeJson left the reply unterminated).

Options: --cxx, --sanitize, --component <dir>.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
MARKER = 'const char* cmd_espnow_sessions(const String& argsInput)'


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    a = p.parse_args()
    if not a.cxx:
        p.error('a host C++17 compiler is required')
    sys.path.insert(0, str(HERE))
    from test_web_batch_handlers import extract_block

    source = (a.component / 'System_ESPNow.cpp').read_text()
    if source.count(MARKER) != 1:
        sys.exit(f'FAIL extraction: {MARKER!r} found {source.count(MARKER)} times, expected 1')
    harness = (HERE / 'espnow_sessions_output_harness.cpp').read_text()
    assert harness.count('// INSERT_PRODUCTION') == 1
    harness = harness.replace('// INSERT_PRODUCTION', extract_block(source, MARKER))

    json_include = a.component.parent / 'hardwareone_libs' / 'ArduinoJson' / 'src'
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-sessions-output-') as tmp:
        t = Path(tmp)
        unit, exe = t / 'espnow_sessions_output.cpp', t / 'espnow_sessions_output'
        unit.write_text(harness)
        shutil.copy(HERE / 'web_batch_stubs' / 'Arduino.h', t / 'Arduino.h')
        shutil.copy(HERE / 'psram_json_doc_host.h', t / 'psram_json_doc_host.h')
        for name in ('System_MacText.h', 'System_ESPNow_SessionCapsCore.h'):
            shutil.copy(a.component / name, t / name)
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
               '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1', '-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0',
               '-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0', '-DARDUINOJSON_ENABLE_PROGMEM=0',
               '-I', tmp, '-I', str(json_include), str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                        '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True, timeout=240)
        if subprocess.run([str(exe)], timeout=60).returncode:
            sys.exit('FAIL espnow_sessions_output')
    print('PASS espnow_sessions_output')


if __name__ == '__main__':
    main()
