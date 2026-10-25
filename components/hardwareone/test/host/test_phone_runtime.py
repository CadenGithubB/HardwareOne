#!/usr/bin/env python3
"""Run the phone owner's runtime on the real BLE setup worker.

BLE_PhonePipe.cpp's runtime (step machine, admission, enrollment, records,
the phone's setup-worker client, the app console's link record) is extracted
unchanged. BLE_SetupWorker.h/.cpp and BLE_SetupWorkerCore.h compile as is
against host stubs; only the five public functions the harness spies on are
renamed so the spies can call them.
phone_runtime_harness.cpp co-simulates the phone task on a fake clock while
the worker runs its turns (docs/BLE_SETUP_WORKER_PLAN.md). Native GATT and the
wire core have separate harnesses.

--component points at another copy of components/hardwareone (mutation runs).
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

# The worker API the harness wraps: (definition in BLE_SetupWorker.cpp, renamed).
SPIED = (
    ('bool bleSetupClaim(', 'bool realSetupClaim('),
    ('bool bleSetupPublish(', 'bool realSetupPublish('),
    ('void bleSetupCancelClaim(', 'void realSetupCancelClaim('),
    ('BleSetupWithdraw bleSetupWithdraw(', 'BleSetupWithdraw realSetupWithdraw('),
    ('void bleSetupWake() ', 'void realSetupWake() '),
)
# Production order. TurnEnv, the setup ops and phonePipeSetupOps sit between
# BEFORE and AFTER.
BEFORE = (
    'void setStep(', 'void routeSet(', 'bool matches(', 'bool hasService(', 'bool gapCallback(',
    'bool currentJob(', 'void setReady() ', 'bool clearReady() ', 'void fail(', 'void cleanupExpired(',
    'void leaveSetup() ', 'void endTurn() ',
    'void configureScan(', 'void startForget(', 'hw1coex::ScanRecord scanRecord(', 'void noteScan(',
    'void noteScanFailed(uint32_t now) ', 'void noteAttempt(', 'void noteAttemptIfWanted(', 'void flushRecords(',
    'void consume(', 'void noteSetup(', 'bool authCoolingDown(', 'void beginJob(',
    # `phone pair new`'s window (phone link v2).
    'const char* startEnrollmentLocked(', 'uint32_t openPairWindow(', 'void endPairWindow(',
    'bool closePairWindowLocked(', 'void tickLocked(', 'void pairTickLocked(',
    'void publishSummary() ', 'void passLocked(',
    # The task's exit (run() is not compiled; the harness's phonePass is its body).
    'TaskHandle_t workerHandle() ', 'bool leaveIfReleased(bool idleRetire) ',
    'BlePeerConnectAdmission admit(', 'void disconnect() ', 'void giveStateBack(',
    # The pipe key, published only if no invalidation raced its derivation.
    'PskLoad loadPsk(',
)
SETUP_OPS = ('const char* setupLabel(', 'hw1setup::Verdict setupReady(', 'hw1setup::End setupRun(', 'void setupFinish(')
AFTER = (
    'bool phonePipeStop(', 'bool phoneLinkClose(', 'bool phonePipeHostStopped(', 'bool phonePipeWorkerRelease(', 'bool phonePipeInvalidateSecret(',
    'bool phonePipeRecoveryNeeded(', 'bool phonePipeQuiesceForHostRecovery(',
    # Phone link v2: relay availability, the console's writer and the status snapshot.
    'bool phonePipeRelayAvailable(', 'bool phonePipeOwnerHeldByCurrentTask(', 'PhonePipePush phonePipeConsolePush(',
    'bool phonePipeLinkSummary(',
    'bool phonePipeLinkRecord(', 'void phonePipeNoteConsoleActivity(',
    'bool phonePipeEnrolling(', 'PhonePairView phonePipePairView(', 'void phonePipePairClose(',
    'PhonePipeOpenResult phonePipeOpen(', 'void phonePipeClose(', 'static const char* scanCommand(',
    # The `phone` and `relay` command handlers (the tables are registry data).
    'static bool localAdmin(', 'static const char* startLinkIfStopped(', 'static bool liftVersionHold(',
    'static const char* cmd_openphone(', 'static const char* cmd_closephone(',
    'static const char* phoneScanCommand(', 'static const char* phoneForgetCommand(',
    'static const char* phonePairCommand(', 'static const char* phonePairNewCommand(',
    'static const char* phoneStatsCommand(',
    'static const char* relayStatusCommand(',
)


def worker_sources(component: Path) -> tuple[str, str]:
    header = (component / 'BLE_SetupWorker.h').read_text()
    for gone in ('#pragma once\n', '#include "System_BuildConfig.h"\n'):
        assert header.count(gone) == 1, gone
        header = header.replace(gone, '')
    source = (component / 'BLE_SetupWorker.cpp').read_text()
    source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
    for old, new in SPIED:
        assert source.count(old) == 1, old
        source = source.replace(old, new)
    return header, source


def check_sources(phone: str) -> None:
    """READY and the console's link record change together (S4)."""
    # Every write of ready goes through setReady()/clearReady(), which also
    # write the record, so no path ends READY and leaves the record set.
    storage = 'PhonePipeLinkRecord linkRecord;bool linkValid=false;'
    assert phone.count(storage) == 1, storage
    rest = phone.replace(storage, '')
    for pair in ('void setReady() ', 'bool clearReady() '):
        rest = rest.replace(extract_block(phone, pair), '')
    for write in (r'(?<![\w.])ready\s*=\s*(?:true|false)', r'(?<![\w.])ready\.(?:store|exchange)\(',
                  r'(?<![\w.])linkValid\s*=', r'(?<![\w.])linkRecord\s*='):
        found = re.findall(write, rest)
        assert not found, f'{write} outside setReady/clearReady: {found}'
    # RELAY_ON goes out with READY and is cleared before it drops, so relay
    # users never see it without a READY session.
    set_ready, clear_ready = extract_block(phone, 'void setReady() '), extract_block(phone, 'bool clearReady() ')
    assert set_ready.index('relayOn=') < set_ready.index('ready=true'), 'relayOn before READY'
    assert clear_ready.index('relayOn=false') < clear_ready.index('ready.exchange(false)'), 'relayOn cleared first'
    # The pipe task's pass: flags before the poll, the console driver and the
    # snapshot after the step machine, its row work after the owner mutex.
    tick = extract_block(phone, 'void tickLocked(')
    assert tick.index('setLocalFlags(pipeConsoleLocalFlags())') < tick.index('phoneGatt.poll(now,pressure,displayBusy);')
    passing = extract_block(phone, 'void passLocked(')
    assert passing.index('tickLocked(now);') < passing.index('pairTickLocked(now);') < \
        passing.index('pipeConsoleTickLocked(') < passing.index('publishSummary();')
    # `phone pair new`: every refusal before the first change (the version
    # hold), then the code, the secret steps, the link, the window, the one
    # sink. The code never goes through a broadcast, the debug queue or a log.
    pair_new = extract_block(phone, 'static const char* phonePairNewCommand(')
    first_change = pair_new.index('liftVersionHold()')
    for refusal in ('RETURN_VALID_IF_VALIDATE_CSTR', 'localAdmin(auth)', 'oledPhonePairCodeSinkReady()',
                    'serialCommandSecretSinkReady()', 'isBleServerInitialized()', 'gSettings.bleEnabled',
                    'blePeerOwnerSessionSnapshot(', 'captureTransportSessionEpoch(', 'authCoolingDown('):
        assert pair_new.index(refusal) < first_change, refusal
    assert first_change < pair_new.index('esp_random()') < pair_new.index('phonePipeReplaceSecret(') < \
        pair_new.index('startLinkIfStopped()') < pair_new.index('openPairWindow(') < \
        pair_new.index('oledShowPhonePairCode()') < pair_new.index('serialWriteCommandSecret(')
    for sink in ('broadcastOutput', 'BROADCAST_PRINTF', 'DEBUG_', 'ESP_LOG', 'logSystemEvent', 'Serial.'):
        assert sink not in pair_new, sink
    run = extract_block(phone, 'void run(')
    assert run.index('passLocked(millis());}') < run.index('pipeConsoleAfterUnlock();'), 'row work outside the owner mutex'
    # The gBLEState hold: taken once the host is up and before the owner lock,
    # given back after it (the lifecycle lock is never taken under it).
    start = extract_block(phone, 'bool phoneLinkOpen(')
    assert start.index('bluetoothHalStatus().ready()') < start.index('blePipeConsoleStateAcquire()') < \
        start.index('Lock lock;') < start.index('if(!started)giveStateBack();')
    assert 'bleCentralPhoneEnabled(!versionLatched.load())' in start, 'a held pipe leaves the ring its link'
    for name in ('bool phonePipeStop(', 'bool phonePipeHostStopped('):
        body = extract_block(phone, name)
        assert 'giveStateBack();' in body and 'Lock' not in body, name
    # Only a running pipe's pass reads the secret's kind (pipeConsoleLocalFlags
    # parses it): its writers change it off the owner mutex once the pipe
    # stopped. The snapshot takes what the session was told this pass.
    code = re.sub(r'//[^\n]*', '', phone)
    assert code.count('pipeConsoleLocalFlags()') == 1, 'one read of the console flags'
    assert tick.index('if(!active.load())return;') < tick.index('pipeConsoleLocalFlags()')
    assert 's.consoleOffered=s.active && (phoneGatt.pipe.localFlags()&hw1pipe::kP4ConsoleOffered)!=0;' in \
        extract_block(phone, 'void publishSummary() ')
    # The key: phoneLinkOpen only through loadPsk, which derives into its own
    # stack (wiped) and never into the published key; every invalidation counts.
    assert 'loadPsk()' in start and 'scPbkdf2' not in start and 'cachedPsk' not in start
    load = extract_block(phone, 'PskLoad loadPsk(')
    assert '100000,psk)' in load and 'sodium_memzero(psk,sizeof(psk));' in load
    assert load.index('seen=pskInvalidations;') < load.index('scPbkdf2(') < load.index('pskInvalidations==seen')
    invalidate = extract_block(phone, 'bool phonePipeInvalidateSecret(')
    assert invalidate.index('Lock lock(1000);') < invalidate.index('++pskInvalidations;')
    # The window's session is written with its code, under codeMux, for the
    # code screen's view; `phone pair new` checks its sink again under the
    # lock that opens the window.
    window = extract_block(phone, 'uint32_t openPairWindow(')
    assert window.index('portENTER_CRITICAL(&codeMux);') < window.index('pairEpoch=epoch;pairSource=source;') < \
        window.index('portEXIT_CRITICAL(&codeMux);')
    view = extract_block(phone, 'PhonePairView phonePipePairView(')
    assert view.index('portENTER_CRITICAL(&codeMux);') < view.index('v.source=pairSource;v.epoch=pairEpoch;') < \
        view.index('portEXIT_CRITICAL(&codeMux);')
    late = pair_new[pair_new.index('Lock lock(1000);'):pair_new.index('openPairWindow(')]
    assert 'display?oledPhonePairCodeSinkReady():serialCommandSecretSinkReady()' in late and '}' not in late
    # The task's exit: after the records are flushed, before the delay. The
    # handle is nulled by the task itself inside the init critical section, in
    # the step that consumes the request; the release asks the host stop
    # first, then the idle slot, raises and withdraws the request under the
    # same lock (withdraw and read in one step) and never takes the owner
    # mutex. Nothing else writes either. The idle exit on an up host: the
    # pass publishes idleOnHost last (owner mutex held), run() clocks it and
    # asks the setup slot before the same exit step.
    assert run.index('flushRecords();') < run.index('if(!idleOnHost.load())idleSince=0;') < \
        run.index('bleSetupIdle(hw1setup::clientMask(hw1setup::Client::Phone));') < \
        run.index('if(leaveIfReleased(idleRetire))vTaskDelete(nullptr);') < run.index('vTaskDelay(')
    assert passing.index('publishSummary();') < passing.index('idleOnHost=!active.load() && step==Step::Idle && phoneGatt.idle() && nativeAppId==0xffff &&')
    leave = extract_block(phone, 'bool leaveIfReleased(bool idleRetire) ')
    assert leave.index('portENTER_CRITICAL(&initMux);') < \
        leave.index('const bool leave=(exitWanted || idleRetire) && !active.load() && !hostResetWanted.load();') < \
        leave.index('if(leave){worker=nullptr;exitWanted=false;}') < leave.index('portEXIT_CRITICAL(&initMux);')
    # phoneLinkOpen publishes active, then looks for the task under initMux
    # (workerHandle) and makes one if it left meanwhile.
    assert start.index('active=true;') < start.index('if(!workerHandle() && !resources()) {') < \
        start.index('started=true;')
    release = extract_block(phone, 'bool phonePipeWorkerRelease(')
    assert 'Lock' not in release and release.index('if(!workerHandle())return true;') < \
        release.index('if(active.load() || !phonePipeHostStopped())return false;') < release.index('bleSetupIdle(') < \
        release.index('portENTER_CRITICAL(&initMux);exitWanted=true;portEXIT_CRITICAL(&initMux);') < \
        release.index('while(workerHandle() && ') < \
        release.index('portENTER_CRITICAL(&initMux);exitWanted=false;const bool gone=!worker;portEXIT_CRITICAL(&initMux);')
    rest = code.replace(extract_block(code, 'bool leaveIfReleased(bool idleRetire) '), '') \
        .replace(extract_block(code, 'bool phonePipeWorkerRelease('), '')
    assert re.findall(r'(?<![\w.])worker\s*=(?!=)', rest) == ['worker='] and 'TaskHandle_t worker=nullptr;' in rest
    assert re.findall(r'(?<![\w.])exitWanted\s*=(?!=)', rest) == ['exitWanted='] and 'bool exitWanted=false;' in rest


def generated(component: Path) -> str:
    phone = (component / 'BLE_PhonePipe.cpp').read_text()
    check_sources(phone)
    kinds = '\n'.join(extract_block(phone, k) + ';' for k in
                      ('enum class Step', 'enum class GapKind', 'struct GapEvent', 'struct Route', 'enum class PskLoad'))
    # The console's record type, from the public header.
    public = (component / 'BLE_PhonePipe.h').read_text()
    kinds += '\n' + '\n'.join(extract_block(public, k) + ';' for k in
                              ('struct PhonePipeLinkRecord', 'enum class PhonePipePush', 'struct PhonePipeLinkSummary',
                               'enum class PhonePairWindow', 'struct PhonePairView'))
    console = (component / 'BLE_PhonePipeConsole.h').read_text()
    kinds += '\n' + '\n'.join(extract_block(console, k) + ';' for k in
                              ('enum class PipeConsoleState', 'struct PipeConsoleStatus'))
    storage = phone[phone.index('// Protocol storage and adapter'):phone.index('\nclass Lock')]
    ops = re.search(r'extern const BleSetupOps phonePipeSetupOps\s*=\s*\{[^}]*\};', phone)
    assert ops, 'phonePipeSetupOps'
    functions = '\n\n'.join(
        [extract_block(phone, 'class Role') + ';']
        + [extract_block(phone, k) for k in BEFORE]
        + [extract_block(phone, 'struct TurnEnv') + ';']
        + [extract_block(phone, k) for k in SETUP_OPS]
        + [ops.group(0)]
        + [extract_block(phone, k) for k in AFTER])
    header, worker = worker_sources(component)
    harness = (HERE / 'phone_runtime_harness.cpp').read_text()
    for marker, text in (('// INSERT_WORKER_HEADER', header), ('// INSERT_TYPES', kinds),
                         ('// INSERT_STORAGE', storage), ('// INSERT_WORKER', worker),
                         ('// INSERT_PRODUCTION', functions)):
        assert harness.count(marker + '\n') == 1, marker
        harness = harness.replace(marker + '\n', text + '\n')
    return harness


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--component', type=Path, default=COMPONENT,
                        help='components/hardwareone copy whose BLE_PhonePipe.cpp and BLE_SetupWorker.* are tested')
    args = parser.parse_args()
    component = args.component.resolve()
    with tempfile.TemporaryDirectory(prefix='hw1-phone-runtime-') as tmp:
        unit = Path(tmp) / 'runtime.cpp'
        binary = Path(tmp) / 'runtime'
        unit.write_text(generated(component))
        command = [os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-parameter',
                   '-I', str(component), str(unit), '-o', str(binary)]
        if args.sanitize:
            command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        for external in (0, 1):
            subprocess.run(command + [f'-DCONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY={external}'], check=True)
            subprocess.run([str(binary)], check=True, timeout=100)
