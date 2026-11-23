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
           'System_ESPNow_MeshKeys.h', 'System_ESPNow_RxCtx.h', 'System_ESPNow_Wire.h')


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
                     '// ---- Phase 3.4', name)]
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
        one(ident_h, 'enum EspNowEventCategory', 'System_ESPNow_Identity.h') + ';',
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
    for marker in ('bool computeKeyExHmac(', 'void logMac(', 'bool ctMemcmp32(', 'int verifyKeyExPayload('):
        parts.append(one(crypto, marker, name))
    # The accepted-HELLO rate limit (absent on a pre-fix tree, where the handler
    # does not call it).
    if 'bool keyExHelloAdmit(' in crypto:
        parts += ['#define HW1_HAS_HELLO_LIMIT 1',
                  line(crypto, r'^constexpr uint8_t\s+kKeyExHelloSlots\s*=\s*\d+;', 'kKeyExHelloSlots'),
                  line(crypto, r'^constexpr uint32_t\s+kKeyExHelloMinIntervalMs\s*=\s*\d+;',
                       'kKeyExHelloMinIntervalMs'),
                  'static_assert(kKeyExHelloMinIntervalMs < kKeyExRetryMs, "as in the source");',
                  one(crypto, 'struct KeyExHelloSeen', name) + ';',
                  line(crypto, r'^EXT_RAM_BSS_ATTR KeyExHelloSeen gKeyExHelloSeen\[kKeyExHelloSlots\] = \{\};',
                       'gKeyExHelloSeen'),
                  one(crypto, 'bool keyExHelloAdmit(', name)]
    parts.append(one(crypto, 'static void sendKeyExConfirm(', name))
    parts.append(one(crypto, 'void v4hKeyExHello(const V4RxCtx& ctx)', name))
    # The in-flight table the REPLY handler reads, then the handler.
    parts += [line(crypto, r'^constexpr uint8_t\s+kKeyExInFlightSlots\s*=.*;', 'kKeyExInFlightSlots'),
              line(crypto, r'^constexpr uint8_t\s+kKeyExMaxRetries\s*=.*;', 'kKeyExMaxRetries'),
              one(crypto, 'struct KeyExInFlight', name) + ';',
              line(crypto, r'^KeyExInFlight gKeyExInFlight\[kKeyExInFlightSlots\] = \{\};', 'gKeyExInFlight'),
              one(crypto, 'void keyExInFlightArm(', name),
              one(crypto, 'static void keyExClearInFlight(const uint8_t peerMac[6]) ', name)]
    if 'void espnowKeyExCancel(const uint8_t peerMac[6])' in crypto:
        parts += ['#define HW1_HAS_KEYEX_CANCEL 1',
                  one(crypto, 'void espnowKeyExCancel(const uint8_t peerMac[6])', name),
                  one(crypto, 'static bool keyExCancelled(const uint8_t peerMac[6]) ', name)]
    parts.append(one(crypto, 'void v4hKeyExReply(const V4RxCtx& ctx)', name))
    return '\n\n'.join(parts)


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
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
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
    print('test_espnow_keyex_hello: all cases passed')


if __name__ == '__main__':
    main()
