#!/usr/bin/env python3
"""Characterize actual sensor presentation code across available/invalid/busy reads.

Sensor owner APIs are mocked here; their synchronization has separate owner tests.
The pre-change consumer functions can be run with --baseline for byte comparison.
"""
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
SENSORS = [
    ('bno055', 'ImuCache', 'gImuCache', 'imuCacheSnapshot'),
    ('vl53l4cx', 'TofCache', 'gTofCache', 'tofCacheSnapshot'),
    ('apds9960', 'APDSCache', 'gApdsCache', 'apdsCacheSnapshot'),
    ('ds3231', 'RTCCache', 'gRtcCache', 'rtcCacheSnapshot'),
    ('mlx90640', 'ThermalCache', 'gThermalCache', 'thermalCacheSnapshot'),
    ('seesaw', 'InputCache', 'gInputCache', 'inputCacheSnapshot'),
    ('ano_encoder', 'AnoEncoderCache', 'gAnoEncoderCache', 'anoEncoderCacheSnapshot'),
    ('rda5807', 'FMRadioCache', 'gFmRadioCache', 'fmRadioCacheSnapshot'),
]
FORMATTERS = ['imuG2FormatValue', 'tofG2FormatValue', 'thermalG2FormatValue',
              'apdsG2FormatValue', 'gamepadG2FormatValue', 'anoG2FormatValue',
              'rtcG2FormatValue', 'fmG2FormatValue', 'g2BuildGamepadArt',
              'g2BuildAnoArt', 'g2BuildRtcReadout', 'g2BuildImuReadout',
              'g2BuildTofReadout', 'g2BuildApdsReadout', 'g2BuildFmReadout']


def generate(component, baseline):
    types, state, mocks, constants = [], [], [], []
    for chip, typ, glob, api in SENSORS:
        text = (component / f'i2csensor_{chip}.h').read_text()
        if typ == 'RTCCache':
            types.append(extract_block(text, 'struct RTCDateTime') + ';')
        if typ == 'InputCache' and 'struct InputCache' not in text:
            text = (component / 'HAL_Input.h').read_text()
        types.append(extract_block(text, f'struct {typ}') + ';')
        state.append(f'{typ} {glob}{{}};')
        mocks.append(f'''bool {api}({typ}& out, uint32_t ms=10) {{
  assert(ms == 5 || ms == 10 || ms == 50);
  assert(!held); out={{}}; if (!admit) return false; out={glob}; return true;
}}''')
        if chip in ('seesaw', 'ano_encoder'):
            text = (component / f'i2csensor_{chip}.h').read_text()
            constants += re.findall(r'^#define (?:GAMEPAD_BUTTON_\w+|ANO_\w+) +[^\\\n]*$', text, re.M)
    g2 = (component / 'G2_Page_Sensors.cpp').read_text()
    constants.append(re.search(r'static const char\* kRtcDow\[8\] = \{.*?\};', g2, re.S)[0])
    functions = [extract_block(g2, 'static void ' + name + '(')
                 for name in ['gpCell', 'anoCell', *FORMATTERS]]
    functions.append(extract_block((component / 'Bluetooth.cpp').read_text(),
                                   'static void buildSensorDataJSON('))
    functions.append(extract_block((component / 'G2_Glasses.cpp').read_text(),
                                   'static void buildFmTunerReadout('))
    logs = (component / 'System_SensorLogging.cpp').read_text()
    types.append(extract_block((COMPONENT / 'System_SensorLogging.h').read_text(),
                               'struct SensorCacheSnapshot') + ';')
    blocks = '\n'.join(extract_block(logs, f'\n    if (mask & LOG_{sensor})')
                       for sensor in ['THERMAL', 'TOF', 'IMU', 'GAMEPAD', 'APDS'])
    functions.append('SensorCacheSnapshot logReadings(unsigned mask) { SensorCacheSnapshot snap{};\n' + blocks + '\nreturn snap; }')
    automation_source = (component / 'System_Automation.cpp').read_text()
    automation_source = automation_source[automation_source.index('bool evaluateCondition(const char* condition) {'):]
    automation = extract_block(automation_source, 'bool evaluateCondition(')
    branches = ' else '.join(extract_block(automation, f'if (strcmp(sensor, "{sensor}") == 0)')
                             for sensor in ['TEMP', 'DISTANCE', 'LIGHT', 'MOTION'])
    comparisons = automation[automation.index('  // Evaluate condition'):]
    functions.append('bool sensorCondition(const char* sensor, const char* op, char* value) {\n'
                     'float currentValue=0; bool isNumeric=true; char currentStringValue[32]="";\n' +
                     branches + ' else { return false; }\n' + comparisons)
    reset_mutex = '\n'.join(f'{glob}.mutex = &semaphore;' for _, _, glob, _ in SENSORS) if baseline else ''
    return ((HERE / 'sensor_reading_consumers_harness.cpp').read_text()
            .replace('// TYPES', '\n'.join(types))
            .replace('// CONSTANTS', '\n'.join(constants))
            .replace('// STATE', '\n'.join(state))
            .replace('// MOCKS', '\n'.join(mocks))
            .replace('// FUNCTIONS', '\n\n'.join(functions))
            .replace('// RESET_MUTEX', reset_mutex)
            .replace('// FORMATTERS', ', '.join(FORMATTERS)))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--baseline', type=Path)
    p.add_argument('--record-baseline', action='store_true')
    args = p.parse_args()
    flags = ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unused-function']
    if args.sanitize: flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
    with tempfile.TemporaryDirectory(prefix='hw1-sensor-consumers-') as d:
        work = Path(d)
        def run(component, baseline):
            source = work / ('baseline.cpp' if baseline else 'current.cpp')
            source.write_text(generate(component, baseline))
            binary = source.with_suffix('')
            subprocess.run([args.cxx, *flags, f'-DTEST_BASELINE={int(baseline)}', str(source), '-o', str(binary)], check=True)
            return subprocess.check_output([str(binary)], text=True)
        fixture = HERE / 'fixtures/sensor_reading_consumers_expected.json'
        if args.record_baseline:
            require(args.baseline is not None, 'recording requires the preserved baseline')
            fixture.write_text(json.dumps(run(args.baseline, True).splitlines(True), indent=2) + '\n')
        current = run(COMPONENT, False)
        require(current == ''.join(json.loads(fixture.read_text())), 'Sensor consumer presentation changed')
        if args.baseline:
            require(current == run(args.baseline, True), 'Sensor consumer baseline comparison failed')
    print('Sensor consumers: 15 G2 formatters, BLE, logging and FM readout match; formatting outside locks')

if __name__ == '__main__': main()
