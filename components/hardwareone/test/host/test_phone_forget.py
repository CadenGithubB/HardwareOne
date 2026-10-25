#!/usr/bin/env python3
"""Run the real registry unpair transactions against isolated lock/state boundaries."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from test_web_batch_handlers import extract_block
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--sanitize',action='store_true');args=p.parse_args()
    source=(ROOT/'BLE_Peers.cpp').read_text()
    functions='\n'.join(extract_block(source,k) for k in (
        'bool blePeerBeginPhonePipeForget(', 'bool blePeerPhonePipeForgetIsCurrent(',
        'bool blePeerCommitPhonePipeForget(', 'bool blePeerClearPhonePipeEnrollment(',
        'bool blePeerCommitPhonePipeEnrollmentIfCurrent('))
    unit=(HERE/'phone_forget_harness.cpp').read_text().replace('// INSERT_PRODUCTION',functions)
    with tempfile.TemporaryDirectory(prefix='hw1-phone-forget-') as tmp:
        path=Path(tmp)/'forget.cpp';binary=Path(tmp)/'forget';path.write_text(unit)
        command=[os.environ.get('CXX','clang++'),'-std=c++17','-Wall','-Wextra','-Werror',str(path),'-o',str(binary)]
        if args.sanitize:command[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
        subprocess.run(command,check=True);subprocess.run([str(binary)],check=True)
