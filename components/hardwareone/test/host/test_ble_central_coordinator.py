#!/usr/bin/env python3
"""Compile actual coordinator and TX owners; use threads for contending peers."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--sanitize',action='store_true')
    args=parser.parse_args()
    here=Path(__file__).resolve().parent
    source=here.parents[1]
    with tempfile.TemporaryDirectory(prefix='hw1-ble-coordinator-') as tmp:
        binary=str(Path(tmp)/'test')
        cmd=[os.environ.get('CXX','clang++'),'-std=c++17','-Wall','-Wextra','-Werror',
             '-pedantic','-pthread','-I',str(here/'ble_coordinator_stubs'),'-I',str(source),
             str(here/'ble_central_coordinator_harness.cpp'),str(source/'BLE_CentralCoordinator.cpp'),
             str(source/'BLE_CentralTx.cpp'),'-o',binary]
        if args.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
        subprocess.run(cmd,check=True)
        subprocess.run([binary],check=True)
