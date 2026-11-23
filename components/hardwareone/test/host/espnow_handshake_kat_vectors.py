#!/usr/bin/env python3
"""Known-answer vectors for the ESP-NOW 1.0 handshake, computed from the spec.

Independent of the firmware: this script assembles every authenticated byte
string from the written layout (System_ESPNow_HandshakeCore.h's header comment,
DESIGN.md section 2.3 and 5.1 C, docs/COMPATIBILITY.md) with Python's own
hashlib/hmac, and prints the vectors. test_espnow_handshake_kat.py imports the
assembly functions below, compares them with the bytes the firmware's
HandshakeCore builders produce, and pins the printed digests (GOLDEN there).

Run it to print the vectors: python3 espnow_handshake_kat_vectors.py
"""
import hashlib
import hmac
import struct

# The pinned names (the spec, not read from the firmware).
SALT_PREFIX = b'hw1-espnow-1-mesh-salt:'
PBKDF2_ROUNDS = 100000
LABEL_KXHELLO = b'hw1-espnow-1-kxhello'
LABEL_KXREPLY = b'hw1-espnow-1-kxreply'
LABEL_KXCONF = b'hw1-espnow-1-kxconf'
LABEL_SOPEN = b'hw1-espnow-1-sopen'
LABEL_SCONF = b'hw1-espnow-1-sconf'
LABEL_REKEY = b'hw1-espnow-1-rekey'
LABEL_SOPEN_MK = b'hw1-espnow-1-sopen-mk'
LABEL_SCONF_MK = b'hw1-espnow-1-sconf-mk'
LABEL_REKEY_MK = b'hw1-espnow-1-rekey-mk'
LABEL_GROUP = b'hw1-espnow-1-group'
LABEL_SESSION = b'hw1-espnow-1-session'


def kdf(master, subkey_id, context):
    """libsodium crypto_kdf_blake2b_derive_from_key, 32-byte output."""
    return hashlib.blake2b(b'', digest_size=32, key=master,
                           salt=struct.pack('<Q', subkey_id) + bytes(8),
                           person=context + bytes(16 - len(context))).digest()


def tag(key, msg):
    return hmac.new(key, msg, hashlib.sha256).digest()[:16]


def stretch(passphrase, label):
    salt = hashlib.sha256(SALT_PREFIX + label).digest()
    return hashlib.pbkdf2_hmac('sha256', passphrase, salt, PBKDF2_ROUNDS, 32)


def mesh_id(stretched):
    sub = kdf(stretched, 3, b'esp-mid1')
    for i in range(16):
        v = sub[2 * i] | (sub[2 * i + 1] << 8)
        if v:
            return v
    return 0


def mesh_keys(passphrase, label):
    s = stretch(passphrase, label)
    return {'stretched': s, 'id': mesh_id(s), 'bootstrap': kdf(s, 1, b'esp-boot'), 'group': kdf(s, 2, b'esp-grup')}


u16 = lambda v: struct.pack('<H', v)  # noqa: E731
u32 = lambda v: struct.pack('<I', v)  # noqa: E731
u64 = lambda v: struct.pack('<Q', v)  # noqa: E731


def hello_input(mesh, mac, pub, nonce):
    return LABEL_KXHELLO + u16(mesh) + mac + pub + nonce


def reply_input(mesh, resp_mac, resp_pub, reply_nonce, init_mac, init_pub, hello_nonce):
    return LABEL_KXREPLY + u16(mesh) + resp_mac + resp_pub + reply_nonce + init_mac + init_pub + hello_nonce


def confirm_head(mesh, confirmer_mac, status, key_id):
    return u16(mesh) + confirmer_mac + bytes([status, 0]) + key_id


def confirm_input(head, init_mac, resp_mac, hello_nonce, reply_nonce, init_pub, resp_pub):
    return LABEL_KXCONF + head + init_mac + resp_mac + hello_nonce + reply_nonce + init_pub + resp_pub


def open_transcript(sid, init_mac, resp_mac, eph, nonce_a, caps, epoch_ms):
    return LABEL_SOPEN + u16(sid) + init_mac + resp_mac + eph + nonce_a + u32(caps) + u64(epoch_ms)


def confirm_transcript(sid, resp_mac, init_mac, eph, nonce_a, nonce_b, caps):
    return LABEL_SCONF + u16(sid) + resp_mac + init_mac + eph + nonce_a + nonce_b + u32(caps)


def rekey_transcript(sid, sender, receiver, eph, nonce, prev_seq):
    return LABEL_REKEY + u16(sid) + sender + receiver + eph + nonce + u32(prev_seq)


def mesh_tag_input(label, mesh, transcript, signature):
    return label + u16(mesh) + transcript + signature


def session_keys(bootstrap, shared):
    prk = hmac.new(bootstrap, LABEL_SESSION + shared, hashlib.sha256).digest()
    return {'prk': prk, 'AtoB': kdf(prk, 1, b'esp-AtoB'), 'BtoA': kdf(prk, 2, b'esp-BtoA'),
            'bond': kdf(prk, 3, b'esp-bond')[:16]}


def group_tag(group_key, hdr30, payload):
    return tag(group_key, LABEL_GROUP + hdr30 + payload)


# ---- The fixed inputs of every vector ------------------------------------------
PASSPHRASE = b'lab-passphrase-1'
LABEL = b'primary'
MAC_A = bytes([0x02, 0x00, 0x00, 0x00, 0x00, 0xA1])   # the initiator
MAC_B = bytes([0x02, 0x00, 0x00, 0x00, 0x00, 0xB2])   # the responder
PUB_A = bytes(range(0x10, 0x30))
PUB_B = bytes(range(0x40, 0x60))
NONCE_H = bytes([0x11] * 16)
NONCE_R = bytes([0x22] * 16)
EPH = bytes(range(0x80, 0xA0))
NONCE_A = bytes(range(0x30, 0x40))
NONCE_B = bytes(range(0x50, 0x60))
SIG = bytes((i * 7) & 0xFF for i in range(64))
SHARED = bytes(range(0xC0, 0xE0))
SID = 0x1234
CAPS = 0x00000213
EPOCH_MS = 1800000000123
PREV_SEQ = 0x01020304
HDR30 = bytes(range(30))
PAYLOAD = b'hello mesh'


def vectors():
    mk = mesh_keys(PASSPHRASE, LABEL)
    boot, mesh = mk['bootstrap'], mk['id']
    head_ok = confirm_head(mesh, MAC_A, 0, PUB_A[:8])
    head_refuse = confirm_head(mesh, MAC_B, 3, PUB_B[:8])
    op = open_transcript(SID, MAC_A, MAC_B, EPH, NONCE_A, CAPS, EPOCH_MS)
    co = confirm_transcript(SID, MAC_B, MAC_A, EPH, NONCE_A, NONCE_B, CAPS)
    rk = rekey_transcript(SID, MAC_A, MAC_B, EPH, NONCE_A, PREV_SEQ)
    sk = session_keys(boot, SHARED)
    return {
        'stretched': mk['stretched'].hex(),
        'mesh_id': mesh,
        'bootstrap': boot.hex(),
        'group': mk['group'].hex(),
        'hello_tag': tag(boot, hello_input(mesh, MAC_A, PUB_A, NONCE_H)).hex(),
        'reply_tag': tag(boot, reply_input(mesh, MAC_B, PUB_B, NONCE_R, MAC_A, PUB_A, NONCE_H)).hex(),
        'confirm_tag': tag(boot, confirm_input(head_ok, MAC_A, MAC_B, NONCE_H, NONCE_R, PUB_A, PUB_B)).hex(),
        'refusal_tag': tag(boot, confirm_input(head_refuse, MAC_A, MAC_B, NONCE_H, bytes(16), PUB_A,
                                               bytes(32))).hex(),
        'open_transcript_sha256': hashlib.sha256(op).hexdigest(),
        'confirm_transcript_sha256': hashlib.sha256(co).hexdigest(),
        'rekey_transcript_sha256': hashlib.sha256(rk).hexdigest(),
        'open_mesh_tag': tag(boot, mesh_tag_input(LABEL_SOPEN_MK, mesh, op, SIG)).hex(),
        'confirm_mesh_tag': tag(boot, mesh_tag_input(LABEL_SCONF_MK, mesh, co, SIG)).hex(),
        'rekey_mesh_tag': tag(boot, mesh_tag_input(LABEL_REKEY_MK, mesh, rk, SIG)).hex(),
        'session_AtoB': sk['AtoB'].hex(),
        'session_BtoA': sk['BtoA'].hex(),
        'session_bond': sk['bond'].hex(),
        'group_tag': group_tag(mk['group'], HDR30, PAYLOAD).hex(),
    }


if __name__ == '__main__':
    for k, v in vectors().items():
        print(f'    {k!r}: {v!r},' if not isinstance(v, int) else f'    {k!r}: 0x{v:04X},')
