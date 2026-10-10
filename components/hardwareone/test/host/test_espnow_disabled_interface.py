#!/usr/bin/env python3
"""Compile the production disabled ESP-NOW interface without WiFi header leakage.

Both disabled source variants use the real public header and build configuration.
The unchanged enabled MAC cache/getters are extracted intact for a focused runtime
check; platform error types, mutex operations and the WiFi driver are fixtures.
"""

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block


COMPONENT = Path(__file__).resolve().parents[2]
REPO = COMPONENT.parents[1]
CORE = COMPONENT.parent / "arduino" / "cores" / "esp32"

DRIVER = r'''
#pragma once
#include "esp_err.h"
#include <cstdint>
#define HW1_TEST_WIFI_DRIVER_INCLUDED 1
#if !ENABLE_WIFI
#error A disabled network implementation must not require the WiFi driver
#endif
enum wifi_interface_t { WIFI_IF_STA, WIFI_IF_AP };
esp_err_t esp_wifi_get_mac(wifi_interface_t interface, uint8_t out[6]);
'''

HARNESS = r'''
#include <cassert>
#include <cstring>
#include <type_traits>
#if TEST_ENABLED_CACHE
#include "esp_wifi.h"
static int criticalDepth = 0;
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(mux) ((void)(mux), ++criticalDepth)
#define taskEXIT_CRITICAL(mux) ((void)(mux), --criticalDepth)
// INSERT_ENABLED_MAC_IMPLEMENTATION
#else
#include "System_ESPNow.h"
#include "System_ESPNow.h"
static_assert(!ENABLE_ESPNOW);
static_assert(ENABLE_WIFI == TEST_EXPECT_WIFI);
#ifdef HW1_TEST_WIFI_DRIVER_INCLUDED
#error The disabled public header must not import the WiFi driver
#endif
#if ENABLE_WIFI
#include "esp_wifi.h"
#endif
#endif

using MacGetter = esp_err_t (*)(uint8_t*);
static_assert(std::is_same_v<decltype(&espnowSelfStaMac), MacGetter>);
static_assert(std::is_same_v<decltype(&espnowSelfApMac), MacGetter>);
#if ENABLE_WIFI
static int driverCalls = 0;
static esp_err_t driverResult = ESP_OK;
esp_err_t esp_wifi_get_mac(wifi_interface_t interface, uint8_t out[6]) {
#if TEST_ENABLED_CACHE
  assert(criticalDepth == 0);
#endif
  assert(out);
  ++driverCalls;
  if (driverResult == ESP_OK) memset(out, interface == WIFI_IF_STA ? 0x31 : 0x42, 6);
  return driverResult;
}
#endif

int main() {
  assert(espnowSelfStaMac(nullptr) == ESP_ERR_INVALID_ARG);
  assert(espnowSelfApMac(nullptr) == ESP_ERR_INVALID_ARG);
  uint8_t sta[6], ap[6];
  memset(sta, 0xAA, sizeof(sta));
  memset(ap, 0xBB, sizeof(ap));
#if ENABLE_WIFI
  assert(driverCalls == 0);
  assert(espnowSelfStaMac(sta) == ESP_OK);
  assert(espnowSelfApMac(ap) == ESP_OK);
  assert(driverCalls == 2);
  for (int i = 0; i < 6; ++i) assert(sta[i] == 0x31 && ap[i] == 0x42);
  driverResult = ESP_ERR_WIFI_NOT_INIT;
  assert(espnowSelfStaMac(sta) == ESP_ERR_WIFI_NOT_INIT);
  assert(espnowSelfApMac(ap) == ESP_ERR_WIFI_NOT_INIT);
  for (int i = 0; i < 6; ++i) assert(sta[i] == 0x31 && ap[i] == 0x42);
#if TEST_ENABLED_CACHE
  const uint8_t cachedSta[6] = {1, 2, 3, 4, 5, 6};
  const uint8_t cachedAp[6] = {6, 5, 4, 3, 2, 1};
  setEspNowSelfMacCache(cachedSta, cachedAp);
  assert(espnowSelfStaMac(sta) == ESP_OK && espnowSelfApMac(ap) == ESP_OK);
  assert(memcmp(sta, cachedSta, 6) == 0 && memcmp(ap, cachedAp, 6) == 0);
  assert(driverCalls == 4); // Hosted radio is not queried while cache is valid.
  setEspNowSelfMacCache(nullptr, cachedAp);
  assert(espnowSelfStaMac(sta) == ESP_ERR_WIFI_NOT_INIT);
  assert(espnowSelfApMac(ap) == ESP_OK && driverCalls == 5);
  assert(criticalDepth == 0);
#endif
#else
  assert(espnowSelfStaMac(sta) == ESP_ERR_NOT_SUPPORTED);
  assert(espnowSelfApMac(ap) == ESP_ERR_NOT_SUPPORTED);
  for (int i = 0; i < 6; ++i) assert(sta[i] == 0 && ap[i] == 0);
#endif
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    if not args.cxx:
        raise SystemExit("a host C++17 compiler is required")
    production = (COMPONENT / "System_ESPNow.cpp").read_text()
    cache_start = production.index("static portMUX_TYPE gEspNowSelfMacLock")
    cache_end = production.index("// A null argument invalidates that cache.", cache_start)
    enabled_mac = production[cache_start:cache_end] + "\n".join(
        extract_block(production, marker) for marker in (
            "static void setEspNowSelfMacCache(",
            "esp_err_t espnowSelfStaMac(", "esp_err_t espnowSelfApMac("))

    with tempfile.TemporaryDirectory(prefix="hw1-espnow-interface-") as temp:
        work = Path(temp)
        (work / "Arduino.h").write_text('#pragma once\n#include <cstddef>\n#include <WString.h>\n')
        (work / "esp_attr.h").write_text("#pragma once\n#define EXT_RAM_BSS_ATTR\n")
        (work / "esp_err.h").write_text(
            "#pragma once\nusing esp_err_t = int;\n"
            "constexpr esp_err_t ESP_OK=0, ESP_ERR_INVALID_ARG=0x102, "
            "ESP_ERR_NOT_SUPPORTED=0x106, ESP_ERR_WIFI_NOT_INIT=0x3001;\n")
        (work / "esp_wifi.h").write_text(DRIVER)
        (work / "sdkconfig.h").write_text(
            '#define CONFIG_IDF_TARGET "esp32s3"\n#define CONFIG_IDF_TARGET_ESP32S3 1\n')
        unit = work / "probe.cpp"
        executable = work / "probe"
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-parameter", "-pedantic", "-I", str(work),
                   "-I", str(COMPONENT), "-I", str(CORE)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        for wifi in (0, 1):
            profile = work / "features.h"
            profile.write_text(f"#undef NETWORK_FEATURE_LEVEL\n#define NETWORK_FEATURE_LEVEL {wifi}\n")
            unit.write_text(HARNESS)
            subprocess.run(command + ["-DTEST_ENABLED_CACHE=0", f"-DTEST_EXPECT_WIFI={wifi}",
                "-DARDUINO_XIAO_ESP32S3_DEV=1",
                f'-DHW1_BOARD_CONFIG_HEADER="{REPO / "boards/xiao_s3.features.h"}"',
                f'-DHW1_FEATURE_CONFIG_HEADER="{profile}"',
                str(COMPONENT / "System_ESPNow.cpp"), str(unit), "-o", str(executable)],
                check=True, timeout=120)
            subprocess.run([str(executable)], check=True, timeout=60)
        unit.write_text(HARNESS.replace("// INSERT_ENABLED_MAC_IMPLEMENTATION", enabled_mac))
        subprocess.run(command + ["-DTEST_ENABLED_CACHE=1", "-DENABLE_WIFI=1",
                                  str(unit), "-o", str(executable)], check=True, timeout=120)
        subprocess.run([str(executable)], check=True, timeout=60)
    print("ESP-NOW interface: WiFi-only, networking-off and enabled MAC-cache paths passed")


if __name__ == "__main__":
    main()
