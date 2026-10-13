#!/usr/bin/env python3
"""Compile the real BLE command ingress with the real Secure Channel module and check its policy.

System_BleSecureChannel.cpp is compiled whole against the harness-local
ble_sc_stubs/ headers (libsodium, mbedTLS, FreeRTOS, settings and Arduino
stand-ins). The GATT write entry, the deferred secure-frame and plaintext jobs,
the command tail with its file-browser gate, and the directed and session
senders are spliced in from Bluetooth.cpp. The fake GATT keeps one inbox per
connection and the executor records what would run; the GATT entry runs as
BTC_TASK and deferred jobs run after it as cmd_exec_task. The suite checks that a
plaintext write in secure-required mode gets the fixed refusal on its own
connection, that file verbs run only when the command itself arrived encrypted,
that no reply to a plaintext write waits on the secure-channel tx mutex on
BTC_TASK (B cases), and that a plaintext write's pool slot, which can carry a
login password and prefers PSRAM, is wiped before every release (W cases).
A source check reads the command registry: blesecure,
blesecret and blerequireauth share the superadmin tier. --component points the
suite at another copy of the sources (the pre-fix tree fails the refusal,
file-verb, tier and BTC_TASK cases).
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from test_espsr_runtime import function

HERE = Path(__file__).resolve().parent
STUBS = HERE / 'ble_sc_stubs'

# (slot, marker, required). Markers start at the return type, so the same text
# matches a static definition in an older tree; function() skips the forward
# declarations of processIncomingBLECommand and bleRawNotifyToSession.
BLOCKS = [
    ('FENCE_STRUCT', 'struct BleSessionFence', True),
    ('FENCE_LIVE', 'static bool bleSessionFenceLive(', False),  # pre-fix sources only
    ('RAW_SESSION', 'bool bleRawNotifyToSession(', True),
    ('SEND', 'bool sendBLEResponseToSession(', True),
    ('LOGIN_JOB', 'struct BleLoginAsyncJob', True),
    ('REPLY_CTX', 'struct BleCommandReplyContext', True),
    ('AUTH_REQUIRED', 'static void bleSendAuthRequired(', True),
    ('FILE_VERB', 'static bool bleIsFileBrowserCommand(', True),
    ('LINE', 'static void processBleCommandLine(', True),
    ('DEFERRED_STRUCT', 'struct BleScDeferred', True),
    ('DEFERRED', 'static void bleScDeferredInbound(', True),
    ('PLAIN_DEFERRED', 'static void blePlaintextDeferredInbound(', False),  # fixed sources only
    ('INGRESS', 'static void processIncomingBLECommand(', True),
]

# Commands that set the Bluetooth authentication and encryption posture.
SUPERADMIN_ROWS = ('blerequireauth', 'blesecret', 'blesecure')


def registry_row(source, name):
    """Return the fields of the CommandEntry row `{ "name", ... }`, comments removed."""
    start = source.index('{ "' + name + '",')
    fields, current, pos, depth = [], '', start + 1, 1
    while depth:
        ch = source[pos]
        if ch == '"':
            end = pos + 1
            while source[end] != '"':
                end += 2 if source[end] == '\\' else 1
            current += source[pos:end + 1]
            pos = end + 1
            continue
        if source.startswith('/*', pos):
            pos = source.index('*/', pos) + 2
            continue
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if not depth:
                break
        if ch == ',' and depth == 1:
            fields.append(current.strip())
            current = ''
        else:
            current += ch
        pos += 1
    fields.append(current.strip())
    return [f for f in fields if f]


def tier_failures(source):
    failures = []
    for name in SUPERADMIN_ROWS:
        fields = registry_row(source, name)
        super_admin = len(fields) > 5 and fields[5] == 'true'
        if not super_admin:
            failures.append(f'FAIL T1 registry tier: `{name}` is not superadmin-only, '
                            f'so an ordinary admin can change the Bluetooth security posture '
                            f'that the other two commands guard at superadmin')
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--component', type=Path, default=HERE.parents[1])
    args = parser.parse_args()
    if not args.cxx:
        parser.error('a host C++17 compiler is required')
    source = (args.component / 'Bluetooth.cpp').read_text()

    failures = tier_failures(source)
    for line in failures:
        print(line, file=sys.stderr, flush=True)

    harness = (HERE / 'ble_sc_policy_harness.cpp').read_text()
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
    with tempfile.TemporaryDirectory(prefix='hw1-ble-sc-policy-') as tmp:
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
        run = subprocess.run([str(exe)])
    if failures or run.returncode:
        raise SystemExit(1)
    print('BLE command registry: blerequireauth, blesecret and blesecure are superadmin-only')


if __name__ == '__main__':
    main()
