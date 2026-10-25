#!/usr/bin/env python3
"""Run the phone link v2 console driver (BLE_PhonePipeConsole.cpp) as shipped.

The driver, its header and the pure headers it uses (BLE_PhonePipeCore.h,
BLE_PhonePipeConsoleCore.h, BLE_PhonePairCode.h) are copied unchanged into a
temporary directory beside generated fakes of the headers they include. The
fakes carry the real declarations, extracted from Bluetooth.h, BLE_PhonePipe.h,
BLE_CentralTx.h, System_Utils.h and System_SelfDevice.h, so a changed
signature fails to compile here. phone_console_harness.cpp includes the driver
source and co-simulates the pipe task (owner mutex, the real Session), cmd_exec,
the main loop, the debug task and a phone on one clock, with recording doubles
of the FreeRTOS locks that enforce the lock order (critic section 3) on every
call.

The harness also runs a stored code without Settings::phonePipeSecretGenerated
(typed with `phone secret`): the console is then never offered. The text checks pin
what a host build cannot see: the pool in PSRAM with no constructor, no logging
on the chain paths, the CMake source entry.

--component points at another copy of components/hardwareone (mutation runs).
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
REAL = ('BLE_PhonePipeConsole.cpp', 'BLE_PhonePipeConsole.h', 'BLE_PhonePipeCore.h',
        'BLE_PhonePipeConsoleCore.h', 'BLE_PhonePairCode.h')


def found(pattern: str, text: str, what: str) -> str:
    match = re.search(pattern, text, re.MULTILINE | re.DOTALL)
    require(match is not None, what)
    return match[0]


def strip_comments(text: str) -> str:
    return re.sub(r'//[^\n]*', '', text)


def check_sources(component: Path) -> None:
    source = (component / 'BLE_PhonePipeConsole.cpp').read_text()
    require(source.startswith('#include "BLE_PhonePipeConsole.h"\n#if ENABLE_BLUETOOTH && ENABLE_BLE_PHONE_PIPE\n')
            and source.rstrip().endswith('#endif'),
            'the driver compiles only with the pipe (its stubs live in BLE_PhonePipeConsole.h)')
    cmake = (component / 'CMakeLists.txt').read_text()
    srcs = found(r'^set\(hardwareone_srcs\n.*?\)', cmake, 'the component source list')
    require(re.search(r'\n\s+BLE_PhonePipe\.cpp\n\s+BLE_PhonePipeConsole\.cpp\n', srcs) is not None,
            'BLE_PhonePipeConsole.cpp is a component source beside BLE_PhonePipe.cpp')
    code = strip_comments(source)
    # The ingress pool: 8 x 528 B in PSRAM, all-zero at boot (no constructor).
    require('alignas(4) EXT_RAM_BSS_ATTR PoolSlot pool[kPoolSlots];' in code, 'the pool lives in PSRAM .bss')
    slot = extract_block(code, 'struct PoolSlot')
    require('=' not in slot and 'static_assert(sizeof(PoolSlot) == 528' in code,
            'pool slots have no default member initialisers and are 528 B')
    require(re.search(r'kPoolSlots = 8\b', code) or re.search(r'kPoolSlots\s*=\s*8\b', code),
            'eight pool slots')
    require(re.search(r'^StaticSemaphore_t chainStorage;$', code, re.MULTILINE) is not None,
            'the chain mutex storage stays in internal RAM')
    require('xSemaphoreCreateMutexStatic(&chainStorage)' in code and 'Recursive' not in code,
            'the chain mutex is a static, non-recursive mutex')
    # Chain paths never log: a line routed to BLE would come back as output.
    for banned in ('BLE_DEBUGF', 'broadcastOutput', 'Serial.', 'printf', 'ESP_LOG', 'DEBUG_'):
        require(banned not in code, f'the console driver does not log ({banned})')
    for banned in ('new ', 'malloc', 'std::string', 'String(', 'xTaskCreate', 'ps_alloc'):
        require(banned not in code, f'the console driver allocates nothing and starts no task ({banned})')
    # Writers push the caller's bytes straight from its buffer: the only arrays
    # on their stacks are record headers.
    for marker in ('PipeConsoleWrite pipeConsoleWrite(', 'Put put(', 'bool endChain('):
        body = extract_block(code, marker)
        arrays = re.findall(r'\b(?:uint8_t|char)\s+\w+\[([^\]]*)\]', body)
        require(all(size.strip() == 'hw1rec::kHeader' for size in arrays),
                f'{marker} keeps no record-sized buffer: {arrays}')
    # The pipe task's pass never touches the BLE row under the owner mutex.
    for marker in ('void pipeConsoleTickLocked(', 'void streamPass(', 'void ingress(', 'void letGo(',
                   'void sessionEnded(', 'void closeStream('):
        body = extract_block(code, marker)
        for call in ('blePipeConsoleBind', 'blePipeConsoleEnd', 'blePipeConsoleInbound', 'pipeConsoleWrite',
                     'xSemaphoreTake', 'vTaskDelay', 'phonePipeConsolePush'):
            require(call + '(' not in body, f'{marker} must not call {call} (owner mutex held)')
    settings = (component / 'System_Settings.h').read_text()
    require('String phonePipeSecret;' in settings and 'bool bleConsoleOnPipe;' in settings,
            'Settings carries the pipe secret and bleConsoleOnPipe')
    require('using TransportSessionEpoch = uint32_t;' in (component / 'System_User.h').read_text(),
            'TransportSessionEpoch is uint32_t (the driver API passes it as such)')


def fake_headers(component: Path) -> dict[str, str]:
    bluetooth = (component / 'Bluetooth.h').read_text()
    pipe = (component / 'BLE_PhonePipe.h').read_text()
    central = (component / 'BLE_CentralTx.h').read_text()
    utils = (component / 'System_Utils.h').read_text()
    self_device = (component / 'System_SelfDevice.h').read_text()

    def decl(pattern: str, text: str, what: str) -> str:
        return found(r'^' + pattern + r'[^;{]*;', text, what)

    bt = '\n'.join([
        '#pragma once', '#include <cstddef>', '#include <cstdint>',
        'using TransportSessionEpoch = uint32_t;',
        extract_block(bluetooth, 'struct BlePipeConsoleIdentity') + ';',
        extract_block(bluetooth, 'inline constexpr uint16_t blePipeConsoleRowId('),
        decl(r'bool blePipeConsoleBind\(', bluetooth, 'Bluetooth.h declares blePipeConsoleBind'),
        decl(r'void blePipeConsoleEnd\(', bluetooth, 'Bluetooth.h declares blePipeConsoleEnd'),
        decl(r'void blePipeConsoleIdentity\(', bluetooth, 'Bluetooth.h declares blePipeConsoleIdentity'),
        decl(r'void blePipeConsoleInbound\(', bluetooth, 'Bluetooth.h declares blePipeConsoleInbound'),
        decl(r'bool bleLifecycleHeldByCurrentTask\(', bluetooth, 'Bluetooth.h declares bleLifecycleHeldByCurrentTask'),
    ])
    owner = '\n'.join([
        '#pragma once', '#include <cstddef>', '#include <cstdint>',
        found(r'^struct PhonePipeLinkRecord \{[^}]*\};', pipe, 'BLE_PhonePipe.h declares PhonePipeLinkRecord'),
        found(r'^enum class PhonePipePush : uint8_t \{[^}]*\};', pipe, 'BLE_PhonePipe.h declares PhonePipePush'),
        decl(r'PhonePipePush phonePipeConsolePush\(', pipe, 'BLE_PhonePipe.h declares phonePipeConsolePush'),
        decl(r'bool phonePipeOwnerHeldByCurrentTask\(', pipe, 'phonePipeOwnerHeldByCurrentTask'),
        decl(r'void phonePipeNoteConsoleActivity\(', pipe, 'phonePipeNoteConsoleActivity'),
        decl(r'bool phonePipeLinkRecord\(', pipe, 'phonePipeLinkRecord'),
        decl(r'bool phonePipeReady\(', pipe, 'phonePipeReady'),
    ])
    return {
        'System_BuildConfig.h': '#pragma once\n#define ENABLE_BLUETOOTH 1\n#define ENABLE_BLE_PHONE_PIPE 1\n'
                                '#define BOARD_NAME "Host \\"Board\\""\n',
        'Bluetooth.h': bt + '\n',
        'BLE_PhonePipe.h': owner + '\n',
        'BLE_CentralTx.h': '#pragma once\n' + decl(r'bool bleCentralTxAccessoryPressure\(', central,
                                                   'BLE_CentralTx.h declares bleCentralTxAccessoryPressure') + '\n',
        'System_Utils.h': '#pragma once\nstruct ExecReq { typedef void (*DeferredFn)(void* arg); };\n' +
                          decl(r'bool submitDeferredToCmdExec\(', utils, 'System_Utils.h declares submitDeferredToCmdExec')
                          + '\n',
        'System_SelfDevice.h': '#pragma once\nnamespace SelfDevice {\n' +
                               decl(r'const char\* firmwareVersion\(', self_device, 'SelfDevice::firmwareVersion') +
                               '\n}\n',
        'System_Settings.h': '#pragma once\n#include <Arduino.h>\nstruct Settings {\n  bool bleConsoleOnPipe = true;\n'
                             '  String phonePipeSecret;\n  bool phonePipeSecretGenerated = false;\n};\n'
                             'extern Settings gSettings;\n',
        'Arduino.h': '#pragma once\n#include <cstdint>\n#include <string>\nuint32_t millis();\n'
                     'class String {\n public:\n  String(const char* s = "") : v_(s ? s : "") {}\n'
                     '  const char* c_str() const { return v_.c_str(); }\n'
                     '  unsigned length() const { return unsigned(v_.size()); }\n private:\n  std::string v_;\n};\n',
        'esp_attr.h': '#pragma once\n#define EXT_RAM_BSS_ATTR\n',
        'freertos/FreeRTOS.h': '\n'.join([
            '#pragma once', '#include <cstdint>',
            'typedef uint32_t TickType_t; typedef int BaseType_t; typedef unsigned UBaseType_t;',
            '#define pdTRUE 1', '#define pdFALSE 0', '#define portMAX_DELAY 0xffffffffu',
            '#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))',
            'struct HostTask; typedef HostTask* TaskHandle_t;',
            'struct HostQueue; typedef HostQueue* QueueHandle_t;',
            'struct HostSem; typedef HostSem* SemaphoreHandle_t;',
            'struct StaticSemaphore_t { unsigned char storage[80]; };',
            'struct portMUX_TYPE { int held; };', '#define portMUX_INITIALIZER_UNLOCKED {0}',
            'void hostEnterCritical(portMUX_TYPE*); void hostExitCritical(portMUX_TYPE*);',
            '#define portENTER_CRITICAL(m) hostEnterCritical(m)', '#define portEXIT_CRITICAL(m) hostExitCritical(m)',
        ]) + '\n',
        'freertos/semphr.h': '#pragma once\n#include "freertos/FreeRTOS.h"\n'
                             'SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t*);\n'
                             'BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);\n'
                             'BaseType_t xSemaphoreGive(SemaphoreHandle_t);\n'
                             'TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t);\n',
        'freertos/task.h': '#pragma once\n#include "freertos/FreeRTOS.h"\n'
                           'TaskHandle_t xTaskGetCurrentTaskHandle();\nvoid vTaskDelay(TickType_t);\n',
        'freertos/queue.h': '#pragma once\n#include "freertos/FreeRTOS.h"\n'
                            'UBaseType_t uxQueueSpacesAvailable(QueueHandle_t);\n',
    }


def stage(component: Path, directory: Path) -> Path:
    src = directory / 'src'
    (src / 'freertos').mkdir(parents=True)
    for name in REAL:
        shutil.copyfile(component / name, src / name)
    for name, text in fake_headers(component).items():
        (src / name).write_text(text)
    shutil.copyfile(HERE / 'phone_console_harness.cpp', src / 'phone_console_harness.cpp')
    return src


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('c++'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--component', type=Path, help='a copy of components/hardwareone (mutation runs)')
    args = parser.parse_args()
    component = (args.component or COMPONENT).resolve()
    require(bool(args.cxx), 'a host C++17 compiler is required')
    check_sources(component)
    flags = ['-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer', '-g']
    with tempfile.TemporaryDirectory(prefix='hw1-phone-console-') as directory:
        src = stage(component, Path(directory))
        binary = Path(directory) / 'console'
        subprocess.run([args.cxx, *flags, '-I', str(src), str(src / 'phone_console_harness.cpp'), '-o', str(binary)],
                       check=True, timeout=300)
        output = subprocess.check_output([str(binary)], text=True, timeout=300)
        lines = output.strip().splitlines()
        require(lines and lines[-1].startswith('phone console: '), f'harness summary missing: {output[-400:]}')
        print(lines[-1])


if __name__ == '__main__':
    main()
