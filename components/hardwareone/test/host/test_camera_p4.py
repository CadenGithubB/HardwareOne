#!/usr/bin/env python3
"""Exercise actual P4 camera code under host faults; no hardware access."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('c++'))
    args = parser.parse_args()
    flags = ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic', '-Wno-unused-const-variable']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-g']
    with tempfile.TemporaryDirectory(prefix='hw1-camera-p4-host-') as tmp:
        work = Path(tmp)
        def run(source, name, extra=()):
            binary = work / name
            subprocess.run([args.cxx, *flags, '-I', str(COMPONENT), *extra,
                            str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
        run(HERE / 'test_camera_rgb565.cpp', 'resize')
        source = (COMPONENT / 'HAL_Camera_P4.cpp').read_text()
        backend = source[source.index('// Calls are serialized'):source.rindex('#endif')]
        harness = (HERE / 'camera_p4_backend_harness.cpp').read_text()
        generated = work / 'backend.cpp'
        for board in ('P4X_EYE','WaveshareP4'):
            header=(COMPONENT/f'System_Board_{board}.h').read_text()
            definitions='\n'.join(line for line in header.splitlines() if line.startswith('#define CAMERA_'))
            definitions+='\n#define CAMERA_SCCB_I2C_PORT '+('1' if board=='WaveshareP4' else '0')+'\n'
            if board=='P4X_EYE': definitions+='#define CAMERA_SCCB_SHARED 0\n'
            generated.write_text(harness.replace('// INSERT_PRODUCTION_P4_BACKEND',backend).replace('// INSERT_BOARD_CONFIG',definitions))
            run(generated, 'backend-'+board, ('-DHW_CAMERA_HAL_HOST_TEST=1',str(COMPONENT/'HAL_Camera.cpp')))

if __name__ == '__main__':
    main()
