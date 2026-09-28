#!/usr/bin/env python3
"""Compile/run the platform-independent streaming segmenter regression harness."""
import argparse
from pathlib import Path
import subprocess
import tempfile
HERE=Path(__file__).resolve().parent

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize',action='store_true')
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='hw1-stt-segmenter-') as temporary:
        binary=Path(temporary)/'segmenter'
        command=['clang++','-std=c++17','-O2','-Wall','-Wextra','-Werror',str(HERE/'stt_segmenter_harness.cpp'),'-I'+str(HERE.parents[1]/'stt'),'-o',str(binary)]
        if args.sanitize:command[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(command,check=True)
        subprocess.run([str(binary)],check=True)
if __name__=='__main__':main()
