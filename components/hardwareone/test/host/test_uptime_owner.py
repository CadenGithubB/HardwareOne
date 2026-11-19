#!/usr/bin/env python3
"""Uptime has one owner, SelfDevice::uptimeSeconds(), and it does not restart at 49.7 days.

`millis() / 1000` starts again at 0 when millis() wraps. The owner returned
exactly that, and 21 other places divided millis() themselves: heartbeat and
capability fields, status screens and JSON, the command audit stamp, I2C rates
(a division by it), the automation UPTIME sensor. 18 of them ask the owner
now; 3 are fallbacks for a wall clock that reads 0 and stay.

The production uptimeSeconds (System_SelfDevice.cpp) is compiled against a
settable 64-bit boot timer: it agrees with millis() / 1000 up to the wrap and
counts on after it. Source rules: every listed place asks the owner, and the
only `millis() / 1000` left in the firmware are the three file-name and
capture-time fallbacks for a clock that reads 0.

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
    ("System_SensorLogging.cpp", 1),  # the track stamp without GPS time
    ("System_Utils.cpp", 2),          # the command audit stamp (and the status reply)
)
# A division of the millisecond clock that is allowed to stay: a stand-in for
# wall time when the clock reads 0.
FALLBACK = re.compile(r"if \((?:ts == 0|now <= 0)\)")
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


def strip_comments(source: str) -> str:
    # Block comments keep their line breaks, so reported line numbers stay right.
    source = re.sub(r"/\*.*?\*/", lambda match: "\n" * match.group(0).count("\n"), source, flags=re.S)
    return re.sub(r"//[^\n]*", "", source)


def rule_failures(component: Path) -> list[str]:
    failures = []
    for name, least in ASKERS:
        code = strip_comments((component / name).read_text())
        found = code.count("SelfDevice::uptimeSeconds()")
        if found < least:
            failures.append(f"{name}: {found} call(s) of SelfDevice::uptimeSeconds(), expected at least {least}")
        if '#include "System_SelfDevice.h"' not in code:
            failures.append(f"{name}: does not include System_SelfDevice.h")
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
    failures += rule_failures(component)
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        print(f"uptime owner: {len(failures)} failure(s)", file=sys.stderr)
        sys.exit(1)
    print(f"uptime owner: the owner counts past the millis() wrap and {len(ASKERS)} files ask it", flush=True)


if __name__ == "__main__":
    main()
