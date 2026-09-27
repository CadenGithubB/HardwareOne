#!/usr/bin/env python3
"""Exercise actual power policy, failure handling, labels and JSON off-device.

Run: python3 experiments/p4_ble_roles/tests/test_power_profile.py
Set HW1_POWER_TEST_APP to test another isolated app copy.
Uses the real ArduinoJson headers and extracts unmodified source functions.
It does not access boards, build firmware or edit any application source.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

APP = Path(os.environ.get('HW1_POWER_TEST_APP',
    str(Path(__file__).resolve().parents[1] / 'private/app'))).resolve()
HW = APP / 'components/hardwareone'


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class PowerProfileTests(unittest.TestCase):
    def test_profiles_and_refused_changes(self):
        power = (HW / 'System_Power.cpp').read_text()
        body = power[power.index('struct PowerModeConfig'):power.index('void checkAutoPowerMode()')]
        body += '\n' + function(power, 'void buildPowerJson(')
        g2 = (HW / 'G2_Page_Power.cpp').read_text()
        rows = g2[g2.index('#define POWER_ROW_LEN'):g2.index('// -----------------------------------------------------------------------------', g2.index('#define POWER_ROW_LEN'))]
        body += '\n' + rows + '\n' + function(g2, 'static size_t buildCpuRows(')
        oled = (HW / 'OLED_Mode_Power.cpp').read_text()
        body += '\n' + function(oled, 'static void populatePowerCpuMenu(')
        stubs = r'''
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <vector>
#include <ArduinoJson.h>
#include "System_Power.h"
#define EXT_RAM_BSS_ATTR
#define DEBUG_SYSTEMF(...) ((void)0)
#define INFO_SYSTEMF(...) ((void)0)
#define ERROR_SYSTEMF(...) ((void)0)
#define SYSEVT_POWER_MODE_CHANGED 1
struct Settings {
  uint8_t powerMode = 0;
  uint8_t oledBrightness = 255;
  bool powerAutoMode = true;
  uint8_t powerBatteryThreshold = 100, powerDisplayDimLevel = 100;
  uint32_t powerSaveTimeoutMinutes = UINT32_MAX, powerTransitionCooldownMs = UINT32_MAX;
} gSettings;
static uint32_t current = EXPECT_MAX;
static bool acceptSet = true, wrongReadback = false;
static int setCalls = 0, events = 0, batteryEvents = 0;
uint32_t getCpuFrequencyMhz() { return current; }
bool setCpuFrequencyMhz(uint32_t mhz) {
  ++setCalls;
  if (!acceptSet) return false;
  current = wrongReadback ? mhz - 1 : mhz;
  return true;
}
uint32_t getXtalFrequencyMhz() { return 40; }
uint32_t getApbFrequency() { return 80000000; }
unsigned long millis() { return UINT32_MAX; }
unsigned long powerSaveLastActivityMs() { return 0; }
bool powerSleepTransitionAllowed(unsigned long* remaining) { *remaining = UINT32_MAX; return false; }
bool otaSafetyIsPendingVerification() { return true; }
template <typename T, typename U> void setSetting(T& target, U value) { target = value; }
void systemEventPost(int, const char*) { ++events; }
void batteryLogEvent(const char*) { ++batteryEvents; }
struct Scroll { std::vector<const char*> labels; } sPowerCpuScroll;
void initPowerScrollStates() {}
void oledScrollClearKeepSelection(Scroll* s) { s->labels.clear(); }
void oledScrollAddItem(Scroll* s, const char* label) { s->labels.push_back(label); }
void oledScrollClampSelection(Scroll*) {}
'''
        checks = r'''
int main() {
  assert(getPowerCpuFrequencyCount() == 3);
  const uint32_t expected[] = {EXPECT_FLOOR, EXPECT_BALANCED, EXPECT_MAX};
  for (size_t i = 0; i < 3; ++i) {
    assert(getPowerCpuFrequencyMhz(i) == expected[i]);
    assert(isPowerCpuFrequencySupported(expected[i]));
  }
  assert(getPowerCpuFrequencyMhz(3) == 0);
  assert(!isPowerCpuFrequencySupported(0));
  assert(!isPowerCpuFrequencySupported(40));
  assert(!isPowerCpuFrequencySupported(EXPECT_REJECT));
  assert(getPowerModeActiveCpuFreq(0) == EXPECT_MAX);
  assert(getPowerModeActiveCpuFreq(1) == EXPECT_BALANCED);
  assert(getPowerModeActiveCpuFreq(2) == EXPECT_FLOOR);
  assert(getPowerModeActiveCpuFreq(3) == EXPECT_FLOOR);
  assert(getPowerModeIdleCpuFreq(3) == 40);
  assert(getPowerModeActiveCpuFreq(4) == EXPECT_MAX);
  assert(getPowerModeIdleCpuFreq(4) == EXPECT_MAX);
  assert(getPowerModeCpuFreq(255) == EXPECT_MAX);
  assert(!applyPowerMode(255) && events == 0 && setCalls == 0);
  current = EXPECT_FLOOR; acceptSet = false;
  assert(!applyPowerMode(0));
  assert(current == EXPECT_FLOOR && events == 0 && batteryEvents == 0);
  assert(gSettings.oledBrightness == 255);
  acceptSet = true; wrongReadback = true;
  assert(!applyPowerMode(1));
  assert(events == 0 && batteryEvents == 0);
  wrongReadback = false;
  assert(applyPowerMode(1));
  assert(current == EXPECT_BALANCED && events == 1 && batteryEvents == 1);
  assert(gSettings.oledBrightness == 204);
  assert(buildCpuRows() == POWER_MODE_COUNT + 1);
  for (size_t i = 1; i <= POWER_MODE_COUNT; ++i) {
    const char* row = gRowPtrs[i];
    assert(std::strlen(row) < POWER_ROW_LEN);
    assert(std::strcmp(row + std::strlen(row) - 3, "MHz") == 0);
  }
  char maxRow[40]; std::snprintf(maxRow, sizeof(maxRow), "[X] Perf %u/%uMHz", EXPECT_MAX, EXPECT_FLOOR);
  assert(std::strcmp(gRows[1], maxRow) == 0);
  populatePowerCpuMenu();
  assert(sPowerCpuScroll.labels.size() == POWER_MODE_COUNT);
  for (const auto* label : sPowerCpuScroll.labels) {
    assert(std::strlen(label) > 3);
    assert(std::strcmp(label + std::strlen(label) - 3, "MHz") == 0);
  }
  JsonDocument doc; buildPowerJson(doc);
  assert(doc["supportedCpuMhz"].size() == 3);
  assert(doc["interactiveFloorMhz"].as<unsigned>() == EXPECT_FLOOR);
  assert(doc["modes"][0]["activeMhz"].as<unsigned>() == EXPECT_MAX);
  assert(doc["modes"][4]["idleMhz"].as<unsigned>() == EXPECT_MAX);
  const size_t bytes = measureJson(doc);
  assert(bytes < 1024);
  char json[1024]; assert(serializeJson(doc, json, sizeof(json)) == bytes);
  JsonDocument roundTrip; assert(!deserializeJson(roundTrip, json));
  std::printf("PASS profile max=%u floor=%u JSON=%zu bytes\n", EXPECT_MAX, EXPECT_FLOOR, bytes);
}
'''
        compiler = shutil.which('c++')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory(prefix='hw1-power-test-') as temp:
            work = Path(temp)
            (work / 'Arduino.h').write_text('#include <cstdint>\n#include <cstddef>\n')
            (work / 'test.cpp').write_text(stubs + '\n' + body + '\n' + checks)
            profiles = [
                ('s3', [], 80, 160, 240, 400),
                ('p4-rev3', ['-DCONFIG_IDF_TARGET_ESP32P4=1'], 100, 200, 400, 240),
                ('p4-older', ['-DCONFIG_IDF_TARGET_ESP32P4=1', '-DCONFIG_ESP32P4_SELECTS_REV_LESS_V3=1'], 90, 180, 360, 400),
            ]
            for name, flags, floor, balanced, maximum, reject in profiles:
                with self.subTest(profile=name):
                    binary = work / name
                    command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                               '-Wno-unused-function', '-I'+str(work), '-I'+str(HW),
                               '-I'+str(APP / 'components/hardwareone_libs/ArduinoJson/src'),
                               f'-DEXPECT_FLOOR={floor}', f'-DEXPECT_BALANCED={balanced}',
                               f'-DEXPECT_MAX={maximum}', f'-DEXPECT_REJECT={reject}',
                               *flags, str(work/'test.cpp'), '-o', str(binary)]
                    result = subprocess.run(command, text=True, capture_output=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    result = subprocess.run([str(binary)], text=True, capture_output=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    print(result.stdout.strip())


if __name__ == '__main__':
    unittest.main()
