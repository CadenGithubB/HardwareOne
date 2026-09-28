#!/usr/bin/env python3
"""Compile real BLE callback, sender, trace and history seams with fake GATT."""
import argparse
from pathlib import Path
import subprocess
import tempfile
from test_espsr_runtime import function

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize',action='store_true')
    args=parser.parse_args()
    source=(SOURCE/'Bluetooth.cpp').read_text()
    harness=(HERE/'stt_ble_privacy_harness.cpp').read_text()
    names=[('CLASSIFY','static bool bleOutputHasPrivateSTT('),
           ('TRACE','static void bleTxTrace('),
           ('HISTORY','static void bleRememberOutput('),
           ('RAW_SESSION','static bool bleRawNotifyToSession('),
           ('RAW_BROADCAST','bool bleRawNotify('),
           ('SEND','bool sendBLEResponseToSession('),
           ('CALLBACK','static void bleCommandResultCallback(')]
    for marker,name in names:
        harness=harness.replace('// INSERT_'+marker,function(source,name))
    with tempfile.TemporaryDirectory(prefix='hw1-stt-ble-privacy-') as tmp:
        unit=Path(tmp)/'test.cpp';exe=Path(tmp)/'test';unit.write_text(harness)
        command=['clang++','-std=c++17','-Wall','-Wextra','-Werror',
                 '-Wno-unused-function','-Wno-unused-parameter',str(unit),'-o',str(exe)]
        if args.sanitize:command[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(command,check=True);subprocess.run([str(exe)],check=True)


if __name__=='__main__':main()
