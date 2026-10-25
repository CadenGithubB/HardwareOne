#!/usr/bin/env python3
"""Run the one BLE setup worker's scheduler, and check how the firmware wires it.

BLE_SetupWorkerCore.h is dependency-free, so ble_setup_worker_harness.cpp runs
it as is: turn order, the pass-over bound, wait accounting, exactly-once
finish (with a two-thread race), no allocations, and the phone's turn. The
text checks below cover what the harness cannot run: the FreeRTOS task and
its cooperative exit (bleSetupWorkerRelease; test_ring_phone_yield runs it),
the client ops tables and the removal of the old worker and setup gate
(docs/BLE_SETUP_WORKER_PLAN.md).
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def text(name: str) -> str:
    return (ROOT / name).read_text()


def check_wiring() -> None:
    core = text('BLE_SetupWorkerCore.h')
    assert 'enum class Client : uint8_t { G2 = 0, RingRetire = 1, Ring = 2, Phone = 3 };' in core, 'turn order'
    worker = text('BLE_SetupWorker.cpp')
    table = worker.split('kOps[hs::kClients] = {', 1)[1].split('};', 1)[0]
    order = re.findall(r'&(\w+SetupOps)', table)
    assert order == ['g2SetupOps', 'g2RingRetireSetupOps', 'g2RingSetupOps', 'phonePipeSetupOps'], order
    assert 'ENABLE_G2_GLASSES ? 5120 : 4096' in worker, 'stack in bytes; 5120 where the connect bodies run'
    assert re.search(r'xTaskCreateLogged\(loop, "ble_setup", kStackBytes, nullptr, 5, &task, "ble.setup", APP_CORE\)', worker)
    assert 'EXT_RAM' not in worker and 'xTaskCreateStatic' not in worker, 'never a PSRAM stack'
    publish = worker.split('bool bleSetupPublish(', 1)[1].split('\n}\n', 1)[0]
    assert publish.index('portEXIT_CRITICAL') < publish.index('bleSetupWake()'), 'publish wakes the worker, outside the lock'
    withdraw = worker.split('BleSetupWithdraw bleSetupWithdraw(', 1)[1].split('\n}\n', 1)[0]
    exit_at, finish_at = withdraw.index('portEXIT_CRITICAL'), withdraw.index('->finish(')
    assert exit_at < finish_at < withdraw.index('sCore.finished(client)'), 'finish runs without the lock, then is counted'
    run = worker.split('hs::End run(hs::Client c', 1)[1].split('\n  }\n', 1)[0]
    assert 'hs::runRecorded(hooks, o->label(t), o->recordWait, info' in run
    hooks = worker.split('struct RecordHooks {', 1)[1].split('};', 1)[0]
    for call in ('bleCoexLogSetupWait(label, ms)', 'bleCoexSetAccessoryWait(ms)', 'bleCoexClearAccessoryWait()'):
        assert call in hooks, call
    assert 'bleSetupIdle(mask)' in worker.split('bool bleSetupWaitIdle(', 1)[1].split('\n}\n', 1)[0]

    # A release ends the task cooperatively: it marks the worker Stopping, so
    # a claim (Ready only) and a start are refused meanwhile; the worker
    # leaves only when idle, nulls its own handle under the lock a start
    # checks and deletes itself right after; the wake semaphore is made once,
    # so a restart reuses it; never from the worker itself.
    assert 'enum class Init : uint8_t { Uninitialized, Starting, Ready, Stopping };' in worker
    claim = worker.split('bool bleSetupClaim(', 1)[1].split('\n}\n', 1)[0]
    assert 'sInit == Init::Ready && sCore.claim(client, token)' in claim
    loop = worker.split('void loop(void*) {', 1)[1].split('\n}\n', 1)[0]
    assert re.search(r'hs::serve\(sCore, env\);\s*if \(workerLeaves\(\)\) vTaskDelete\(nullptr\);', loop), loop
    assert worker.count('vTaskDelete(') == 1, 'only the worker deletes itself'
    leaves = worker.split('bool workerLeaves() {', 1)[1].split('\n}\n', 1)[0]
    assert 'sInit == Init::Stopping && sCore.idle(hs::kAll)' in leaves, 'never with a job claimed, queued, running or finishing'
    assert leaves.count('portENTER_CRITICAL(&sCoreMux)') == 1 and leaves.count('portEXIT_CRITICAL(&sCoreMux)') == 1
    assert leaves.index('portENTER_CRITICAL(&sCoreMux)') < leaves.index('sTask = nullptr;') < \
        leaves.index('sInit = Init::Uninitialized;') < leaves.index('portEXIT_CRITICAL(&sCoreMux)')
    assert len(re.findall(r'^\s+sTask = nullptr;', worker, re.M)) == 1, 'only the worker clears its handle'
    start = worker.split('bool bleSetupWorkerStart() {', 1)[1].split('\n}\n', 1)[0]
    assert 'if (!sWake) sWake = xSemaphoreCreateBinaryStatic(&sWakeStorage);' in start
    assert worker.count('xSemaphoreCreateBinaryStatic(') == 1
    release = worker.split('bool bleSetupWorkerRelease(uint32_t timeoutMs) {', 1)[1].split('\n}\n', 1)[0]
    assert 'const TaskHandle_t self = xTaskGetCurrentTaskHandle();' in release and 'self == sTask' in release
    for step in ('sInit = Init::Stopping;', 'bleSetupWaitIdle(hs::kAll, timeoutMs)', 'bleSetupWake();'):
        assert step in release, step
    assert release.index('sInit = Init::Stopping;') < release.index('bleSetupWaitIdle(hs::kAll, timeoutMs)') < \
        release.index('bleSetupWake();') < release.index('sInit = released ? Init::Uninitialized : Init::Ready;')
    header = text('BLE_SetupWorker.h')
    assert 'bool bleSetupWorkerRelease(uint32_t timeoutMs);' in header
    assert 'inline bool bleSetupWorkerRelease(uint32_t) { return true; }' in header

    glasses = text('G2_Glasses.cpp')
    phone = text('BLE_PhonePipe.cpp')
    tables = {
        'g2SetupOps': (glasses, 'g2SetupLabel, g2SetupReady, nullptr, g2SetupRun, g2SetupFinish, true'),
        'g2RingSetupOps': (glasses, 'ringSetupLabel, ringSetupReady, ringSetupWaiting, ringSetupRun, ringSetupFinish, true'),
        'g2RingRetireSetupOps': (glasses, 'ringRetireLabel, ringRetireReady, nullptr, ringRetireRun, ringRetireFinish, false'),
        'phonePipeSetupOps': (phone, 'setupLabel,setupReady,nullptr,setupRun,setupFinish,false'),
    }
    for name, (source, body) in tables.items():
        found = re.search(r'extern const BleSetupOps ' + name + r'\s*=\s*\{\s*([^}]*)\}', source)
        assert found and re.sub(r'\s+', ' ', found.group(1)).strip() == body, name
    retire = glasses.split('#if ENABLE_BLE_PHONE_PIPE\n// --- Ring retirement', 1)
    assert len(retire) == 2 and 'g2RingRetireSetupOps' not in retire[0], 'ring retirement exists only with the pipe'

    # The old worker and the setup gate are gone, in code and stubs alike.
    everything = '\n'.join(p.read_text() for p in ROOT.glob('*.[ch]*'))
    for gone in ('g2_ble_connect", ', 'BleCentralSetupGuard', 'bleCentralSetupTake', 'bleCentralSetupGive',
                 'bleCentralAccessoryEnqueued', 'bleCentralAccessoryDequeued', 'bleCentralAccessoryBusy',
                 'kBleConnectQueueDepth', 'gBleConnectJobActive', 'bleConnectShutdown', 'BleConnectKind::BARRIER'):
        assert gone not in everything, gone
    assert 'BLE_SetupWorker.cpp' in text('CMakeLists.txt')


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--sanitize', action='store_true')
    args = p.parse_args()
    check_wiring()
    with tempfile.TemporaryDirectory(prefix='hw1-ble-setup-') as tmp:
        runs = [['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g'] if args.sanitize else [],
                ['-fsanitize=thread', '-g']]  # the two-thread race, under TSan as well
        for i, flags in enumerate(runs):
            binary = Path(tmp) / f'worker{i}'
            command = [os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                       *flags, '-I', str(ROOT), str(HERE / 'ble_setup_worker_harness.cpp'), '-o', str(binary)]
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True)
