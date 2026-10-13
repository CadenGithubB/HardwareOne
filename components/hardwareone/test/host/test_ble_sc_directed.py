#!/usr/bin/env python3
"""Compile the real BLE Secure Channel module with the real directed senders over two fake peers.

System_BleSecureChannel.cpp is compiled whole against the harness-local
ble_sc_stubs/ headers (libsodium, mbedTLS, FreeRTOS, settings and Arduino
stand-ins). The directed raw sender, the session sender, the console-mirror
entry and the deferred inbound job are spliced in from Bluetooth.cpp. The fake
GATT keeps one inbox per connection, so the suite checks that HELLO_ACK,
CONFIRM_ACK, REJECT and every DATA fragment reach only the connection and
session epoch they belong to, admitted under the BLE lifecycle guard. Crypto is
a routing-neutral stand-in; keys, radio timing and scheduling are not tested.
--component points the suite at another copy of the sources (the pre-fix tree
fails the cross-connection cases).
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_espsr_runtime import function

HERE = Path(__file__).resolve().parent
STUBS = HERE / 'ble_sc_stubs'

# (slot, marker, required). Markers start at the return type so the same text
# matches a static definition in an older tree; none matches a declaration,
# a comment or a call first.
BLOCKS = [
    ('FENCE_STRUCT', 'struct BleSessionFence', True),
    ('FENCE_LIVE', 'static bool bleSessionFenceLive(', False),  # pre-fix sources only
    ('RAW_SESSION', 'bool bleRawNotifyToSession(', True),
    ('SEND', 'bool sendBLEResponseToSession(', True),
    ('MIRROR_COLLECT', 'static int bleCollectMirrorSessions(', False),  # console-mirror collector
    ('MIRROR_SEND', 'bool sendBLEResponse(', True),
    ('DEFERRED_STRUCT', 'struct BleScDeferred', True),
    ('DEFERRED', 'static void bleScDeferredInbound(', True),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--component', type=Path, default=HERE.parents[1])
    args = parser.parse_args()
    if not args.cxx:
        parser.error('a host C++17 compiler is required')
    source = (args.component / 'Bluetooth.cpp').read_text()
    harness = (HERE / 'ble_sc_directed_harness.cpp').read_text()
    for slot, marker, required in BLOCKS:
        token = '/*@' + slot + '@*/'
        assert harness.count(token) == 1, token
        if marker in source:
            harness = harness.replace(token, function(source, marker))
        elif required:
            raise SystemExit(f'{marker!r} not found in {args.component / "Bluetooth.cpp"}')
        else:
            print(f'{marker[:-1]} absent from this source tree; slot left empty', flush=True)
            harness = harness.replace(token, '')
    with tempfile.TemporaryDirectory(prefix='hw1-ble-sc-directed-') as tmp:
        work = Path(tmp)
        shutil.copytree(STUBS, work, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns('._*'))
        for name in ('System_BleSecureChannel.cpp', 'System_BleSecureChannel.h', 'BLE_Types.h'):
            shutil.copy(args.component / name, work / name)
        unit = work / 'harness.cpp'
        unit.write_text(harness)
        exe = work / 'test'
        command = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-Wno-unused-function', '-Wno-unused-parameter', '-I', str(work),
                   str(work / 'System_BleSecureChannel.cpp'), str(unit), '-o', str(exe)]
        if args.sanitize:
            command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(command, check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
