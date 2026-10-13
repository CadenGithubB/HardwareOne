#!/usr/bin/env python3
"""Run the production SESSION_REKEY initiator, responder and retry sweep from
System_ESPNow_Handlers_Crypto.cpp, with the session lifecycle from
System_ESPNow_Sessions.cpp, against modelled peers.

The production session record, REKEY wire payload and identity structs are used
unchanged. Doubles: a settable clock, captured log lines, a counting lock,
calloc-backed ps_alloc, a capturing radio and deterministic stand-in crypto.
The cases drop REKEY frames and refuse sends: an unanswered initiator REKEY must
be re-sent as the identical frame with its original msgId, a still-unanswered
session must be dropped so the next send re-handshakes, answered, aborted,
replaced and responder-side rekeys must never be re-sent, and a refused send
must leave the session usable. The espnow tick's REKEY auto-trigger (step 1e of
processMeshHeartbeats in System_ESPNow.cpp) is checked by a source contract and
run on the 10 ms tick: a session the initiator would refuse gets no per-tick
log line. No radio or hardware is accessed.
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
           'System_ESPNow_Deadline.h', 'System_ESPNow.cpp')

# Step 1e of processMeshHeartbeats: the periodic REKEY auto-trigger (espnow tick).
AUTO_TRIGGER_MARKER = '// 1e. Phase 3.6'
AUTO_TRIGGER_INFO = 'INFO_ESPNOWF("REKEY auto-trigger'


def match(source, pattern, what):
    m = re.search(pattern, source, re.M)
    if not m:
        raise SystemExit(f'test_espnow_rekey_retry: {what} not found in the production source')
    return m


def build_types(sessions_h, wire, ident):
    types = [sessions_h[sessions_h.index('enum SessionStateLifecycle'):
                        sessions_h.index('struct SessionState {')],
             extract_block(sessions_h, 'struct SessionState') + ';',
             match(sessions_h, r'^static_assert\(sizeof\(SessionState\)[^;]*;', 'SessionState size assert').group(0),
             extract_block(wire, 'struct __attribute__((packed)) V4PayloadSessionRekey') + ';',
             match(wire, r'^static_assert\(sizeof\(V4PayloadSessionRekey\) == \d+[^;]*;',
                   'REKEY payload size assert').group(0)]
    for name, ctype in (('ESPNOW_V4_TYPE_SESSION_REKEY', 'uint8_t'),
                        ('ESPNOW_V4_FLAG_HANDSHAKE', 'uint16_t'),
                        ('ESPNOW_V4_FLAG_ACK_REQ', 'uint16_t')):
        value = match(wire, r'\b' + name + r'\s*=\s*(0x[0-9A-Fa-f]+|\d+)', name).group(1)
        types.append(f'constexpr {ctype} {name} = {value};')
    types.append(extract_block(ident, 'struct EspNowIdentity') + ';')
    types.append(extract_block(ident, 'struct PeerIdentity') + ';')
    # Handshake-freshness verdict used by runDeferredRekey, when the tree has it.
    if 'enum SessionPeerRekeyVerdict' in sessions_h:
        types.append(match(sessions_h, r'^constexpr uint32_t kRekeySeqSlack = \d+;', 'kRekeySeqSlack').group(0))
        types.append(extract_block(sessions_h, 'enum SessionPeerRekeyVerdict') + ';')
    return '\n\n'.join(types)


def build_sessions(sessions):
    ns_start = sessions.index('namespace {')
    ns_end = sessions.index('}  // namespace', ns_start) + len('}  // namespace')
    pieces = [sessions[ns_start:ns_end]]
    for marker in ('bool sessionsInit(', 'SessionState* sessionFindByPeer(',
                   'SessionState* sessionFindBySessionId(', 'SessionState* sessionAllocate(',
                   'void sessionClear(', 'bool sessionIsASide(', 'const SessionState* sessionAt(',
                   'uint8_t sessionSlotCount('):
        pieces.append(extract_block(sessions, marker))
    pieces.append(match(sessions, r'^static constexpr uint32_t kSessionEstablishTimeoutMs = \d+;',
                        'kSessionEstablishTimeoutMs').group(0))
    for marker in ('uint8_t sessionEstablishingTimeoutSweep(', 'void sessionApplyRekeyedKeys(',
                   'bool sessionMarkRekeyInitiated(', 'void sessionAbortRekey(',
                   'void sessionRekeyPrevKeysSweep('):
        pieces.append(extract_block(sessions, marker))
    for marker in ('SessionPeerRekeyVerdict sessionClassifyPeerRekey(', 'void sessionNotePeerRekey(',
                   'void sessionHoldRekeyReply(', 'bool sessionOpenIdStale('):
        if marker in sessions:
            pieces.append(extract_block(sessions, marker))
    return '\n\n'.join(pieces)


def build_crypto(crypto):
    """Returns (anonymous-namespace internals, public entry points). The retry
    table and sweep are optional: a source without them (the pre-fix tree) gets
    an empty sweep, so the cases fail on behaviour rather than on a missing
    symbol."""
    internal = [match(crypto, r'^constexpr const char\*\s+kRekeyLabel\s*=[^;]*;', 'kRekeyLabel').group(0),
                match(crypto, r'^constexpr size_t\s+kLabelLen\s*=[^;]*;', 'kLabelLen').group(0),
                extract_block(crypto, 'void buildRekeyTranscript('),
                extract_block(crypto, 'struct DeferredRekeyWork') + ';',
                extract_block(crypto, 'bool deriveRekeyedKeysFromShared(')]
    for name in ('kRekeyInFlightSlots', 'kRekeyMaxRetries', 'kRekeyRetryMs'):
        m = re.search(r'^constexpr\s+uint\d+_t\s+' + name + r'\s*=\s*\d+;', crypto, re.M)
        if m:
            internal.append(m.group(0))
    if 'struct RekeyInFlight' in crypto:
        internal.append(extract_block(crypto, 'struct RekeyInFlight') + ';')
        internal.append(match(crypto, r'^RekeyInFlight\* gRekeyInFlight = nullptr;', 'gRekeyInFlight').group(0))
    for marker in ('bool ensureRekeyInFlightAllocated(', 'void rekeyInFlightArm('):
        if marker in crypto:
            internal.append(extract_block(crypto, marker))
    internal += [extract_block(crypto, 'bool sendRekey('), extract_block(crypto, 'void runDeferredRekey(')]
    public = [extract_block(crypto, 'bool espnowRekeyInitiate(')]
    public.append(extract_block(crypto, 'void rekeyRetrySweep(') if 'void rekeyRetrySweep(' in crypto
                  else 'void rekeyRetrySweep(uint32_t) {}')
    return '\n\n'.join(internal), '\n\n'.join(public)


def strip_comments(code):
    """Blanks // and /* */ comments, leaving string and character literals intact."""
    out, pos, n = [], 0, len(code)
    while pos < n:
        char, following = code[pos], code[pos + 1:pos + 2]
        if char == '/' and following == '/':
            end = code.find('\n', pos)
            pos = n if end < 0 else end
        elif char == '/' and following == '*':
            end = code.find('*/', pos + 2)
            pos = n if end < 0 else end + 2
            out.append(' ')
        elif char in ('"', "'"):
            end = pos + 1
            while end < n and code[end] != char:
                end += 2 if code[end] == '\\' else 1
            out.append(code[pos:end + 1])
            pos = end + 1
        else:
            out.append(char)
            pos += 1
    return ''.join(out)


def auto_trigger_contract(espnow):
    """Source contract on the espnow tick's REKEY auto-trigger: the branch that
    logs 'REKEY auto-trigger' must itself skip a session that espnowRekeyInitiate
    would refuse (inside kRekeyMinIntervalMs of the last attempt or key change);
    otherwise a refused send, which keeps the session ACTIVE, makes every 10 ms
    tick log the INFO line plus the initiate's 'skipping' line. Returns the
    extracted block (run by case c8) and a list of (ok, label)."""
    block = extract_block(espnow, AUTO_TRIGGER_MARKER)
    code = strip_comments(block)
    info = code.find(AUTO_TRIGGER_INFO)
    branch = code.rfind('else if', 0, info) if info >= 0 else -1
    cond = code[branch:info] if branch >= 0 else ''
    checks = [
        (info >= 0 and branch >= 0, 'contract: the 1e block has an else-if branch that logs "REKEY auto-trigger"'),
        ('rekeyInitiatedAtMs' in cond and 'kRekeyMinIntervalMs' in cond,
         'contract: the auto-trigger branch tests rekeyInitiatedAtMs against kRekeyMinIntervalMs '
         'before its INFO line (FAILS ON BASE and without the tick guard)'),
        (re.search(r'nowMs2\s*-\s*sc->establishedAtMs\s*\)\s*>=\s*kRekeyMinIntervalMs', cond) is not None,
         'contract: the auto-trigger branch tests establishedAtMs against kRekeyMinIntervalMs '
         'before its INFO line (FAILS ON BASE and without the tick guard)'),
    ]
    return block, checks


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

    def locate(name):
        overlay = a.source_dir / name if a.source_dir else None
        return overlay if overlay and overlay.exists() else a.component / name

    def read(name):
        return locate(name).read_text()

    # The session functions compare their deadlines through this header once
    # the tree has it; '' when neither the overlay nor the component does.
    deadline = locate('System_ESPNow_Deadline.h')
    deadline = deadline.read_text() if deadline.exists() else ''

    auto_trigger, contract = auto_trigger_contract(read('System_ESPNow.cpp'))
    contract_failures = 0
    for ok, label in contract:
        print(('ok   ' if ok else 'FAIL ') + label)
        contract_failures += 0 if ok else 1

    crypto_internal, crypto_public = build_crypto(read('System_ESPNow_Handlers_Crypto.cpp'))
    harness = (HERE / 'espnow_rekey_retry_harness.cpp').read_text()
    for tag, value in (('// INSERT_DEADLINE', deadline),
                       ('// INSERT_TYPES', build_types(read('System_ESPNow_Sessions.h'),
                                                        read('System_ESPNow_Wire.h'),
                                                        read('System_ESPNow_Identity.h'))),
                       ('// INSERT_SESSIONS', build_sessions(read('System_ESPNow_Sessions.cpp'))),
                       ('// INSERT_CRYPTO_INTERNAL', crypto_internal),
                       ('// INSERT_CRYPTO_PUBLIC', crypto_public),
                       ('// INSERT_AUTO_TRIGGER', auto_trigger)):
        assert harness.count(tag) == 1, tag
        harness = harness.replace(tag, value)
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-rekey-retry-') as tmp:
        unit, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
        unit.write_text(harness)
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic', str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True, timeout=120)
        sys.stdout.flush()
        result = subprocess.run([str(exe)], timeout=60)
    if contract_failures or result.returncode:
        raise SystemExit(f'test_espnow_rekey_retry: {contract_failures} source contract failure(s), '
                         f'harness exit {result.returncode}')
    print('test_espnow_rekey_retry: all cases passed')


if __name__ == '__main__':
    main()
