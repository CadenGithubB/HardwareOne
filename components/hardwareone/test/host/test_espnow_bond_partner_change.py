#!/usr/bin/env python3
"""Run the bond partner change and the worker's way to SYNCED.

Bench 2026-11-17, step c1: the P4 ran 'bondconnect' (master), then the XIAO
(worker). The worker's espnow_task received the master's capability exchange
and acted on it while its own bondconnect was still running; bondconnect then
reset the sync state and wiped it. The master was SYNCED and asked for nothing
more, so the worker stayed "SYNCING (cap=0 ... settTx=1)" until the master ran
'bondresync --all'.

Extracts from System_ESPNow.cpp the frame hold (when the source has it),
isBondPeerMac, isBondModeOnline, isBondSynced, resetBondSync, the CAP_REQ,
CAP_RESP and STATUS_REQ handlers, the worker's capability request (when
present), bondconnect with its handshake kick, bonddisconnect and the
espnowbondmodeenabled / espnowbondpeermac commands, and the bond role and
capability types from System_ESPNow.h. The harness doubles the settings
writes (each one lets espnow_task run: the partner's frames are offered to the
production receive gate, bond mode on and isBondPeerMac), the master (a small
model of its sync tick: CAP_REQ until it has our capability, its CAP_RESP once,
SETTINGS_REQ, then a status request every 30 s) and the worker's tick stages
9b/9e (answer CAP_REQ, answer SETTINGS_REQ). The harness also lets espnow_task run at every use
of gEspNow and every INFO line, so a reset that moved after the end of the hold
is caught at run time; source rules keep every reset inside the hold. Checks
that no frame accepted during a partner change is wiped, that the worker reaches SYNCED after the
bench's order of events, that a worker which lost its sync record while the
master stayed SYNCED recovers, that espnowbondpeermac and espnowbondmodeenabled
reset the old partner's state (and a save of the same value does not), and that
an unasked CAP_RESP does not restart a manifest transfer.

The bond parts of the espnow_task loop (processMeshHeartbeats: the heartbeat
send with its timeout, and the sync tick from the master and worker branches
to the streaming setup) are pasted as two functions and run at every point
where espnow_task may take the CPU. Checks that while a partner change holds
bond frames off nothing is sent, no session is cleared, no offline event is
posted and no manifest reference is recorded (finding X5, round 3), and that
the tick works again after the hold. cmd_espnow_unpair is compiled with
doubles for its file and registry steps: unpairing the bond partner ends the
bond first (settings, state, offline event, a reply line), after which the
heartbeat starts nothing toward it; unpairing another device leaves the bond
alone; a partner set while bond mode is off is cleared without an event
(finding X4).

Round 4: the sync tick slice also takes the status steps 9j, 9j2 and 9k, and
the hold counter (gBondHoldEpoch, when present) is pasted with the hold. Checks
that the deferred answers (manifest, settings, schema, status) and the status
push and poll wait out a partner change and are not sent to the old partner
after it; that a partner change run while the tick waits (the gap between
stream commands, the FS lock in the manifest lookup) ends that tick's work;
and that a bond file answer queued on cmd_exec (runDeferredBondFileSend) is
dropped when an unpair or partner change ran before it.

--source-dir replays other production sources (for example cca36fa9) against
this suite.
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

HOLD_LINE = 'static std::atomic<uint8_t> gBondPartnerChanging{0};'
HOLD = (
    'static bool bondPartnerChanging()',
    'static void bondWithFramesHeld(Fn&& fn)',
    'static void bondForgetPartnerState()',
)
EPOCH_LINE = 'static std::atomic<uint32_t> gBondHoldEpoch{0};'
EPOCH = ('static uint32_t bondHoldEpoch()', 'static bool bondTickHeld(uint32_t epoch)')
END = 'static void bondEndPartnership()'
FILE_JOB = 'static void runDeferredBondFileSend(void* arg)'
UNPAIR = 'const char* cmd_espnow_unpair(const String& argsInput)'
TICK_CONSTANTS = (r'^static const uint32_t BOND_HEARTBEAT_INTERVAL_MS = \d+;',
                  r'^static const uint32_t BOND_HEARTBEAT_TIMEOUT_MS = \d+;',
                  r'^static const uint32_t BOND_SYNC_RETRY_MS = \d+;')
BASE = (
    'bool isBondPeerMac(const uint8_t* mac)',
    'bool isBondModeOnline()',
    'bool isBondSynced()',
    'static void resetBondSync() ',
    'static void v4h_bond_cap_req(const V4RxCtx& ctx)',
    'static void v4h_bond_cap_resp(const V4RxCtx& ctx)',
    'static void v4h_bond_status_req(const V4RxCtx& ctx)',
)
ASK_LINE = 'static const uint32_t BOND_WORKER_CAP_WAIT_MS = 10000;'
ASK = 'static void bondWorkerAskForCap(uint32_t now)'
KICK = 'static void bondStartHandshakeNow(const uint8_t peerMac[6])'
COMMANDS = (
    'const char* cmd_bond_connect(const String& argsInput)',
    'const char* cmd_bond_disconnect(const String& argsInput)',
)
SETTING_HELPER = 'static const char* bondSettingCommand(const char* jsonKey, const String& args)'
SETTING_COMMANDS = (
    'const char* cmd_espnow_bondmodeenabled(const String& a)',
    'const char* cmd_espnow_bondpeermac(const String& a)',
)
OLD_MACRO = '#define ESPNOW_SETTING_CMD(func, jsonKey)'
OLD_LINES = (
    'ESPNOW_SETTING_CMD(cmd_espnow_bondmodeenabled, "bondModeEnabled")',
    'ESPNOW_SETTING_CMD(cmd_espnow_bondpeermac, "bondPeerMac")',
)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--source-dir', type=Path,
                   help='directory whose System_ESPNow.cpp / System_ESPNow.h replace the component copies')
    a = p.parse_args()
    if not a.cxx:
        p.error('a host C++17 compiler is required')
    sys.path.insert(0, str(a.component / 'test/host'))
    from test_web_batch_handlers import extract_block

    def read(name):
        overlay = a.source_dir / name if a.source_dir else None
        return (overlay if overlay and overlay.exists() else a.component / name).read_text()

    source = read('System_ESPNow.cpp')
    header = read('System_ESPNow.h')

    def definition(text, marker, what):
        count = text.count(marker)
        if count != 1:
            sys.exit(f'FAIL extraction: {what} marker {marker!r} found {count} times, expected 1')
        return extract_block(text, marker)

    have_hold = HOLD_LINE in source
    have_end = END in source
    have_epoch = EPOCH_LINE in source
    have_ask = ASK in source
    have_kick = KICK in source
    have_setting_helper = SETTING_HELPER in source

    pieces = []
    types = [definition(header, 'enum BondSyncRequestType : uint8_t', 'System_ESPNow.h') + ';',
             definition(header, 'struct CapabilitySummary', 'System_ESPNow.h') + ';']
    if have_hold:
        pieces.append(HOLD_LINE)
        for marker in HOLD:
            body = definition(source, marker, 'System_ESPNow.cpp')
            if 'Fn&& fn' in marker:
                body = 'template <typename Fn>\n' + body
                if have_epoch:   # the hold counter comes before the hold
                    pieces.append(EPOCH_LINE)
                    pieces += [definition(source, m, 'System_ESPNow.cpp') for m in EPOCH]
            pieces.append(body)
        if have_end:
            pieces.append(definition(source, END, 'System_ESPNow.cpp'))
    pieces += [definition(source, m, 'System_ESPNow.cpp') for m in BASE]
    if have_ask:
        pieces.append(ASK_LINE)
        pieces.append(definition(source, ASK, 'System_ESPNow.cpp'))
    if have_kick:
        pieces.append(definition(source, KICK, 'System_ESPNow.cpp'))
    pieces += [definition(source, m, 'System_ESPNow.cpp') for m in COMMANDS]
    pieces.append(definition(source, UNPAIR, 'System_ESPNow.cpp'))
    pieces.append(definition(source, FILE_JOB, 'System_ESPNow.cpp'))
    if have_setting_helper:
        pieces.append(definition(source, SETTING_HELPER, 'System_ESPNow.cpp'))
        pieces += [definition(source, m, 'System_ESPNow.cpp') for m in SETTING_COMMANDS]
    else:
        start = source.index(OLD_MACRO)
        end = source.index('\n\n', start)
        pieces.append(source[start:end])
        for line in OLD_LINES:
            if source.count(line) != 1:
                sys.exit(f'FAIL extraction: {line!r}')
            pieces.append(line)

    # The bond parts of processMeshHeartbeats, pasted as two functions of `now`:
    # the heartbeat send and timeout (the ENABLE_BONDED_MODE block after the
    # backup-master step) and the sync tick up to the deferred STREAM_CTRL.
    loop = definition(source, 'void processMeshHeartbeats()', 'System_ESPNow.cpp')
    hb_at = loop.index('// 3. Send periodic paired-mode heartbeat if bonded')
    hb_start = loop.rindex('#if ENABLE_BONDED_MODE\n', 0, hb_at) + len('#if ENABLE_BONDED_MODE\n')
    hb_slice = loop[hb_start:loop.index('#endif // ENABLE_BONDED_MODE', hb_at)]
    sync_at = loop.index('// BOND SYNC TICK (Option B)')
    sync_start = loop.rindex('#if ENABLE_BONDED_MODE\n', 0, sync_at) + len('#if ENABLE_BONDED_MODE\n')
    sync_slice = loop[sync_start:loop.index('// 9f2. Bond: deferred STREAM_CTRL', sync_at)]
    # ... and the status steps 9j, 9j2 and 9k at its end (9f2 to 9i are local).
    status_at = loop.index('// 9j. Bond: deferred status response', sync_at)
    sync_slice += loop[status_at:loop.index('#endif // ENABLE_BONDED_MODE', status_at)]
    constants = []
    for pattern in TICK_CONSTANTS:
        found = re.findall(pattern, source, re.M)
        if len(found) != 1:
            sys.exit(f'FAIL extraction: {pattern!r} found {len(found)} times')
        constants.append(found[0])
    ticks = '\n'.join(constants) + (
        '\n\nstatic void hbTickProduction(uint32_t now) {\n' + hb_slice + '}\n'
        '\nstatic void syncTickProduction(uint32_t now) {\n' + sync_slice + '}\n')

    # What runs on espnow_task in processMeshHeartbeats is pinned in the source:
    # the worker branch of the sync tick asks for a missing capability, and the
    # receive table drops bond opcodes while a partner change holds them.
    if have_ask:
        tick = definition(source, 'void processMeshHeartbeats()', 'System_ESPNow.cpp')
        worker = tick[tick.index('// Worker: just consume stale flags silently'):]
        worker = worker[:worker.index('// POST-SYNC FALLBACK GUARD')]
        assert 'bondWorkerAskForCap(now);' in worker, 'the worker tick does not ask for a missing capability'
    if have_hold:
        dispatch = definition(source, 'static bool v4_dispatch_table_try(', 'System_ESPNow.cpp')
        assert '(!gSettings.bondModeEnabled || bondPartnerChanging())' in dispatch, \
            'the receive table does not drop bond opcodes quietly during a partner change'
        for marker, what in (('const char* cmd_bond_resync(const String& argsInput)', 'bondresync'),
                             ('const char* cmd_bond_role(const String& argsInput)', 'bondrole')):
            body = definition(source, marker, 'System_ESPNow.cpp')
            assert 'bondWithFramesHeld(' in body and body.count('resetBondSync();') == 1, \
                f'{what} resets the sync state outside the frame hold'
            held = body[body.index('bondWithFramesHeld('):]
            assert 'resetBondSync();' in held[:held.index('});')], f'{what}: the reset is not inside the hold'
        # The partner changes: the reset runs before the hold is released (a
        # reset after it is the bench's window, step c1).
        # bonddisconnect (and unpair of the partner) end the bond in one helper.
        disconnect = definition(source, 'const char* cmd_bond_disconnect(const String& argsInput)',
                                'System_ESPNow.cpp')
        if have_end:
            assert 'bondEndPartnership();' in disconnect and 'bondWithFramesHeld(' not in disconnect, \
                'bonddisconnect does not end the bond through bondEndPartnership'
        for marker, what in (('const char* cmd_bond_connect(const String& argsInput)', 'bondconnect'),
                             (END if have_end else 'const char* cmd_bond_disconnect(const String& argsInput)',
                              'bondEndPartnership' if have_end else 'bonddisconnect'),
                             (SETTING_HELPER, 'espnowbondmodeenabled / espnowbondpeermac')):
            body = definition(source, marker, 'System_ESPNow.cpp')
            assert body.count('bondWithFramesHeld(') == 1 and body.count('bondForgetPartnerState();') == 1, \
                f'{what}: expected one frame hold and one partner state reset'
            held = body[body.index('bondWithFramesHeld('):]
            assert 'bondForgetPartnerState();' in held[:held.index('});')], \
                f'{what}: the partner state reset is not inside the frame hold'
        assert source.count('ESPNOW_SETTING_CMD(cmd_espnow_bondmodeenabled') == 0 and \
            source.count('ESPNOW_SETTING_CMD(cmd_espnow_bondpeermac') == 0, \
            'a bond setting command still bypasses the partner change'

    harness = (HERE / 'espnow_bond_partner_change_harness.cpp').read_text()
    for tag, value in (('// INSERT_TYPES', '\n\n'.join(types)),
                       ('// INSERT_PRODUCTION', '\n\n'.join(pieces)),
                       ('// INSERT_TICKS', ticks)):
        assert harness.count(tag) == 1, tag
        harness = harness.replace(tag, value)

    print(f'frame hold: {"present" if have_hold else "absent"}; '
          f'worker capability request: {"present" if have_ask else "absent"}; '
          f'bondconnect handshake kick: {"present" if have_kick else "absent"}; '
          f'bond setting commands: {"held" if have_setting_helper else "generic"}; '
          f'bond end helper: {"present" if have_end else "absent"}; '
          f'hold counter: {"present" if have_epoch else "absent"}', flush=True)
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-bond-partner-') as tmp:
        unit, exe = Path(tmp) / 'espnow_bond_partner_change.cpp', Path(tmp) / 'espnow_bond_partner_change'
        unit.write_text(harness)
        shutil.copy(a.component / 'System_MacText.h', Path(tmp) / 'System_MacText.h')
        # The pasted commands keep parameters this harness does not use (the
        # firmware builds without -Wextra).
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic', '-Wno-unused-function',
               '-Wno-unused-parameter',
               f'-DHW1_HAVE_HOLD={1 if have_hold else 0}',
               f'-DHW1_HAVE_ASK={1 if have_ask else 0}',
               f'-DHW1_HAVE_KICK={1 if have_kick else 0}',
               '-I', tmp, str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                        '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True, timeout=240)
        result = subprocess.run([str(exe)], timeout=60)
    if result.returncode:
        print('FAIL espnow_bond_partner_change: checks reported failures', file=sys.stderr)
        sys.exit(1)
    print('PASS espnow_bond_partner_change')


if __name__ == '__main__':
    main()
