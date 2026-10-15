#!/usr/bin/env python3
"""Execute the phone owner's actual enrollment/scheduling state machine.

SDK/BLE/account/clock operations are host boundaries; source bodies and state
are extracted unchanged. Native GATT and the wire core have separate harnesses.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from test_web_batch_handlers import extract_block

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]

def generated():
    source=(ROOT/'BLE_PhonePipe.cpp').read_text()
    kinds='\n'.join(extract_block(source,k)+';' for k in
                    ('enum class Step','enum class GapKind','struct GapEvent','struct Route','struct Wipe'))
    storage=source[source.index('Gatt phoneGatt;'):source.index('\nclass Lock')]
    functions='\n\n'.join(extract_block(source,k) for k in (
        'void setStep(', 'void routeSet(', 'bool matches(', 'bool hasService(',
        'int bondAddress(', 'bool bondFor(', 'bool currentJob(', 'void fail(', 'bool restorePolicy(',
        'void releaseSetup(', 'void configureScan(', 'void prepareBond(',
        'void finishForget(', 'void startForget(', 'void consume(', 'bool configureSecurityPolicy() ', 'void startSecurity(',
        'void beginJob(', 'void tickLocked('))
    return (HERE/'phone_runtime_harness.cpp').read_text().replace('// INSERT_TYPES',kinds).replace(
        '// INSERT_STORAGE',storage).replace('// INSERT_PRODUCTION',functions)

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--sanitize',action='store_true');args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='hw1-phone-runtime-') as tmp:
        unit=Path(tmp)/'runtime.cpp';binary=Path(tmp)/'runtime';unit.write_text(generated())
        command=[os.environ.get('CXX','clang++'),'-std=c++17','-Wall','-Wextra','-Werror',
                 '-Wno-unused-function','-Wno-unused-variable','-Wno-unused-parameter',
                 '-I',str(ROOT),str(unit),'-o',str(binary)]
        if args.sanitize:command[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
        subprocess.run(command,check=True);subprocess.run([str(binary)],check=True)
