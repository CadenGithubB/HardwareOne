#!/usr/bin/env python3
"""Exercise the production role owner's stuck-phone host restart transaction."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from test_web_batch_handlers import extract_block

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sanitize', action='store_true')
    args = p.parse_args()
    here = Path(__file__).resolve().parent
    source = here.parents[1]
    body = extract_block((source/'Bluetooth.cpp').read_text(), 'void bleRecoverStuckPhone(')
    unit = (here/'phone_recovery_harness.cpp').read_text().replace('// INSERT_PRODUCTION', body)
    with tempfile.TemporaryDirectory(prefix='hw1-phone-recovery-') as tmp:
        cpp, binary = Path(tmp)/'recovery.cpp', Path(tmp)/'recovery'
        cpp.write_text(unit)
        command = [os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra',
                   '-Werror', str(cpp), '-o', str(binary)]
        if args.sanitize:
            command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(command, check=True)
        for case in range(11):
            subprocess.run([str(binary), str(case)], check=True)
