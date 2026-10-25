#!/usr/bin/env python3
"""Run the production reconnect admission commit through repeated attempts.

The phone's schedule caps at 30 s (A-3); glasses and ring keep 5/15/45/90/180 s.
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
    source=(ROOT/'BLE_Peers.cpp').read_text()
    production='\n'.join([
        source[source.index('static constexpr uint32_t kAdmissionBusyRetryMs'):source.index('struct ReconnectAdmissionClaim')],
        extract_block(source,'struct ReconnectAdmissionClaim')+';',
        extract_block(source,'static uint32_t bleReconnectBackoffMs('),
        extract_block(source,'static bool commitAdmission('),
    ])
    unit=(HERE/'ble_reconnect_schedule_harness.cpp').read_text().replace('// INSERT_PRODUCTION',production)
    with tempfile.TemporaryDirectory(prefix='hw1-reconnect-schedule-') as tmp:
        path=Path(tmp)/'schedule.cpp';binary=Path(tmp)/'schedule';path.write_text(unit)
        command=[os.environ.get('CXX','clang++'),'-std=c++17','-Wall','-Wextra','-Werror',
                 '-Wno-unused-function','-Wno-unused-const-variable',str(path),'-o',str(binary)]
        if args.sanitize:command[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
        subprocess.run(command,check=True);subprocess.run([str(binary)],check=True)
