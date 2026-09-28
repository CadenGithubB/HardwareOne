#!/usr/bin/env python3
"""Execute real shared battery policy and extracted OTA gate under host faults.

Hardware/SDK boundaries are the only mocks. This does not certify divider
accuracy, resistor population, charger behavior, or an attached battery.
"""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('c++'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    if not args.cxx:
        parser.error('a C++17 host compiler is required')
    flags = ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-g']
    with tempfile.TemporaryDirectory(prefix='hw1-battery-host-') as directory:
        work = Path(directory)
        def run(name: str, source: Path, defines: tuple[str, ...] = ()) -> None:
            binary = work / name
            subprocess.run([args.cxx, *flags, '-I', str(COMPONENT), *defines,
                            str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
        run('policy', HERE / 'test_battery_policy.cpp')
        # Compile the actual board-selection header. A generic P4 must not
        # inherit EYE wiring, nor should a Sense board acquire a nonexistent ADC.
        neutral = work / 'battery-board-features.h'
        neutral.write_text('#undef ENABLE_LLM_SOURCE_CM5\n#define ENABLE_LLM_SOURCE_CM5 0\n'
                           '#undef ENABLE_RASPBERRY_PI_HOST_POWER\n#define ENABLE_RASPBERRY_PI_HOST_POWER 0\n'
                           '#undef ENABLE_RASPBERRY_PI_HOST_FAN\n#define ENABLE_RASPBERRY_PI_HOST_FAN 0\n')
        boards = (
            ('eye', ('HW_BOARD_P4X_EYE=1', 'CONFIG_IDF_TARGET_ESP32P4=1'), 49, 1, 1, 0, -1, '1332.0f / 332.0f', 6),
            ('eye-disabled-marker', ('HW_BOARD_P4X_EYE=0', 'ARDUINO_XIAO_ESP32S3_DEV=1'), -1, 0, 0, 0, -1, '2.0f', 12),
            ('p4-generic', ('CONFIG_IDF_TARGET_ESP32P4=1',), -1, 0, 0, 0, -1, '2.0f', 12),
            ('feather-esp32', ('ARDUINO_ADAFRUIT_FEATHER_ESP32_V2_DEV=1',), 35, 1, 1, 0, -1, '2.0f', 12),
            ('xiao-sense', ('ARDUINO_XIAO_ESP32S3_DEV=1',), -1, 0, 0, 0, -1, '2.0f', 12),
            ('feathers3-gauge', ('ARDUINO_UM_FEATHERS3_DEV=1',), -1, 1, 0, 1, 34, '2.0f', 12),
        )
        for name, definitions, pin, enabled, adc, gauge, vbus, divider, attenuation in boards:
            board_source = work / f'board-{name}.cpp'
            board_source.write_text(f'''#include <cassert>
#include <cmath>
#define ADC_ATTEN_DB_6 6
#define ADC_ATTEN_DB_12 12
#include "System_BuildConfig.h"
static_assert(BATTERY_ADC_PIN == {pin});
static_assert(ENABLE_BATTERY_MONITOR == {enabled});
static_assert(BATTERY_BACKEND_ADC == {adc});
static_assert(BATTERY_BACKEND_FUEL_GAUGE == {gauge});
static_assert(BATTERY_VBUS_SENSE_PIN == {vbus});
static_assert(BATTERY_ADC_ATTEN == {attenuation});
int main() {{ assert(std::fabs(BATTERY_ADC_DIVIDER - ({divider})) < 0.00001f); }}
''')
            run(f'board-{name}', board_source, tuple('-D' + item for item in definitions) +
                (f'-DHW1_DEPLOYMENT_CONFIG_HEADER="{neutral}"', '-Wno-cpp', '-Wno-pedantic'))
        print('Battery board configurations: EYE, generic P4, classic Feather, XIAO Sense, FeatherS3 passed')
        source = (COMPONENT / 'System_Battery.cpp').read_text()
        backend = source[source.index('BatteryState gBatteryState;'):source.index('void buildBatteryJson(')]
        calibration = extract_block(source, 'const char* cmd_battery_calibrate(')
        backend_harness = (HERE / 'battery_backend_harness.cpp').read_text()
        generated_backend = work / 'battery_backend.cpp'
        generated_backend.write_text(backend_harness.replace('// INSERT_PRODUCTION_BACKEND_AND_ACCESSORS', backend)
                                     .replace('// INSERT_PRODUCTION_CALIBRATION', calibration)
                                     .replace('// INSERT_PRODUCTION_JSON', extract_block(source, 'void buildBatteryJson(')))
        variants = (
            ('p4-curve', 1, 1, 0, 49, '1332.0f/332.0f', 6, 1, 0, 1100, -1, 1, 0),
            ('s3-curve', 1, 1, 0, 2, '2.0f', 12, 1, 0, 1100, -1, 0, 1),
            ('esp32-line', 1, 1, 0, 35, '2.0f', 12, 0, 1, 1100, -1, 0, 7),
            ('esp32-line-strict', 1, 1, 0, 35, '2.0f', 12, 0, 1, 0, -1, 0, 7),
            ('s3-gauge', 1, 0, 1, -1, '2.0f', 12, 1, 0, 1100, 34, 0, 1),
            ('disabled', 0, 0, 0, -1, '2.0f', 12, 1, 0, 1100, -1, 0, 1),
        )
        keys = ('ENABLE_BATTERY_MONITOR', 'BATTERY_BACKEND_ADC', 'BATTERY_BACKEND_FUEL_GAUGE',
                'BATTERY_ADC_PIN', 'BATTERY_ADC_DIVIDER', 'BATTERY_ADC_ATTEN',
                'ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED', 'ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED',
                'BATTERY_ADC_DEFAULT_VREF_MV', 'BATTERY_VBUS_SENSE_PIN', 'EXPECTED_UNIT', 'EXPECTED_CHANNEL')
        for variant in variants:
            name, *values = variant
            definitions = tuple(f'-D{key}={value}' for key, value in zip(keys, values))
            run('backend-' + name, generated_backend,
                definitions + (f'-DCONFIG_IDF_TARGET_ESP32={int(name.startswith("esp32"))}',
                               '-pthread', '-Wno-unused-function', '-Wno-unused-variable',
                               '-I', str(COMPONENT.parent / 'hardwareone_libs/ArduinoJson/src')))
        gate = extract_block((COMPONENT / 'System_OTA.cpp').read_text(),
                             'bool powerIsSafe(')
        harness = (HERE / 'battery_ota_harness.cpp').read_text()
        marker = '// INSERT_PRODUCTION_OTA_POWER_GATE'
        assert harness.count(marker) == 1
        generated = work / 'battery_ota.cpp'
        generated.write_text(harness.replace(marker, gate))
        for enabled, adc in ((0, 0), (1, 0), (1, 1)):
            run(f'ota-{enabled}-{adc}', generated,
                (f'-DENABLE_BATTERY_MONITOR={enabled}', f'-DBATTERY_BACKEND_ADC={adc}'))


if __name__ == '__main__':
    main()
