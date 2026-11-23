#!/usr/bin/env python3
"""Execute APDS, IMU, ToF and RTC production snapshots, JSON and OLED on host.

Only RTOS admission, display and ToF device I/O are substituted. --baseline
compares full serialized/display/poll output to captured pre-migration sources.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
SENSORS = [('apds', 'apds9960', 'ApdsCache', 'Apds', 'APDS'),
           ('imu', 'HAL_Motion', 'ImuCache', 'Imu', 'IMU'),
           ('tof', 'vl53l4cx', 'TofCache', 'Tof', 'TOF'),
           ('rtc', 'ds3231', 'RtcCache', 'Rtc', 'RTC')]


def source(file: str, suffix: str) -> str:
    """The owner's file name: a HAL layer is named as it is (HAL_Motion.cpp, HAL_Motion_OLED.h)."""
    if file.startswith('HAL_'):
        return file + ('_OLED.h' if suffix == '_oled.h' else suffix)
    return f'i2csensor_{file}{suffix}'


def generate(component: Path, baseline: bool) -> str:
    mutex = (COMPONENT / 'System_Mutex.cpp').read_text()
    keys = component / 'System_SensorJsonKeys.h'  # the builders' format strings use it
    types = [keys.read_text().replace('#pragma once\n', '') if keys.exists() else '',
             extract_block((COMPONENT / 'System_Mutex.h').read_text(), 'struct SensorCacheGuard') + ';',
             extract_block((component / 'i2csensor_ds3231.h').read_text(), 'struct RtcDateTime') + ';']
    state, functions = [], [extract_block(mutex, 'SensorCacheGuard::SensorCacheGuard('),
                           extract_block(mutex, 'SensorCacheGuard::~SensorCacheGuard('),
                           extract_block((COMPONENT / 'System_I2C.cpp').read_text(), 'int sensorEnvelopeBegin(')]
    for short, file, typename, title, flag in SENSORS:
        if baseline and file.startswith('HAL_'):
            file = 'bno055'  # the IMU's files before the motion layer
        owner = (component / source(file, '.cpp')).read_text()
        header = (component / source(file, '.h')).read_text()
        types.append(extract_block(header, f'struct {typename}') + ';')
        if baseline:
            state.append(f'{typename} g{title}Cache{{}};\n#define {short.upper()} g{title}Cache\n'
                         f'#define {short.upper()}_MUTEX g{title}Cache.mutex')
        else:
            for name in [f's{title}Cache', f's{title}CacheMutex']:
                state.append(re.search(r'(?m)^(?:EXT_RAM_BSS_ATTR )?static[^\n]*\b' + name + r'\b[^\n]*;', owner)[0])
            state.append(f'#define {short.upper()} s{title}Cache\n#define {short.upper()}_MUTEX s{title}CacheMutex')
            state.append(re.search(r'(?m)^bool ' + short + r'CacheSnapshot\([^\n]*;', header)[0])
            functions += [extract_block(owner, f'bool {short}CacheSnapshot('),
                          extract_block(owner, f'static bool {short}InvalidateCache(')]
            if short != 'rtc': functions.append(extract_block(owner, f'size_t {short}CacheStorageBytes('))
            start = extract_block(owner, f'bool {short}StartInternal()')
            reset = extract_block(start, '// Clean up any stale cache')
            functions.append(f'static bool reset{title}Reading() {{\n' + reset + '\nreturn true; }')
            require('SemaphoreHandle_t' not in types[-1], f'{typename} exposes synchronization')
            require(not re.search(r'extern.*\bg' + title + r'Cache\b', header), f'{short} live cache is public')
        functions.append(extract_block(owner, f'int {short}BuildDataJson('))
        oled = {'apds':'void displayApdsData()', 'imu':'static void displayImuActions()',
                'tof':'static void displayTofData()', 'rtc':'static void displayRtcData()'}[short]
        functions.append(extract_block((component / source(file, '_oled.h')).read_text(), oled))
        if short == 'apds':
            functions += [extract_block(owner, marker) for marker in ['uint8_t apdsGetProximity(',
                          *[f'uint16_t apdsGetColor{c}(' for c in 'RGBC']]]
        if short == 'rtc':
            functions += [extract_block(owner, marker) for marker in ['static int rtcDaysInMonth(',
                         'static int rtcDayOfWeek(', 'RtcDateTime rtcLocalTime(',
                         *[f'int rtcGet{p}(' for p in ['Year','Month','Day','Hour','Minute','Second']],
                         'float rtcGetTemperature(', 'static void rtcReadAndCacheOnce(']]
        if short == 'tof': functions.append(extract_block(owner, 'bool tofPoll()'))
    # RTC display calls rtcLocalTime before its definition in the generated unit.
    state.append('RtcDateTime rtcLocalTime(const RtcDateTime*);')
    return ((HERE / 'small_sensor_snapshot_harness.cpp').read_text()
            .replace('// INSERT_TYPES', '\n'.join(types)).replace('// INSERT_STATE', '\n'.join(state))
            .replace('// INSERT_FUNCTIONS', '\n\n'.join(functions)))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('c++'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--baseline', type=Path)
    args = parser.parse_args()
    require(bool(args.cxx), 'a host C++17 compiler is required')
    flags = ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-parameter']
    if args.sanitize: flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
    with tempfile.TemporaryDirectory(prefix='hw1-small-sensor-snapshot-') as directory:
        work = Path(directory)
        def run(name, source, extra=()):
            path = work / (name + '.cpp'); path.write_text(source)
            subprocess.run([args.cxx, *flags, *extra, str(path), '-o', str(work / name)], check=True, timeout=60)
            return subprocess.check_output([str(work / name)], text=True, timeout=60)
        # -I: the OLED IMU page reads the pure motion header (HAL_MotionCore.h).
        output = run('current', generate(COMPONENT, False), ['-DTEST_BASELINE=0', '-I', str(COMPONENT)])
        fixture = HERE / 'fixtures/small_sensor_snapshot_expected.json'
        require(output == ''.join(json.loads(fixture.read_text())), 'characterized sensor output changed')
        if args.baseline:
            old = run('baseline', generate(args.baseline, True), ['-DTEST_BASELINE=1'])
            require(output == old, 'full output differs from original production sources')
            print('APDS/IMU/ToF/RTC full JSON, OLED and ToF poll output match baseline')
        (work / 'Arduino.h').write_text('#include <cstdint>\n#include <cstddef>\nclass String;\n')
        (work / 'System_BuildConfig.h').write_text('\n'.join(f'#define ENABLE_{x[4]}_SENSOR 0' for x in SENSORS))
        (work / 'freertos').mkdir()
        (work / 'freertos/FreeRTOS.h').write_text('#pragma once\nusing TaskHandle_t=void*; using UBaseType_t=unsigned;\n')
        (work / 'freertos/task.h').write_text('#include "FreeRTOS.h"\n')
        disabled = ['#include <cassert>']
        for short, file, typename, title, flag in SENSORS:
            shutil.copyfile(COMPONENT / source(file, '.h'), work / source(file, '.h'))
            disabled.append(f'#include "{source(file, ".h")}"')
        disabled.append('int main() {')
        for short, _, typename, _, _ in SENSORS:
            field = {'apds':'apdsDataValid','imu':'imuDataValid','tof':'tofDataValid','rtc':'dataValid'}[short]
            disabled.append(f'''{{ {typename} out{{}}; out.{field}=true;
 assert(!{short}CacheSnapshot(out) && !out.{field}); out.{field}=true;
 assert(!{short}CacheSnapshot(out, 50) && !out.{field});
 char b[8]="filled"; assert({short}BuildDataJson(b,sizeof(b))==0 && !b[0]);
 assert({short}BuildDataJson(nullptr,2)==0); b[0]='Z';
 assert({short}BuildDataJson(b,0)==0 && b[0]=='Z'); }}''')
            if short != 'rtc': disabled.append(f'assert({short}CacheStorageBytes()==0);')
        disabled.append('}')
        run('disabled', '\n'.join(disabled), ['-I', str(work)])
    print('Small sensors: production snapshots, lock budgets, invalidation, serializers, OLED, ToF polling and disabled headers passed')

if __name__ == '__main__': main()
