#!/usr/bin/env python3
"""Drive the production SESSION_OPEN / SESSION_CONFIRM / SESSION_REKEY handlers
and initiators from System_ESPNow_Handlers_Crypto.cpp, with the session
lifecycle, wrap and unwrap functions from System_ESPNow_Sessions.cpp, between
two modelled devices.

The production session record, wire header and handshake payloads are used
unchanged. Doubles: a settable clock, per-device logs, outboxes and deferred
(cmd_exec) queues, calloc-backed ps_alloc, a counting lock and deterministic
stand-in crypto. The cases re-deliver captured handshake frames: a REKEY or
SESSION_OPEN copy must be ignored without a reply and without touching the live
keys, an older REKEY copy must end in one converged extra round instead of an
endless exchange, and normal, concurrent, retransmitted, glare and post-reboot
handshakes must still converge. No radio or hardware is accessed.
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

SOURCES = ('System_ESPNow_Handlers_Crypto.cpp', 'System_ESPNow_Sessions.cpp',
           'System_ESPNow_Sessions.h', 'System_ESPNow_Wire.h', 'System_ESPNow_Identity.h',
           'System_ESPNow_RxCtx.h', 'System_ESPNow_Deadline.h')


def fail(what):
    raise SystemExit(f'test_espnow_session_rekey: {what}')


def one(source, marker, name):
    """extract_block for a marker that must occur exactly once (a definition,
    never a declaration, comment or call)."""
    hits = source.count(marker)
    if hits != 1:
        fail(f'{marker!r} occurs {hits} times in {name} (expected 1)')
    return extract_block(source, marker)


def optional(source, marker, name):
    return one(source, marker, name) if marker in source else ''


def line(source, pattern, what):
    m = re.search(pattern, source, re.M)
    if not m:
        fail(f'{what} not found')
    return m.group(0)


def between(source, start, end, name):
    if source.count(start) != 1:
        fail(f'{start!r} must occur once in {name}')
    i = source.index(start)
    j = source.index(end, i)
    return source[i:j]


def namespace_around(source, anchor, name):
    """The anonymous-namespace block that contains `anchor`."""
    if source.count(anchor) != 1:
        fail(f'{anchor!r} must occur once in {name}')
    at = source.index(anchor)
    start = source.rindex('namespace {', 0, at)
    end = source.index('}  // namespace', at) + len('}  // namespace')
    return source[start:end]


def build_wire(wire):
    parts = [line(wire, r'^#define ESPNOW_V4_MAX_PAYLOAD\b.*$', 'ESPNOW_V4_MAX_PAYLOAD'),
             line(wire, r'^#define ESPNOW_V4_AEAD_TAG_LEN\b.*$', 'ESPNOW_V4_AEAD_TAG_LEN'),
             line(wire, r'^#define ESPNOW_V4_MAX_PLAINTEXT\b.*$', 'ESPNOW_V4_MAX_PLAINTEXT'),
             one(wire, 'enum EspNowV4Flags', 'System_ESPNow_Wire.h') + ';',
             between(wire, 'struct __attribute__((packed)) EspNowV4Header',
                     'static_assert(sizeof(EspNowV4Header) == 32', 'System_ESPNow_Wire.h'),
             line(wire, r'^static_assert\(sizeof\(EspNowV4Header\) == 32[^;]*;', 'header size assert'),
             between(wire, 'struct __attribute__((packed)) V4PayloadSessionOpen {',
                     '// ---- Phase 5 — event subscription registry', 'System_ESPNow_Wire.h')]
    for name in ('OPEN', 'CONFIRM', 'REKEY'):
        value = re.search(r'\bESPNOW_V4_TYPE_SESSION_' + name + r'\s*=\s*(\d+)', wire)
        if not value:
            fail(f'ESPNOW_V4_TYPE_SESSION_{name} not found')
        parts.append(f'constexpr uint8_t ESPNOW_V4_TYPE_SESSION_{name} = {value.group(1)};')
    return '\n\n'.join(parts)


def build_types(sessions_h, ident, rxctx):
    parts = [between(sessions_h, 'enum SessionStateLifecycle',
                     '// One-time allocation of the gSessions table', 'System_ESPNow_Sessions.h')]
    # Handshake-freshness verdict (declared further down the header), when present.
    if 'enum SessionPeerRekeyVerdict' in sessions_h:
        parts.append(line(sessions_h, r'^constexpr uint32_t kRekeySeqSlack = \d+;', 'kRekeySeqSlack'))
        parts.append(one(sessions_h, 'enum SessionPeerRekeyVerdict', 'System_ESPNow_Sessions.h') + ';')
    parts.append(one(ident, 'struct EspNowIdentity', 'System_ESPNow_Identity.h') + ';')
    parts.append(one(ident, 'struct PeerIdentity', 'System_ESPNow_Identity.h') + ';')
    parts.append(one(rxctx, 'struct V4RxCtx', 'System_ESPNow_RxCtx.h') + ';')
    return '\n\n'.join(parts)


def build_sessions(sessions):
    name = 'System_ESPNow_Sessions.cpp'
    pieces = [namespace_around(sessions, 'constexpr uint8_t kSessionSlots', name)]
    for marker in ('bool sessionsInit(', 'SessionState* sessionFindByPeer(',
                   'SessionState* sessionFindBySessionId(', 'SessionState* sessionAllocate(',
                   'void sessionClear(', 'bool sessionIsASide(', 'bool sessionDeriveAeadKeys(',
                   'const SessionState* sessionAt(', 'uint8_t sessionSlotCount('):
        pieces.append(one(sessions, marker, name))
    pieces.append(namespace_around(sessions, 'constexpr size_t kAeadTagBytes', name))
    pieces.append(between(sessions, 'static const char* kSessionStateNames[]',
                          'bool sessionWrapFrame(', name))
    for marker in ('bool sessionWrapFrame(', 'bool sessionUnwrapFrame(',
                   'void sessionApplyRekeyedKeys(', 'bool sessionMarkRekeyInitiated(',
                   'void sessionAbortRekey(', 'void sessionRekeyPrevKeysSweep('):
        pieces.append(one(sessions, marker, name))
    # Handshake-freshness helpers; absent on a pre-fix tree, where nothing calls them.
    for marker in ('SessionPeerRekeyVerdict sessionClassifyPeerRekey(', 'void sessionNotePeerRekey(',
                   'void sessionHoldRekeyReply(', 'bool sessionOpenIdStale('):
        pieces.append(optional(sessions, marker, name))
    return '\n\n'.join(p for p in pieces if p)


def build_crypto(crypto):
    """Returns (anonymous-namespace internals, public entry points)."""
    name = 'System_ESPNow_Handlers_Crypto.cpp'
    internal = [line(crypto, r'^constexpr const char\*\s+kOpenLabel\s*=[^;]*;', 'kOpenLabel'),
                line(crypto, r'^constexpr const char\*\s+kConfirmLabel\s*=[^;]*;', 'kConfirmLabel'),
                line(crypto, r'^constexpr const char\*\s+kRekeyLabel\s*=[^;]*;', 'kRekeyLabel'),
                line(crypto, r'^constexpr size_t\s+kLabelLen\s*=[^;]*;', 'kLabelLen')]
    for marker in ('void buildOpenTranscript(', 'void buildConfirmTranscript(',
                   'void buildRekeyTranscript(', 'static uint16_t sessionRejectLogCount('):
        internal.append(one(crypto, marker, name))
    for marker in ('struct DeferredSessionOpenWork', 'struct DeferredSessionConfirmWork'):
        internal.append(one(crypto, marker, name) + ';')
    for marker in ('void runDeferredSessionOpen(void* arg)', 'void runDeferredSessionConfirm(void* arg)'):
        internal.append(one(crypto, marker, name))
    internal.append(one(crypto, 'struct DeferredRekeyWork', name) + ';')
    internal.append(one(crypto, 'bool deriveRekeyedKeysFromShared(', name))
    # The initiator REKEY retry table (present once the retry fix is in the tree).
    for const in ('kRekeyInFlightSlots', 'kRekeyMaxRetries', 'kRekeyRetryMs'):
        m = re.search(r'^constexpr\s+uint\d+_t\s+' + const + r'\s*=\s*\d+;', crypto, re.M)
        if m:
            internal.append(m.group(0))
    if 'struct RekeyInFlight' in crypto:
        internal.append(one(crypto, 'struct RekeyInFlight', name) + ';')
        internal.append(line(crypto, r'^RekeyInFlight\* gRekeyInFlight = nullptr;', 'gRekeyInFlight'))
    internal.append(optional(crypto, 'bool ensureRekeyInFlightAllocated(', name))
    internal.append(optional(crypto, 'void rekeyInFlightArm(', name))
    internal.append(one(crypto, 'bool sendRekey(', name))
    internal.append(one(crypto, 'void runDeferredRekey(void* arg)', name))
    public = [one(crypto, marker, name) for marker in
              ('void v4hSessionOpen(const V4RxCtx& ctx)', 'void v4hSessionConfirm(const V4RxCtx& ctx)',
               'bool espnowSessionOpenInitiate(const uint8_t peerMac[6], const char* meshLabel)',
               'void v4hSessionRekey(const V4RxCtx& ctx)',
               'bool espnowRekeyInitiate(const uint8_t peerMac[6])')]
    return '\n\n'.join(p for p in internal if p), '\n\n'.join(public)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--source-dir', type=Path,
                   help='directory whose ' + ', '.join(SOURCES) + ' replace the component copies '
                        '(System_ESPNow_Deadline.h is optional)')
    a = p.parse_args()
    if not a.cxx:
        p.error('a host C++17 compiler is required')

    def locate(name):
        overlay = a.source_dir / name if a.source_dir else None
        return overlay if overlay and overlay.exists() else a.component / name

    def read(name):
        return locate(name).read_text()

    # The session functions compare deadlines through this header once the
    # tree has it; '' when neither the overlay nor the component does.
    deadline = locate('System_ESPNow_Deadline.h')
    deadline = deadline.read_text() if deadline.exists() else ''

    crypto_internal, crypto_public = build_crypto(read('System_ESPNow_Handlers_Crypto.cpp'))
    harness = (HERE / 'espnow_session_rekey_harness.cpp').read_text()
    for tag, value in (('// INSERT_DEADLINE', deadline),
                       ('// INSERT_WIRE', build_wire(read('System_ESPNow_Wire.h'))),
                       ('// INSERT_TYPES', build_types(read('System_ESPNow_Sessions.h'),
                                                        read('System_ESPNow_Identity.h'),
                                                        read('System_ESPNow_RxCtx.h'))),
                       ('// INSERT_SESSIONS', build_sessions(read('System_ESPNow_Sessions.cpp'))),
                       ('// INSERT_CRYPTO_INTERNAL', crypto_internal),
                       ('// INSERT_CRYPTO_PUBLIC', crypto_public)):
        assert harness.count(tag) == 1, tag
        harness = harness.replace(tag, value)
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-session-rekey-') as tmp:
        unit, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
        unit.write_text(harness)
        # The pasted header and source slices carry constants and helpers that
        # this harness does not exercise, and espnowSessionOpenInitiate keeps a
        # meshLabel parameter it does not read (the firmware builds without -Wextra).
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
               '-Wno-unused-const-variable', '-Wno-unused-function', '-Wno-unused-parameter',
               str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True, timeout=180)
        result = subprocess.run([str(exe)], timeout=60)
    if result.returncode:
        fail(f'harness reported failures (exit {result.returncode})')
    print('test_espnow_session_rekey: all cases passed')


if __name__ == '__main__':
    main()
