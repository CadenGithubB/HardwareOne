#!/usr/bin/env python3
"""Compile real BLE callback, sender, trace, history and console-mirror seams with fake GATT.

The harness models a four-slot GATT peer table: directed sends land in the
addressed slot's inbox, the characteristic-wide notify lands in every connected
slot's inbox (the vendored Arduino wrapper's semantics). Besides the private-STT
and exact-session checks, the console-mirror chokepoint sendBLEResponse must
reach only logged-in non-guest sessions. --component points the suite at another
copy of the sources (the pre-fix tree fails the unauthenticated-peer case).
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_espsr_runtime import function

HERE = Path(__file__).resolve().parent


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx',default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    parser.add_argument('--sanitize',action='store_true')
    parser.add_argument('--component',type=Path,default=HERE.parents[1])
    args=parser.parse_args()
    if not args.cxx:
        parser.error('a host C++17 compiler is required')
    source=(args.component/'Bluetooth.cpp').read_text()
    harness=(HERE/'stt_ble_privacy_harness.cpp').read_text()
    names=[('CLASSIFY','static bool bleOutputHasPrivateSTT('),
           ('TRACE','static void bleTxTrace('),
           ('HISTORY','static void bleRememberOutput('),
           ('RAW_SESSION','bool bleRawNotifyToSession('),
           ('SEND','bool sendBLEResponseToSession('),
           ('CALLBACK','static void bleCommandResultCallback('),
           ('MIRROR_SEND','bool sendBLEResponse(')]
    for marker,name in names:
        harness=harness.replace('// INSERT_'+marker,function(source,name))
    # The characteristic-wide sender and the callback session fence exist only in
    # pre-fix sources; the harness keeps both call shapes so either tree compiles.
    for marker,name in [('RAW_BROADCAST','bool bleRawNotify('),
                        ('FENCE','static bool bleSessionFenceLive(')]:
        harness=harness.replace('// INSERT_'+marker,function(source,name) if name in source else '')
    # The recipient collector exists only in fixed sources; a pre-fix overlay
    # still compiles and reaches the behavioral assertions, which name the defect.
    collector='static int bleCollectMirrorSessions('
    if collector in source:
        harness=harness.replace('// INSERT_MIRROR_COLLECT',function(source,collector))
    else:
        print('collector absent (pre-fix source): sendBLEResponse runs without it',flush=True)
    with tempfile.TemporaryDirectory(prefix='hw1-stt-ble-privacy-') as tmp:
        unit=Path(tmp)/'test.cpp';exe=Path(tmp)/'test';unit.write_text(harness)
        command=[args.cxx,'-std=c++17','-Wall','-Wextra','-Werror',
                 '-Wno-unused-function','-Wno-unused-parameter',str(unit),'-o',str(exe)]
        if args.sanitize:command[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(command,check=True);subprocess.run([str(exe)],check=True)


if __name__=='__main__':main()
