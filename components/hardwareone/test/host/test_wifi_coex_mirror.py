#!/usr/bin/env python3
"""Run System_WiFi.cpp's coexistence mirror over fake Arduino Wi-Fi events.

The mirror's atomics, scan and connect scopes, event handler, install and
wifiCoexTag() are extracted unchanged (docs/WIFI_BLE_COEXISTENCE_STUDY.md,
lever 1); the classifier they feed runs in `ble_coex_records`.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def generated() -> str:
    source = (ROOT / 'System_WiFi.cpp').read_text()
    start = source.index('#if ENABLE_BLE_COEX_RECORDS\nnamespace {\n')
    state = source[start + len('#if ENABLE_BLE_COEX_RECORDS\n'):source.index('#else', start)]
    functions = '\n\n'.join(extract_block(source, marker) for marker in (
        'static void wifiCoexEvent(', 'bool wifiCoexMirrorInstall()', 'WifiCoexTag wifiCoexTag()'))
    harness = (HERE / 'wifi_coex_mirror_harness.cpp').read_text()
    assert harness.count('// INSERT_PRODUCTION') == 1
    return harness.replace('// INSERT_PRODUCTION', state + '\n' + functions)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(); parser.add_argument('--sanitize', action='store_true'); args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='hw1-wifi-coex-mirror-') as tmp:
        unit, binary = Path(tmp) / 'mirror.cpp', Path(tmp) / 'mirror'
        unit.write_text(generated())
        command = [os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
                   '-I', str(ROOT), str(unit), '-o', str(binary)]
        if args.sanitize:
            command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
