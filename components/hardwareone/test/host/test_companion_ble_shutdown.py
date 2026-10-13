#!/usr/bin/env python3
"""The C6 companion reset stops the BLE server only through its owner.

shutDownBleForCompanionReset() (System_RadioCompanion.cpp) is compiled against
a fake BLE owner layer. deinitBluetooth() reports false while the Ring owner has
not quiesced; the reset must then retry the stop a bounded number of times and
never force bluetoothHalDeinit() under the busy Ring owner. A stop that never
completes leaves the host up, raises the lifecycle fault (Bluetooth off until a
reboot) and logs why. A stop that completes, a G2-client or bare-host teardown,
a busy role claim and an idle stack behave as before. --source-dir points the
suite at another copy of the component (the pre-fix tree forces the host down
in K1 and does not retry in K2).
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
MARKER = 'void shutDownBleForCompanionReset(const RadioUsers& users)'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--source-dir', '--component', dest='source_dir', type=Path, default=COMPONENT)
    args = parser.parse_args()
    source = (args.source_dir / 'System_RadioCompanion.cpp').read_text()
    if source.count(MARKER) != 1:
        raise SystemExit(f'expected one definition of {MARKER!r} in System_RadioCompanion.cpp')
    harness = (HERE / 'companion_ble_shutdown_harness.cpp').read_text()
    token = '/*@SHUTDOWN@*/'
    assert harness.count(token) == 1, token
    harness = harness.replace(token, extract_block(source, MARKER))
    with tempfile.TemporaryDirectory(prefix='hw1-companion-ble-shutdown-') as tmp:
        unit = Path(tmp) / 'harness.cpp'
        exe = Path(tmp) / 'test'
        unit.write_text(harness)
        command = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-Wno-unused-function', '-Wno-unused-parameter', str(unit), '-o', str(exe)]
        if args.sanitize:
            command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(command, check=True)
        run = subprocess.run([str(exe)])
    sys.exit(run.returncode)


if __name__ == '__main__':
    main()
