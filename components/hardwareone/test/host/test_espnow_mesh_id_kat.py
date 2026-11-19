#!/usr/bin/env python3
"""Known answers for the mesh id derivation, a cross-device contract.

Every device on a mesh must compute the same 16-bit mesh id from the same mesh
key (System_ESPNow_MeshIdCore.h): the libsodium KDF subkey meshid::kSubkeyId
with context meshid::kKdfContext of the PBKDF2-stretched key, then meshid::pick
(the first nonzero little-endian u16 of its 16 byte pairs).

libsodium's crypto_kdf_blake2b_derive_from_key is BLAKE2b-256 keyed with the
master key, salt = LE64(subkey id) || 8 zero bytes, personal = context || 8
zero bytes, empty message (managed_components/espressif__libsodium
crypto_kdf/blake2b/kdf_blake2b.c), so Python's hashlib reproduces it without a
host libsodium. The checks:
  - the constants in System_ESPNow_MeshIdCore.h are the pinned ones (3, "esp-mid1");
  - the golden ids below come out of that derivation;
  - the production pick() (compiled from the header) gives the same id for the
    same subkey bytes, and skips a zero first pair;
  - System_ESPNow_MeshKeys.cpp derives the id through espnowCryptoKdfSubkey with
    those constants and pick, and espnowCryptoKdfSubkey is the libsodium KDF.
Changing any of them changes every mesh id: devices built before and after the
change drop each other's frames at the mesh gate.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent

# (stretched key, expected subkey, expected id). Pinned 2026-11-18.
GOLDEN = (
    (bytes(range(32)),
     'e4f1eeb9d3c37f91842d2ebc2ef6ef6ef22e149fa8bf3244334f547fe438af56', 0xF1E4),
    (bytes([0xA5] * 32),
     '21d75dc3c2d43e14ca299ace62d2aa54f0c5c0f8e4ed1e3b93dc610123a3137e', 0xD721),
)

HARNESS = r'''
#include "System_ESPNow_MeshIdCore.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
int main(int argc, char** argv) {
  for (int a = 1; a < argc; a++) {
    uint8_t sub[32];
    if (std::strlen(argv[a]) != 64) return 2;
    for (int i = 0; i < 32; i++) {
      char hex[3] = {argv[a][2 * i], argv[a][2 * i + 1], 0};
      sub[i] = (uint8_t)std::strtoul(hex, nullptr, 16);
    }
    std::printf("%u\n", (unsigned)meshid::pick(sub));
  }
  return 0;
}
'''


def subkey(master, subkey_id, context):
    return hashlib.blake2b(b'', digest_size=32, key=master,
                           salt=struct.pack('<Q', subkey_id) + bytes(8),
                           person=context + bytes(16 - len(context))).digest()


def pick(sub):
    for i in range(16):
        v = sub[2 * i] | (sub[2 * i + 1] << 8)
        if v:
            return v
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--sanitize', action='store_true')
    a = p.parse_args()
    failures = []

    core = (a.component / 'System_ESPNow_MeshIdCore.h').read_text()
    m_id = re.search(r'constexpr uint64_t kSubkeyId = (\d+);', core)
    m_ctx = re.search(r'constexpr const char\* kKdfContext = "([^"]*)";', core)
    if not m_id or not m_ctx:
        sys.exit('FAIL extraction: meshid::kSubkeyId / kKdfContext not found')
    subkey_id, context = int(m_id.group(1)), m_ctx.group(1).encode()
    if (subkey_id, context) != (3, b'esp-mid1'):
        failures.append(f'the mesh id KDF parameters changed to ({subkey_id}, {context!r}); every mesh id changes')
    if len(context) != 8:
        failures.append('a libsodium KDF context is exactly 8 bytes')

    subs = []
    for master, want_sub, want_id in GOLDEN:
        sub = subkey(master, subkey_id, context)
        subs.append(sub)
        if sub.hex() != want_sub or pick(sub) != want_id:
            failures.append(f'golden {master[:2].hex()}..: subkey {sub.hex()} id 0x{pick(sub):04X}, '
                            f'expected 0x{want_id:04X}')
    # A subkey whose first pair is zero: the id is the second pair.
    zero_first = bytes([0, 0, 0x34, 0x12]) + bytes(28)
    subs.append(zero_first)

    with tempfile.TemporaryDirectory(prefix='hw1-mesh-id-kat-') as tmp:
        unit, exe = Path(tmp) / 'kat.cpp', Path(tmp) / 'kat'
        unit.write_text(HARNESS)
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic', '-I', str(a.component),
               str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-g']
        subprocess.run(cmd, check=True, timeout=120)
        out = subprocess.run([str(exe)] + [s.hex() for s in subs], check=True, capture_output=True,
                             text=True, timeout=30).stdout.split()
    got = [int(x) for x in out]
    want = [g[2] for g in GOLDEN] + [0x1234]
    if got != want:
        failures.append(f'meshid::pick gives {[hex(x) for x in got]}, expected {[hex(x) for x in want]}')

    keys = (a.component / 'System_ESPNow_MeshKeys.cpp').read_text()
    id_for = keys[keys.find('uint16_t meshKeysIdFor('):]
    id_for = id_for[:id_for.find('\n}\n')]
    if 'espnowCryptoKdfSubkey(sub, stretched, meshid::kSubkeyId, meshid::kKdfContext)' not in id_for or \
            'meshid::pick(sub)' not in id_for:
        failures.append('meshKeysIdFor does not derive the id with the pinned KDF subkey and pick')
    derive = keys[keys.find('bool meshKeysDerive('):]
    derive = derive[:derive.find('\n}\n')]
    if 'meshKeysIdFor(m.passphraseStretchedKey)' not in derive or 'm.fingerprint = id;' not in derive:
        failures.append('meshKeysDerive does not set the mesh id from the key')
    # The refresh gives every keyed mesh, enabled or not, its key's id (a
    # disabled mesh has no derived keys, so meshKeysDerive never sets it);
    # test_espnow_mesh_slots runs a double of it, so its source is pinned here.
    refresh = keys[keys.find('uint8_t meshKeysRefreshIds('):]
    refresh = refresh[:refresh.find('\n}\n')]
    if 'meshKeysIdFor(m.passphraseStretchedKey)' not in refresh or 'm.fingerprint = id;' not in refresh or \
            'm.enabled' in refresh:
        failures.append('meshKeysRefreshIds does not set every keyed mesh\'s id (enabled or not) from its key')
    # Never 0 on the air: every send path that stamps a header's mesh id
    # refuses a destination whose mesh has none.
    espnow = (a.component / 'System_ESPNow.cpp').read_text().splitlines()
    stamps = 0
    for i, line in enumerate(espnow):
        m = re.search(r'(?:h\.|h->)meshFingerprint\s*=(?!=)\s*([^;]+);', line)
        if not m:
            continue
        stamps += 1
        value = m.group(1).strip()
        if value.startswith('fingerprintForPeer('):
            ok = any('meshFingerprint == 0' in l for l in espnow[i + 1:i + 7])
        else:
            ok = any(f'{value} == 0' in l for l in espnow[max(0, i - 120):i])
        if not ok:
            failures.append(f'System_ESPNow.cpp:{i + 1}: a frame is stamped with mesh id {value} '
                            f'without refusing id 0')
    if stamps < 4:
        failures.append(f'System_ESPNow.cpp: {stamps} header mesh id stamps found, expected at least 4')
    crypto = (a.component / 'System_ESPNow_Crypto.cpp').read_text()
    if 'crypto_kdf_blake2b_derive_from_key(out, 32, subkeyId, ctxBuf, masterKey)' not in crypto:
        failures.append('espnowCryptoKdfSubkey is no longer the libsodium BLAKE2b KDF this test reproduces')

    for f in failures:
        print(f'FAIL {f}')
    if failures:
        sys.exit(1)
    print(f'PASS espnow_mesh_id_kat: {len(GOLDEN)} golden ids, pick on {len(subs)} subkeys, derivation pinned')


if __name__ == '__main__':
    main()
