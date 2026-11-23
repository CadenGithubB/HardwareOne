#!/usr/bin/env python3
"""A replayed KEY_EX_HELLO costs no flash write and at most one reply per interval.

A HELLO's HMAC covers the mesh fingerprint, the sender MAC and the sender's
public key, and nothing fresh: a recorded HELLO stays valid, and a copy with a
new header msgId passes the dispatcher's duplicate ring. The receive handler
must therefore not rewrite the peer identity file when the key and mesh are
unchanged (the stored lastSeenSec moves forward once an hour at most), and must
act on at most one verified HELLO per sender MAC per interval, while a first
HELLO, the initiator's own retry and a HELLO with a changed key are handled as
before. A failed file write must leave the identity cache equal to the file.

The KEY_EX_REPLY handler (v4hKeyExReply) is compiled too, with the KEY_EX
in-flight table it reads: a REPLY to a key exchange this device started is
stored and confirmed as before, and one that answers a key exchange an unpair
called off (espnowKeyExCancel) stores nothing, so the unpaired peer's identity
does not come back (finding X4, round 3); a new key exchange after the unpair
is answered and stored again.

Rule A and the revoked list (ESP-NOW 1.0 step 3, cases K14-K19): a HELLO or
REPLY from a MAC with no registry entry stores nothing and is answered
KEY_EX_CONFIRM status 3 through a radio peer that is added, used and deleted
(none is left); a revoked key is refused under any MAC and a revoked MAC with
any key, with no answer at all, and a pairsecure entry waiting for that key is
rolled back; a registry entry stores its first key only inside its first-key
window, and the keyId is logged when it binds; a locked (unreadable) revoked
list refuses new keys. The rule is the production espnowrevoke::admit
(System_ESPNow_RevokedCore.h); its inputs are doubles.

Compiled: v4hKeyExHello with its HMAC check, rate limit and CONFIRM builder
from System_ESPNow_Handlers_Crypto.cpp, and the peer cache with
peerIdentityPersist from System_ESPNow_Identity.cpp, spliced into
espnow_keyex_hello_harness.cpp with the production wire payloads and records.
Doubles: clocks, log, outbox, a counting peer-file writer, a pairing window,
mesh keys and a keyed digest in place of HMAC-SHA256. No radio or hardware is
accessed.

--source-dir <component-directory> reads the production files from another
tree (for example the pre-fix base) while the harness comes from this folder;
the base tree must fail the replay cases.
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
sys.path.insert(0, str(HERE))
from test_web_batch_handlers import extract_block  # noqa: E402

SOURCES = ('System_ESPNow_Handlers_Crypto.cpp', 'System_ESPNow_Identity.cpp', 'System_ESPNow_Identity.h',
           'System_ESPNow_MeshKeys.h', 'System_ESPNow_RxCtx.h', 'System_ESPNow_Wire.h', 'System_ESPNow.cpp')


def fail(what):
    raise SystemExit(f'test_espnow_keyex_hello: {what}')


def one(source, marker, name):
    """extract_block for a marker that must occur exactly once (a definition,
    never a declaration, comment or call)."""
    hits = source.count(marker)
    if hits != 1:
        fail(f'{marker!r} occurs {hits} times in {name} (expected 1)')
    return extract_block(source, marker)


def line(source, pattern, what):
    m = re.search(pattern, source, re.M)
    if not m:
        fail(f'{what} not found')
    return m.group(0)


def between(source, start, end, name):
    if source.count(start) != 1:
        fail(f'{start!r} must occur once in {name}')
    i = source.index(start)
    return source[i:source.index(end, i)]


def build_wire(wire):
    name = 'System_ESPNow_Wire.h'
    parts = [one(wire, 'enum EspNowV4Flags', name) + ';',
             between(wire, 'struct __attribute__((packed)) EspNowV4Header',
                     'static_assert(sizeof(EspNowV4Header) == 32', name),
             line(wire, r'^static_assert\(sizeof\(EspNowV4Header\) == 32[^;]*;', 'header size assert'),
             between(wire, 'struct __attribute__((packed)) V4PayloadKeyExHello {',
                     '// ---- SESSION establishment payloads', name)]
    for kind in ('HELLO', 'REPLY', 'CONFIRM'):
        value = re.search(r'\bESPNOW_V4_TYPE_KEY_EX_' + kind + r'\s*=\s*(\d+)', wire)
        if not value:
            fail(f'ESPNOW_V4_TYPE_KEY_EX_{kind} not found')
        parts.append(f'constexpr uint8_t ESPNOW_V4_TYPE_KEY_EX_{kind} = {value.group(1)};')
    return '\n\n'.join(parts)


def build_types(ident_h, meshkeys_h, rxctx_h):
    return '\n\n'.join([
        one(ident_h, 'struct EspNowIdentity', 'System_ESPNow_Identity.h') + ';',
        one(ident_h, 'struct PeerIdentity', 'System_ESPNow_Identity.h') + ';',
        one(meshkeys_h, 'struct MeshDerivedKeys', 'System_ESPNow_MeshKeys.h') + ';',
        one(rxctx_h, 'struct V4RxCtx', 'System_ESPNow_RxCtx.h') + ';'])


def build_identity(ident):
    name = 'System_ESPNow_Identity.cpp'
    parts = [line(ident, r'^constexpr uint8_t kPeerSlots = \d+;', 'kPeerSlots'),
             line(ident, r'^EXT_RAM_BSS_ATTR PeerIdentity gPeerIdentities\[kPeerSlots\] = \{\};', 'gPeerIdentities')]
    # The lastSeenSec refresh interval exists only on a tree that skips unchanged writes.
    refresh = re.search(r'^constexpr uint32_t kPeerLastSeenRefreshSec = \d+;', ident, re.M)
    if refresh:
        parts.append(refresh.group(0))
    for marker in ('int findSlotByMac(', 'int findFreeSlot(',
                   'const PeerIdentity* peerIdentityFindByMac(', 'bool peerIdentityPersist('):
        parts.append(one(ident, marker, name))
    return '\n\n'.join(parts)


def build_keyex(crypto):
    name = 'System_ESPNow_Handlers_Crypto.cpp'
    parts = [line(crypto, r'^constexpr uint32_t kKeyExRetryMs\s*=\s*\d+;', 'kKeyExRetryMs')]
    # 1.0 step 4: 16-byte tags with a label per frame type (HandshakeCore), the
    # mesh slot of a frame, the HELLO table with its nonce memory and REPLY
    # records, and the CONFIRM checks against both.
    for marker in ('bool keyExTag(', 'bool computeHelloTag(', 'bool computeReplyTag(', 'bool computeConfirmTag(',
                   'espnowhs::KeyExContext keyExHelloRefusalContext(', 'void logMac(', 'int keyExMeshSlot('):
        parts.append(one(crypto, marker, name))
    parts += ['#define HW1_HAS_HELLO_LIMIT 1',
              line(crypto, r'^constexpr uint8_t\s+kKeyExHelloSlots\s*=\s*\d+;', 'kKeyExHelloSlots'),
              line(crypto, r'^constexpr uint32_t\s+kKeyExHelloMinIntervalMs\s*=\s*\d+;', 'kKeyExHelloMinIntervalMs'),
              line(crypto, r'^constexpr uint8_t\s+kKeyExHelloNonceMemory\s*=\s*\d+;', 'kKeyExHelloNonceMemory'),
              line(crypto, r'^constexpr uint8_t\s+kKeyExReplyMemory\s*=\s*\d+;', 'kKeyExReplyMemory'),
              'static_assert(kKeyExHelloMinIntervalMs < kKeyExRetryMs, "as in the source");',
              one(crypto, 'struct KeyExReplySent', name) + ';',
              one(crypto, 'struct KeyExHelloSeen', name) + ';',
              line(crypto, r'^EXT_RAM_BSS_ATTR KeyExHelloSeen gKeyExHelloSeen\[kKeyExHelloSlots\] = \{\};',
                   'gKeyExHelloSeen'),
              one(crypto, 'enum class HelloVerdict', name) + ';']
    for marker in ('KeyExHelloSeen* keyExHelloSlotFor(', 'HelloVerdict keyExHelloAdmit(', 'void keyExNoteReply(',
                   'bool keyExConfirmMatchesReply(', 'uint8_t keyExStoreSlot('):
        parts.append(one(crypto, marker, name))
    # The in-flight table (nonces of our HELLOs), then its accessors.
    parts += [line(crypto, r'^constexpr uint8_t\s+kKeyExInFlightSlots\s*=.*;', 'kKeyExInFlightSlots'),
              line(crypto, r'^constexpr uint8_t\s+kKeyExMaxRetries\s*=.*;', 'kKeyExMaxRetries'),
              line(crypto, r'^constexpr uint8_t\s+kKeyExHelloNonces\s*=.*;', 'kKeyExHelloNonces'),
              one(crypto, 'struct KeyExInFlight', name) + ';',
              line(crypto, r'^EXT_RAM_BSS_ATTR KeyExInFlight gKeyExInFlight\[kKeyExInFlightSlots\] = \{\};',
                   'gKeyExInFlight')]
    for marker in ('KeyExInFlight* keyExInFlightArm(', 'void keyExInFlightNewNonce(', 'KeyExInFlight* keyExInFlightFind(',
                   'bool keyExConfirmMatchesHello(', 'static void keyExClearInFlight(const uint8_t peerMac[6]) '):
        parts.append(one(crypto, marker, name))
    parts += ['#define HW1_HAS_KEYEX_CANCEL 1',
              one(crypto, 'void espnowKeyExCancel(const uint8_t peerMac[6])', name),
              one(crypto, 'static bool keyExCancelled(const uint8_t peerMac[6]) ', name),
              one(crypto, 'bool keyExIsInFlight(const uint8_t peerMac[6])', name)]
    for marker in ('static void sendKeyExConfirm(', 'static void sendKeyExNotAccepted(', 'static void keyExRevokedDrop(',
                   'static void logKeyBound('):
        parts.append(one(crypto, marker, name))
    parts += [one(crypto, 'void v4hKeyExHello(const V4RxCtx& ctx)', name),
              one(crypto, 'void v4hKeyExReply(const V4RxCtx& ctx)', name),
              '#define HW1_HAS_CONFIRM_KEEPS_RETRY 1',
              one(crypto, 'void v4hKeyExConfirm(const V4RxCtx& ctx)', name)]
    return '\n\n'.join(parts)


def build_binding(espnow):
    """The PAIR_REQUEST key binding (1.0 step 4) from System_ESPNow.cpp."""
    name = 'System_ESPNow.cpp'
    return '\n\n'.join([one(espnow, 'struct PairKeyBinding', name) + ';',
                         line(espnow, r'^static constexpr uint8_t kPairKeyBindings = \d+;', 'kPairKeyBindings'),
                         line(espnow, r'^EXT_RAM_BSS_ATTR static PairKeyBinding gPairKeyBind\[kPairKeyBindings\];',
                              'gPairKeyBind'),
                         one(espnow, 'static void pairKeyBind(', name),
                         one(espnow, 'static void pairKeyUnbind(', name),
                         one(espnow, 'static bool pairKeyMismatch(', name)])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--source-dir', type=Path,
                   help='directory whose ' + ', '.join(SOURCES) + ' replace the component copies')
    a = p.parse_args()
    if not a.cxx:
        p.error('a host C++17 compiler is required')

    def read(name):
        overlay = a.source_dir / name if a.source_dir else None
        return (overlay if overlay and overlay.exists() else a.component / name).read_text()

    harness = (HERE / 'espnow_keyex_hello_harness.cpp').read_text()
    for tag, value in (('// INSERT_WIRE', build_wire(read('System_ESPNow_Wire.h'))),
                       ('// INSERT_TYPES', build_types(read('System_ESPNow_Identity.h'),
                                                        read('System_ESPNow_MeshKeys.h'),
                                                        read('System_ESPNow_RxCtx.h'))),
                       ('// INSERT_IDENTITY', build_identity(read('System_ESPNow_Identity.cpp'))),
                       ('// INSERT_BINDING', build_binding(read('System_ESPNow.cpp'))),
                       ('// INSERT_KEYEX', build_keyex(read('System_ESPNow_Handlers_Crypto.cpp')))):
        if harness.count(tag) != 1:
            fail(f'harness: {tag} must appear once')
        harness = harness.replace(tag, value)
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-keyex-hello-') as tmp:
        unit, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
        (unit.parent / 'System_MacText.h').write_text((HERE.parents[1] / 'System_MacText.h').read_text())
        # clockcore::isValidEpoch: a creation or bond time is stored only when the clock is set.
        (unit.parent / 'System_ClockCore.h').write_text((HERE.parents[1] / 'System_ClockCore.h').read_text())
        unit.write_text(harness)
        # The pasted slices carry helpers and parameters this harness does not
        # use (the firmware builds without -Wextra).
        (unit.parent / 'System_ESPNow_RevokedCore.h').write_text(
            (HERE.parents[1] / 'System_ESPNow_RevokedCore.h').read_text())
        (unit.parent / 'System_ESPNow_HandshakeCore.h').write_text(
            (HERE.parents[1] / 'System_ESPNow_HandshakeCore.h').read_text())
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
               '-isystem', str(HERE.parents[2] / 'hardwareone_libs' / 'ArduinoJson' / 'src'),
               '-Wno-unused-const-variable', '-Wno-unused-function', '-Wno-unused-parameter',
               str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True, timeout=180)
        result = subprocess.run([str(exe)], timeout=60)
    if result.returncode:
        fail(f'harness reported failures (exit {result.returncode})')
    # KEY_EX_REPLY is not compiled here. Source rule: a REPLY that finds the
    # session with the responder already ACTIVE (a late second answer, or a
    # replay) sends what is waiting over it and never kicks SESSION_OPEN, which
    # on an ACTIVE session would replace it.
    crypto = read('System_ESPNow_Handlers_Crypto.cpp')
    if 'bool keyExHelloAdmit(' in crypto:
        reply = extract_block(crypto, 'void v4hKeyExReply(const V4RxCtx& ctx)')
        live = reply.find('live->state == SESSION_ACTIVE')
        drain = reply.find('pendingFrameDrainForPeer(msg->responderMac);')
        kick = reply.find('espnowSessionOpenInitiate(msg->responderMac')
        other = reply.find('} else if (pendingFrameHasForPeer(msg->responderMac)) {')
        if not (0 < live < drain < other < kick):
            fail('v4hKeyExReply: an ACTIVE session must be drained, and only otherwise kicked')
        if reply.count('espnowSessionOpenInitiate(') != 1:
            fail('v4hKeyExReply: one SESSION_OPEN kick, in the not-ACTIVE branch')
    # The PAIR_REQUEST key binding (1.0 step 4, from step 3's review): the
    # binding rule is compiled above (pairKeyBind / pairKeyMismatch); its wiring
    # in System_ESPNow.cpp is pinned here.
    espnow = read('System_ESPNow.cpp')
    admit = extract_block(espnow, 'espnowrevoke::Admit espnowKeyExAdmit(const uint8_t mac[6], const uint8_t pub[32])')
    if 'pairKeyMismatch(mac, pub)' not in admit or 'return espnowrevoke::Admit::NotAccepted;' not in admit:
        fail('espnowKeyExAdmit does not refuse a key other than the accepted request\'s (status 3)')
    accept = extract_block(espnow, 'const char* cmd_espnow_accept(const String& argsInput)')
    if not 0 < accept.find('pairKeyBind(req.mac, pub);') < accept.find('cmd_espnow_pairsecure('):
        fail('espnowaccept does not bind the request\'s key before the pairing (and its key exchange) starts')
    request = extract_block(espnow, 'static void v4h_pair_request(const V4RxCtx& ctx)')
    if 'memcpy(gPairInPub, req.senderPub, 32);' not in request:
        fail('the pending pair request does not keep the key it carries')
    if 'pairKeyUnbind(mac);' not in extract_block(espnow, 'void espnowFirstKeyBound(const uint8_t mac[6])') or \
            'pairKeyUnbind(mac);' not in extract_block(espnow, 'static void espnowUnpairClearPairing(const uint8_t mac[6])'):
        fail('a binding does not end when the key binds or the device is unpaired')
    local = extract_block(espnow, 'bool espnowLocalPairingOpen(const uint8_t mac[6])')
    if 'pairingWindowOpenFor(mac)' not in local or 'espnowrevoke::firstKeyOpen(' not in local:
        fail('espnowLocalPairingOpen is not the pairsecure window or the first-key window')
    print('test_espnow_keyex_hello: all cases passed')


if __name__ == '__main__':
    main()
