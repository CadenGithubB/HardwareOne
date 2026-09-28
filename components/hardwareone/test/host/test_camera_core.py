#!/usr/bin/env python3
"""Compile exact production camera lifecycle functions against a failure-injecting HAL.

The real worker is qualified separately; this harness exercises shared ownership,
serialization, recovery fencing, temporary settings, and persisted control failures.
"""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    source = (ROOT / 'System_Camera_DVP.cpp').read_text()
    blocks = [
        source[source.index('// Static storage prevents'):source.index('const char* buildCameraStatusJson()')],
        source[source.index('static const char* cameraIntegerCommand('):source.index('#define CAMERA_INT_COMMAND')],
        source[source.index('static const char* applyCameraResolutionSetting('):source.index('const char* cmd_cameratiny(')],
    ]
    with tempfile.TemporaryDirectory(prefix='hw1-camera-core-') as tmp:
        tmp = Path(tmp)
        (tmp / 'camera_core_extracted.inc').write_text('\n'.join(blocks))
        cmd = [os.environ.get('CXX', 'c++'), '-std=c++17', '-pthread', '-Wall', '-Wextra',
               '-Wno-unused-variable', '-Wno-unused-function', '-DHW_CAMERA_HAL_HOST_TEST=1',
               '-I', str(ROOT), '-I', str(tmp), str(Path(__file__).with_name('camera_core_harness.cpp')),
               str(ROOT / 'HAL_Camera.cpp'), '-o', str(tmp / 'camera-core')]
        if args.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(cmd, check=True)
        subprocess.run([str(tmp / 'camera-core')], check=True)

if __name__ == '__main__':
    main()
