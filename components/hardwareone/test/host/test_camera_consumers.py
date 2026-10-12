#!/usr/bin/env python3
"""Compile the actual G2 camera settings page with a host runtime adapter.

The hijack-page enum, the command cookie and the redraw declaration come from
the production headers, so a boundary change breaks this build instead of
leaving the stubs compiling against a stale copy.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

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
    glasses = (COMPONENT / 'G2_Glasses.h').read_text()
    hijack = (COMPONENT / 'G2_HijackCmd.h').read_text()
    redraw = re.findall(r'^bool g2EnqueueRedraw\([^;]*\);', hijack, flags=re.M)
    require(len(redraw) == 1, 'G2_HijackCmd.h declares g2EnqueueRedraw exactly once')
    # The harness static_asserts that its stub has the declared function type.
    boundary = '\n'.join((
        extract_block(glasses, 'enum G2HijackPage') + ';',
        extract_block(hijack, 'struct G2CmdCookie') + ';',
        'namespace header_boundary {', redraw[0], '}'))
    harness = (HERE / 'camera_consumers_harness.cpp').read_text()
    for marker, value in (('// INSERT_G2_BOUNDARY_TYPES', boundary),
                          ('// INSERT_PRODUCTION_CAMERA_SETTINGS', source)):
        require(harness.count(marker) == 1, f'one insertion marker: {marker}')
        harness = harness.replace(marker, value)
    unit_text = harness
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
