#!/usr/bin/env python3
"""Run the shipping Wi-Fi connect hunt against a scripted driver and clock.

connectToBestWiFiNetwork, connectWiFiId, connectWiFiTarget and the attempt
function come from System_WiFi.cpp unchanged, with the hunt's bounds helpers
and the boot budget derivation when the tree has them. The driver (status
sequence per saved network), millis()/delay() (a virtual clock), the serial
console and the command context are doubles. On a tree without the bounded
hunt the runner compiles the old entry points behind the same adapters, so the
budget, grace and console-origin checks fail behaviourally rather than at
compile time.
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

FORWARD_DECLARATIONS = re.compile(
    r"static bool connectWiFi(?:TargetWithin|Target|Id)\([^;{]*\);")


def generate() -> str:
    source = (COMPONENT / "System_WiFi.cpp").read_text()
    header = (COMPONENT / "System_WiFi.h").read_text()
    bounded = "WifiConnectOrigin" in header and "struct WifiConnectBounds" in source
    # The attempt helpers sit after the global-flag definitions; slicing there
    # keeps the forward declarations near the top from matching their markers.
    tail = source[source.index("volatile bool gWifiUserCancelled = false;"):]

    support = []
    if bounded:
        support.append(extract_block(header, "enum class WifiConnectOrigin") + ";")
        support.append(re.search(r"^constexpr uint32_t kWifiConnectHuntBudgetMs = \d+;$",
                                 header, re.MULTILINE).group())
        support.append(extract_block(source, "struct WifiConnectBounds") + ";")
        support.extend(re.findall(r"^static constexpr uint32_t kWifi\w+Ms = \d+;$", source, re.MULTILINE))
        support.append(extract_block(source, "static bool wifiConsoleCancelApplies("))
        support.append(extract_block(source, "static WifiConnectBounds wifiConnectBounds("))
        support.append(extract_block(source, "uint32_t wifiBootConnectBudgetMs("))
        support.append("#define HW1_WIFI_CONNECT_BUDGET_API 1")
        support.append("bool connectToBestWiFiNetwork(uint32_t budgetMs, WifiConnectOrigin origin);")
        support.append("static bool huntBest(uint32_t budgetMs, WifiConnectOrigin origin) "
                       "{ return connectToBestWiFiNetwork(budgetMs, origin); }")
    else:
        support.append("enum class WifiConnectOrigin : uint8_t { COMMAND = 0, BOOT };")
        support.append("constexpr uint32_t kWifiConnectHuntBudgetMs = 150000;")
        support.append("#define HW1_WIFI_CONNECT_BUDGET_API 0")
        support.append("bool connectToBestWiFiNetwork();")
        support.append("static bool huntBest(uint32_t, WifiConnectOrigin) "
                       "{ return connectToBestWiFiNetwork(); }")
    declarations = FORWARD_DECLARATIONS.findall(source)
    require(len(declarations) >= 2, "System_WiFi.cpp forward-declares the connect helpers")
    support.extend(declarations)

    hunt = extract_block(source, "bool connectToBestWiFiNetwork(")
    attempt = [extract_block(tail, "static const char* wifiStatusToString("),
               extract_block(tail, "static void wifiEnsureIdle("),
               extract_block(tail, "static bool connectWiFiId("),
               extract_block(tail, "static bool connectWiFiTarget(const WifiConnectionTarget& nw")]
    if bounded:
        attempt.append(extract_block(tail, "static bool connectWiFiTargetWithin(const WifiConnectionTarget& nw"))

    harness = (HERE / "wifi_connect_budget_harness.cpp").read_text()
    for marker in ("// INSERT_PRODUCTION_SUPPORT", "// INSERT_PRODUCTION_HUNT", "// INSERT_PRODUCTION_ATTEMPT"):
        require(harness.count(marker) == 1, f"one {marker} marker")
    return (harness.replace("// INSERT_PRODUCTION_SUPPORT", "\n\n".join(support))
                   .replace("// INSERT_PRODUCTION_HUNT", hunt)
                   .replace("// INSERT_PRODUCTION_ATTEMPT", "\n\n".join(attempt)))


def check_boot_callers() -> None:
    """Connects that run on the boot task pass the BOOT origin, so a console
    keystroke can still skip them; command-context resolution would find no
    context there and leave the console unread."""
    boot = (COMPONENT / "HardwareOne.cpp").read_text()
    require("setupWiFi(wifiBootConnectBudgetMs(), WifiConnectOrigin::BOOT);" in boot,
            "the boot connect passes the trial-aware budget and the BOOT origin")
    setup = (COMPONENT / "System_FirstTimeSetup.cpp").read_text()
    require(setup.count("WifiConnectOrigin::BOOT") == 2,
            "both first-time-setup connects (restore and final) pass the BOOT origin")
    require("    setupWiFi();\n" not in setup and "    connectToBestWiFiNetwork();  //" not in setup,
            "no first-time-setup connect falls back to the command origin")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--keep", type=Path, help="write the generated unit here as well")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    check_boot_callers()
    unit_text = generate()
    if args.keep:
        args.keep.write_text(unit_text)
    with tempfile.TemporaryDirectory(prefix="hw1-wifi-connect-budget-") as temporary:
        unit, binary = Path(temporary) / "test.cpp", Path(temporary) / "test"
        unit.write_text(unit_text)
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-I", str(HERE / "web_batch_stubs"), str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=120)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
        require(result.returncode == 0, f"wifi connect budget failed:\n{result.stdout}\n{result.stderr}")
        count = re.search(r"(?m)^checks=(\d+)$", result.stderr)
        require(count is not None, "missing completed check count")
        print(f"{result.stdout.strip()} ({count[1]} checks)", flush=True)


if __name__ == "__main__":
    main()
