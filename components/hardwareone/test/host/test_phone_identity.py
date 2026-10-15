#!/usr/bin/env python3
"""Compile the production phone identity boundary with synthetic addresses."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix='hw1-phone-identity-') as tmp:
        binary = str(Path(tmp) / 'test')
        cmd = [os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra',
               '-Werror', '-pedantic', '-I', str(here.parents[1]),
               str(here / 'phone_identity_harness.cpp'), '-o', binary]
        if args.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True)
        subprocess.run([binary], check=True)
