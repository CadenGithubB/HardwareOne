#!/usr/bin/env python3
"""Run the production BLE host-recycle guard with a READY phone pipe.

With the glasses off the phone can be the only live central link. An accessory
admission wedge must not tear it down, and a forced client retirement that does
recycle must request the phone's reconnect. Host and peer calls are fakes.
"""
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
    source=(ROOT/'G2_Glasses.cpp').read_text()
    unit=(HERE/'ble_recycle_phone_harness.cpp').read_text().replace(
        '// INSERT_PRODUCTION',extract_block(source,'bool bleStackRecycleIfWedged(void)'))
    with tempfile.TemporaryDirectory(prefix='hw1-ble-recycle-') as tmp:
        path=Path(tmp)/'recycle.cpp';binary=Path(tmp)/'recycle';path.write_text(unit)
        command=[os.environ.get('CXX','clang++'),'-std=c++17','-Wall','-Wextra','-Werror',
                 '-Wno-unused-function','-Wno-unused-parameter',str(path),'-o',str(binary)]
        if args.sanitize:command[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
        subprocess.run(command,check=True);subprocess.run([str(binary)],check=True)
