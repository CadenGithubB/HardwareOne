#!/usr/bin/env python3
"""Pin the coexistence-record hooks to their places in the G2 and ring code.

The record logic itself runs in `ble_coex_records`; G2_Glasses.cpp and
G2_Ring.cpp are too large to compile on the host, so this checks the production
source order: a scan record starts after the epoch is claimed and before the
radio scan starts, ends after the callbacks quiesce, and every connect is
bracketed through the shared chokepoint.
"""
from pathlib import Path
import re
from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def order(text: str, *needles: str) -> None:
    at = -1
    for needle in needles:
        found = text.find(needle, at + 1)
        assert found > at, f'{needle!r} missing or out of order after position {at}'
        at = found


def no_return_between(text: str, begin: str, end: str) -> None:
    """A Begin always reaches its End: no early exit between them."""
    first = text.index(begin)
    span = text[first:text.index(end, first)]
    assert 'return' not in span and 'goto' not in span, f'an exit between {begin!r} and {end!r}'


if __name__ == '__main__':
    g2 = (ROOT / 'G2_Glasses.cpp').read_text()
    ring = (ROOT / 'G2_Ring.cpp').read_text()

    body = extract_block(g2, 'static bool g2ConnectSync(')
    order(body, 'g2ScanCallbackBegin()', 'if (scanEpoch == 0)', 'bleCoexScanBegin(', 'gScan->start(',
          'g2ScanCallbackEnd(scanEpoch)', 'bleCoexScanEnd(', 'g2ScanClearMacContext()')
    assert body.count('bleCoexScanBegin(') == 1 and body.count('bleCoexScanEnd(') == 1
    no_return_between(body, 'bleCoexScanBegin(', 'bleCoexScanEnd(')
    order(body, 'const hw1coex::Purpose connectPurpose', 'bleCoexScanBegin(hw1coex::Peer::Glasses, connectPurpose',
          'connectTemple(gL, connectPurpose)', 'connectTemple(gR, connectPurpose)')

    body = extract_block(g2, 'static bool attemptMissingArmRecoverySync(')
    order(body, 'g2ScanCallbackBegin()', 'if (scanEpoch == 0)', 'bleCoexScanBegin(', 'gScan->start(',
          'g2ScanCallbackEnd(scanEpoch)', 'bleCoexScanEnd(', 'g2ScanClearMacContext()')
    assert body.count('hw1coex::Purpose::Repair') == 2, 'the repair scan and its connect'
    no_return_between(body, 'bleCoexScanBegin(', 'bleCoexScanEnd(')
    order(body, 'bleCoexScanEnd(', 'connectTemple(*missing, hw1coex::Purpose::Repair)')

    body = extract_block(g2, 'void g2GapEventHandler(esp_gap_ble_cb_event_t event,\n')
    order(body, 'bleCoexOnGapEvent(', 'if (event != ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT')

    body = extract_block(g2, 'class G2ScanCallbacks')
    order(body, 'callbackClaim.current()', 'scanTarget == G2_EYE_LEFT', 'bleCoexOnScanMatch(', 'if (t.advertisedDevice)')

    body = extract_block(g2, 'bool bleConnectWatched(')
    order(body, 'bleCoexConnectBegin(', 'purpose, target.getNative())', 'connectTimeout(', 'bleNoteClientConnectOutcome(')
    no_return_between(body, 'bleCoexConnectBegin(', 'bleNoteClientConnectOutcome(')
    assert 'bleConnectWatched(t.client, t.advertisedDevice, 35000, connTag, purpose)' in extract_block(
        g2, 'static bool connectTemple(G2Temple& t, hw1coex::Purpose purpose) ')
    body = extract_block(g2, 'void bleNoteClientConnectOutcome(')
    assert body.count('bleCoexConnectEnd(') == 2, 'both outcome paths close the connect record'
    order(body, 'getLastConnectOutcome()', 'bleCoexConnectEnd(ok, uint8_t(detail.stage), detail.gattStatus')

    # The shared client stack (BLE_CentralStack.cpp) installs the G2 GAP
    # handler and the observer on the host it brings up for every device.
    body = extract_block((ROOT / 'BLE_CentralStack.cpp').read_text(), 'bool bleStackUp()')
    order(body, 'setCustomGapHandler(g2GapEventHandler)', 'bleCoexInstallGattcObserver()')

    body = extract_block(ring, 'bool g2RingScan(')
    order(body, 'bleCentralRingShouldYield()', 'ringScanCallbackBegin(', 'if (scanEpoch == 0)', 'bleCoexScanBegin(',
          'scan->start(', 'ringScanCallbackEnd(scanEpoch)', 'bleCoexScanEnd(', 'if (!scanQuiesced)')
    no_return_between(body, 'bleCoexScanBegin(', 'bleCoexScanEnd(')

    body = extract_block(ring, 'class RingScanCallbacks')
    order(body, "if (hex != 6", 'bleCoexOnScanMatch(', 'if (gRingAdvertisedDevice) return;')

    body = extract_block(ring, 'bool ringPerformConnect(')
    order(body, 'bleCoexConnectBegin(hw1coex::Peer::Ring, hw1coex::Purpose::Saved, addr.getNative())',
          'connect(addr, addrType, 35000)',
          'bleCoexConnectBegin(hw1coex::Peer::Ring, hw1coex::Purpose::Discovery, target.getNative())',
          'connectTimeout(gRingAdvertisedDevice, 35000)', 'bleNoteClientConnectOutcome(gRing.client')
    no_return_between(body, 'bleCoexConnectBegin(', 'bleNoteClientConnectOutcome(gRing.client')

    # Records and their log lines carry no addresses or names: the record
    # types have no address fields, and the formatters print only record fields.
    records = (ROOT / 'BLE_CoexRecords.cpp').read_text()
    for banned in ('getAddress', 'toString', 'remote_bda[0]', 'gConnectTarget[0]', 'ssid', 'SSID'):
        assert banned not in records.replace('remote_bda, 6', ''), banned
    header = (ROOT / 'BLE_CoexRecords.h').read_text()
    for record in ('struct ScanRecord', 'struct ConnectRecord', 'struct WifiSpan'):
        fields = re.sub(r'//[^\n]*', '', extract_block(header, record))
        for banned in ('address', 'bda', 'mac', 'ssid', 'name'):
            assert banned not in fields.lower(), f'{record} holds {banned}'
    pipe = (ROOT / 'BLE_PhonePipe.cpp').read_text()
    for note in ('hw1coex::ScanRecord scanRecord(', 'void noteAttempt('):
        body = extract_block(pipe, note)
        assert 'target' not in body and 'address' not in body, f'{note} copies an address'
    # A user disconnect holds the pipe while it bumps the intent, so the pipe
    # task cannot record the stop as "replaced".
    body = extract_block(pipe, 'void disconnect() ')
    order(body, 'Lock lock;', 'blePeerNoteUserDisconnect(BLE_PEER_PHONE);', 'CauseDetail::Stopped')
    # The cancel is decided before the adapter's events are consumed. No
    # cancel names another client's setup: the phone's setup runs to its end.
    body = extract_block(pipe, 'void tickLocked(')
    order(body, 'CauseDetail::Expired', 'phoneGatt.poll(now,pressure,displayBusy);')
    assert 'bleCentralAccessoryPending' not in pipe and 'Yielded' not in pipe
    assert 'Yielded' not in header
    # Glasses and ring connect records carry the job's wait for other setups
    # on the setup worker: logged, set before the job runs, cleared after
    # (test_ble_setup_worker runs runRecorded and checks which clients record).
    records_runtime = records.split('#if ENABLE_BLE_COEX_RECORDS\n', 1)[1].split('\n#else\n', 1)[0]
    assert 'r.waitMs = gAccessoryWaitMs;' in extract_block(records_runtime, 'void bleCoexConnectEnd(')
    core = (ROOT / 'BLE_SetupWorkerCore.h').read_text()
    recorded = extract_block(core, 'End runRecorded(')
    order(recorded, 'hooks.logSetupWait(label, waited);', 'hooks.setAccessoryWait(waited);', 'run();',
          'hooks.clearAccessoryWait();')
    no_return_between(recorded, 'hooks.setAccessoryWait(waited);', 'hooks.clearAccessoryWait();')
    worker = (ROOT / 'BLE_SetupWorker.cpp').read_text()
    assert 'hs::runRecorded(hooks, o->label(t), o->recordWait, info' in worker
    # The phone gives its turn back only after its route stops claiming scan
    # events, so the next client's scan is never swallowed.
    body = extract_block(pipe, 'void leaveSetup() ')
    order(body, 'routeSet();', 'turn.ended.store(heldToken);', 'bleSetupWake();')
    body = extract_block(pipe, 'void cleanupExpired(')
    order(body, 'bleCentralRecoveryBlock(true);', 'endTurn();')
    # The Wi-Fi mirror reads Arduino's event list: no default event loop at boot.
    wifi = (ROOT / 'System_WiFi.cpp').read_text()
    install = extract_block(wifi, 'bool wifiCoexMirrorInstall()')
    assert 'WiFi.onEvent(wifiCoexEvent)' in install and 'esp_event' not in install
    print('BLE coexistence hooks: G2 full, saved and repair scans, ring scans, temple and ring connects, observer install')
