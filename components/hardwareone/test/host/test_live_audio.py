#!/usr/bin/env python3
"""Compile the shipping HAL and complete live-audio module against boundary mocks."""
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
    harness = (HERE / 'live_audio_harness.cpp').read_text()
    for marker, filename in (
        ('HAL_HEADER', 'HAL_Audio.h'), ('HAL_SOURCE', 'HAL_Audio.cpp'),
        ('LIVE_HEADER', 'System_LiveAudio.h'), ('LIVE_SOURCE', 'System_LiveAudio.cpp'),
    ):
        source = (COMPONENT / filename).read_text()
        source = re.sub(r'^#include[^\n]*', '', source, flags=re.M)
        harness = harness.replace('// INSERT_' + marker, source)
    with tempfile.TemporaryDirectory(prefix='hw1-live-audio-') as tmp:
        unit = Path(tmp) / 'test.cpp'
        unit.write_text(harness)
        cmd = [args.cxx, '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
               '-pedantic', '-Wno-unused-const-variable', str(unit), '-o', str(Path(tmp) / 'test')]
        if args.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-g']
        subprocess.run(cmd, check=True)
        subprocess.run([str(Path(tmp) / 'test')], check=True)


if __name__ == '__main__':
    main()
