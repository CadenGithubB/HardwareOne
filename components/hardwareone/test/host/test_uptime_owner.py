#!/usr/bin/env python3
"""Uptime has one owner, SelfDevice::uptimeSeconds(), and it does not restart at 49.7 days.

`millis() / 1000` starts again at 0 when millis() wraps. The owner returned
exactly that, and 21 other places divided millis() themselves: heartbeat and
capability fields, status screens and JSON, the command audit stamp, I2C rates
(a division by it), the automation UPTIME sensor. 18 of them ask the owner
now; 3 are fallbacks for a wall clock that reads 0 and stay. One more was found
later: uptime_hms in buildSystemInfoJson (status json, /api/system, SSE, the
dashboard) divided a copy of millis(), which the division rule below does not
see. It asks the owner now and has its own rule.

The production uptimeSeconds (System_SelfDevice.cpp) is compiled against a
settable 64-bit boot timer: it agrees with millis() / 1000 up to the wrap and
counts on after it. Source rules: every listed place asks the owner, and the
only `millis() / 1000` left in the firmware are the three file-name and
capture-time fallbacks for a clock that reads 0.

Two more facts of the same facade (System_SelfDevice.cpp) are compiled here,
because no other host test builds them:
- chipTemperatureC, the one chip-temperature reader, built for the classic
  ESP32 and for the other chips: a failed read (NAN) and a value outside
  -40..125 C are no reading, the classic ESP32's raw 128 (53.3 C) is no reading
  on that chip only, and `out` is left alone without one. Source rules:
  temperatureRead() is called in System_SelfDevice.cpp and nowhere else, and
  each of the four askers does the right thing without a reading (they are
  not compiled here): the CHIPTEMP condition is false, `temperature` sends
  both JSON keys as null and a text that does not lead with "Error", the
  glasses status line carries the uptime alone, and the stats feed reports no
  value.
- macBytes, which keeps the first successful non-zero station MAC for the boot:
  a failed read zeroes out[] and is asked again, an all-zero answer is not
  kept, and after the first good answer the driver is not asked again.

Options: --cxx <compiler>, --sanitize (ASan + UBSan), --component <dir> to run
the same cases and rules against another components/hardwareone directory.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
OWNER = "uint32_t uptimeSeconds()"

# (file, calls of SelfDevice::uptimeSeconds() it must hold at least)
ASKERS = (
    ("BLE_Server.cpp", 2),            # the status characteristic and the system event JSON
    ("G2_Glasses.cpp", 1),            # the glasses status page
    ("OLED_Mode_System.cpp", 1),      # the OLED system page
    ("System_Automation.cpp", 1),     # the UPTIME condition sensor
    ("System_ESPNow.cpp", 7),         # heartbeats (v4FillHeartbeat), bond status, capability summary, stats
    ("System_I2C.cpp", 1),            # i2cmetrics rates
    ("System_Utils.cpp", 3),          # the command audit stamp, uptime_hms in buildSystemInfoJson, the uptime reply
)
# A division of the millisecond clock that is allowed to stay: a stand-in for
# wall time when the clock reads 0.
FALLBACK = re.compile(r"if \((?:ts == 0|now <= 0|!Clock::isValidEpoch\(now\))\)")
DIVISION = re.compile(r"\b(?:millis\(\)|now)\s*/\s*(?:1000|60000)(?:UL|u|U|L)?\b")

HARNESS = r"""
#include <cstdint>
#include <cstdio>
#include <string>

static int64_t gBootMicros = 0;
static int64_t esp_timer_get_time() { return gBootMicros; }
// Arduino's millis(): the same timer, cut to 32 bits of milliseconds.
static uint32_t millis() { return static_cast<uint32_t>(gBootMicros / 1000); }

namespace SelfDevice {
// INSERT_PRODUCTION
}

static int failures = 0, checks = 0;
static void check(bool ok, const std::string& what) {
  ++checks;
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

int main() {
  constexpr int64_t kSecond = 1000000;
  constexpr int64_t kDay = 86400 * kSecond;
  constexpr int64_t kWrap = 4294967296LL * 1000;  // millis() returns to 0 here: 49.7 days

  for (int64_t at : {int64_t{0}, 999999 * int64_t{1}, kSecond, 59 * kSecond, kDay, 24 * kDay, 25 * kDay, 49 * kDay,
                     kWrap - 1}) {
    gBootMicros = at;
    check(SelfDevice::uptimeSeconds() == static_cast<uint32_t>(at / kSecond),
          "uptime at " + std::to_string(at / kSecond) + " s");
    check(SelfDevice::uptimeSeconds() == millis() / 1000u, "before the wrap it is millis() / 1000");
  }
  // The wrap of millis(): the uptime counts on.
  gBootMicros = kWrap - kSecond;
  const uint32_t before = SelfDevice::uptimeSeconds();
  gBootMicros = kWrap + kSecond;
  const uint32_t after = SelfDevice::uptimeSeconds();
  check(before == 4294966u && after == 4294968u, "uptime goes from 4294966 s to 4294968 s across the wrap");
  check(millis() / 1000u == 1u, "while millis() / 1000 is back at 1");
  for (int64_t days : {int64_t{50}, int64_t{100}, int64_t{365}, int64_t{3650}}) {
    gBootMicros = days * kDay + 5 * kSecond;
    check(SelfDevice::uptimeSeconds() == static_cast<uint32_t>(days * 86400 + 5),
          "uptime after " + std::to_string(days) + " days");
  }
  // Minutes for the automation UPTIME sensor: 100 days is 144000 minutes.
  gBootMicros = 100 * kDay;
  check(SelfDevice::uptimeSeconds() / 60u == 144000u, "100 days is 144000 minutes");

  if (failures != 0) {
    std::fprintf(stderr, "%d of %d uptime checks failed\n", failures, checks);
    return 1;
  }
  std::printf("uptime owner: %d checks passed\n", checks);
  return 0;
}
"""

# chipTemperatureC and macBytes against a settable sensor and Wi-Fi driver.
# Built twice: with CONFIG_IDF_TARGET_ESP32 (the classic chip) and without.
FACTS_HARNESS = r"""
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "System_MetricsCore.h"

static int failures = 0, checks = 0;
static void check(bool ok, const std::string& what) {
  ++checks;
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

static float gChipTemp = 0.0f;
static int gTempReads = 0;
static float temperatureRead() { ++gTempReads; return gChipTemp; }

using esp_err_t = int;
static constexpr esp_err_t ESP_OK = 0;
static constexpr esp_err_t ESP_FAIL = -1;
enum wifi_interface_t { WIFI_IF_STA };
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
static int gLockDepth = 0, gLockTakes = 0;
static void portENTER_CRITICAL(portMUX_TYPE*) { check(gLockDepth == 0, "the MAC lock does not nest"); ++gLockDepth; ++gLockTakes; }
static void portEXIT_CRITICAL(portMUX_TYPE*) { check(gLockDepth == 1, "the MAC lock is balanced"); --gLockDepth; }
static esp_err_t gMacResult = ESP_FAIL;
static uint8_t gDriverMac[6] = {0, 0, 0, 0, 0, 0};
static int gMacAsks = 0;
static esp_err_t esp_wifi_get_mac(wifi_interface_t, uint8_t* out) {
  check(gLockDepth == 0, "the driver is asked outside the lock (the query may block)");
  ++gMacAsks;
  // A failing driver may leave anything in the buffer.
  if (gMacResult == ESP_OK) std::memcpy(out, gDriverMac, 6); else std::memset(out, 0xEE, 6);
  return gMacResult;
}

namespace SelfDevice {
// INSERT_PRODUCTION
}

static bool reads(float value, float& out) {
  gChipTemp = value;
  return SelfDevice::chipTemperatureC(out);
}

static bool same(const uint8_t* a, const uint8_t* b) { return std::memcmp(a, b, 6) == 0; }

int main() {
  // ---- chip temperature ----
  float out = -999.0f;
  check(reads(42.0f, out) && out == 42.0f, "42 C is a reading");
  check(gTempReads == 1, "one sensor read per ask");
  check(reads(-40.0f, out) && out == -40.0f, "-40 C is a reading");
  check(reads(125.0f, out) && out == 125.0f, "125 C is a reading");
  out = -999.0f;
  check(!reads(NAN, out) && out == -999.0f, "a failed read (NAN) is no reading and leaves out alone");
  check(!reads(INFINITY, out) && out == -999.0f, "infinity is no reading");
  check(!reads(-41.0f, out) && out == -999.0f, "-41 C is no reading");
  check(!reads(126.0f, out) && out == -999.0f, "126 C is no reading");
  const float raw128 = (128.0f - 32.0f) / 1.8f;  // 53.33 C
#ifdef CONFIG_IDF_TARGET_ESP32
  check(!reads(raw128, out) && out == -999.0f, "classic ESP32: raw 128 (53.3 C) is no reading");
#else
  check(reads(raw128, out) && out == raw128, "other chips: 53.3 C is a reading");
#endif
  check(reads((127.0f - 32.0f) / 1.8f, out) && reads((129.0f - 32.0f) / 1.8f, out),
        "raw 127 and raw 129 are readings on every chip");

  // ---- station MAC ----
  static const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
  static const uint8_t first[6] = {0x24, 0x6F, 0x28, 0xA7, 0x00, 0x01};
  static const uint8_t other[6] = {0x24, 0x6F, 0x28, 0xA7, 0x00, 0x02};
  uint8_t mac[6];
  check(!SelfDevice::macBytes(nullptr) && gMacAsks == 0, "no buffer: false, the driver is not asked");

  std::memset(mac, 0xAA, 6);
  check(!SelfDevice::macBytes(mac) && same(mac, zero) && gMacAsks == 1, "a failed read: false, out zeroed");
  std::memset(mac, 0xAA, 6);
  check(!SelfDevice::macBytes(mac) && same(mac, zero) && gMacAsks == 2, "a failed read is not kept: asked again");

  gMacResult = ESP_OK;  // the driver answers, but with no address yet
  std::memset(mac, 0xAA, 6);
  check(SelfDevice::macBytes(mac) && same(mac, zero) && gMacAsks == 3, "an all-zero answer is handed on as before");
  check(SelfDevice::macBytes(mac) && gMacAsks == 4, "an all-zero answer is not kept: asked again");

  std::memcpy(gDriverMac, first, 6);
  std::memset(mac, 0xAA, 6);
  check(SelfDevice::macBytes(mac) && same(mac, first) && gMacAsks == 5, "the first good answer");

  std::memcpy(gDriverMac, other, 6);  // nothing sets the address at run time; the kept one stands
  for (int i = 0; i < 5; ++i) {
    std::memset(mac, 0xAA, 6);
    check(SelfDevice::macBytes(mac) && same(mac, first), "the kept address is returned");
  }
  gMacResult = ESP_FAIL;  // radio down
  std::memset(mac, 0xAA, 6);
  check(SelfDevice::macBytes(mac) && same(mac, first), "the kept address is returned while the driver fails");
  check(gMacAsks == 5, "after the first good answer the driver is not asked again");
  check(gLockDepth == 0 && gLockTakes > 0, "the lock is used and released");

  if (failures != 0) {
    std::fprintf(stderr, "%d of %d device-fact checks failed\n", failures, checks);
    return 1;
  }
  std::printf("device facts (%s): %d checks passed\n",
#ifdef CONFIG_IDF_TARGET_ESP32
              "classic ESP32",
#else
              "other chips",
#endif
              checks);
  return 0;
}
"""
FACTS = ("bool chipTemperatureC(float& out)", "bool macBytes(uint8_t out[6])")


def strip_comments(source: str) -> str:
    # Block comments keep their line breaks, so reported line numbers stay right.
    source = re.sub(r"/\*.*?\*/", lambda match: "\n" * match.group(0).count("\n"), source, flags=re.S)
    return re.sub(r"//[^\n]*", "", source)


def no_reading_failures(component: Path) -> list[str]:
    """What the four askers of chipTemperatureC do when it returns false."""
    failures = []

    def body(name: str, marker: str) -> str:
        return strip_comments(extract_block((component / name).read_text(), marker))

    # Automation: a condition on a temperature that was not read is false.
    # (The marker ends before the brace; the declaration earlier in the file ends in ";".)
    automation = (component / "System_Automation.cpp").read_text()
    defined = automation.find("bool evaluateCondition(const char* condition) {")
    condition = strip_comments(extract_block(automation[max(defined, 0):],
                                             "bool evaluateCondition(const char* condition)"))
    branch = re.search(r'strcmp\(sensor, "CHIPTEMP"\) == 0\) \{(.*?)\} else if \(', condition, flags=re.S)
    if branch is None:
        failures.append("System_Automation.cpp: no CHIPTEMP branch in evaluateCondition")
    elif not re.search(r"if \(!SelfDevice::chipTemperatureC\(tc\)\) return false;\s*currentValue = tc;",
                       branch.group(1)):
        failures.append("System_Automation.cpp: CHIPTEMP does not return false when chipTemperatureC has no reading")

    # `temperature`: both JSON keys stay and are null; the text says so and
    # does not lead with "Error" (the command did not fail).
    command = body("System_Utils.cpp", "const char* cmd_temperature(")
    asked = command.find("const bool haveTemp = SelfDevice::chipTemperatureC(tempC);")
    nulls = command.find('if (!haveTemp) return "{\\"schema\\":1,\\"tempC\\":null,\\"tempF\\":null}";')
    numbers = command.find('\\"tempC\\":%.1f,\\"tempF\\":%.1f}')
    text = command.find('if (!haveTemp) return "ESP32 Internal Temperature:\\n  unavailable";')
    figures = command.find("%.1f°C (%.1f°F)")
    if asked < 0:
        failures.append("System_Utils.cpp: cmd_temperature does not keep the result of chipTemperatureC")
    if not 0 <= asked < nulls < numbers:
        failures.append("System_Utils.cpp: `temperature json` without a reading must send tempC and tempF as null "
                        "before the numbers are formatted")
    if not 0 <= numbers < text < figures:
        failures.append("System_Utils.cpp: `temperature` without a reading must say \"unavailable\" "
                        "before the numbers are formatted")

    # Glasses status line: the uptime alone, no stale or fake temperature.
    status = body("G2_Glasses.cpp", "buildG2StatusSnapshot(char* out, size_t cap)")
    if "const bool haveTemp = SelfDevice::chipTemperatureC(tempC);" not in status or not re.search(
            r'if \(haveTemp\) \{\s*snprintf\(line, sizeof\(line\),\s*"Up %luh%lum - %\.0fC\\n",\s*'
            r'hours, minutes, \(double\)tempC\);\s*\} else \{\s*'
            r'snprintf\(line, sizeof\(line\), "Up %luh%lum\\n", hours, minutes\);\s*\}', status):
        failures.append("G2_Glasses.cpp: the status line must carry the uptime alone (\"Up %luh%lum\\n\") "
                        "when chipTemperatureC has no reading")

    # Stats feed: no value.
    if "if (!SelfDevice::chipTemperatureC(c)) return unavailable();" not in body("System_Metrics.cpp",
                                                                                "Raw readTemperature()"):
        failures.append("System_Metrics.cpp: readTemperature must return unavailable() without a reading")
    return failures


def rule_failures(component: Path) -> list[str]:
    failures = []
    for name, least in ASKERS:
        code = strip_comments((component / name).read_text())
        found = code.count("SelfDevice::uptimeSeconds()")
        if found < least:
            failures.append(f"{name}: {found} call(s) of SelfDevice::uptimeSeconds(), expected at least {least}")
        if '#include "System_SelfDevice.h"' not in code:
            failures.append(f"{name}: does not include System_SelfDevice.h")
    # uptime_hms of the system info document: from the owner, with no
    # millisecond clock in the lines that build it.
    info = strip_comments(extract_block((component / "System_Utils.cpp").read_text(), "void buildSystemInfoJson("))
    field = info.find('doc["uptime_hms"]')
    if field < 0:
        failures.append("System_Utils.cpp: buildSystemInfoJson no longer sets uptime_hms")
    else:
        lines = info[:field].splitlines()[-8:]
        built = "\n".join(lines)
        if "SelfDevice::uptimeSeconds()" not in built:
            failures.append("System_Utils.cpp: uptime_hms in buildSystemInfoJson does not ask SelfDevice::uptimeSeconds()")
        if "millis()" in built:
            failures.append("System_Utils.cpp: uptime_hms in buildSystemInfoJson reads millis()")
        if '"%luh %lum %lus"' not in built:
            failures.append("System_Utils.cpp: uptime_hms no longer has the \"%luh %lum %lus\" text")
    # One chip-temperature reader: only the facade calls the sensor.
    for path in sorted(list(component.glob("*.cpp")) + list(component.glob("*.h"))):
        if path.name == "System_SelfDevice.cpp":
            continue
        if re.search(r"\btemperatureRead\s*\(", strip_comments(path.read_text())):
            failures.append(f"{path.name}: calls temperatureRead(); ask SelfDevice::chipTemperatureC")
    facade = strip_comments((component / "System_SelfDevice.cpp").read_text())
    if len(re.findall(r"\btemperatureRead\s*\(", facade)) != 1:
        failures.append("System_SelfDevice.cpp: expected exactly one temperatureRead() call (in chipTemperatureC)")
    failures += no_reading_failures(component)
    lowest = strip_comments(extract_block(facade, "uint32_t minFreeHeapBytes()"))
    if "hw1InternalMinFreeBytes()" not in lowest or "esp_get_minimum_free_heap_size" in lowest:
        failures.append("System_SelfDevice.cpp: minFreeHeapBytes is not the internal-RAM minimum "
                        "(hw1InternalMinFreeBytes); esp_get_minimum_free_heap_size counts PSRAM")
    for path in sorted(list(component.glob("*.cpp")) + list(component.glob("*.h"))):
        for number, line in enumerate(strip_comments(path.read_text()).splitlines(), 1):
            if DIVISION.search(line) and not FALLBACK.search(line):
                failures.append(f"{path.name}:{number}: seconds or minutes taken from the 32-bit millisecond "
                                f"clock: {line.strip()!r}")
    return failures


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component.resolve()
    owner = (component / "System_SelfDevice.cpp").read_text()
    require(owner.count(OWNER + " {") == 1, "System_SelfDevice.cpp defines uptimeSeconds once")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wno-unused-function"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    failures = []
    with tempfile.TemporaryDirectory(prefix="hw1-uptime-owner-") as directory:
        source = Path(directory) / "uptime_owner.cpp"
        binary = Path(directory) / "uptime_owner"
        source.write_text(HARNESS.replace("// INSERT_PRODUCTION", extract_block(owner, OWNER)))
        if subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)], timeout=180).returncode != 0:
            failures.append("the uptime harness does not compile")
        elif subprocess.run([str(binary)], timeout=60).returncode != 0:
            failures.append("the uptime harness failed")
        if all(owner.count(marker + " {") == 1 for marker in FACTS):
            facts = "\n\n".join(extract_block(owner, marker) for marker in FACTS)
            source = Path(directory) / "device_facts.cpp"
            source.write_text(FACTS_HARNESS.replace("// INSERT_PRODUCTION", facts))
            for chip, define in (("classic ESP32", ["-DCONFIG_IDF_TARGET_ESP32=1"]), ("other chips", [])):
                binary = Path(directory) / ("device_facts_" + ("classic" if define else "other"))
                command = [args.cxx, *flags, *define, "-I", str(component), str(source), "-o", str(binary)]
                if subprocess.run(command, timeout=180).returncode != 0:
                    failures.append(f"the device-facts harness does not compile ({chip})")
                elif subprocess.run([str(binary)], timeout=60).returncode != 0:
                    failures.append(f"the device-facts harness failed ({chip})")
        else:
            failures.append("System_SelfDevice.cpp does not define chipTemperatureC and macBytes once each")
    failures += rule_failures(component)
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        print(f"uptime owner: {len(failures)} failure(s)", file=sys.stderr)
        sys.exit(1)
    print(f"uptime owner: the owner counts past the millis() wrap and {len(ASKERS)} files ask it; "
          "chip temperature and station MAC have one reader each", flush=True)


if __name__ == "__main__":
    main()
