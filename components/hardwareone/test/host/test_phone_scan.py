#!/usr/bin/env python3
"""Run the phone-pipe scan profile, callback tally and scan log (firmware-007).

The header has no SDK dependencies; the pipe's own use of it (callback, scan
setup, records, `phone scan`) runs in `phone_runtime`.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--sanitize',action='store_true');args=p.parse_args()
    with tempfile.TemporaryDirectory(prefix='hw1-phone-scan-') as tmp:
        binary=Path(tmp)/'scan'
        command=[os.environ.get('CXX','clang++'),'-std=c++17','-Wall','-Wextra','-Werror','-pthread',
                 '-I',str(ROOT),str(HERE/'phone_scan_harness.cpp'),'-o',str(binary)]
        if args.sanitize:command[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
        subprocess.run(command,check=True);subprocess.run([str(binary)],check=True)
