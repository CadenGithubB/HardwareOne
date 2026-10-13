#!/usr/bin/env python3
"""Compile the FS-RPC lock discipline of System_ESPNow_FsList.cpp with host threads.

The suite splices the production pending table, deferred slot, lock guard,
send/cancel API, request capture, reply matching, cmd_exec runner and timeout
sweep into espnow_fslist_locking_harness.cpp. Host threads play httpd (client
sends), espnow_task (RX capture and replies) and cmd_exec (the deferred
runner); the FreeRTOS mutex double honors bounded waits and records its owner.
It checks that no client send holds the module mutex, that a captured request is
served or answered TOO_BUSY while a client send is blocked or another task holds
the lock briefly, that the deferred slot is always released, that cancel always
frees its slot, and that send failure, queue-full and single-slot behavior are
unchanged. The tx dispatcher and radio delivery are not modeled.

--source-dir <component-directory> reads production files from another tree
(for example the pre-fix base) while the harness always comes from this folder.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent


def slice_between(text, start, end, what):
    if text.count(start) != 1 or text.count(end) != 1:
        raise SystemExit(f'{what}: anchors not unique ({start!r}: {text.count(start)}, {end!r}: {text.count(end)})')
    return text[text.index(start):text.index(end)]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--source-dir', type=Path)
    a = p.parse_args()
    if not a.cxx:
        p.error('a host C++17 compiler is required')
    sys.path.insert(0, str(HERE))
    from test_web_batch_handlers import extract_block

    def read(name):
        overlay = a.source_dir / name if a.source_dir else None
        return (overlay if overlay and overlay.exists() else a.component / name).read_text()

    source = read('System_ESPNow_FsList.cpp')
    header = read('System_ESPNow_FsList.h')
    wire = read('System_ESPNow_Wire.h')
    # Real packed request/reply structs with their static assertions.
    wire = slice_between(wire, 'struct __attribute__((packed)) V4PayloadFsListReq',
                         '// ---- Multi-hop reachability invariants', 'System_ESPNow_Wire.h')
    # Tunables and the three callback typedefs.
    hdr = slice_between(header, '#define FS_LIST_MAX_PENDING', '// Initialize internal state',
                        'System_ESPNow_FsList.h')
    # Tables, lock guard, init, slot helpers, senders, cancel, reply receivers,
    # BUSY reply, cmd_exec runner, capture, request receivers and the deferred
    # dispatcher; the per-op workers it forward-declares are harness doubles.
    tables = slice_between(source, 'enum FsRpcOp : uint8_t',
                           '// FS RPC authenticates a device peer, not an account.',
                           'System_ESPNow_FsList.cpp')
    tick_marker = 'void fsListTick()'
    if source.count(tick_marker) != 1:
        raise SystemExit(f'System_ESPNow_FsList.cpp: {tick_marker!r} found {source.count(tick_marker)} times')
    tick = extract_block(source[source.index(tick_marker):], tick_marker)

    harness = (HERE / 'espnow_fslist_locking_harness.cpp').read_text()
    for tag, value in (('// INSERT_WIRE', wire), ('// INSERT_HEADER', hdr),
                       ('// INSERT_PRODUCTION', tables + '\n' + tick + '\n')):
        if harness.count(tag) != 1:
            raise SystemExit(f'harness: {tag} must appear once')
        harness = harness.replace(tag, value)

    with tempfile.TemporaryDirectory(prefix='hw1-fslist-locking-') as tmp:
        unit, exe = Path(tmp) / 'espnow_fslist_locking.cpp', Path(tmp) / 'espnow_fslist_locking'
        unit.write_text(harness)
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-pedantic', '-Werror', '-pthread',
               str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True, timeout=180)
        result = subprocess.run([str(exe)], timeout=120)
        if result.returncode:
            print('espnow_fslist_locking: FAILED', file=sys.stderr)
            return 1
    print('espnow_fslist_locking: OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
