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

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def last_block(text: str, marker: str) -> str:
    """The definition when a forward declaration comes first."""
    return extract_block(text[text.rindex(marker):], marker)


def line(text: str, prefix: str) -> str:
    start = text.index(prefix)
    return text[start:text.index('\n', start)]


def generated(component: Path) -> str:
    owner = (component / 'G2_Glasses.cpp').read_text()
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
    owner = (component / 'G2_Glasses.cpp').read_text()
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
    order(activity, 'if (wearResumed) {', 'systemEventPost(SYSEVT_G2_WORN', 'g2RepairNudge(/*gesture=*/false);')
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
          'held vs busy, ladder deferral, status text')


if __name__ == '__main__':
    main()
