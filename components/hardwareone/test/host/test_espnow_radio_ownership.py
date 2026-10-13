#!/usr/bin/env python3
"""ESP-NOW's radio-ownership predicate and the Wi-Fi decisions that use it.

deinitEspNow clears `initialized` before its bounded waits, and a close that
times out keeps espnow_task, espnow_tx and the esp_now driver alive with only
the close flag set. System_ESPNow exports one answer for "does ESP-NOW hold
the radio" (espnowOwnsRadio: initialized, or a close still pending), with an
always-false stub when ESP-NOW is compiled out; isEspNowInitialized keeps its
meaning.

The production predicate, isEspNowInitialized and espnowShutdownPending are
compiled unchanged against a scripted close sequence, and the disabled-build
stub from the public header is compiled and run. A source contract checks that
the five System_WiFi radio decisions (the connect hunt's mode, closewifi's
power-down, radiopower off, the targeted connect's driver restart and the lazy
Wi-Fi init) ask the predicate rather than isEspNowInitialized or a local
`extern`. The connect behaviour itself runs in wifi_connect_budget.
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

HARNESS = r'''
#include <atomic>
#include <cstdio>
#include <cstdlib>

static int checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); std::exit(1); } } while (0)

struct EspNowState { bool initialized = false; };
static EspNowState* gEspNow = nullptr;
static std::atomic<bool> gEspNowDeinitInProgress{false};

// INSERT_PRODUCTION

namespace disabled {
// INSERT_DISABLED_STUB
}

int main() {
  CHECK(!isEspNowInitialized() && !espnowOwnsRadio());          // never started
  EspNowState state;
  gEspNow = &state;
  CHECK(!espnowOwnsRadio());
  state.initialized = true;                                     // openespnow
  CHECK(isEspNowInitialized() && espnowOwnsRadio());
  // closeespnow: the flag first, then `initialized` cleared before the waits.
  gEspNowDeinitInProgress.store(true);
  CHECK(espnowOwnsRadio());
  state.initialized = false;
  // A wait timed out: the runtime and driver stay; only the flag says so.
  CHECK(!isEspNowInitialized() && espnowShutdownPending() && espnowOwnsRadio());
  gEspNowDeinitInProgress.store(false);                         // retry finished
  CHECK(!isEspNowInitialized() && !espnowOwnsRadio());
  CHECK(!disabled::espnowOwnsRadio());                          // compiled out
  std::printf("espnow radio ownership: %d checks passed\n", checks);
  return 0;
}
'''

# System_WiFi.cpp radio decisions that must ask the owner's predicate.
WIFI_DECISIONS = (
    "bool connectToBestWiFiNetwork(",
    "const char* cmd_wifidisconnect(",
    "static RadioPowerOffResult radioPowerOff(",
    "static bool connectWiFiTargetWithin(const WifiConnectionTarget& nw",
    "bool ensureWiFiInitialized(",
)
# radiopower off records whether ESP-NOW was RUNNING, to restore exactly that
# on power-on; a pending close is not something to restart.
RESTORE_SNAPSHOT = "gRadioPrevEspNow = isEspNowInitialized();"


def wifi_contract(wifi: str) -> None:
    require('#include "System_ESPNow.h"' in wifi,
            "System_WiFi.cpp includes the ESP-NOW header for espnowOwnsRadio")
    require(not re.search(r"extern\s+bool\s+espnow(?:OwnsRadio|ShutdownPending)\s*\(", wifi),
            "System_WiFi.cpp declares no local extern for the ESP-NOW radio state")
    for marker in WIFI_DECISIONS:
        require(wifi.count(marker) == 1, f"System_WiFi.cpp: one {marker}")
        body = extract_block(wifi, marker).replace(RESTORE_SNAPSHOT, "")
        body = re.sub(r"/\*.*?\*/|//[^\n]*", "", body, flags=re.S)  # code only
        require("espnowOwnsRadio()" in body,
                f"{marker} decides radio ownership with espnowOwnsRadio()")
        require("isEspNowInitialized()" not in body and "espnowShutdownPending()" not in body,
                f"{marker} still decides from isEspNowInitialized()/espnowShutdownPending()")


def production(espnow: str, header: str) -> tuple[str, str]:
    for marker in ("bool isEspNowInitialized(", "bool espnowShutdownPending(", "bool espnowOwnsRadio("):
        require(espnow.count(marker) == 1, f"System_ESPNow.cpp: one {marker}")
    enabled, _, disabled = header.partition("#else // !ENABLE_ESPNOW")
    require(bool(disabled), "System_ESPNow.h keeps its disabled-build block")
    require(re.search(r"(?m)^bool espnowOwnsRadio\(\);", enabled) is not None,
            "System_ESPNow.h declares espnowOwnsRadio() for enabled builds")
    stub = re.search(r"(?m)^inline bool espnowOwnsRadio\(\)[^\n]*\}", disabled)
    require(stub is not None, "System_ESPNow.h stubs espnowOwnsRadio() when ESP-NOW is compiled out")
    code = "\n\n".join(extract_block(espnow, marker) for marker in (
        "bool isEspNowInitialized(", "bool espnowShutdownPending(", "bool espnowOwnsRadio("))
    return code, stub[0]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="components/hardwareone directory to read production sources from")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component
    wifi_contract((component / "System_WiFi.cpp").read_text())
    code, stub = production((component / "System_ESPNow.cpp").read_text(),
                            (component / "System_ESPNow.h").read_text())
    unit_text = HARNESS.replace("// INSERT_PRODUCTION", code).replace("// INSERT_DISABLED_STUB", stub)
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-espnow-radio-owner-") as temporary:
        unit, binary = Path(temporary) / "test.cpp", Path(temporary) / "test"
        unit.write_text(unit_text)
        subprocess.run([args.cxx, *flags, str(unit), "-o", str(binary)], check=True, timeout=120)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
