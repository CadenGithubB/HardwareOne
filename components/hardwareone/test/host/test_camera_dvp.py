#!/usr/bin/env python3
"""Exercise the production DVP backend against simulated camera and SCCB edges."""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--sanitize', action='store_true')
    args = p.parse_args()
    source = (ROOT / 'HAL_Camera_DVP.cpp').read_text()
    body = source[source.index('namespace {'):source.rindex('#endif')]
    with tempfile.TemporaryDirectory(prefix='hw1-camera-dvp-') as tmp:
        tmp = Path(tmp)
        (tmp / 'camera_dvp_extracted.inc').write_text(body)
        for port in (0, 1):
            cmd = [os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra',
                   '-DHW_CAMERA_HAL_HOST_TEST=1', f'-DCAMERA_SCCB_I2C_PORT={port}',
                   f'-DCONFIG_SCCB_HARDWARE_I2C_PORT1={port}', '-I', str(ROOT), '-I', str(tmp),
                   str(HERE / 'camera_dvp_harness.cpp'), str(ROOT / 'HAL_Camera.cpp'),
                   str(ROOT / 'HAL_JPEG.cpp'), '-o', str(tmp / f'dvp-{port}')]
            if args.sanitize:
                cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            subprocess.run(cmd, check=True)
            subprocess.run([str(tmp / f'dvp-{port}'), str(HERE / 'jpeg_fixtures/rgb420_17x19.jpg')], check=True)

if __name__ == '__main__':
    main()
