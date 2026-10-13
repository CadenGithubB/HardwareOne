#!/usr/bin/env python3
"""Run the actual accountless FS-RPC LIST/STAT/GET handlers with bounded I/O fakes.

Uses production authorization and VFS normalization, storage routing, directory
reader, transcript classifier and packed wire structs.
The host fakes count trusted scopes, VFS accesses and transfers so an error reply
cannot conceal a read. They also model the bond state the handlers consult, so
the suite checks who is served (the bonded peer with a live session, under the
bond identity) and who is refused before any identity or VFS call; a second
compile with ENABLE_BONDED_MODE=0 expects every request to be refused. Source
contracts confirm the requesters (web /api/bond/fs/list|get, the OLED peer
browser) pass the executor's admin gate first. No hardware or transport is accessed.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent

def client_contracts(read, extract_block):
    """The requesters need httpd/semaphore/OLED doubles this suite does not have,
    so their gate is a source contract: the local admin check runs before the request is sent."""
    failures = []
    bond = read('WebPage_Bond.cpp')
    for handler, send in (('handleBondFsList', 'fsListSendRequest('), ('handleBondFsGet', 'fsGetSendRequest(')):
        body = extract_block(bond, f'static esp_err_t {handler}(httpd_req_t* req)')
        gate, request = body.find('hasAdminPrivilege(ctx)'), body.find(send)
        if not 0 <= gate < request:
            failures.append(f'{handler}: no hasAdminPrivilege(ctx) gate before {send}')
    oled = read('OLED_Mode_FileBrowser.cpp')
    marker = 'static bool peerBrowseAuthorized('
    if marker not in oled or 'hasAdminPrivilege(' not in extract_block(oled, marker):
        failures.append('OLED peer browser: no peerBrowseAuthorized() calling hasAdminPrivilege(')
    for function, send in (('peerStartRequest', 'fsListSendRequest('), ('peerActivate', 'fsGetSendRequest(')):
        body = extract_block(oled, f'static void {function}(')
        gate, request = body.find('peerBrowseAuthorized()'), body.find(send)
        if not 0 <= gate < request:
            failures.append(f'OLED {function}: no peerBrowseAuthorized() gate before {send}')
    return failures

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--source-dir', type=Path)
    a = p.parse_args()
    if not a.cxx:
        p.error('a host C++17 compiler is required')
    sys.path.insert(0, str(a.component / 'test/host'))
    from test_web_batch_handlers import extract_block
    def read(name):
        overlay = a.source_dir / name if a.source_dir else None
        return (overlay if overlay and overlay.exists() else a.component / name).read_text()
    source = read('System_ESPNow_FsList.cpp')
    filesystem = read('System_Filesystem.cpp')
    vfs = read('System_VFS.cpp')
    vfs_header = read('System_VFS.h')
    wire = read('System_ESPNow_Wire.h')
    # Keep the real packed request/reply definitions and their static assertions.
    wire = wire[wire.index('struct __attribute__((packed)) V4PayloadFsListReq'):wire.index('// ---- Multi-hop reachability invariants')]
    pieces = [extract_block(filesystem, 'bool normalizeFsPath('),
              extract_block(source, 'static bool fsRpcPathAllowed(')]
    # The requester gate exists only in sources that have it; an overlay without it
    # still compiles and reaches the behavioral assertions (which then name the defect).
    requester = 'static bool fsRpcRequesterIdentity('
    if requester in source:
        pieces.append(extract_block(source, requester))
    for kind, req in [('List','ListReq'), ('Stat','StatReq'), ('Get','GetReq')]:
        marker = f'static void process{kind}Deferred(const uint8_t srcMac[6], const V4PayloadFs{req}& req)'
        body = source[source.index(marker + ' {'):]
        pieces.append(extract_block(body, marker))
    # An overlay supplies production code, never a historical copy of the test.
    # This lets the current regressions run against the pre-fix implementation.
    harness = (HERE / 'espnow_fs_transcript_privacy_harness.cpp').read_text()
    routing = '\n\n'.join(extract_block(vfs, marker) for marker in
                            ('String normalize(', 'StorageType getStorageType(',
                             'String stripSdPrefix('))
    reader = vfs_header[vfs_header.index('struct DirectoryEntry {'):
                        vfs_header.index('bool exists(const String& path);')]
    reader += vfs[vfs.index('DirectoryReader::DirectoryReader('):
                  vfs.index('static FS* fsForPath(')]
    for tag, value in [('// INSERT_POLICY', read('Transcript_PathPolicy.h').replace('#pragma once','')),
                       ('// INSERT_WIRE', wire),
                       ('// INSERT_VFS', routing + '\n\n' + reader),
                       ('// INSERT_PRODUCTION', '\n\n'.join(pieces))]:
        assert harness.count(tag) == 1
        harness = harness.replace(tag,value)
    failures = client_contracts(read, extract_block)
    for failure in failures:
        print('FAIL client contract:', failure, file=sys.stderr)
    with tempfile.TemporaryDirectory(prefix='hw1-fsrpc-transcript-') as tmp:
        for label, defines in (('bond mode on', []), ('bond mode off', ['-DENABLE_BONDED_MODE=0'])):
            unit, exe = Path(tmp)/f'test_{label[-2:].strip()}.cpp', Path(tmp)/f'test_{label[-2:].strip()}'
            unit.write_text(harness)
            cmd = [a.cxx,'-std=c++17','-Wall','-Wextra','-Werror',*defines,str(unit),'-o',str(exe)]
            if a.sanitize:
                cmd[1:1] = ['-fsanitize=address,undefined','-fno-omit-frame-pointer']
            subprocess.run(cmd,check=True,timeout=120)
            if subprocess.run([str(exe)],timeout=60).returncode:
                failures.append(f'handler suite ({label}) reported failures')
    if failures:
        print(f'{len(failures)} failure(s): ' + '; '.join(failures), file=sys.stderr)
        sys.exit(1)
if __name__ == '__main__':
    main()
