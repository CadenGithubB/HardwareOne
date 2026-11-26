#!/usr/bin/env python3
"""The ring's direct connect bound, its failed-connect handling and its retry wiring.

ring_connect_bound_harness.cpp runs the production RING_DIRECT_CONNECT_TIMEOUT_MS,
its static_assert and ringSettleClientAfterFailedConnect (G2_Ring.cpp) over a
fake BLEClient outcome: 25 s, under Bluedroid's 30 s link timer
(CONFIG_BT_BLE_ESTAB_LINK_CONN_TOUT); a clean open-timeout gives the link slot
back without the shared quarantine (which would arm a whole-stack recycle and
take the temples down). The static_assert must refuse a timer at or under the
bound, and every build must keep the timer above it.

The text checks pin what the unit cannot run: both connect calls use the
constant, a failed link touches no glasses state, the ring asks for no
connection priority, and the faceclaw backoff (2 s doubling to 60 s, counted
from the failure, reset by a finished connect) is wired into the failure
paths, the completion and the scheduler admission. ring_phone_yield_queue_harness
runs that backoff through the real admission (test_ring_phone_yield.py).
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def one_line(text: str, prefix: str) -> str:
    hits = [l for l in text.splitlines() if l.startswith(prefix)]
    assert len(hits) == 1, (prefix, hits)
    return hits[0]


def production(ring: str) -> str:
    guard = ring[ring.index('#if defined(CONFIG_BT_BLE_ESTAB_LINK_CONN_TOUT)'):]
    guard = guard[:guard.index('#endif') + len('#endif')]
    return '\n'.join([
        one_line(ring, 'static constexpr uint32_t RING_DIRECT_CONNECT_TIMEOUT_MS'),
        guard,
        extract_block(ring, 'static void ringSettleClientAfterFailedConnect() '),
    ])


def check_text(component: Path) -> None:
    ring = (component / 'G2_Ring.cpp').read_text()
    assert one_line(ring, 'static constexpr uint32_t RING_DIRECT_CONNECT_TIMEOUT_MS') == \
        'static constexpr uint32_t RING_DIRECT_CONNECT_TIMEOUT_MS = 25000;'
    perform = extract_block(ring, 'bool ringPerformConnect(const String& savedMac')
    # Both overloads take the bound, and no other connect bound is spelled out.
    assert 'gRing.client->connect(addr, addrType, RING_DIRECT_CONNECT_TIMEOUT_MS);' in perform
    assert re.search(r'gRing\.client->connectTimeout\(gRingAdvertisedDevice,\s*RING_DIRECT_CONNECT_TIMEOUT_MS\);',
                     perform)
    for call in re.finditer(r'->connect(?:Timeout)?\(([^;]*)\);', perform):
        assert 'RING_DIRECT_CONNECT_TIMEOUT_MS' in call.group(1), call.group(0)
    assert '35000' not in ring, 'the old 35 s bound is gone'
    # A failed link: report, start the hold, release or quarantine the client.
    # Nothing on that path names a temple or the glasses.
    failed = perform[perform.index('if (!connOk) {'):]
    failed = failed[:failed.index('return false;')]
    assert failed.index('ringNoteConnectFailure("link", dt);') < failed.index('ringSettleClientAfterFailedConnect();')
    assert not re.search(r'\bg2[A-Z]|\bgL\b|\bgR\b|G2_|Temple', failed), failed
    assert 'bleQuarantineClientAfterFailedConnect' not in perform
    assert ring.count('bleQuarantineClientAfterFailedConnect(') == 1  # only inside the helper
    # The ring link asks for no connection priority (faceclaw: HIGH's 5 s
    # supervision timeout killed the ring); only the glasses are lowered to
    # BALANCED while it connects.
    for forbidden in ('esp_ble_gap_update_conn_params', 'setConnPriority', 'CONNECTION_PRIORITY_HIGH',
                      'g2SetAllTemplesConnPriority('):
        assert forbidden not in ring.replace('// g2SetAllTemplesConnPriority', ''), forbidden
    guard = extract_block(ring, 'struct GlassesPriorityGuard ')
    assert 'g2ConnPriRequestBalanced("ring-connect")' in guard and 'g2ConnPriReleaseBalanced("ring-connect")' in guard
    # The faceclaw backoff: constants, the failure note, the watchdog's event
    # that counts no attempt, the reset at a finished connect, the admission.
    assert one_line(ring, 'static constexpr uint32_t RING_RETRY_BASE_MS') == \
        'static constexpr uint32_t RING_RETRY_BASE_MS = 2000;'
    assert one_line(ring, 'static constexpr uint32_t RING_RETRY_MAX_MS') == \
        'static constexpr uint32_t RING_RETRY_MAX_MS = 60000;'
    note = extract_block(ring, 'static void ringNoteConnectFailure(const char* reason, uint32_t elapsedMs) ')
    assert 'ringRetryNoteFailure(millis());' in note and 'ringPostConnectFailureEvent(reason, elapsedMs);' in note
    gate = extract_block(ring, 'static bool ringConnectGateOpen(uint32_t* generationOut) ')
    assert 'ringPostConnectFailureEvent("stuck"' in gate and 'ringNoteConnectFailure' not in gate
    completion = perform[perform.index('if (completionCurrent) {'):]
    assert completion.index('ringRetryNoteConnected();') < completion.index('INFO_RINGF("Connected to %s')
    assert ring.count('ringRetryNoteConnected();') == 1
    thunk = extract_block(ring, 'static BlePeerConnectAdmission ringPeerConnectSavedAdmissionThunk(\n'
                                '    const BlePeerConnectRequest& request) ')
    hold = thunk.index('if (ringConnectInFlightNormal(nowMs) || ringRetryHoldRemainingMs(nowMs) > 0) {')
    assert thunk.index('bleCentralRingShouldYield()') < hold < thunk.index('g2RingInit()') < \
        thunk.index('ringConnectGateOpen(&cancelGeneration)')
    assert thunk[hold:].split('}', 1)[0].count('return BlePeerConnectAdmission::BUSY;') == 1
    # Manual paths are never held.
    for manual in ('bool g2RingConnect() ', 'bool g2RingConnectSaved() ', 'bool g2RingConnectMac(const String& mac) '):
        body = extract_block(ring, manual)
        assert 'ringRetryHoldRemainingMs' not in body and 'ringConnectInFlightNormal' not in body, manual


def check_sdkconfigs(component: Path) -> None:
    """Every build keeps Bluedroid's link timer above the ring bound.

    No defaults layer sets the timer, so builds take ESP-IDF's Kconfig default;
    any layer that sets it, and any local build folder, must stay above the bound.
    """
    repo = component.parents[1]
    key = 'CONFIG_BT_BLE_ESTAB_LINK_CONN_TOUT'
    layers = [*sorted((repo / 'config').glob('sdkconfig*.defaults*')),
              *sorted((repo / 'boards').glob('*.defaults')),
              *sorted(repo.glob('deployments/*/boards/*/sdkconfig.defaults')),
              *sorted(repo.glob('build-config-*/sdkconfig'))]
    for path in layers:
        match = re.search(rf'^{key}=(\d+)$', path.read_text(), re.M)
        if match:
            assert int(match[1]) * 1000 > 25000, (path, match[0])
    idf = Path(os.environ.get('IDF_PATH') or Path.home() / 'esp' / 'esp-idf')
    kconfig = idf / 'components' / 'bt' / 'host' / 'bluedroid' / 'Kconfig.in'
    if kconfig.exists():
        block = kconfig.read_text().split('config BT_BLE_ESTAB_LINK_CONN_TOUT', 1)[1]
        default = re.search(r'^\s*default (\d+)$', block, re.M)
        assert default and int(default[1]) * 1000 > 25000, (kconfig, default)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('c++'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--component', type=Path, default=COMPONENT,
                        help='a copy of components/hardwareone to read production from')
    args = parser.parse_args()
    component = args.component.resolve()
    ring = (component / 'G2_Ring.cpp').read_text()
    harness = (HERE / 'ring_connect_bound_harness.cpp').read_text()
    assert harness.count('// INSERT_PRODUCTION') == 1
    unit = harness.replace('// INSERT_PRODUCTION', production(ring))
    with tempfile.TemporaryDirectory(prefix='hw1-ring-connect-bound-') as tmp_name:
        tmp = Path(tmp_name)
        cpp, binary = tmp / 'bound.cpp', tmp / 'bound'
        cpp.write_text(unit)
        base = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror']
        if args.sanitize:
            base += ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-g']
        subprocess.run([*base, str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=60)
        # The static_assert refuses a Bluedroid timer at or under the bound.
        for seconds in (25, 20):
            refused = subprocess.run([*base, f'-DCONFIG_BT_BLE_ESTAB_LINK_CONN_TOUT={seconds}', '-fsyntax-only',
                                      str(cpp)], capture_output=True, text=True)
            assert refused.returncode != 0 and "Bluedroid's own link timer" in refused.stderr, seconds
    check_text(component)
    check_sdkconfigs(component)
    print('Ring connect bound text: both connect calls bounded at 25 s, a failed link touches no glasses '
          'state, no ring connection priority, faceclaw backoff wired; sdkconfigs keep the 30 s timer above it')
