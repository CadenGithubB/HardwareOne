#!/usr/bin/env python3
"""Run the ring and glasses clients on the one BLE setup worker, as shipped.

Four units, each built from production text on every run:

* ring_phone_yield_queue_harness.cpp runs the real worker (BLE_SetupWorker.cpp
  over BLE_SetupWorkerCore.h) on its own thread with a virtual clock, the real
  glasses / ring / ring-retirement ops and submit paths, the ring admission
  and submitters, g2Disconnect, g2RingDisconnect, g2RingYieldToPhonePipe and
  g2RingRetireForPhonePipe (G2_Glasses.cpp, G2_Ring.cpp), against the real
  BLE_CentralCoordinator.cpp. The phone is a fake client on the real
  runPhoneTurn.
* ring_phone_yield_harness.cpp runs g2RingYieldToPhonePipe and
  g2RingRetireForPhonePipe with the pipe compiled in and out.
* ring_scan_poll_harness.cpp runs the polled g2RingScan with a fake BLEScan.
* ring_owner_lifetime_harness.cpp runs the real ringOwnerTask on a thread with
  ringWakeOwner, ringOwnerEnsure and g2RingOwnerRelease (G2_Ring.cpp) over a
  faked FreeRTOS: the owner is released only while the link is down, nulls its
  own handle before deleting itself, and is re-created by ringOwnerEnsure.

The text checks after them cover what the units cannot run: the ring
commands, g2RingPrepareForStackTeardown, deinitG2Client, the C6 link limit
(docs/BLE_SETUP_WORKER_PLAN.md), and the owner-lifetime rules g2RingInit and
the status readers must keep (no task-handle wake or stack read).

--component points every production read at another copy of
components/hardwareone (mutation runs); the harnesses come from this folder.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent


def between(text: str, start: str, end: str) -> str:
    """From `start` (inclusive) to the next `end` (exclusive); both must exist."""
    assert text.count(start) == 1, start
    i = text.index(start)
    return text[i:text.index(end, i)]


def line(text: str, prefix: str) -> str:
    hits = [l for l in text.splitlines() if l.startswith(prefix)]
    assert len(hits) == 1, (prefix, hits)
    return hits[0]


def block(text: str, marker: str) -> str:
    return extract_block(text, marker)


def strip_includes(text: str) -> str:
    return '\n'.join(l for l in text.splitlines()
                     if not l.lstrip().startswith('#include') and l.strip() != '#pragma once')


def insert(harness: str, parts: dict) -> str:
    for marker, text in parts.items():
        assert harness.count(marker) == 1, marker
        harness = harness.replace(marker, text)
    return harness


def types(component: Path) -> str:
    glasses_h = (component / 'G2_Glasses.h').read_text()
    peers = (component / 'BLE_Peers.h').read_text()
    bluetooth = (component / 'Bluetooth.h').read_text()
    ring_h = (component / 'G2_Ring.h').read_text()
    return '\n'.join([
        extract_block(glasses_h, 'enum class BleConnectKind') + ';',
        extract_block(glasses_h, 'struct BleConnectJob') + ';',
        extract_block(glasses_h, 'enum G2State') + ';',
        extract_block(glasses_h, 'enum G2Eye') + ';',
        extract_block(peers, 'enum BlePeerKind : uint8_t') + ';',
        extract_block(peers, 'enum class BlePeerConnectAdmission') + ';',
        line(peers, 'constexpr size_t BLE_PEER_ADDRESS_TEXT_CAPACITY'),
        line(peers, 'constexpr uint8_t BLE_PEER_DEFAULT_ADDRESS_TYPE'),
        extract_block(peers, 'struct BlePeerSavedTarget ') + ';',
        extract_block(peers, 'struct BlePeerConnectRequest') + ';',
        extract_block(bluetooth, 'enum class BleRoleTransition : uint8_t') + ';',
        line(ring_h, 'constexpr uint32_t kRingAudioWaitLimitMs'),
    ])


def worker_api(component: Path) -> str:
    return strip_includes((component / 'BLE_SetupWorker.h').read_text())


def worker_source(component: Path) -> str:
    worker = strip_includes((component / 'BLE_SetupWorker.cpp').read_text())
    # Every client's ops go through the harness's spies (which forward to the
    # real tables), in the table's own order.
    head, rest = worker.split('kOps[hs::kClients] = {', 1)
    table, tail = rest.split('};', 1)
    spies = {'&g2SetupOps': '&gSpyOps[kSpyG2]', '&g2RingRetireSetupOps': '&gSpyOps[kSpyRetire]',
             '&g2RingSetupOps': '&gSpyOps[kSpyRing]', '&phonePipeSetupOps': '&gSpyOps[kSpyPhone]'}
    for real, spy in spies.items():
        assert table.count(real) == 1, real
        table = table.replace(real, spy)
    worker = head + 'kOps[hs::kClients] = {' + table + '};' + tail
    # A seam to fail one publish after a successful claim.
    publish = 'bool bleSetupPublish(BleSetupClient client, uint32_t token) {'
    assert worker.count(publish) == 1
    return worker.replace(publish, 'static bool realBleSetupPublish(BleSetupClient client, uint32_t token) {')


def queue_unit(component: Path) -> str:
    glasses = (component / 'G2_Glasses.cpp').read_text()
    ring = (component / 'G2_Ring.cpp').read_text()
    g2 = [
        line(glasses, 'static portMUX_TYPE gControlLifecycleMux'),
        line(glasses, 'static volatile bool  gConnectTaskActive'),
        line(glasses, 'static volatile bool  gConnectCancel '),
        line(glasses, 'static portMUX_TYPE    gConnectAdmissionMux'),
        line(glasses, 'static volatile uint32_t gBleConnectCancelEpoch'),
        line(glasses, 'static uint8_t            gControlStartBlocks'),
        line(glasses, 'static uint8_t            gBleConnectSubmitters'),
        line(glasses, 'static volatile uint8_t gBleClientRetirementPeerMask'),
        line(glasses, 'static g2repair::Schedule gRepair;'),
        line(glasses, 'static portMUX_TYPE gRecoveryMux'),
        line(glasses, 'static uint8_t gG2PublishedTempleMask'),
        *(block(glasses, marker) for marker in (
            'static bool g2ManualConnectAdmissionOpen() ', 'static bool g2RoleAdmissionOpen() ',
            'static void g2ControlStartBlockAcquire() ', 'static void g2ControlStartBlockRelease() ',
            'static bool g2BleConnectSubmitBegin() ', 'static void g2BleConnectSubmitDone() ',
            'static bool g2BleConnectSubmitIdle() ', 'static bool g2ConnectClaim(uint32_t* cancelEpochOut = nullptr) ',
            'static void g2ConnectRelease() ', 'static uint32_t bleConnectCancelEpochSnapshot() ',
            'static uint32_t bleConnectCancelAdvance() ',
            'static void copyBleAddressText(char (&dst)[BLE_PEER_ADDRESS_TEXT_CAPACITY],',
            'static void g2PublishDisconnectIntent(bool userInitiated = false) ')),
    ]
    rings = [
        between(ring, 'static volatile bool  gRingConnectTaskActive', 'static uint32_t ringConnectCancelGenerationSnapshot()'),
        between(ring, 'static bool ringConnectGenerationCurrent(uint32_t expected)', '// Push a `ring-status` SSE'),
        *(block(ring, marker) for marker in (
            'static bool ringRequestCurrent(', 'bool g2RingConnectRequestCurrent(',
            'static BlePeerConnectAdmission ringPeerConnectSavedAdmissionThunk(\n    const BlePeerConnectRequest& request) ',
            'bool g2RingConnect() ', 'bool g2RingConnectSaved() ', 'bool g2RingConnectMac(const String& mac) ',
            'bool g2RingScanAsync(uint32_t timeoutSec) ', 'void g2RingDisconnect(bool userInitiated) ',
            'void g2RingYieldToPhonePipe() ', 'bool g2RingRetireForPhonePipe() ')),
    ]
    section = between(glasses, '// Glasses and ring jobs on the BLE setup worker (BLE_SetupWorker.h)',
                      '// Public API: non-blocking connect.')
    production = '\n\n'.join(g2 + rings + [section, block(glasses, 'bool g2Disconnect(bool userInitiated) ')])
    return insert((HERE / 'ring_phone_yield_queue_harness.cpp').read_text(), {
        '// INSERT_TYPES': types(component),
        '// INSERT_WORKER_API': worker_api(component),
        '// INSERT_PRODUCTION': production,
        '// INSERT_WORKER': worker_source(component),
    })


def yield_unit(component: Path) -> str:
    ring = (component / 'G2_Ring.cpp').read_text()
    body = '\n\n'.join(block(ring, m) for m in ('void g2RingYieldToPhonePipe() ', 'bool g2RingRetireForPhonePipe() '))
    return insert((HERE / 'ring_phone_yield_harness.cpp').read_text(), {'// INSERT_PRODUCTION': body})


def scan_unit(component: Path) -> str:
    ring = (component / 'G2_Ring.cpp').read_text()
    body = between(ring, 'static portMUX_TYPE gRingScanCallbackMux', 'bool g2RingScanAsync(uint32_t timeoutSec)')
    return insert((HERE / 'ring_scan_poll_harness.cpp').read_text(), {'// INSERT_PRODUCTION': body})


def owner_unit(component: Path) -> str:
    ring = (component / 'G2_Ring.cpp').read_text()
    body = '\n\n'.join([
        line(ring, 'static constexpr uint32_t RING_OWNER_STACK_BYTES'),
        between(ring, 'static TaskHandle_t sRingOwnerTask = nullptr;', 'static SemaphoreHandle_t sSetupDone'),
        *(block(ring, m) for m in ('static void ringWakeOwner() ', 'static void ringOwnerTask(void* /*arg*/) ',
                                   'static bool ringOwnerEnsure() ', 'bool g2RingOwnerRelease(uint32_t timeoutMs) ')),
    ])
    return insert((HERE / 'ring_owner_lifetime_harness.cpp').read_text(), {'// INSERT_PRODUCTION': body})


def check_text(component: Path) -> None:
    ring = (component / 'G2_Ring.cpp').read_text()
    glasses = (component / 'G2_Glasses.cpp').read_text()
    check_owner_lifetime_text(component, ring)
    # Every scan/connect path rechecks the runtime yield, including a late OPEN.
    for signature in ('bool g2RingScan(', 'bool ringPerformConnect(', 'static bool ringRequestCurrent('):
        assert 'bleCentralRingShouldYield()' in block(ring, signature), signature
    # A connect's own scan does not give way to queued jobs; the manual scan does.
    assert 'g2RingScan(15, cancelGeneration, false)' in block(ring, 'bool ringPerformConnect(')
    scan = block(ring, 'bool g2RingScan(')
    measured = scan[scan.index('bleCoexScanBegin('):scan.index('bleCoexScanEnd(')]
    assert not re.search(r'\breturn\b', measured), 'no return between bleCoexScanBegin and bleCoexScanEnd'
    # The commands say why they refuse, before any pairing stamp or submit.
    for command, first_effect in (('static const char* cmd_openring(', 'bleStampPairedByIfBlank('),
                                  ('static const char* cmd_ringscan(', 'g2RingScanAsync(')):
        handler = block(ring, command)
        assert 'if (bleCentralRingShouldYield()) return ringYieldRefusal();' in handler, command
        assert handler.index('bleCentralRingShouldYield()') < handler.index(first_effect), command
    # A queued ring connect is taken back before a running scan is stopped.
    for path in ('void g2RingDisconnect(bool userInitiated) ', 'bool g2RingPrepareForStackTeardown(uint32_t timeoutMs) '):
        body = block(ring, path)
        assert body.count('g2RingWithdrawQueuedConnect();') == 1, path
        assert body.index('ringBeginLogicalDown(') < body.index('g2RingWithdrawQueuedConnect();') < \
            body.index('g2CancelActiveRingScan();'), path
    # g2Disconnect takes back a queued glasses job right after the logical
    # down, and again on every pass of its wait for a running one.
    disconnect = block(glasses, 'bool g2Disconnect(bool userInitiated) ')
    withdraw = '(void)bleSetupWithdraw(BleSetupClient::G2, 0);'
    first = disconnect.index(withdraw)
    assert disconnect.index('g2PublishDisconnectIntent(userInitiated);') < first < disconnect.index('if (gConnectTaskActive)')
    wait = disconnect[disconnect.index('while (gConnectTaskActive && millis() < deadline) {'):]
    wait = wait[:wait.index('}')]
    assert withdraw in wait and disconnect.count(withdraw) == 2
    # deinitG2Client is glasses-only: it leaves the ring alone and drains only
    # the glasses' own setup slot (never forced) before the temples go. The
    # ring's close (g2RingClose) takes the link down, takes a queued yield
    # retirement back (pipe builds), retires the native client, then lets the
    # owner leave.
    deinit = block(glasses, 'bool deinitG2Client() ')
    assert 'g2RingDisconnect(' not in deinit and 'kAccessories' not in deinit and 'RingRetire' not in deinit
    assert deinit.index('bleSetupWaitIdle(hw1setup::clientMask(hw1setup::Client::G2), 6000)') < deinit.index('templeReset(gL)')
    close = block(ring, 'bool g2RingClose(bool userInitiated) ')
    down = close.index('g2RingDisconnect(userInitiated);')
    retire = close.index('(void)bleSetupWithdraw(BleSetupClient::RingRetire, 0);')
    assert down < retire < close.index('bleRetireClientForReplacement(') < close.index('g2RingOwnerRelease(1000)')
    assert close.rfind('#if ENABLE_BLE_PHONE_PIPE', 0, retire) > down, 'retirement withdraw only with the pipe'
    # The submit path: claim, write the payload, publish; a failed publish
    # gives the claim back; only glasses preempt a ring scan.
    submit = block(glasses, 'bool g2SubmitBleConnect(const BleConnectJob& job) ')
    claim, payload = submit.index('bleSetupClaim(client, &token)'), submit.index('(g2 ? sG2Job : sRingJob) = admitted;')
    publish = submit.index('if (!bleSetupPublish(client, token)) {')
    assert claim < payload < publish < submit.index('bleSetupCancelClaim(client, token);')
    assert submit.index('if (g2) bleConnectPreemptForG2Repair();') > publish
    # The C6's reported link limit holds for its transport session: a failed
    # identity probe keeps it (or the ring would yield mid-session on a
    # four-link C6), and every way the transport ends clears it.
    radio = (component / 'HAL_Radio.cpp').read_text()
    assert 'const uint8_t reported = sSessionBleLinks;' in block(radio, 'uint8_t radioHalCompanionBleLinks()')
    refresh = block(radio, 'esp_err_t radioHalRefreshIdentity()')
    assert 'if (id.infoKnown) sSessionBleLinks = id.bleMaxConnections;' in refresh
    assert refresh.count('sSessionBleLinks') == 1
    for down in re.finditer(r'^[ \t]+sTransportUp = false;', radio, re.MULTILINE):
        nearby = radio[down.start():down.start() + 80]
        assert 'sSessionBleLinks = 0;' in nearby, radio[max(0, down.start() - 200):down.start() + 80]
    # A ring job restarts the in-flight watchdog's clock and clears any stale
    # scan stop before it publishes its kind (so a later preempt is kept); a
    # stop request only sets the flag, and only the scanning job stops the
    # scanner (a late request never stops the next client's scan).
    run = block(glasses, 'static hw1setup::End ringSetupRun(')
    assert run.index('g2RingConnectMarkRunning();') < run.index('g2RingScanStopReset();') < \
        run.index('bleSetActiveKind(job.kind, true);')
    assert block(ring, 'void g2RingScanStop() ').count(';') == 1 and 'stop()' not in block(ring, 'void g2RingScanStop() ')
    assert 'gRingScanStopRequested = false' not in scan, 'a request made before start() is kept'
    # A host teardown after a glasses session frees the ring's kept client:
    # the one teardown (BLE_CentralStack's hostTeardown) drops the pointer
    # first, and ring retirement needs the client stack (any ring client
    # without it predates the host). The pipe and the companion reset own no
    # host teardown of their own any more.
    assert '!bleStackReady()' in block(ring, 'void g2RingYieldToPhonePipe() ')
    phone = (component / 'BLE_PhonePipe.cpp').read_text()
    stack = (component / 'BLE_CentralStack.cpp').read_text()
    teardown = block(stack, 'bool hostTeardown()')
    assert teardown.index('g2RingPrepareForStackTeardown(2000)') < teardown.index('bluetoothHalDeinit()') < \
        teardown.index('bleCentralClientsTerminalTeardownAcknowledged();')
    assert 'bluetoothHalDeinit(' not in phone and 'bluetoothHalInit(' not in phone, 'the pipe owns no host'
    companion = block((component / 'System_RadioCompanion.cpp').read_text(), 'void shutDownBleForCompanionReset(')
    assert 'bleStackDown()' in companion and 'bluetoothHalDeinit(' not in companion
    # The pipe open enables the ring's yield unless a version-mismatch hold
    # keeps the phone from connecting (phone link v2).
    assert 'bleCentralPhoneEnabled(!versionLatched.load())' in block(phone, 'bool phoneLinkOpen(')
    assert 'g2RingYieldToPhonePipe()' in block(phone, 'void phonePipeTick(')


def check_owner_lifetime_text(component: Path, ring: str) -> None:
    """The owner task can be released and re-created: nothing may hold its handle."""
    # The wake is the binary semaphore, with no task handle anywhere near it.
    assert 'xTaskNotifyGive' not in ring and 'ulTaskNotifyTake' not in ring
    wake = block(ring, 'static void ringWakeOwner() ')
    assert 'xSemaphoreGive(sRingWake)' in wake and 'sRingOwnerTask' not in wake
    # Status readers take the figure the owner publishes about itself.
    assert 'uxTaskGetStackHighWaterMark(sRingOwnerTask' not in ring
    owner = block(ring, 'static void ringOwnerTask(void* /*arg*/) ')
    assert 'sRingOwnerStackFreeMin.store(\n        (uint32_t)uxTaskGetStackHighWaterMark(nullptr));' in owner
    for reader in ('void g2RingGetStatus(', 'bool g2RingBuildStatusJson('):
        assert 'sRingOwnerStackFreeMin.load()' in block(ring, reader) and 'sRingOwnerTask' not in block(ring, reader)
    # The owner's exit: the handle is nulled under the mux and the task deletes
    # itself the moment the critical section ends, with nothing in between; it
    # is the only place the handle is nulled and the only vTaskDelete here.
    assert re.search(r'sRingOwnerTask = nullptr;\s*portEXIT_CRITICAL\(&sRingOwnerMux\);\s*vTaskDelete\(nullptr\);', owner)
    assert len(re.findall(r'^\s*sRingOwnerTask = nullptr;', ring, re.M)) == 1  # the declaration aside
    assert ring.count('vTaskDelete(') == 1
    assert owner.index('if (!online) {') < owner.index('vTaskDelete(nullptr);') < owner.index('if (online && currentGeneration != 0 &&')
    assert 'ulTaskNotifyTake' not in owner and owner.count('xSemaphoreTake(sRingWake, ') == 3
    # The wake semaphore is a static RTOS object in internal memory, created
    # once by g2RingInit and part of its failure check; the task itself is
    # created only by ringOwnerEnsure.
    for text_line in ring.splitlines():
        if 'StaticSemaphore_t' in text_line:
            assert 'EXT_RAM' not in text_line, text_line
    init = block(ring, 'bool g2RingInit() ')
    assert 'if (!sRingWake) {' in init and 'sRingWake = xSemaphoreCreateBinaryStatic(&sRingWakeStorage);' in init
    assert '!sRingWake ||' in init and 'xTaskCreateLogged' not in init and init.count('ringOwnerEnsure()') == 2
    assert 'if (gRing.initialized) return ringOwnerEnsure();' in init
    assert ring.count('xTaskCreateLogged(ringOwnerTask') == 1
    ensure = block(ring, 'static bool ringOwnerEnsure() ')
    assert ensure.index('sActiveTransaction = RingActiveTransaction{};') < ensure.index('ringResetRxIngressForInit();') < \
        ensure.index('xTaskCreateLogged(ringOwnerTask')
    # The release's contract is in the header, with the disabled stub beside
    # the teardown stub.
    ring_h = (component / 'G2_Ring.h').read_text()
    assert 'bool g2RingOwnerRelease(uint32_t timeoutMs);' in ring_h
    assert 'inline bool g2RingOwnerRelease(uint32_t /*timeoutMs*/) { return true; }' in ring_h
    assert 'g2RingDisconnect' in ring_h[ring_h.index('bool g2RingOwnerRelease') - 900:ring_h.index('bool g2RingOwnerRelease')]


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--component', type=Path, help='a copy of components/hardwareone to read production from')
    args = parser.parse_args()
    component = (args.component or HERE.parents[1]).resolve()
    cxx = os.environ.get('CXX', 'clang++')
    with tempfile.TemporaryDirectory(prefix='hw1-ring-phone-yield-') as tmp_name:
        tmp = Path(tmp_name)

        def build_and_run(unit: str, name: str, defines=(), coordinator=True) -> None:
            cpp, binary = tmp / f'{name}.cpp', tmp / name
            cpp.write_text(unit)
            command = [cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread', *defines,
                       '-I', str(HERE / 'ble_coordinator_stubs'), '-I', str(component), str(cpp)]
            if coordinator:
                command.append(str(component / 'BLE_CentralCoordinator.cpp'))
            command += ['-o', str(binary)]
            if args.sanitize:
                command[1:1] = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                                '-fno-omit-frame-pointer', '-g']
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True, timeout=100)

        # Behaviour first, so a mutated --component fails on what the firmware
        # does rather than on the source contracts below.
        build_and_run(queue_unit(component), 'queue')
        for pipe in (0, 1):
            build_and_run(yield_unit(component), f'yield{pipe}', [f'-DENABLE_BLE_PHONE_PIPE={pipe}'])
        build_and_run(scan_unit(component), 'scan', coordinator=False)
        build_and_run(owner_unit(component), 'owner', coordinator=False)
    check_text(component)
