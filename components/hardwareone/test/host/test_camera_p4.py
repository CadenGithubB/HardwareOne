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
    flags = ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic']
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
        generated.write_text(harness.replace('// INSERT_PRODUCTION_P4_BACKEND', backend))
        run(generated, 'backend', ('-DHW_CAMERA_HAL_HOST_TEST=1',
                                 str(COMPONENT / 'HAL_Camera.cpp')))

if __name__ == '__main__':
    main()
