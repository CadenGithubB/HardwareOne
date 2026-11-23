#!/usr/bin/env python3
"""Run a bond partner that is not there, and one that comes back.

Bench 2026-11-17, step d2: with bond mode on and the partner set to an address
no device has, the P4 started a key exchange with every bond heartbeat (5 s)
and logged "KEY_EX: no REPLY ... giving up" plus "pending: timeout ..." or
"pending: evicting ..." each time, for as long as it ran.

Compiles the production pieces that run on that path and drives them through
simulated hours with a 100 ms tick:
  - System_ESPNow_HandshakeBackoff.h (when the tree has it), whole;
  - System_ESPNow_Handlers_Crypto.cpp: the KEY_EX in-flight table, its arm,
    clear, query and retry sweep, the key-exchange start (automatic and
    manual) and, when present, the back-off table and its entry points;
  - System_ESPNow_Sessions.cpp: the pending-frame queue (queue, drain, timeout
    sweep, count) and the ESTABLISHING timeout sweep;
  - System_ESPNow.cpp: bondSendEncryptedAsync (with bondFrameIsPeriodic when
    present) and v4_send_encrypted_or_queue.
Doubles: the radio (HELLO and SESSION_OPEN are recorded; a peer that is there
answers 100 ms later the way v4hKeyExReply and the session confirm do), peer
identities, session slots, espnow_tx (runs the job at once), the clock and the
log.

Checks that an absent partner costs at most a few warning lines and a bounded
number of key exchanges per hour, that heartbeats are never queued behind a
missing session, that a partner that comes back (heard after a quiet spell)
re-forms within 15 s, that a partner heard all along but never answering keeps
its back-off, that an answering partner behaves as before, that a manual
key exchange starts at once, is always reported and ends the back-off, that
bondconnect / bondresync (espnowHandshakeStartNow) and espnowsessionopen
start at once and their failure is printed even after the partner has been
away a while, that bond sensor data is dropped like the heartbeat, and that
the pending-frame warnings carry the count they held back, and that an unpair
(espnowKeyExCancel, pendingFrameDropForPeer) stops this device's HELLO
retries to the peer, frees that entry one retry interval later without a
warning, lets a new key exchange start at once and drops only that peer's
queued frames, each reported failed (finding X4, round 3); since 2026-11-18 that frames waiting for a
session this device answered go out in order once it has settled (pendingFrameDrainSettled, on the tick),
and that the bond request commands' send (bondSendRequestForReply) answers queued, sent or refused from
what became of the frame. The wiring that cannot run here (the
calls from the receive handlers, the session hook, the mesh online edge, unpair
and the manual commands) is pinned in the source.

--source-dir replays other production sources (for example cca36fa9).
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
BACKOFF_H = 'System_ESPNow_HandshakeBackoff.h'
QUEUE_H = 'System_ESPNow_PendingQueue.h'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--source-dir', type=Path,
                   help='directory whose ESP-NOW sources replace the component copies')
    a = p.parse_args()
    if not a.cxx:
        p.error('a host C++17 compiler is required')
    sys.path.insert(0, str(a.component / 'test/host'))
    from test_web_batch_handlers import extract_block

    def path_of(name):
        overlay = a.source_dir / name if a.source_dir else None
        if overlay and overlay.exists():
            return overlay
        if a.source_dir:
            return None if name in (BACKOFF_H, QUEUE_H) else a.component / name
        return a.component / name

    def read(name):
        return path_of(name).read_text()

    def definition(text, marker, what):
        count = text.count(marker)
        if count != 1:
            sys.exit(f'FAIL extraction: {what} marker {marker!r} found {count} times, expected 1')
        return extract_block(text, marker)

    def line(text, pattern, what):
        found = re.findall(pattern, text, re.M)
        if len(found) != 1:
            sys.exit(f'FAIL extraction: {what} ({pattern!r}) found {len(found)} times')
        return found[0]

    crypto = read('System_ESPNow_Handlers_Crypto.cpp')
    sessions = read('System_ESPNow_Sessions.cpp')
    sessions_h = read('System_ESPNow_Sessions.h')
    espnow = read('System_ESPNow.cpp')
    backoff_path = path_of(BACKOFF_H)
    have_backoff = backoff_path is not None and backoff_path.exists() and \
        'bool espnowHandshakeAutoStart(' in crypto
    have_cancel = 'void espnowKeyExCancel(const uint8_t peerMac[6])' in crypto
    have_drop = 'uint8_t pendingFrameDropForPeer(const uint8_t peerMac[6])' in sessions
    have_start_now = have_backoff and 'bool espnowHandshakeStartNow(const uint8_t peerMac[6])' in crypto and \
        'void sessionMarkUserStarted(const uint8_t peerMac[6])' in sessions

    # ---- System_ESPNow_Sessions.cpp ---------------------------------------
    # Since 2026-11-18 the queue keeps several frames per peer, oldest first,
    # with its rules in System_ESPNow_PendingQueue.h (included whole).
    queue_path = path_of(QUEUE_H)
    have_fifo = queue_path is not None and queue_path.exists() and f'#include "{QUEUE_H}"' in sessions_h
    pending = [line(sessions_h, r'^constexpr uint16_t kPendingFramePlaintextMax\s*=.*;', 'kPendingFramePlaintextMax'),
               line(sessions_h, r'^constexpr uint32_t kPendingFrameTimeoutMs\s*=.*;', 'kPendingFrameTimeoutMs')]
    if not have_fifo:
        pending.append(line(sessions, r'^constexpr uint8_t kPendingFrameSlots = \d+;', 'kPendingFrameSlots'))
    pending += [definition(sessions, 'struct PendingFrame', 'Sessions') + ';',
                line(sessions, r'^PendingFrame\* gPending = nullptr;', 'gPending')]
    if have_fifo:
        pending.append(line(sessions, r'^uint32_t gPendingSeq = 0;', 'gPendingSeq'))
    pending.append(definition(sessions, 'bool ensurePendingAllocated()', 'Sessions'))
    if have_backoff:
        pending += [line(sessions, r'^constexpr uint32_t kPendingWarnWindowMs = \d+;', 'kPendingWarnWindowMs'),
                    line(sessions, r'^WarnLimiter gPendingRefuseWarn = \{\};' if have_fifo else
                         r'^WarnLimiter gPendingEvictWarn = \{\};', 'the pending warning limiter'),
                    line(sessions, r'^WarnLimiter gPendingTimeoutWarn = \{\};', 'gPendingTimeoutWarn')]
    if have_fifo:
        pending.append(definition(sessions, 'void pendingFrameRelease(PendingFrame& p, bool failed)', 'Sessions'))
    for marker in ('bool pendingFrameQueue(', 'void pendingFrameDrainForPeer(',
                   'uint8_t pendingFrameTimeoutSweep(', 'uint8_t pendingFrameCount()',
                   'bool pendingFrameHasForPeer('):
        pending.append(definition(sessions, marker, 'Sessions'))
    if have_fifo:
        for marker in ('uint8_t pendingFrameCountForPeer(const uint8_t peerMac[6])',
                       'bool pendingFrameIsWaiting(uint32_t msgId, const uint8_t peerMac[6])',
                       'uint8_t pendingFrameDropAll()'):
            pending.append(definition(sessions, marker, 'Sessions'))
    # The tick's drain of a settled session (review of 2026-11-18): frames that
    # wait for a session this device answered.
    have_settled = have_fifo and 'uint8_t pendingFrameDrainSettled(uint32_t nowMs)' in sessions
    if have_settled:
        pending.append(definition(sessions, 'uint8_t pendingFrameDrainSettled(uint32_t nowMs)', 'Sessions'))
    if have_drop:
        pending.append(definition(sessions, 'uint8_t pendingFrameDropForPeer(const uint8_t peerMac[6])', 'Sessions'))
    pending.append(line(sessions, r'^static constexpr uint32_t kSessionEstablishTimeoutMs = \d+;.*$',
                        'kSessionEstablishTimeoutMs'))
    pending.append(definition(sessions, 'uint8_t sessionEstablishingTimeoutSweep(', 'Sessions'))
    if have_start_now:
        pending.append(definition(sessions, 'void sessionMarkUserStarted(const uint8_t peerMac[6])', 'Sessions'))

    # ---- System_ESPNow_Handlers_Crypto.cpp ----------------------------------
    keyex = [line(crypto, r'^constexpr uint8_t\s+kKeyExInFlightSlots\s*=.*;', 'kKeyExInFlightSlots'),
             line(crypto, r'^constexpr uint8_t\s+kKeyExMaxRetries\s*=.*;', 'kKeyExMaxRetries'),
             line(crypto, r'^constexpr uint32_t\s+kKeyExRetryMs\s*=.*;', 'kKeyExRetryMs'),
             line(crypto, r'^constexpr uint8_t\s+kKeyExHelloNonces\s*=.*;', 'kKeyExHelloNonces'),
             definition(crypto, 'struct KeyExInFlight', 'Crypto') + ';',
             line(crypto, r'^EXT_RAM_BSS_ATTR KeyExInFlight gKeyExInFlight\[kKeyExInFlightSlots\] = \{\};',
                  'gKeyExInFlight'),
             definition(crypto, 'KeyExInFlight* keyExInFlightArm(', 'Crypto'),
             definition(crypto, 'KeyExInFlight* keyExInFlightFind(', 'Crypto')]
    if have_backoff:
        keyex += [line(crypto, r'^constexpr uint32_t kKeyExGiveUpWarnWindowMs = .*;', 'kKeyExGiveUpWarnWindowMs'),
                  line(crypto, r'^WarnLimiter gKeyExGiveUpWarn = \{\};', 'gKeyExGiveUpWarn'),
                  line(crypto, r'^HandshakeBackoffTable gHandshakeBackoff = \{\};', 'gHandshakeBackoff'),
                  line(crypto, r'^portMUX_TYPE gHandshakeBackoffLock = portMUX_INITIALIZER_UNLOCKED;',
                       'gHandshakeBackoffLock')]
    keyex += [definition(crypto, 'static void keyExClearInFlight(const uint8_t peerMac[6]) ', 'Crypto')]
    if have_cancel:
        keyex += [definition(crypto, 'void espnowKeyExCancel(const uint8_t peerMac[6])', 'Crypto'),
                  definition(crypto, 'static bool keyExCancelled(const uint8_t peerMac[6]) ', 'Crypto')]
    keyex += [definition(crypto, 'bool keyExIsInFlight(const uint8_t peerMac[6]) ', 'Crypto'),
              definition(crypto, 'void keyExRetrySweep(uint32_t nowMs)', 'Crypto')]
    if have_backoff:
        for marker in ('static bool keyExInitiate(const uint8_t peerMac[6], const char* meshLabel, bool automatic)',
                       'bool espnowKeyExInitiate(const uint8_t peerMac[6], const char* meshLabel) ',
                       'bool espnowKeyExInitiateManual(const uint8_t peerMac[6], const char* meshLabel)',
                       'void espnowHandshakeBackoffClear(const uint8_t peerMac[6])',
                       'void espnowHandshakeHeard(const uint8_t peerMac[6])',
                       'bool espnowHandshakeAutoStart(const uint8_t peerMac[6])'):
            keyex.append(definition(crypto, marker, 'Crypto'))
        if have_start_now:
            keyex.append(definition(crypto, 'bool espnowHandshakeStartNow(const uint8_t peerMac[6])', 'Crypto'))
    else:
        keyex.append(definition(crypto, 'bool espnowKeyExInitiate(const uint8_t peerMac[6], const char* meshLabel) ',
                                'Crypto'))

    # ---- System_ESPNow.cpp ------------------------------------------------
    send = []
    if 'static bool bondFrameIsPeriodic(uint8_t type)' in espnow:
        send.append(definition(espnow, 'static bool bondFrameIsPeriodic(uint8_t type)', 'ESPNow'))
    send.append(definition(espnow, 'bool v4_send_encrypted_or_queue(const uint8_t dst[6], uint8_t type, '
                                   'uint16_t baseFlags,', 'ESPNow'))
    send.append(definition(espnow, 'static bool bondSendEncryptedAsync(const uint8_t* mac, uint8_t type, '
                                   'uint16_t flags,', 'ESPNow'))
    # The bond request commands' send (review of 2026-11-18): it answers from
    # what became of the frame, not from the session state before espnow_tx ran.
    have_bond_reply = have_fifo and 'static bool bondSendRequestForReply(' in espnow
    if have_bond_reply:
        send.append(definition(espnow, 'static bool espnowSessionActiveWith(const uint8_t* mac)', 'ESPNow'))
        send.append(definition(espnow, 'static bool bondSendRequestForReply(', 'ESPNow'))

    # ---- Wiring pinned in the source (counted as checks) ---------------------
    source_checks = []

    def pin(ok, what):
        source_checks.append((bool(ok), what))

    def body(text, marker):
        return extract_block(text, marker) if text.count(marker) == 1 else ''

    reply = body(crypto, 'V4Verdict v4hKeyExReply(const V4RxCtx& ctx)')
    clear_at = reply.find('espnowHandshakeBackoffClear(msg->responderMac);')
    pin(0 < reply.find('keyExClearInFlight(msg->responderMac);') < clear_at,
        'KEY_EX_REPLY (the peer answered) ends its back-off')
    confirm = body(crypto, 'V4Verdict v4hKeyExConfirm(const V4RxCtx& ctx)')
    # The last status-0 branch: the initiator's CONFIRM ending an exchange we
    # answered (1.0 step 4: an earlier one refuses a status 0 that answers our HELLO).
    ok_branch = confirm[confirm.rfind('if (msg->status == 0) {'):]
    ok_branch = ok_branch[:ok_branch.find('} else {')] if '} else {' in ok_branch else ''
    pin('espnowHandshakeBackoffClear(msg->confirmerMac);' in ok_branch and
        confirm.count('espnowHandshakeBackoffClear(') == 1,
        'KEY_EX_CONFIRM ends the back-off only for status 0 (a refusing peer keeps it)')
    notify = body(espnow, 'void bondNotifySessionEstablished(const uint8_t* peerMac)')
    pin(0 <= notify.find('espnowHandshakeBackoffClear(peerMac)') < notify.find('#if ENABLE_BONDED_MODE'),
        'a session that comes up (any peer) ends its back-off')
    dispatch = body(espnow, 'static V4Verdict v4_dispatch_table_try(')
    pin('espnowHandshakeHeard(recv_info->src_addr);' in dispatch and
        'memcmp(h->origin, recv_info->src_addr, 6) == 0' in dispatch,
        'an authenticated frame sent straight from its origin counts as hearing that peer')
    tick = body(espnow, 'void processMeshHeartbeats()')
    # 1.0 step 8: the edge is "heard again" (the link left None), not the online
    # event, which now waits for two-way proof and a session.
    pin('if (heard && !p.heardLogged) espnowHandshakeBackoffClear(pm);' in tick,
        'the mesh table hearing a peer again ends its back-off')
    forget = body(espnow, 'static bool espnowForgetPeerCrypto(const uint8_t mac[6]')  # the signature grew an optional out-parameter
    pin('espnowHandshakeBackoffClear(mac);' in forget, 'unpair / forget drops the peer\'s back-off entry')
    for marker, what in (('const char* cmd_espnow_keyex(const String& argsInput)', 'espnowkeyex'),
                         ('const char* cmd_espnow_probe(const String& argsInput)', 'espnowprobe'),
                         ('const char* cmd_espnow_pairsecure(const String& argsInput)', 'espnowpairsecure')):
        cmd = body(espnow, marker)
        pin('espnowKeyExInitiateManual(' in cmd and 'espnowKeyExInitiate(' not in cmd,
            f'{what} starts a manual key exchange (at once, back-off ended, always reported)')
    sopen = body(espnow, 'const char* cmd_espnow_sessionopen(const String& argsInput)')
    pin(0 <= sopen.find('espnowHandshakeBackoffClear(mac);') < sopen.find('espnowSessionOpenInitiate('),
        'espnowsessionopen ends the back-off before it starts')
    pin(0 <= sopen.find('espnowSessionOpenInitiate(') < sopen.find('sessionMarkUserStarted(mac);'),
        'espnowsessionopen marks its handshake as user-started (its timeout is always printed)')
    kick = body(espnow, 'static void bondStartHandshakeNow(const uint8_t peerMac[6])')
    pin('espnowHandshakeStartNow(peerMac)' in kick and 'espnowHandshakeAutoStart(' not in kick,
        'bondconnect / bondresync start a manual handshake (always reported), not an automatic one')
    alloc = body(sessions, 'SessionState* sessionAllocate(const uint8_t peerMac[6], uint8_t meshId)')
    pin('existing->userStarted = 0;' in alloc,
        'a slot reused for a new handshake drops the user-started mark of the last one')
    estab = body(sessions, 'uint8_t sessionEstablishingTimeoutSweep(')
    pin('logCooldownOk(sLastEstabWarnMs, 15UL * 60UL * 1000UL)' in estab and 'sEstabWarnHeld' in estab,
        'the ESTABLISHING timeout warning comes once per 15 minutes with the count held back')
    if have_fifo:
        deinit = body(espnow, 'static bool deinitEspNow(bool publishOffEvent)')
        pin(0 < deinit.find('stopEspNowTask(5000)') < deinit.find('pendingFrameDropAll()'),
            'closeespnow drops every waiting frame (marked failed) once the draining task is stopped')
        sender = body(espnow, 'bool v4_send_encrypted_or_queue(const uint8_t dst[6], uint8_t type, uint16_t baseFlags,')
        fast = sender[sender.find('if (s && s->state == SESSION_ACTIVE) {'):]
        pin(0 <= fast.find('pendingFrameDrainForPeer(dst);') < fast.find('v4_send_session_wrapped('),
            'a send over an ACTIVE session drains the frames still waiting first (no overtaking)')
        pin(0 <= tick.find('pendingFrameDrainSettled((uint32_t)millis());') <
            tick.find('pendingFrameTimeoutSweep((uint32_t)millis());'),
            'the tick sends the frames waiting for a settled session before it times frames out')
        for marker, what in (('const char* cmd_bond_requestcap(const String& argsInput)', 'bondrequestcap'),
                             ('const char* cmd_bond_requestmanifest(const String& argsInput)', 'bondrequestmanifest'),
                             ('const char* cmd_bond_requestsettings(const String& argsInput)', 'bondrequestsettings'),
                             ('const char* cmd_bond_requestschema(const String& argsInput)', 'bondrequestschema')):
            cmd = body(espnow, marker)
            pin(cmd.count('bondSendRequestForReply(') == 1 and 'bondSendEncryptedAsync(' not in cmd and
                'espnowSessionActiveWith(' not in cmd and 'queued ?' in cmd,
                f'{what} answers "queued" or "sent" from what became of its frame')
        resync = body(espnow, 'const char* cmd_bond_resync(const String& argsInput)')
        pin(resync.count('bondSendRequestForReply(') == 4 and 'bondSendEncryptedAsync(' not in resync and
            'espnowSessionActiveWith(' not in resync,
            'bondresync (worker) answers "queued" or "sent" from what became of its four frames')
        responder = body(crypto, 'void runDeferredSessionOpen(void* arg)')
        pin('pendingFrameDrainForPeer(' not in responder,
            'the responder does not drain at once (the initiator has not handled the CONFIRM yet)')
    hb = tick[tick.find('// 3. Send periodic paired-mode heartbeat if bonded'):]
    hb = hb[:hb.find('} else if (gSettings.bondModeEnabled && gSettings.bondPeerMac.length() == 0)')]
    pin('if (sent) {' in hb and 'gEspNow->bondHeartbeatsSent++;' in hb[hb.find('if (sent) {'):] and
        'if (sent && (gBondHeartbeatSeqNum' in hb,
        'a heartbeat that did not go out is neither counted nor logged')

    harness = (HERE / 'espnow_handshake_backoff_harness.cpp').read_text()
    for tag, value in (('// INSERT_PENDING', '\n\n'.join(pending)),
                       ('// INSERT_KEYEX', '\n\n'.join(keyex)),
                       ('// INSERT_SEND', '\n\n'.join(send))):
        assert harness.count(tag) == 1, tag
        harness = harness.replace(tag, value)

    print(f'handshake back-off: {"present" if have_backoff else "absent"}; '
          f'unpair call-off: {"present" if have_cancel else "absent"}; '
          f'pending drop: {"present" if have_drop else "absent"}; '
          f'pending FIFO: {"present" if have_fifo else "absent"}; '
          f'settled-session drain: {"present" if have_settled else "absent"}', flush=True)
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-hs-backoff-') as tmp:
        unit, exe = Path(tmp) / 'espnow_handshake_backoff.cpp', Path(tmp) / 'espnow_handshake_backoff'
        unit.write_text(harness)
        shutil.copy(a.component / 'System_MacText.h', Path(tmp) / 'System_MacText.h')
        shutil.copy(a.component / 'System_Deadline.h', Path(tmp) / 'System_Deadline.h')
        if have_backoff:
            shutil.copy(backoff_path, Path(tmp) / BACKOFF_H)
        if have_fifo:
            shutil.copy(queue_path, Path(tmp) / QUEUE_H)
        # The pasted slices keep parameters and helpers this harness does not
        # use (the firmware builds without -Wextra).
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
               '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-unused-const-variable',
               f'-DHW1_HAVE_BACKOFF={1 if have_backoff else 0}',
               f'-DHW1_HAVE_START_NOW={1 if have_start_now else 0}',
               f'-DHW1_HAVE_CANCEL={1 if have_cancel else 0}',
               f'-DHW1_HAVE_PENDING_DROP={1 if have_drop else 0}',
               f'-DHW1_HAVE_PENDING_FIFO={1 if have_fifo else 0}',
               f'-DHW1_HAVE_PENDING_SETTLED={1 if have_settled else 0}',
               f'-DHW1_HAVE_BOND_REQUEST_REPLY={1 if have_bond_reply else 0}',
               '-I', tmp, str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                        '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True, timeout=240)
        result = subprocess.run([str(exe)], timeout=120)

    pin_failures = [what for ok, what in source_checks if not ok]
    for what in pin_failures:
        print(f'FAIL source: {what}')
    print(f'source wiring: {len(source_checks)} checks, {len(pin_failures)} failed')
    if result.returncode or pin_failures:
        print('FAIL espnow_handshake_backoff', file=sys.stderr)
        sys.exit(1)
    print('PASS espnow_handshake_backoff')


if __name__ == '__main__':
    main()
