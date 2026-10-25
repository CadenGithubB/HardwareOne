#!/usr/bin/env python3
"""Compile actual coordinator and TX owners; use threads for contending peers."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from test_web_batch_handlers import extract_block

if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--sanitize',action='store_true')
    args=parser.parse_args()
    here=Path(__file__).resolve().parent
    source=here.parents[1]
    # Admission must precede both allocation and native connect in every
    # production accessory caller, so a refusal never reaches wedge accounting.
    for file,signature in [('G2_Glasses.cpp','static bool connectTemple(G2Temple& t, hw1coex::Purpose purpose) '),
                           ('G2_Ring.cpp','bool ringPerformConnect(')]:
        body=extract_block((source/file).read_text(),signature)
        admission=body.index('BleCentralLinkGuard link(')
        refusal=body.index('if (!link)',admission)
        allocation=body.index('BLEDevice::createClient()',refusal)
        assert admission < refusal < body.index('return false;',refusal) < allocation
        assert body.index('link.retain(',allocation) > allocation
    with tempfile.TemporaryDirectory(prefix='hw1-ble-coordinator-') as tmp:
        binary=str(Path(tmp)/'test')
        cmd=[os.environ.get('CXX','clang++'),'-std=c++17','-Wall','-Wextra','-Werror',
             '-pedantic','-pthread','-I',str(here/'ble_coordinator_stubs'),'-I',str(source),
             str(here/'ble_central_coordinator_harness.cpp'),str(source/'BLE_CentralCoordinator.cpp'),
             str(source/'BLE_CentralTx.cpp'),'-o',binary]
        if args.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
        subprocess.run(cmd,check=True)
        subprocess.run([binary],check=True,timeout=180)
