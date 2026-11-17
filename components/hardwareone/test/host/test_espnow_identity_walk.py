#!/usr/bin/env python3
"""The boot-time walk over the stored peer identities says when it is incomplete.

peerIdentityLoadAll() reads /system/espnow/peers/<12-hex-MAC>/identity.json
into the cache gPeerIdentities[], and peerIdentitiesLoaded() tells the mesh
commands whether that cache shows every stored identity (a mesh slot is only
freed when it does). Checked here: a readable identity loads with its mesh
slot; no peers directory counts as read; an unmounted filesystem or a peers
path that will not open as a directory does not; folders without an
identity.json (empty, only identity.tmp, name not an address) are skipped; an
identity.json that stays out of the cache (unreadable, another address inside,
a 17th when the 16 slots are full) leaves the cache marked incomplete with a
warning naming the address and espnowforget, while the readable ones still
load; peerIdentityForget removes the files, the folder and the cache slot and
marks the cache incomplete when the file stays; the next walk recomputes the
mark.

Also checked: a stored meshId that is not a slot this firmware has (4 and up,
one that does not fit a byte, negative, text, missing) leaves that identity out
like an unreadable one; only a folder spelled as macToPathToken writes it is a
peer folder, so a lower- or mixed-case twin neither loads the identity twice
nor survives a forget in the cache, and one with its own identity.json is
named in a warning without marking the cache incomplete. Two source checks
cover the duplicate-address guard and the range check in peerIdentityPersist.

Compiled: the production functions are taken as text from
System_ESPNow_Identity.cpp and pasted into espnow_identity_walk_harness.cpp,
with the vendored ArduinoJson and the shared in-memory filesystem (fake_vfs.h).

--source-dir <directory> reads System_ESPNow_Identity.cpp (and the header and
System_MacText.h when present there) from another tree; the harness always
comes from this folder. A tree without peerIdentitiesLoaded() still compiles,
with a stand-in that always says "read", and fails the checks that need it.
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

# Each marker stops before the opening brace: extract_block takes the first "{" after it.
HELPERS = (
    'bool hexToBytes(',
    'bool parseMacNoSep(',
    'int findSlotByMac(',
    'bool buildPeerPath(',
    'bool readPeerIdentityFile(',
)
LOADED = 'bool peerIdentitiesLoaded()'
PUBLIC = (
    'bool peerIdentityForget(',
    'uint8_t peerIdentityLoadAll()',
    'const PeerIdentity* peerIdentityAt(',
    'uint8_t peerIdentitySlotCount()',
)
# File-scope lines that are not brace blocks, taken by regex.
CONSTANTS = r'^constexpr (?:const char\*|uint8_t)\s+k(?:PeerSlots|PeersDir|PeerFileVersion)\s*=.*;$'
CACHE = r'^(?:EXT_RAM_BSS_ATTR )?PeerIdentity gPeerIdentities\[kPeerSlots\]\s*=.*;$'
FLAG = r'^(?:static )?bool gPeerIdentitiesLoaded\s*=.*;$'


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
            sys.exit(f'FAIL extraction: {where}: {marker!r} found {text.count(marker)} times, expected 1')
        return extract_block(text, marker)

    def lines(pattern, expected, what):
        found = re.findall(pattern, source, re.M)
        if len(found) != expected:
            sys.exit(f'FAIL extraction: System_ESPNow_Identity.cpp: expected {expected} {what}, found {len(found)}')
        return found

    types = '\n'.join(body(header, m, 'System_ESPNow_Identity.h') + ';'
                      for m in ('struct PeerIdentity', 'enum EspNowEventCategory'))
    have_loaded = LOADED in source  # absent on the trees before the flag
    file_scope = lines(CONSTANTS, 3, 'peer constants') + lines(CACHE, 1, 'gPeerIdentities definition')
    if have_loaded:
        file_scope += lines(FLAG, 1, 'gPeerIdentitiesLoaded definition')
    markers = list(HELPERS) + ([LOADED] if have_loaded else []) + list(PUBLIC)
    production = '\n\n'.join(body(source, m, 'System_ESPNow_Identity.cpp') for m in markers)

    harness = (HERE / 'espnow_identity_walk_harness.cpp').read_text()
    for tag, text in (('// INSERT_TYPES', types), ('// INSERT_GLOBALS', '\n'.join(file_scope)),
                      ('// INSERT_PRODUCTION', production)):
        if harness.count(tag) != 1:
            sys.exit(f'FAIL extraction: harness: {tag} must appear once')
        harness = harness.replace(tag, text + '\n')

    # The mesh slot count the range check compares against comes from the settings header.
    n_meshes = re.findall(r'static constexpr uint8_t N_MESHES = (\d+);', read('System_Settings.h'))
    if len(n_meshes) != 1:
        sys.exit('FAIL extraction: System_Settings.h: expected one N_MESHES definition')

    # Source checks for what the walk cannot reach through the filesystem. Each
    # is counted with the harness's checks and fails on the trees before the fix.
    source_failures = 0
    load_all = body(source, 'uint8_t peerIdentityLoadAll()', 'System_ESPNow_Identity.cpp')
    persist = body(source, 'bool peerIdentityPersist(', 'System_ESPNow_Identity.cpp')
    for ok, what in (
            ('findSlotByMac(mac) >= 0' in load_all,
             'the walk never caches a second entry for an address it already cached'),
            ('meshId >= Settings::N_MESHES' in persist and
             persist.index('meshId >= Settings::N_MESHES') < persist.index('findSlotByMac(mac)'),
             'peerIdentityPersist refuses a mesh slot past N_MESHES before it touches the cache')):
        if not ok:
            print(f'FAIL: {what}', file=sys.stderr)
            source_failures += 1
    print(f'source checks: {2 - source_failures} of 2 passed', flush=True)

    print(f'peerIdentitiesLoaded: {"present" if have_loaded else "absent (stand-in)"}', flush=True)
    json_include = a.component.parent / 'hardwareone_libs' / 'ArduinoJson' / 'src'
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-identity-walk-') as tmp:
        unit, exe = Path(tmp) / 'espnow_identity_walk.cpp', Path(tmp) / 'espnow_identity_walk'
        (Path(tmp) / 'System_MacText.h').write_text(read('System_MacText.h'))
        unit.write_text(harness)
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
               f'-DHW1_HAVE_IDENTITIES_LOADED={1 if have_loaded else 0}', f'-DHW1_N_MESHES={n_meshes[0]}',
               '-I', tmp, '-I', str(HERE / 'web_batch_stubs'), '-I', str(HERE), '-I', str(json_include),
               str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                        '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True, timeout=240)
        result = subprocess.run([str(exe)], timeout=60)
    if result.returncode or source_failures:
        print('FAIL espnow_identity_walk: the peer identity walk checks reported failures', file=sys.stderr)
        sys.exit(1)
    print('PASS espnow_identity_walk')


if __name__ == '__main__':
    main()
