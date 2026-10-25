#!/usr/bin/env python3
"""Exercise the production native adapter with asynchronous synthetic SDK events."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from phone_crypto_build import crypto_flags

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--idf', type=Path)
    parser.add_argument('--crypto-build', type=Path, help='Reuse an already built native crypto directory')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parents[1]
    with tempfile.TemporaryDirectory(prefix='hw1-phone-gatt-') as tmp, crypto_flags(args.idf,args.crypto_build) as (includes,libraries):
        binary = str(Path(tmp) / 'test')
        cmd = [os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra',
               '-Werror', '-pedantic', '-DSYSTEM_BUILDCONFIG_H', '-DENABLE_BLUETOOTH=1',
               '-DENABLE_BLE_PHONE_PIPE=1', '-I', str(here / 'phone_gatt_stubs'),
               '-I', str(root), str(here / 'phone_gatt_harness.cpp'),
               str(root / 'BLE_PhoneGatt.cpp'), *includes, *libraries, '-o', binary]
        if args.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True)
        subprocess.run([binary], check=True)
