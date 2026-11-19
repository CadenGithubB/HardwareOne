#!/usr/bin/env python3
"""Run the G2 one-temple repair as shipped: schedule, queue, finish, nudges, status.

G2_RepairCore.h is compiled as is. The heartbeat tick, the queue path, the
setup worker's finish hook, the wear/gesture nudge, the status text, the
reconnect ladder's admission and g2recover are extracted from G2_Glasses.cpp;
the clock, locks, links, peer intent and the setup worker are host fakes. The
source checks pin the call sites the harness cannot reach: the wear edge, the
three lens event parsers, the status line, the SSE and the attempt line.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require
from g2_client_source import g2_client_text

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def last_block(text: str, marker: str) -> str:
    """The definition when a forward declaration comes first."""
    return extract_block(text[text.rindex(marker):], marker)


def line(text: str, prefix: str) -> str:
    start = text.index(prefix)
    return text[start:text.index('\n', start)]


def generated(component: Path) -> str:
    owner = g2_client_text(component)
    header = (component / 'G2_Glasses.h').read_text()
    peers = (component / 'BLE_Peers.h').read_text()
    types = [extract_block(peers, 'enum BlePeerKind :') + ';',
             extract_block(peers, 'enum class BlePeerConnectAdmission') + ';',
             line(peers, 'constexpr size_t BLE_PEER_ADDRESS_TEXT_CAPACITY'),
             line(peers, 'constexpr uint8_t BLE_PEER_DEFAULT_ADDRESS_TYPE'),
             *(extract_block(peers, marker) + ';' for marker in (
                 'struct BlePeerSavedTarget ', 'struct BlePeerConnectRequest ',
                 'struct BlePeerReconnectSnapshot ', 'struct BlePeerSavedTargetSnapshot ')),
             extract_block(header, 'enum class BleConnectKind') + ';',
             extract_block(header, 'struct BleConnectJob') + ';',
             extract_block(owner, 'struct G2LinkSnapshot') + ';',
             line(owner, 'static g2repair::Schedule gRepair;'),
             line(owner, 'static portMUX_TYPE gRecoveryMux')]
    production = [extract_block(owner, marker) for marker in (
        'static void copyBleAddressText(char (&dst)[BLE_PEER_ADDRESS_TEXT_CAPACITY],',
        'static void endRepairEpisode() ', 'static void g2RepairNudge(bool gesture) ',
        'static void g2RepairNoteLensInput(uint32_t etype) ', 'static const char* g2RepairDescribe(')]
    production.append(line(owner, 'enum class RepairQueue'))
    # The renderer watchdog beatOne applies once its arm gate passes.
    production.append(line(owner, 'static constexpr uint32_t HEARTBEAT_DEAD_THRESHOLD'))
    production.append(extract_block(owner, 'static bool g2RendererHeartbeatAcceptedState('))
    production += [extract_block(owner, marker) for marker in (
        'static bool repairIntentReady(', 'static bool repairSurvivorSaved(', 'static bool repairHeldNow(',
        'static RepairQueue queueMissingArmRecovery(', 'static RepairQueue noteRepairQueued(',
        'static void missingArmRecoveryComplete(', 'static void g2RepairPushIfChanged(',
        'static void recoveryHeartbeatTick() ',
        'static BlePeerConnectAdmission g2PeerConnectSavedAdmissionThunk(\n    const BlePeerConnectRequest& request) ',
        'static const char* cmd_g2recover(')]
    harness = (HERE / 'g2_repair_schedule_harness.cpp').read_text()
    return harness.replace('// INSERT_TYPES', '\n'.join(types)).replace(
        '// INSERT_PRODUCTION', '\n\n'.join(production))


def order(text: str, *needles: str) -> None:
    at = -1
    for needle in needles:
        found = text.find(needle, at + 1)
        require(found > at, f'out of order or missing: {needle}')
        at = found


def check_sources(component: Path) -> None:
    owner = g2_client_text(component)
    core = (component / 'G2_RepairCore.h').read_text()
    require('#include' in core and 'Arduino' not in core and 'freertos' not in core.lower(),
            'the schedule has no OS dependencies')
    require(re.search(r'#include "G2_RepairCore\.h"', owner) is not None, 'G2_Glasses.cpp includes the schedule')
    # The old cap state is gone (its 0 sentinel restarted every episode).
    for old in ('gNextRecoveryAttemptMs', 'gRecoveryAttemptCount', 'kMaxRecoveryAttempts',
                'gRecoveryGiveUpLogged', 'resetRecoveryBackoff'):
        require(old not in owner, f'old repair state remains: {old}')
    # The wear edge: the surviving temple put back on.
    activity = last_block(owner, 'static bool g2NoteEvenCoreRendererActivity(')
    order(activity, 'if (wearResumed) {', 'systemEventPost(SYSEVT_G2_PLUGIN_ALIVE', 'g2RepairNudge(/*gesture=*/false);')
    # Each lens event parser hears the gesture before any branch can return.
    dev = extract_block(owner, 'static void handleDevEvent(')
    for event in ('ListEvent: container', 'TextEvent: container', 'SysEvent: type'):
        section = dev[dev.index(event):]
        require(section.index('g2RepairNoteLensInput(etype);') < min(
            section.index(word) if word in section else len(section) for word in ('continue;', 'return;', 'break;')),
            f'{event} reports the gesture first')
    require(dev.count('g2RepairNoteLensInput(etype);') == 3, 'three lens event parsers')
    # The worker's run passes the fast try (0 = a 10-minute retry); its finish reports.
    order(owner, 'case BleConnectKind::G2_REPAIR: {', 'const uint8_t attempt = gRepair.fastTry();',
          'sG2RunOk = attemptMissingArmRecoverySync(attempt, job);')
    finish = extract_block(owner, 'static void g2SetupFinish(')
    order(finish, 'missingArmRecoveryComplete(info.end == hw1setup::End::Ran && sG2RunOk);', 'g2ConnectRelease();')
    attempt = extract_block(owner, 'static bool attemptMissingArmRecoverySync(')
    require('"attempt %u/%u"' in attempt and '"10-minute retry"' in attempt and
            'g2repair::kFastTries' in attempt, 'the attempt line names the fast try or the slow retry')
    # Status: g2status, the web JSON (via the snapshot) and the SSE.
    fmt = extract_block(owner, 'void formatG2Status(')
    require('repair=%s' in fmt and 'status.repair' in fmt, 'g2status prints the repair')
    snapshot = extract_block(owner, 'bool g2ConnectionStatusSnapshot(')
    require('g2RepairDescribe(' in snapshot, 'the snapshot carries the repair')
    sse = extract_block(owner, 'static void g2PushStatusEvent(const char* reason) ')
    order(sse, '.kv("rp"', 'g2RepairDescribe(gL.connected, gR.connected, repair)', '.kv("w"')
    web = (component / 'WebPage_Bluetooth.cpp').read_text()
    require('link["repair"] = connection.repair;' in web, '/api/ble/status carries the repair')
    # The heartbeat drives the tick.
    require(owner.count('recoveryHeartbeatTick();') == 1, 'one heartbeat call site')
    check_arm_liveness(owner)
    check_reconnect_path(owner)


def check_arm_liveness(owner: str) -> None:
    """(a) LEFT E0 silence never counts; RIGHT drives pluginDead and PLUGIN_SILENT."""
    beat = extract_block(owner, 'static void beatOne(G2Temple& t) ')
    # The TX-wedge rule (LEFT's health) runs first, then the arm gate, then the watchdog.
    order(beat, 'G2_TX_STUCK_DISCONNECT_BEATS', 'disconnectTemple(t);',
          'if (!g2repair::rendererSilenceCounts(t.side)) return;',
          'g2RendererHeartbeatAcceptedState(', 'plugin silent', 'SYSEVT_G2_PLUGIN_SILENT')
    require('firmwareRoutesEvenHubRepliesToRight' not in beat,
            'the firmware allowlist no longer gates LEFT silence')
    silent = beat[beat.index('BROADCAST_PRINTF("[G2] %s temple plugin silent'):]
    silent = silent[:silent.index(';')]
    require('not worn' not in silent and 'no heartbeat reply' in silent,
            'the silent line says what it measures, not "not worn"')
    # Nothing else marks a temple's plugin dead.
    require(re.search(r'pluginDead\s*=\s*true', owner) is None, 'pluginDead is set only by the watchdog')
    require(owner.count('g2RendererHeartbeatAcceptedState(\n        t.heartbeatMissed') == 1,
            'one production watchdog call site (beatOne)')


def check_reconnect_path(owner: str) -> None:
    """(b) saved-address direct reconnect at 25 s, 30% duty scan fallback; (c) 0x02."""
    attempt = extract_block(owner, 'static bool attemptMissingArmRecoverySync(')
    order(attempt, '__atomic_exchange_n(&missing->hostDesyncSuspected, false',
          'g2repair::tryPath(', 'bleSetupOtherReadyQueued(BleSetupClient::G2)',
          'if (path == g2repair::TryPath::Defer) {', 'gRepair.deferred();',
          'if (path == g2repair::TryPath::BackOff) {',
          'if (path == g2repair::TryPath::Direct) {', 'missing->directConnectArmed = true;',
          '} else {', 'gScan->setInterval(g2repair::kScanIntervalMs);',
          'gScan->setWindow(g2repair::kScanWindowMs);',
          'g2repair::kScanIntervalUnits, g2repair::kScanWindowUnits',
          'connectTemple(*missing, hw1coex::Purpose::Repair)')
    # The Defer and BackOff paths return before any scan or connect.
    for path in ('Defer', 'BackOff'):
        section = attempt[attempt.index(f'if (path == g2repair::TryPath::{path}) {{'):]
        section = section[:section.index('\n  }\n')]
        require('return false;' in section and 'gScan' not in section and 'connectTemple' not in section,
                f'the {path} path neither scans nor connects')
    require('setWindow(99)' not in attempt and 'setInterval(100)' not in attempt, 'no ~99% duty repair scan')
    full = extract_block(owner, 'static bool g2ConnectSync(')
    order(full, 'gScan->setActiveScan(true);', 'gScan->setInterval(100);', 'gScan->setWindow(99);',
          'bleCoexScanBegin(hw1coex::Peer::Glasses, connectPurpose, 160, 158,', 'gScan->start(scanSec,')
    require(attempt.count('gScan->start(') == 1, 'one scan, on the no-saved-address path only')
    direct = attempt[attempt.index('if (path == g2repair::TryPath::Direct) {'):]
    direct = direct[:direct.index('} else {')]
    require('gScan' not in direct and 'missing->deviceAddress = savedMac;' in direct and
            'g2repair::savedAddressType(' in direct, 'the direct path dials the saved address, no scan')
    connect = extract_block(owner, 'static bool connectTemple(G2Temple& t, hw1coex::Purpose purpose) ')
    order(connect, 'const bool direct = t.directConnectArmed;', 't.directConnectArmed = false;',
          'bleConnectWatchedAddress(t.client, t.deviceAddress, t.deviceAddressType,',
          'g2repair::kDirectConnectTimeoutMs',
          'gRepairDirectBusy = g2repair::directRefusedBusy(linked, directElapsedMs);',
          'g2repair::countsTowardOpenFailStreak(direct, directElapsedMs)', '++gRepairOpenFailStreak;')
    watched = extract_block(owner, 'static bool bleConnectWatchedAddress(')
    order(watched, 'bleCoexConnectBegin(', 'client->connect(target, addressType, timeoutMs)',
          'bleNoteClientConnectOutcome(client, ok, elapsed, logTag)')
    quarantine = extract_block(owner, 'bool bleQuarantineClientAfterFailedConnect(')
    require('!g2repair::openTimeoutWasOwnBound(connectBoundMs)' in quarantine,
            'an OPEN wait our own 25 s bound ended is an ordinary miss, not a host wedge')
    require('const uint32_t connectBoundMs = direct ? g2repair::kDirectConnectTimeoutMs : 35000;' in connect
            and '(uint8_t)BLE_PEER_G2_GLASSES, connTag, connectBoundMs);' in connect,
            'connectTemple passes its own connect bound to the quarantine')
    # (c) 0x02: named, logged as a desync, and marks the arm.
    names = extract_block(owner, 'static const char* g2DisconnectReasonName(')
    require('case 0x02:   return "host/controller desync";' in names, '0x02 is named')
    down = extract_block(owner, 'static G2TempleDownTransition g2BeginTempleDown(')
    order(down, 'g2repair::isHostDesyncReason(hciReason)',
          '__atomic_store_n(&temple.hostDesyncSuspected, hostDesync', 'if (hostDesync) {',
          'host/controller desync')
    # A new link clears the mark.
    require('__atomic_store_n(&t.hostDesyncSuspected, false, __ATOMIC_RELEASE);' in owner,
            'a promoted link clears the desync mark')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('c++'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--component', type=Path, help='a copy of components/hardwareone to read production from')
    args = parser.parse_args()
    component = (args.component or COMPONENT).resolve()
    require(bool(args.cxx), 'a host C++17 compiler is required')
    flags = ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic', '-Wno-unused-function']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer', '-g']
    with tempfile.TemporaryDirectory(prefix='hw1-g2-repair-') as directory:
        source = Path(directory) / 'repair.cpp'
        binary = Path(directory) / 'repair'
        source.write_text(generated(component))
        subprocess.run([args.cxx, *flags, '-I', str(component), str(source), '-o', str(binary)],
                       check=True, timeout=180)
        print(subprocess.check_output([str(binary)], text=True, timeout=120).strip())
    check_sources(component)
    print('G2 repair: fast tries then every 10 min, no restart at the cap, wear/gesture restart, '
          'held vs busy, ladder deferral, status text, saved-address direct reconnect (25 s), '
          '30% duty scan fallback, 0x02 desync, LEFT silence never counts')


if __name__ == '__main__':
    main()
