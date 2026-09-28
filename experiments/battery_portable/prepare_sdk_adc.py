#!/usr/bin/env python3
"""Build-only IDF fixture containing the actual shared ADC backend.

This does not flash hardware. It verifies IDF API compatibility for both curve
and line fitting independently of HardwareOne's Arduino feature graph. GPIO1
on the S3 fixture is a hypothetical external divider, not XIAO stock wiring.
"""
import argparse
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--target', required=True, choices=['esp32', 'esp32s3', 'esp32p4'])
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
source = root / 'components/hardwareone/System_Battery.cpp'
policy = root / 'components/hardwareone/BatteryPolicy.h'
text = source.read_text()
start = text.index('static adc_oneshot_unit_handle_t sAdcUnit')
end = text.index('\n#if ENABLE_BATTERY_MONITOR && BATTERY_BACKEND_FUEL_GAUGE', start)
backend = text[start:end].rstrip()
assert backend.endswith('#endif'), 'ADC backend extraction marker changed'
backend = backend[:-len('#endif')]
args.output.mkdir(parents=True, exist_ok=True)
main = args.output / 'main'
main.mkdir(exist_ok=True)
(main / 'BatteryPolicy.h').write_bytes(policy.read_bytes())
(args.output / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hw1_adc_api_check)
''')
(main / 'CMakeLists.txt').write_text('''idf_component_register(SRCS "adc_check.cpp" INCLUDE_DIRS "." REQUIRES esp_adc esp_timer esp_rom)
''')
(args.output / 'sdkconfig.defaults').write_text('CONFIG_COMPILER_CXX_EXCEPTIONS=n\nCONFIG_COMPILER_CXX_RTTI=n\n')
pin = {'esp32': 35, 'esp32s3': 1, 'esp32p4': 49}[args.target]
divider = '(1332.0f / 332.0f)' if args.target == 'esp32p4' else '2.0f'
atten = 'ADC_ATTEN_DB_6' if args.target == 'esp32p4' else 'ADC_ATTEN_DB_12'
prefix = f'''#include "BatteryPolicy.h"
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_timer.h>
#include <esp_rom_sys.h>
#include <esp_err.h>
#define BATTERY_ADC_PIN {pin}
#define BATTERY_ADC_DIVIDER {divider}
#define BATTERY_ADC_ATTEN {atten}
#define BATTERY_ADC_DEFAULT_VREF_MV 1100
#define INFO_SYSTEMF(...) ((void)0)
static uint32_t millis() {{ return (uint32_t)(esp_timer_get_time() / 1000); }}
static void delayMicroseconds(uint32_t us) {{ esp_rom_delay_us(us); }}
'''
(main / 'adc_check.cpp').write_text(prefix + backend + '''
extern "C" void app_main() {
  BatteryState state;
  if (adcBackendInit() == ESP_OK) adcBackendSample(state);
  adcBackendDeinit();
}
''')
manifest = {'target': args.target, 'source': str(source.relative_to(root)),
            'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
            'extracted_backend_sha256': hashlib.sha256(backend.encode()).hexdigest(),
            'policy_sha256': hashlib.sha256(policy.read_bytes()).hexdigest(),
            'scope': 'build only; no battery hardware qualification'}
(args.output / 'inputs.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps({'project': str(args.output), **manifest}, indent=2))
