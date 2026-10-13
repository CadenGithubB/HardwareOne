#!/usr/bin/env python3
"""Compile exact production camera lifecycle functions against a failure-injecting HAL.

The real worker task and queue are qualified separately; this harness exercises
shared ownership, serialization, recovery fencing, temporary settings, persisted
control failures, the worker's per-command body (cameraPwrRunOne: a failed start
keeps the saved auto-start and posts one start-failed event) and capture
sessions (recovery, resolution changes and stops leave them to their owners).
"""
from pathlib import Path
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_web_batch_handlers import extract_block  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('c++') or 'c++')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    source = (ROOT / 'System_Camera_DVP.cpp').read_text()
    blocks = [
        source[source.index('// Static storage prevents'):source.index('const char* buildCameraStatusJson()')],
        source[source.index('static const char* cameraIntegerCommand('):source.index('#define CAMERA_INT_COMMAND')],
        source[source.index('static const char* applyCameraResolutionSetting('):source.index('const char* cmd_cameratiny(')],
    ]
    header = (ROOT / 'System_Camera_DVP.h').read_text()
    session_kind = re.search(r'(?m)^enum class CameraSessionKind[^\n]+', header)
    types = '#define HAS_CAMERA_SESSIONS %d\n' % (1 if session_kind else 0)
    if session_kind:
        types += session_kind[0] + '\n'
    worker = '\n'.join([
        re.search(r'(?ms)^enum : uint8_t \{\n  CAM_PWR_CMD_START.*?^\};', source)[0],
        extract_block(source, 'struct CameraPwrMsg') + ';',
        extract_block(source, 'static void cameraPwrRunOne('),
    ])
    with tempfile.TemporaryDirectory(prefix='hw1-camera-core-') as tmp:
        tmp = Path(tmp)
        (tmp / 'camera_core_types.inc').write_text(types)
        (tmp / 'camera_core_extracted.inc').write_text('\n'.join(blocks))
        (tmp / 'camera_pwr_extracted.inc').write_text(worker)
        cmd = [args.cxx, '-std=c++17', '-pthread', '-Wall', '-Wextra',
               '-Wno-unused-variable', '-Wno-unused-function', '-DHW_CAMERA_HAL_HOST_TEST=1',
               '-I', str(ROOT), '-I', str(tmp), str(Path(__file__).with_name('camera_core_harness.cpp')),
               str(ROOT / 'HAL_Camera.cpp'), '-o', str(tmp / 'camera-core')]
        if args.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(cmd, check=True)
        subprocess.run([str(tmp / 'camera-core')], check=True)
        for scenario in ('power-start', 'sessions'):
            subprocess.run([str(tmp / 'camera-core'), scenario], check=True)

if __name__ == '__main__':
    main()
