#!/usr/bin/env python3
"""The ESP-NOW identity writers never promote a short write over the live file.

identity.json holds this device's encrypted Ed25519 secret, and a present but
unreadable identity is never regenerated automatically (only
`espnowregenidentity --confirm-wipe-all-bonds` repairs it, wiping every bond).
On a nearly full LittleFS serializeJson stops short yet returns a positive
count, so a writer that only rejects 0 bytes renames a truncated file into
place. Both writers (own identity and per-peer identity) must compare the
bytes written and the stored file size with measureJson(doc), and on any
mismatch remove the temp and keep the previous file.

Compiled: the production hex encoder, directory helpers, peer path builder,
checked JSON promote and both writers are spliced into
espnow_identity_writes_harness.cpp with the vendored ArduinoJson and an
in-memory filesystem whose free space can run out mid-write.

--source-dir <component-directory> reads System_ESPNow_Identity.cpp(.h) from
another tree (for example the pre-fix base) while the harness always comes
from this folder; the base tree must fail the short-write cases.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent


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

    source = read('System_ESPNow_Identity.cpp')
    header = read('System_ESPNow_Identity.h')

    def body(text, marker, where):
        if text.count(marker) != 1:
            raise SystemExit(f'{where}: {marker!r} found {text.count(marker)} times')
        return extract_block(text, marker)

    types = '\n'.join(body(header, m, 'System_ESPNow_Identity.h') + ';'
                      for m in ('struct EspNowIdentity', 'struct PeerIdentity'))
    constants = re.findall(r'^constexpr (?:const char\*|uint8_t)\s+k(?:Identity\w*|PeersDir|PeerFileVersion)\s*=.*;$',
                           source, re.M)
    if len(constants) != 6:
        raise SystemExit(f'System_ESPNow_Identity.cpp: expected 6 identity constants, found {len(constants)}')
    markers = ['void bytesToHex(', 'bool ensureIdentityDirs(']
    # The checked promote helper exists only on the fixed tree; the pre-fix
    # writers carry their own inline (unchecked) serialize + rename.
    if 'bool writeJsonFileComplete(' in source:
        markers.append('bool writeJsonFileComplete(')
    markers += ['bool writeIdentityFile(', 'void formatMacNoSep(', 'bool buildPeerPath(',
                'bool ensurePeerDir(', 'bool writePeerIdentityFile(']
    production = '\n'.join(constants) + '\n\n' + '\n\n'.join(
        body(source, m, 'System_ESPNow_Identity.cpp') for m in markers)

    harness = (HERE / 'espnow_identity_writes_harness.cpp').read_text()
    for tag, text in (('// INSERT_TYPES', types), ('// INSERT_PRODUCTION', production)):
        if harness.count(tag) != 1:
            raise SystemExit(f'harness: {tag} must appear once')
        harness = harness.replace(tag, text + '\n')

    json_include = a.component.parent / 'hardwareone_libs' / 'ArduinoJson' / 'src'
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-identity-') as tmp:
        unit, exe = Path(tmp) / 'espnow_identity_writes.cpp', Path(tmp) / 'espnow_identity_writes'
        unit.write_text(harness)
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-pedantic', '-Werror',
               '-Wno-unused-parameter', '-Wno-unused-function',
               '-I', str(json_include), str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True, timeout=180)
        result = subprocess.run([str(exe)], timeout=60)
        if result.returncode:
            print('espnow_identity_writes: FAILED', file=sys.stderr)
            return 1
    print('espnow_identity_writes: OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
