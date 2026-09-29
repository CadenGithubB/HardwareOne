#!/usr/bin/env python3
"""Run the actual accountless FS-RPC LIST/STAT/GET handlers with bounded I/O fakes.

Uses production normalization, transcript classifier and packed wire structs.
The host fakes count trusted scopes, VFS accesses and transfers so an error reply
cannot conceal a read. No hardware or transport is accessed.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--source-dir', type=Path)
    a = p.parse_args()
    sys.path.insert(0, str(a.component / 'test/host'))
    from test_web_batch_handlers import extract_block
    def read(name):
        overlay = a.source_dir / name if a.source_dir else None
        return (overlay if overlay and overlay.exists() else a.component / name).read_text()
    source = read('System_ESPNow_FsList.cpp')
    filesystem = read('System_Filesystem.cpp')
    wire = read('System_ESPNow_Wire.h')
    # Keep the real packed request/reply definitions and their static assertions.
    wire = wire[wire.index('struct __attribute__((packed)) V4PayloadFsListReq'):wire.index('// ---- Multi-hop reachability invariants')]
    pieces = [extract_block(filesystem, 'bool normalizeFsPath('),
              extract_block(source, 'static bool fsRpcPathAllowed(')]
    for kind, req in [('List','ListReq'), ('Stat','StatReq'), ('Get','GetReq')]:
        marker = f'static void process{kind}Deferred(const uint8_t srcMac[6], const V4PayloadFs{req}& req)'
        body = source[source.index(marker + ' {'):]
        pieces.append(extract_block(body, marker))
    hp = a.source_dir / 'espnow_fs_transcript_privacy_harness.cpp' if a.source_dir else HERE / 'espnow_fs_transcript_privacy_harness.cpp'
    harness = hp.read_text()
    for tag, value in [('// INSERT_POLICY', read('Transcript_PathPolicy.h').replace('#pragma once','')),
                       ('// INSERT_WIRE', wire), ('// INSERT_PRODUCTION', '\n\n'.join(pieces))]:
        assert harness.count(tag) == 1
        harness = harness.replace(tag,value)
    with tempfile.TemporaryDirectory(prefix='hw1-fsrpc-transcript-') as tmp:
        unit, exe = Path(tmp)/'test.cpp', Path(tmp)/'test'
        unit.write_text(harness)
        cmd = ['clang++','-std=c++17','-Wall','-Wextra','-Werror',str(unit),'-o',str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(cmd,check=True,timeout=120)
        subprocess.run([str(exe)],check=True,timeout=60)
if __name__ == '__main__':
    main()
