#!/usr/bin/env python3
"""Actual shared transcript writer with controlled filesystem/auth/clock faults."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    clean = lambda text: re.sub(r'^#include.*\n|^#pragma once\n', '', text, flags=re.M)
    harness = (HERE / 'transcript_harness.cpp').read_text()
    harness = harness.replace('// INSERT_INTERFACE', clean((SOURCE / 'System_Transcript.h').read_text()))
    harness = harness.replace('// INSERT_RUNTIME', clean((SOURCE / 'System_Transcript.cpp').read_text()))
    with tempfile.TemporaryDirectory(prefix='hw1-transcript-') as tmp:
        source = Path(tmp) / 'test.cpp'
        source.write_text(harness)
        for local, dictate in ((1, 0), (0, 1), (1, 1)):
            exe = Path(tmp) / f'test-{local}-{dictate}'
            cmd = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-Wno-unused-function', f'-DENABLE_LOCAL_STT={local}',
                   f'-DENABLE_DICTATION={dictate}', str(source), '-o', str(exe)]
            if args.sanitize:
                cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            subprocess.run(cmd, check=True, timeout=120)
            subprocess.run([str(exe)], check=True, timeout=60)

if __name__ == '__main__':
    main()
