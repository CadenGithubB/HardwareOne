#!/usr/bin/env python3
"""Known answers for the ESP-NOW 1.0 handshake bytes (step 4, WP2 + revocation C).

Both boards of a pair must build exactly the same authenticated bytes. The
firmware builds them in System_ESPNow_HandshakeCore.h; this test compiles that
header, has it build every input for fixed values, and compares the bytes with
the ones espnow_handshake_kat_vectors.py assembles on its own from the written
layout (Python hashlib/hmac, nothing taken from the firmware). Then it pins the
digests of that independent assembly (GOLDEN, printed by the vectors script),
so a change of either side is caught:
  - the mesh salt prefix, the PBKDF2 rounds and the kdf record (MeshIdCore.h),
    with a full passphrase -> stretched key -> mesh id -> bootstrap/group key chain;
  - KEY_EX_HELLO / REPLY / CONFIRM tags (labels, nonces, the REPLY bound to the
    HELLO, a refusal with zero reply nonce and responder key);
  - the SESSION_OPEN / CONFIRM / REKEY transcripts (labels, openEpochMs);
  - the C1 mesh tags of the three session frames (slot key, mesh id in the input);
  - the C2 session keys and bond token under the bootstrap key;
  - the group (BROADCAST_AUTH) tag label.
Source checks tie the firmware to these builders: the handlers tag and sign
through HandshakeCore, the session keys go through sessionDeriveKeyMaterial,
the group tag carries its label and the stretch uses the pinned salt prefix.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent

# Printed by espnow_handshake_kat_vectors.py (pinned 2026-11-23, ESP-NOW 1.0 step 4).
GOLDEN = {
    'stretched': 'c2ae1b7f7f9ac2aacf5a68d4ae1f2e3f9fa143fbb368a824084dadf0b5c6b750',
    'mesh_id': 0xDEE5,
    'bootstrap': '6f3405ccd1d11d7bae90f1205ef417ee730458e5c2dcce5796e5211d6e067889',
    'group': '852ea444133254c00bc577b0fe693faa9b837155ab3ab6de6a3699b99a98fb65',
    'hello_tag': 'b2b5af038327fd50ef94ec4715997c6e',
    'reply_tag': '37e834295e587ed660079d84fa385e85',
    'confirm_tag': 'fbe32b78be8f03287bfcc9f1332604d1',
    'refusal_tag': 'fc3f1a359f10162f275f431543694f99',
    'open_transcript_sha256': 'e9dc4d2f534f053a8a083224a5347a6e6ebb04f9deeafdf2c87f5999f3533c3e',
    'confirm_transcript_sha256': 'f90cdb2ca11cce9db2b88b7a44511f3f7c76576e6d06b5275f5726c9b3d1dd3d',
    'rekey_transcript_sha256': '4e33dffc4d19c79113788694b0aa3b11d6592197e6ad7e2733f6234c0f7da3e5',
    'open_mesh_tag': '931dcf60301f9c8f7331d6baf889aa28',
    'confirm_mesh_tag': 'a3d0d462fe214fc0e0f069403b33a6dd',
    'rekey_mesh_tag': 'cfb8b7cfe380c3d11387e4ee71bec205',
    'session_AtoB': '56785c0405999bcf222321da8411ce7c2a9dca7c951cf8048c205d989b736326',
    'session_BtoA': 'c66dd5e7901d550534ec1f46c7a0a73ee4a6d11e128c4c4085d41206c9f7ca45',
    'session_bond': '92b77eb931e616c0c128117ba73bbc78',
    'group_tag': 'df10280a0cbdde810bce1105d469f76f',
}

HARNESS = r'''
#include "System_ESPNow_HandshakeCore.h"
#include "System_ESPNow_MeshIdCore.h"
#include <cstdio>
#include <cstring>
using namespace espnowhs;
static void hex(const char* name, const uint8_t* p, size_t n) {
  std::printf("%s ", name);
  for (size_t i = 0; i < n; i++) std::printf("%02x", p[i]);
  std::printf("\n");
}
int main() {
  uint8_t macA[6] = {0x02, 0, 0, 0, 0, 0xA1}, macB[6] = {0x02, 0, 0, 0, 0, 0xB2};
  uint8_t pubA[32], pubB[32], eph[32], shared[32], nonceH[16], nonceR[16], nonceA[16], nonceB[16], sig[64];
  for (int i = 0; i < 32; i++) { pubA[i] = (uint8_t)(0x10 + i); pubB[i] = (uint8_t)(0x40 + i);
                                 eph[i] = (uint8_t)(0x80 + i); shared[i] = (uint8_t)(0xC0 + i); }
  for (int i = 0; i < 16; i++) { nonceH[i] = 0x11; nonceR[i] = 0x22; nonceA[i] = (uint8_t)(0x30 + i);
                                 nonceB[i] = (uint8_t)(0x50 + i); }
  for (int i = 0; i < 64; i++) sig[i] = (uint8_t)((i * 7) & 0xFF);
  const uint16_t mesh = (uint16_t)MESH_ID;
  uint8_t buf[256];
  size_t n = helloInput(buf, mesh, macA, pubA, nonceH);              hex("hello", buf, n);
  n = replyInput(buf, mesh, macB, pubB, nonceR, macA, pubA, nonceH);  hex("reply", buf, n);
  uint8_t head[kConfirmHeadLen] = {};
  head[0] = (uint8_t)(mesh & 0xFF); head[1] = (uint8_t)(mesh >> 8);
  std::memcpy(head + 2, macA, 6); head[8] = 0; head[9] = 0; std::memcpy(head + 10, pubA, 8);
  KeyExContext full = {};
  std::memcpy(full.initMac, macA, 6); std::memcpy(full.respMac, macB, 6);
  std::memcpy(full.helloNonce, nonceH, 16); std::memcpy(full.replyNonce, nonceR, 16);
  std::memcpy(full.initPub, pubA, 32); std::memcpy(full.respPub, pubB, 32);
  n = confirmInput(buf, head, full);                                  hex("confirm", buf, n);
  std::memcpy(head + 2, macB, 6); head[8] = 3; std::memcpy(head + 10, pubB, 8);
  KeyExContext refusal = {};
  std::memcpy(refusal.initMac, macA, 6); std::memcpy(refusal.respMac, macB, 6);
  std::memcpy(refusal.helloNonce, nonceH, 16); std::memcpy(refusal.initPub, pubA, 32);
  n = confirmInput(buf, head, refusal);                               hex("refusal", buf, n);
  uint8_t op[kOpenTranscriptLen], co[kConfirmTranscriptLen], rk[kRekeyTranscriptLen];
  openTranscript(op, 0x1234, macA, macB, eph, nonceA, 0x00000213u, 1800000000123ull);  hex("open", op, sizeof op);
  confirmTranscript(co, 0x1234, macB, macA, eph, nonceA, nonceB, 0x00000213u);         hex("sconf", co, sizeof co);
  rekeyTranscript(rk, 0x1234, macA, macB, eph, nonceA, 0x01020304u);                   hex("rekey", rk, sizeof rk);
  n = meshTagInput(buf, MeshTagKind::Open, mesh, op, sizeof op, sig);      hex("open_mk", buf, n);
  n = meshTagInput(buf, MeshTagKind::Confirm, mesh, co, sizeof co, sig);   hex("sconf_mk", buf, n);
  n = meshTagInput(buf, MeshTagKind::Rekey, mesh, rk, sizeof rk, sig);     hex("rekey_mk", buf, n);
  uint8_t sk[kSessionKeyInputLen];
  sessionKeyInput(sk, shared);                                         hex("session", sk, sizeof sk);
  hex("group_label", reinterpret_cast<const uint8_t*>(kLabelGroup), labelLen(kLabelGroup));
  hex("salt_prefix", reinterpret_cast<const uint8_t*>(meshid::kSaltPrefix), std::strlen(meshid::kSaltPrefix));
  std::printf("rounds %u\nkdf %d\n", (unsigned)meshid::kPbkdf2Iters, meshid::kKdfRecord);
  uint8_t a[16] = {1}, b[16] = {1}, c[16] = {2};
  std::printf("tagEqual %d %d\n", tagEqual(a, b) ? 1 : 0, tagEqual(a, c) ? 1 : 0);
  return 0;
}
'''


def load_vectors():
    spec = importlib.util.spec_from_file_location('espnow_handshake_kat_vectors',
                                                  HERE / 'espnow_handshake_kat_vectors.py')
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--sanitize', action='store_true')
    a = p.parse_args()
    v = load_vectors()
    failures = []

    # 1. The independent assembly gives the pinned digests.
    got = v.vectors()
    for k, want in GOLDEN.items():
        if got.get(k) != want:
            failures.append(f'vector {k}: {got.get(k)!r}, pinned {want!r}')

    # 2. The firmware's builders give the same bytes as the independent assembly.
    mk = v.mesh_keys(v.PASSPHRASE, v.LABEL)
    mesh = mk['id']
    op = v.open_transcript(v.SID, v.MAC_A, v.MAC_B, v.EPH, v.NONCE_A, v.CAPS, v.EPOCH_MS)
    co = v.confirm_transcript(v.SID, v.MAC_B, v.MAC_A, v.EPH, v.NONCE_A, v.NONCE_B, v.CAPS)
    rk = v.rekey_transcript(v.SID, v.MAC_A, v.MAC_B, v.EPH, v.NONCE_A, v.PREV_SEQ)
    expected = {
        'hello': v.hello_input(mesh, v.MAC_A, v.PUB_A, v.NONCE_H),
        'reply': v.reply_input(mesh, v.MAC_B, v.PUB_B, v.NONCE_R, v.MAC_A, v.PUB_A, v.NONCE_H),
        'confirm': v.confirm_input(v.confirm_head(mesh, v.MAC_A, 0, v.PUB_A[:8]), v.MAC_A, v.MAC_B, v.NONCE_H,
                                   v.NONCE_R, v.PUB_A, v.PUB_B),
        'refusal': v.confirm_input(v.confirm_head(mesh, v.MAC_B, 3, v.PUB_B[:8]), v.MAC_A, v.MAC_B, v.NONCE_H,
                                   bytes(16), v.PUB_A, bytes(32)),
        'open': op, 'sconf': co, 'rekey': rk,
        'open_mk': v.mesh_tag_input(v.LABEL_SOPEN_MK, mesh, op, v.SIG),
        'sconf_mk': v.mesh_tag_input(v.LABEL_SCONF_MK, mesh, co, v.SIG),
        'rekey_mk': v.mesh_tag_input(v.LABEL_REKEY_MK, mesh, rk, v.SIG),
        'session': v.LABEL_SESSION + v.SHARED,
        'group_label': v.LABEL_GROUP,
        'salt_prefix': v.SALT_PREFIX,
    }
    with tempfile.TemporaryDirectory(prefix='hw1-handshake-kat-') as tmp:
        unit, exe = Path(tmp) / 'kat.cpp', Path(tmp) / 'kat'
        unit.write_text(HARNESS.replace('MESH_ID', str(mesh)))
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic', '-I', str(a.component),
               str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-g']
        subprocess.run(cmd, check=True, timeout=120)
        out = subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=30).stdout
    lines = dict(line.split(' ', 1) for line in out.strip().splitlines())
    for k, want in expected.items():
        if lines.get(k) != want.hex():
            failures.append(f'HandshakeCore {k}: {lines.get(k)}, the spec gives {want.hex()}')
    if lines.get('rounds') != str(v.PBKDF2_ROUNDS) or lines.get('kdf') != '1':
        failures.append(f'MeshIdCore rounds/kdf record: {lines.get("rounds")}/{lines.get("kdf")}')
    if lines.get('tagEqual') != '1 0':
        failures.append('espnowhs::tagEqual is not an equality test')
    sizes = {'hello': 76, 'reply': 130, 'confirm': 145, 'open': 92, 'sconf': 100, 'rekey': 84}
    for k, n in sizes.items():
        if len(expected[k]) != n:
            failures.append(f'{k} input is {len(expected[k])} bytes, the design says {n}')

    # 3. The firmware uses these builders.
    comp = a.component
    hc = (comp / 'System_ESPNow_Handlers_Crypto.cpp').read_text()
    for needle, what in (('espnowhs::helloInput(', 'the HELLO tag'),
                         ('espnowhs::replyInput(', 'the REPLY tag'),
                         ('espnowhs::confirmInput(', 'the CONFIRM tag'),
                         ('espnowhs::openTranscript(', 'the OPEN transcript'),
                         ('espnowhs::confirmTranscript(', 'the CONFIRM transcript'),
                         ('espnowhs::rekeyTranscript(', 'the REKEY transcript'),
                         ('espnowhs::meshTagInput(', 'the session mesh tag')):
        if needle not in hc:
            failures.append(f'System_ESPNow_Handlers_Crypto.cpp does not build {what} with {needle}')
    if re.search(r'"v4-(sopen|sconf|rekey):"', hc):
        failures.append('System_ESPNow_Handlers_Crypto.cpp still has a v4 transcript label')
    sess = (comp / 'System_ESPNow_Sessions.cpp').read_text()
    material = sess[sess.find('bool sessionDeriveKeyMaterial('):]
    material = material[:material.find('\n}\n')]
    for needle in ('espnowhs::sessionKeyInput(input, shared)', 'espnowCryptoHmacSha256(prk, bootstrapKey, 32',
                   'espnowCryptoKdfSubkey(keyAtoB, prk, 1, "esp-AtoB")',
                   'espnowCryptoKdfSubkey(keyBtoA, prk, 2, "esp-BtoA")',
                   'espnowCryptoKdfSubkey(bondTok, prk, 3, "esp-bond")'):
        if needle not in material:
            failures.append(f'sessionDeriveKeyMaterial: {needle} not found (C2 derivation)')
    rekey = hc[hc.find('bool deriveRekeyedKeysFromShared('):]
    rekey = rekey[:rekey.find('\n}\n')]
    if 'sessionDeriveKeyMaterial(mk->bootstrapKey, shared' not in rekey:
        failures.append('deriveRekeyedKeysFromShared does not mix the bootstrap key in (C2)')
    espnow = (comp / 'System_ESPNow.cpp').read_text()
    gt = espnow[espnow.find('static bool v4_group_tag('):]
    gt = gt[:gt.find('\n}\n')]
    if 'espnowhs::kLabelGroup' not in gt:
        failures.append('v4_group_tag does not put the hw1-espnow-1-group label first')
    keys = (comp / 'System_ESPNow_MeshKeys.cpp').read_text()
    if 'meshid::kSaltPrefix' not in keys or 'meshid::kPbkdf2Iters' not in keys or 'espnow-v4-mesh-salt' in keys:
        failures.append('System_ESPNow_MeshKeys.cpp does not stretch with the pinned salt prefix and rounds')
    if 'e["kdf"] = meshid::kKdfRecord;' not in espnow or 'if (kdf != meshid::kKdfRecord) keyHex = "";' not in espnow:
        failures.append('the settings file does not write and check the kdf record next to the stretched key')

    for f in failures:
        print(f'FAIL {f}')
    if failures:
        sys.exit(1)
    print(f'PASS espnow_handshake_kat: {len(GOLDEN)} pinned vectors, {len(expected)} byte layouts, '
          'firmware builders pinned')


if __name__ == '__main__':
    main()
