#!/usr/bin/env python3
"""Exercise the production phone secure channel using pinned real crypto libraries.

Also generates the Android interoperability fixtures for phone link v2 (inner
protocol v2) from production code and compares them byte for byte with the
files in tools/ble_secure/fixtures:
  phone_secure_channel_v1_pipe_v2_vectors.json  the MTU-247 handshake golden; its
      handshake and key fields equal the frozen v1 golden, its first DATA each way
      is the v2 pipe HELLO/HELLO_ACK
  phone_pipe_v2_sessions.json   X1 v2: sessions at MTU 64/244/247/517
  phone_pipe_v2_negative.json   X2 v2: negative and boundary vectors
  phone_console_v2_records.json X4: console records and pairing-code vectors
The v1 files are frozen (inner protocol v1 no longer exists in production):
they are compared by SHA-256 only (also in test_phone_fixture_pins.py).
Regenerate deliberately with --write-vectors, then update the pins.
"""
import argparse
from collections import defaultdict
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
from phone_crypto_build import crypto_flags
from test_phone_fixture_pins import FIXTURES, V1_PINS, check_v1_pins

GENERATOR = ('components/hardwareone/test/host/phone_session_vectors.cpp: production BLE_PhoneGatt.cpp, '
             'BLE_PhoneSecureChannel.h, BLE_PhonePipeCore.h and BLE_PhonePipeConsoleCore.h on the synthetic SDK in '
             'phone_gatt_fake_sdk.h; only the device key, secure nonce and pipe HELLO nonce are fixed')
RECORDS_GENERATOR = ('components/hardwareone/test/host/phone_console_vectors.cpp: production '
                     'BLE_PhonePipeConsoleCore.h (writer path, PhoneReceiver, scanIngress), BLE_PhonePairCode.h and '
                     'scPbkdf2')
# Handshake and key fields of the golden: the outer channel is unchanged in v2.
HANDSHAKE_FIELDS = ('phone_private_key_hex', 'phone_public_key_hex', 'device_private_key_hex',
                    'device_public_key_hex', 'phone_nonce_hex', 'device_nonce_hex', 'psk_hex', 'shared_secret_hex',
                    'phone_to_p4_key_hex', 'p4_to_phone_key_hex', 'hello_hex', 'hello_ack_hex', 'confirm_hex',
                    'confirm_ack_hex')
PIPE_FIELDS = ('p4_to_phone_plaintext_hello_hex', 'p4_to_phone_data_hex', 'phone_to_p4_plaintext_hello_ack_hex',
               'phone_to_p4_data_hex')


def lines(binary, mode=None):
    out = subprocess.check_output([binary, *([mode] if mode else [])], text=True)
    return [json.loads(line) for line in out.splitlines()]


def grouped(records, kind, key):
    groups = defaultdict(list)
    for record in records:
        if record['type'] == kind:
            item = {k: v for k, v in record.items() if k not in ('type', key)}
            groups[record[key]].append(item)
    return groups


def utf8(data):
    try:
        return bytes.fromhex(data).decode('utf-8')
    except UnicodeDecodeError:
        return None


def sessions_fixture(records):
    meta = next(r for r in records if r['type'] == 'meta')
    frames = grouped(records, 'frame', 'session')
    console = grouped(records, 'record', 'session')
    sessions = []
    for record in records:
        if record['type'] == 'session':
            session = {k: v for k, v in record.items() if k != 'type'}
            session['frames'] = frames[record['att_mtu']]
            session['console_records'] = console[record['att_mtu']]
            sessions.append(session)
    info = utf8(meta['info_hex'])
    return {
        'description': 'HW1-PIPE-v1 outer channel, inner protocol v2, X1: complete encrypted sessions from the CCCD '
                       'write to the phone\'s PAUSED reset, one per ATT MTU. Relay streams, the console stream '
                       '(slot 2) with every record type, LINK_STATE both ways and every console CLOSE reason. '
                       'Synthetic passphrase, keys, targets and content; never device credentials.',
        'generated_by': GENERATOR,
        **{k: v for k, v in meta.items() if k not in ('type', 'info_hex')},
        'flags': {'p4': {'CONSOLE_OFFERED': 1}, 'phone': {'RELAY_ON': 1, 'CONSOLE_WANTED': 2}},
        'pipe_nonces': {'p4_hex': '0102030405060708', 'phone_hex': '1112131415161718'},
        'targets': {'relay': {'stream': 2, 'address_type': 1, 'ipv4': '192.0.2.10', 'port': 443},
                    'relay_cancelled': {'stream': 3, 'address_type': 2, 'ipv6': '2001:db8::10', 'port': 443},
                    'relay_refused': {'stream': 4, 'address_type': 3, 'dns': 'relay.example.test', 'port': 8443},
                    'console': {'streams': [1, 5, 6, 7, 8, 9], 'address_type': 4, 'port': 1, 'host_length': 0}},
        'info_utf8': info,
        'script': [r['line'] for r in records if r['type'] == 'script'],
        'phone_rules': [
            'reads every DATA byte at once and grants consumed + 4096 (relay and console alike), once that is at '
            'least 512 B past its last grant or 100 ms after it',
            'answers PING with PONG and the P4\'s CLOSE with CLOSE_ACK at once; PINGs after 5 s without a P4 frame '
            'while a relay stream it accepted is open (it never did here: the P4\'s RX went quiet first)',
            'sends queued control frames first, in order, then DATA round-robin within the P4\'s credit',
            'one notification at a time, at least 30 ms apart; the P4\'s writes complete 10 ms after submission',
            'answers a console OPEN with OPEN_OK at once and a relay OPEN after a 100 ms TCP connect; OPEN_ERR, '
            'FIN, LINK_STATE, RESET, COMMAND and BINARY are the script\'s',
        ],
        'fields': {
            't_ms': 'P4 adapter clock relative to its CCCD write submission',
            'att': 'write_request = TX CCCD write; write_cmd = RX write without response; notify = TX notification',
            'value_hex': 'exact ATT value bytes',
            'counter': 'outer DATA counter (big endian on the wire); per direction, strictly increasing',
            'inner_hex': 'decrypted 12-byte pipe header (byte 0 = 2) plus body; seq is per direction and '
                         'contiguous from 0',
            'class': 'link (stream 0), relay (slots 0-1), console (slot 2, OPEN address type 4), unopened',
            'console_records': 'console records in the order their receiver completed them: the phone '
                               '(PhoneReceiver) for p4_to_phone, the P4 (scanIngress) for phone_to_p4; '
                               'payload_hex is empty for a malformed record',
            'p4_end': 'how the P4 adapter ended the session: core error, peer_reset (a phone stream-0 RESET), '
                      'phase, auth_cooldown, strike (a version-mismatch strike)',
            'info_utf8': 'the INFO payload the P4 writes in these sessions (synthetic fw and model)',
            'stream ids': 'one increasing id space for relay and console streams',
        },
        'sessions': sessions,
    }


def negative_fixture(records):
    frames = grouped(records, 'frame', 'vector')
    vectors = []
    for record in records:
        if record['type'] == 'vector':
            vector = {k: v for k, v in record.items() if k != 'type'}
            vector['frames'] = frames[record['id']]
            vectors.append(vector)
    return {
        'description': 'HW1-PIPE-v1 outer channel, inner protocol v2, X2: negative and boundary vectors with the P4 '
                       'outcome, and P4-to-phone vectors with the answer the phone owes (a refusal, or CLOSE_ACK for '
                       'a console CLOSE that crossed one). Same synthetic keys as phone_pipe_v2_sessions.json.',
        'generated_by': GENERATOR,
        'base': {
            'handshake': 'Frames start at the P4 CCCD write of a fresh MTU-247 session (t_ms 0).',
            'pipe_hello': 'Frames continue the MTU-247 session of phone_pipe_v2_sessions.json after the P4\'s pipe '
                          'HELLO (frames 0-5: P4 counter 1 and inner sequence 0 are used); the phone\'s next frame '
                          'is counter 1, inner sequence 0. A p4_to_phone vector at this stage takes the place of '
                          'that HELLO (same counter 1, sequence 0).',
            'pipe': 'Frames continue the MTU-247 session of phone_pipe_v2_sessions.json after the phone pipe '
                    'HELLO_ACK (frames 0-6): phone outer counter 1 and inner sequence 0 are used, P4 counter 1 and '
                    'inner sequence 0 are used. Unless a vector says otherwise, phone frames use the next '
                    'counter/seq and P4 frames continue at counter 2, sequence 1.',
            'transport': 'No authenticated state; the adapter stops before the CCCD write.',
        },
        'fields': {
            'source': 'p4_production = outcome decided by the production P4 adapter; contract = P4-produced bytes '
                      'and the answer Android owes under the agreed rule named in "rule" (a refusal, or CLOSE_ACK '
                      'for a console CLOSE that crossed one); contract_synthetic = '
                      'P4-to-phone bytes no production P4 writes, sealed with the P4-to-phone key under the '
                      'counter and sequence the P4 would use, which Android must refuse under "rule"',
            'p4_sends_hex': 'values the P4 wrote after the first vector frame (REJECT, replies)',
            'auth_cooldown': 'the P4 charges its global 30 s authentication cooldown',
            'p4_error': ('the adapter session end reason: for an authenticated frame the pipe refuses, the core\'s own cause '
                         '(a phone RESET\'s code, Flow, Protocol); Unauthorized only for frames that fail authentication'),
            'version_signal': 'the P4 took the phone\'s stream-0 RESET 2 in Hello state as the version signal',
            'strike': 'the session counts as a version-mismatch strike for the owner (the version signal, or '
                      'silence after the HELLO: link loss within 1500 ms or the 3 s HELLO deadline); three in a '
                      'row latch "phone app too old"',
            'synthetic_frames': 'contract_synthetic only: the "i" of each frame no production P4 writes; the other '
                                'frames are production P4 frames that set the vector up',
            'class': 'link (stream 0), relay, console (OPEN address type 4) or unopened (an id never opened)',
            'frame_order': 'the P4\'s order: its writes and receipts on its clock. Frames that crossed are listed '
                           'as the P4 met them: a phone frame after a P4 frame the phone had not yet received '
                           '(console_open_while_unwanted), or a P4 frame queued before a phone frame arrived and '
                           'written after it (console_close_2_after_reset_1)',
            'refused_or_reset_ids': 'the phone accepts a P4 CLOSE with reason 0..5 on any id it has refused '
                                    '(OPEN_ERR) or reset (stream RESET) and answers CLOSE_ACK: the production P4 '
                                    'closes a console that the phone refused or reset when the two cross '
                                    '(console_open_while_unwanted: CLOSE 4; console_close_2_after_reset_1: CLOSE 2). '
                                    'The class bounds (relay 0..1, console 0..5) apply only to ids the phone still '
                                    'tracks as live; it never applies the relay bound to an id whose class it no '
                                    'longer tracks. The P4 keeps the same rule for the phone\'s CLOSEs',
        },
        'vectors': vectors,
    }


def records_fixture(records):
    meta = next(r for r in records if r['type'] == 'meta')
    vectors = []
    for record in records:
        if record['type'] == 'vector':
            vector = {k: v for k, v in record.items() if k != 'type'}
            vector['deliveries'] = [{**d, 'utf8': utf8(d['hex'])} for d in vector['deliveries']]
            vectors.append(vector)
    codes = []
    for record in records:
        if record['type'] == 'code':
            code = {k: v for k, v in record.items() if k != 'type'}
            codes.append({'id': code.pop('id'), 'input_hex': code['input_hex'],
                          'input_utf8': utf8(code.pop('input_hex')), **code})
    return {
        'description': 'Phone link v2 X4: records on the console stream (pipe slot 2) and the pairing code. Pure '
                       'codec vectors, no crypto except the PSK of each accepted code. Synthetic content.',
        'generated_by': RECORDS_GENERATOR,
        'record_header': 'type u8 | flags u8 (bit0 MORE, bits 1-7 zero) | len u16 LE | payload',
        'types': meta['types'],
        'limits': meta['limits'],
        'binary_prefix_hex': meta['binary_prefix_hex'],
        'info_format': '{"v":1,"fw":"<fw>","model":"<model>","line":511,"bin":489,"auth":true,"resume":false}: '
                       'compact, this key order; fw and model JSON-escaped (" and \\ escaped, control bytes and DEL '
                       'as \\u00XX, bytes >= 0x80 as ?), each cut at 64 escaped bytes, never inside an escape; '
                       'an empty model is HardwareOne',
        'rules': [
            'INFO is the first record of every console stream, and comes once',
            'MORE is legal on TEXT and BLOB only; MORE on any other type, unknown and reserved types included, is '
            'malformed',
            'a chain is TEXT or BLOB records with MORE ended by one without MORE (at most 4608 bytes); a record '
            'with MORE that brings the chain to 4608 is malformed (no continuation could fit)',
            'inside an open chain only the same type or ABORT may follow; unknown types and the reserved type '
            'addressed to the receiver are skipped there as anywhere',
            'ABORT ends an open chain (output not confirmed); with no chain open it is ignored',
            'unknown types (0..1024 bytes, no MORE) and the reserved type addressed to the receiver (TICKET at '
            'the phone, RESUME at the P4) are skipped and counted; a known type sent the wrong way is malformed',
            'BINARY must start with 00 48 57 31 4f 54 41 01 (00 HW1OTA 01)',
            'the phone answers a malformed record with stream RESET 1; the P4 closes the stream with CLOSE 1 after '
            'its tx drains',
        ],
        'fields': {
            'direction': 'p4_to_phone or phone_to_p4: the stream direction',
            'source': 'p4_production: bytes as the P4\'s record writer produces them (p4_to_phone; the phone must '
                      'accept them), or the P4\'s production verdict on phone bytes (phone_to_p4); contract: bytes '
                      'the P4 never writes and the phone\'s verdict under the agreed rules',
            'stream_hex': 'the console stream bytes from OPEN_OK, as the receiver sees them (any DATA split)',
            'records': 'each record the receiver examined, up to and including a malformed one: offset in '
                       'stream_hex, type name, code, flags, len (from the header), payload_hex (the bytes present, '
                       'up to len), verdict (deliver, skip, ignore, malformed) and fault',
            'deliveries': 'what the receiver hands on, in order: info, text and blob (one per whole chain), card, '
                          'mirror, abort (with the cut chain\'s bytes so far); at the P4 command and binary. utf8 '
                          'is the bytes decoded as UTF-8, or null',
            'verdict': 'ok or malformed; fault and fault_offset name the first malformed record (none, -1)',
            'receiver_action': 'none, stream_reset_1 (the phone) or close_1 (the P4)',
            'skipped': 'records skipped and counted (unknown or reserved types)',
            'ignored': 'stray ABORT records ignored',
            'code_vectors': 'input_hex/input_utf8 as typed; accepted = the input is a pairing code; canonical = '
                            'the 16 symbols stored and fed to PBKDF2 (normalised_hex = their bytes); display = '
                            'XXXX-XXXX-XXXX-XXXX; psk_hex = PBKDF2-HMAC-SHA256(canonical, "HW1-PIPE-v1", 100000, '
                            '32); reason for a refusal: symbol (a byte that is no symbol after normalisation), '
                            'length (not 16 symbols) or check (wrong check symbol). A refused input stays an '
                            'ordinary passphrase.',
        },
        'code': {
            'alphabet': meta['code_alphabet'],
            'symbols': meta['code_symbols'],
            'check': 'symbol 16 = sum over i = 0..14 of alpha^(i+1) * v_i in GF(2^5), modulus x^5 + x^2 + 1 '
                     '(0x25), alpha = x, addition = XOR; v_i = the symbol\'s index in the alphabet',
            'entry': 'trim outer ASCII whitespace (09-0D, 20); ASCII uppercase; drop - and space anywhere; I and L '
                     'read as 1, O as 0; any other byte (U included) means not a code',
        },
        'vectors': vectors,
        'code_vectors': codes,
    }


def compare(name, fixture, write):
    path = FIXTURES / name
    text = json.dumps(fixture, indent=2) + '\n'
    if write:
        path.write_text(text)
    assert path.read_bytes() == text.encode(), f'Production output differs from {name} (byte for byte)'


def gf_check(values):
    check, weight = 0, 1
    for v in values:
        weight = (weight << 1) ^ (0x25 if weight & 0x10 else 0)
        product, a, b = 0, weight, v
        while b:
            if b & 1:
                product ^= a
            a = (a << 1) ^ (0x25 if a & 0x10 else 0)
            b >>= 1
        check ^= product
    return check


def parse_records(data):
    """Independent header walk of a record stream (well-formed prefix only)."""
    out, offset = [], 0
    while offset + 4 <= len(data):
        length = data[offset + 2] | data[offset + 3] << 8
        out.append((offset, data[offset], data[offset + 1], length, data[offset + 4:offset + 4 + length]))
        offset += 4 + length
    return out


def check_records(x4, golden):
    alphabet = x4['code']['alphabet']
    assert len({v['id'] for v in x4['vectors']}) == len(x4['vectors'])
    assert len({v['id'] for v in x4['code_vectors']}) == len(x4['code_vectors'])
    for vector in x4['vectors']:
        stream = bytes.fromhex(vector['stream_hex'])
        walked = parse_records(stream)
        for record, (offset, code, flags, length, payload) in zip(vector['records'], walked):
            assert (record['offset'], record['code'], record['flags'], record['len']) == (offset, code, flags, length)
            assert record['verdict'] == 'malformed' or record['payload_hex'] == payload.hex()
        assert (vector['verdict'] == 'malformed') == (vector['fault'] != 'none') == (vector['fault_offset'] >= 0)
    accepted = 0
    for code in x4['code_vectors']:
        if not code['accepted']:
            assert code['reason'] and code['canonical'] is None
            continue
        accepted += 1
        canonical = code['canonical']
        values = [alphabet.index(c) for c in canonical]
        assert len(values) == 16 and gf_check(values[:15]) == values[15], code['id']
        assert code['display'] == '-'.join(canonical[i:i + 4] for i in range(0, 16, 4))
        assert code['normalised_hex'] == canonical.encode().hex()
        psk = hashlib.pbkdf2_hmac('sha256', canonical.encode(), golden['label_ascii'].encode(), 100000, 32)
        assert code['psk_hex'] == psk.hex(), code['id']
    singles = [c for c in x4['code_vectors'] if c['id'].startswith('single_error_')]
    assert len(singles) == 16 * 31 and not any(c['accepted'] for c in singles)
    return accepted


def check_sessions(sessions):
    """Console records in X1 equal the console DATA bodies, decoded independently."""
    for session in sessions['sessions']:
        streams = defaultdict(bytearray)
        for frame in session['frames']:
            if frame.get('op') == 'DATA' and frame['class'] == 'console':
                streams[frame['dir'], frame['stream']] += bytes.fromhex(frame['body_hex'])
        annotated = defaultdict(list)
        for record in session['console_records']:
            annotated[record['dir'], record['stream']].append(record)
        assert set(streams) == set(annotated)
        for key, data in streams.items():
            walked = parse_records(bytes(data))
            assert len(walked) == len(annotated[key]), key
            for record, (_, code, flags, length, payload) in zip(annotated[key], walked):
                assert (record['code'], record['flags'], record['len']) == (code, flags, length)
                assert record['verdict'] == 'malformed' or record['payload_hex'] == payload.hex()
        types = {r['record'] for r in session['console_records']}
        assert types >= {'INFO', 'TEXT', 'BLOB', 'CARD', 'MIRROR', 'ABORT', 'COMMAND', 'BINARY'}, types
        for cls, reasons in (('console', [1, 2, 3, 4, 5]), ('relay', [0, 1])):
            closes = sorted({bytes.fromhex(f['body_hex'])[0] for f in session['frames']
                             if f.get('op') == 'CLOSE' and f['class'] == cls and f['dir'] == 'p4_to_phone'})
            assert closes == reasons, (cls, closes)
        assert {f['body_hex'][12:14] for f in session['frames'] if f.get('op') == 'OPEN' and f['flags'] & 1} == \
            {'01', '02', '03', '04'}  # every address type, console (4) included
        assert {f['dir'] for f in session['frames'] if f.get('op') == 'LINK_STATE'} == {'p4_to_phone', 'phone_to_p4'}
        assert all(f['inner_hex'][:2] == '02' for f in session['frames'] if 'inner_hex' in f)


def check_negative(negative):
    """The production console CLOSE that crosses the phone's refusal or reset, and the phone's rule for it."""
    vectors = {v['id']: v for v in negative['vectors']}

    def trace(name):
        return [(f['dir'], f['op'], f['class'], f['body_hex']) for f in vectors[name]['frames']]
    open_console = ('p4_to_phone', 'OPEN', 'console', '0010000001000400')
    assert trace('console_open_while_unwanted') == [
        open_console, ('phone_to_p4', 'LINK_STATE', 'link', '00000000'), ('p4_to_phone', 'CLOSE', 'console', '0400')]
    assert trace('console_close_2_after_reset_1') == [
        open_console, ('phone_to_p4', 'OPEN_OK', 'console', '00100000'), ('phone_to_p4', 'RESET', 'console', '0100'),
        ('p4_to_phone', 'CLOSE', 'console', '0200')]
    for name in ('console_open_while_unwanted', 'console_close_2_after_reset_1'):
        assert vectors[name]['source'] == 'contract' and 'CLOSE_ACK' in vectors[name]['expected'], name
        assert name in negative['fields']['refused_or_reset_ids'], name


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--idf', type=Path)
    parser.add_argument('--crypto-build', type=Path, help='Reuse an already built native crypto directory')
    parser.add_argument('--component', type=Path, help='Sources to test (default: this component)')
    parser.add_argument('--write-vectors', action='store_true', help='Explicitly regenerate the synthetic v2 fixtures')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = (args.component or here.parents[1]).resolve()
    check_v1_pins()
    v1_golden = json.loads((FIXTURES / 'phone_secure_channel_v1_vectors.json').read_text())
    with tempfile.TemporaryDirectory(prefix='hw1-phone-gatt-') as tmp, crypto_flags(args.idf,args.crypto_build) as (includes,libraries):
        binary = str(Path(tmp) / 'test')
        cmd = [os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra',
               '-Werror', '-pedantic', '-DSYSTEM_BUILDCONFIG_H', '-DENABLE_BLUETOOTH=1',
               '-DENABLE_BLE_PHONE_PIPE=1', '-I', str(here / 'phone_gatt_stubs'),
               '-I', str(root), str(here / 'phone_secure_channel_harness.cpp'),
               *includes, *libraries, '-o', binary]
        if args.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True)
        subprocess.run([binary], check=True)
        source = cmd.index(str(here / 'phone_secure_channel_harness.cpp'))

        cmd[source] = str(here / 'phone_handshake_vectors.cpp')
        subprocess.run(cmd, check=True)
        golden = json.loads(subprocess.check_output([binary], text=True))
        # The outer channel is unchanged: every handshake and key field (and the
        # passphrase, label, iterations, MTU) equals the frozen v1 golden.
        assert list(golden) == list(v1_golden)
        assert all(golden[k] == v1_golden[k] for k in golden if k not in PIPE_FIELDS)
        assert all(golden[k] == v1_golden[k] for k in HANDSHAKE_FIELDS)
        assert all(golden[k] != v1_golden[k] for k in PIPE_FIELDS)
        assert golden['p4_to_phone_plaintext_hello_hex'][:2] == '02'
        compare('phone_secure_channel_v1_pipe_v2_vectors.json', golden, args.write_vectors)
        print('Production phone handshake equals the v1 golden; the first DATA each way is the v2 pipe '
              'HELLO/HELLO_ACK and matches phone_secure_channel_v1_pipe_v2_vectors.json')

        cmd[source] = str(here / 'phone_session_vectors.cpp')
        subprocess.run(cmd, check=True)
        sessions = sessions_fixture(lines(binary, 'sessions'))
        negative = negative_fixture(lines(binary, 'negative'))
        # X1 at MTU 247 must reproduce the handshake golden byte for byte.
        frames = next(s for s in sessions['sessions'] if s['att_mtu'] == 247)['frames']
        first = {}
        for frame in frames:
            first.setdefault((frame['dir'], frame['kind']), frame['value_hex'])
        assert first['phone_to_p4', 'secure_hello'] == golden['hello_hex']
        assert first['p4_to_phone', 'secure_hello_ack'] == golden['hello_ack_hex']
        assert first['phone_to_p4', 'secure_confirm'] == golden['confirm_hex']
        assert first['p4_to_phone', 'secure_confirm_ack'] == golden['confirm_ack_hex']
        assert first['p4_to_phone', 'data'] == golden['p4_to_phone_data_hex']
        assert first['phone_to_p4', 'data'] == golden['phone_to_p4_data_hex']
        assert all(sessions[k] == golden[k] for k in golden if k in sessions)
        assert [s['att_mtu'] for s in sessions['sessions']] == [64, 244, 247, 517]
        check_sessions(sessions)
        # A v1 P4's HELLO (contract_synthetic) is exactly the frozen v1 golden's first P4 DATA.
        v1_hello = next(v for v in negative['vectors'] if v['id'] == 'p4_hello_v1')['frames'][0]['value_hex']
        assert v1_hello == v1_golden['p4_to_phone_data_hex']
        ids = [v['id'] for v in negative['vectors']]
        assert len(ids) == len(set(ids))
        keys = {'p4_production': {'id', 'direction', 'source', 'stage', 'att_mtu', 'description', 'expected',
                                  'p4_sends_hex', 'session_continues', 'p4_phase', 'auth_cooldown', 'p4_error',
                                  'version_signal', 'strike', 'frames'},
                'contract': {'id', 'direction', 'source', 'stage', 'att_mtu', 'description', 'expected',
                             'android_auth_cooldown', 'rule', 'frames'}}
        keys['contract_synthetic'] = keys['contract'] | {'synthetic_frames'}
        assert all(set(v) == keys[v['source']] for v in negative['vectors'])
        check_negative(negative)
        compare('phone_pipe_v2_sessions.json', sessions, args.write_vectors)
        compare('phone_pipe_v2_negative.json', negative, args.write_vectors)
        print('Production X1 v2 sessions (MTU 64/244/247/517) and X2 v2 negative vectors match their fixtures')

        cmd[source] = str(here / 'phone_console_vectors.cpp')
        subprocess.run(cmd, check=True)
        x4 = records_fixture(lines(binary))
        accepted = check_records(x4, golden)
        compare('phone_console_v2_records.json', x4, args.write_vectors)
        print(f'Production X4 console records ({len(x4["vectors"])} vectors) and pairing codes '
              f'({len(x4["code_vectors"])}, {accepted} accepted, PSKs checked with hashlib) match their fixture')
