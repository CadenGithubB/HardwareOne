#!/usr/bin/env python3
"""Compile the actual G2 camera settings page with a host runtime adapter."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=shutil.which('c++'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    source = (COMPONENT / 'G2_Page_CameraSettings.cpp').read_text()
    # Keep every function, table and conditional from the real page; replace
    # only includes, whose platform declarations the harness supplies.
    source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
    harness = (HERE / 'camera_consumers_harness.cpp').read_text()
    unit_text = harness.replace('// INSERT_PRODUCTION_CAMERA_SETTINGS', source)
    with tempfile.TemporaryDirectory(prefix='hw1-camera-consumers-') as tmp:
        unit, binary = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
        unit.write_text(unit_text)
        command = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
                   '-DHW_CAMERA_HAL_HOST_TEST=1', '-I', str(COMPONENT),
                   str(unit), str(COMPONENT / 'HAL_Camera.cpp'), '-o', str(binary)]
        if args.sanitize:
            command[1:1] = ['-fsanitize=address,undefined', '-g']
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
        unit.write_text((HERE / 'test_camera_hal.cpp').read_text())
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
