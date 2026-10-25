#!/usr/bin/env python3
"""ServiceWorker (System_TaskUtils.h): the lifecycle protocol the idle service
workers share. Splices the production struct into service_worker_harness.cpp
over a faked kernel and runs it; then pins that the three modules use it and
keep no lifecycle primitive of their own."""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_web_batch_handlers import extract_block  # noqa: E402

COMPONENT = HERE.parents[1]


def check_users(component: Path) -> None:
    header = (component / 'System_TaskUtils.h').read_text()
    struct = extract_block(header, 'struct ServiceWorker')
    # The protocol's load-bearing lines.
    for needle in ('portMUX_TYPE& mux;', 'if (handle || starting) {', 'starting = true;',
                   'if (!wake) {', 'wake = made;', 'if (rc == pdPASS) handle = created;',
                   'starting = false;', 'const bool leave = idle();', 'if (leave) handle = nullptr;'):
        assert needle in struct, needle
    assert struct.index('wake = made;') < struct.index('xTaskCreateLogged(body, name, stackBytes'), 'wake before task'
    users = {'System_QuickNote.cpp': 'ServiceWorker gQnWorker{gQnMux};',
             'System_Dictation.cpp': 'static ServiceWorker gDictWorker{gDictMux};',
             'System_Cm5Presence.cpp': 'static ServiceWorker sPresenceWorker{sPresenceMux};'}
    for name, declaration in users.items():
        source = (component / name).read_text()
        assert declaration in source, (name, declaration)
        for primitive in ('xTaskCreateLogged(', 'xSemaphoreCreateBinaryStatic(', 'xTaskNotifyGive(',
                          'ulTaskNotifyTake(', 'xSemaphoreGive(', 'xSemaphoreTake('):
            assert primitive not in source, (name, primitive)
        assert source.count('vTaskDelete(nullptr)') == 1, name


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='clang++')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    check_users(COMPONENT)
    header = (COMPONENT / 'System_TaskUtils.h').read_text()
    unit = (HERE / 'service_worker_harness.cpp').read_text()
    marker = '// INSERT_SERVICE_WORKER'
    assert unit.count(marker) == 1
    unit = unit.replace(marker, extract_block(header, 'struct ServiceWorker') + ';\n')
    with tempfile.TemporaryDirectory(prefix='hw1-service-worker-') as tmp:
        path = Path(tmp) / 'test.cpp'
        path.write_text(unit)
        exe = Path(tmp) / 'test'
        cmd = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
               str(path), '-o', str(exe)]
        if args.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True)
        subprocess.run([str(exe)], check=True, timeout=60)


if __name__ == '__main__':
    main()
