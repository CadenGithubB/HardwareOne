#!/usr/bin/env python3
"""Execute display wizard cancellation through production input loops.

The OLED keyboard, Wi-Fi picker, first-page buttons and CLI display tick are
shipping functions. Rendering, input delivery, Wi-Fi scans and the CLI exit
boundary are controlled; timeout and cancellation state use production code.
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


def generate() -> str:
    setup = (COMPONENT / "System_SetupWizard.cpp").read_text()
    header = (COMPONENT / "System_SetupWizard.h").read_text()
    oled = (COMPONENT / "OLED_SetupWizard.cpp").read_text()
    first = (COMPONENT / "OLED_FirstTimeSetup.cpp").read_text()
    mode = (COMPONENT / "System_SetupWizardMode.cpp").read_text()
    utils = (COMPONENT / "System_Utils.cpp").read_text()
    types = "\n\n".join([
        extract_block(header, "enum SetupWizardPage") + ";",
        extract_block(header, "enum WizardMenuItemType") + ";",
        extract_block(header, "struct WizardMenuItem") + ";",
        extract_block(header, "struct SetupWizardResult") + ";",
        extract_block(mode, "enum class WizardSubMode") + ";",
        extract_block(mode, "static struct") + " sWizard;",
        extract_block((COMPONENT / "OLED_SetupWizard.h").read_text(), "struct JoystickNav") + ";",
    ])
    state = "\n".join(re.search(r"(?m)^static volatile[^\n]*\b" + name + r"\b[^\n]*;", utils)[0]
                      for name in ("sSerialWaitTimeoutMs", "sSerialWaitLastActivityMs", "sWizardCancelRequested"))
    time_functions = "\n\n".join(extract_block(utils, marker) for marker in (
        "void setSerialWaitTimeout(", "void requestWizardCancel(",
        "void noteWizardInputActivity(", "bool isWizardCancelRequested(",
        "String waitForSerialInputBlocking("))
    time_functions += "\n" + extract_block(setup, "bool wizardHandleCancelInput(")
    cache_start = first.index("static constexpr int FTS_WIFI_MENU_MAX")
    cache_end = first.index("bool getOLEDWiFiSelection(", cache_start)
    functions = "\n\n".join([
        extract_block(first, "static bool isOLEDAvailable("),
        extract_block(first, "static uint32_t waitForButtonPress("),
        extract_block(first, "static uint32_t waitForRetryOrSkip("),
        extract_block(first, "String getOLEDTextInput("),
        "#if ENABLE_WIFI\n" + first[cache_start:cache_end],
        extract_block(first, "bool getOLEDWiFiSelection("),
        extract_block(oled, "static bool wizardNextRequested("),
        extract_block(oled, "bool handleWizardPageInput("),
        extract_block(mode, "static WizardSubMode subModeForPage("),
        extract_block(mode, "static void wizardMode_onTick("),
    ])
    harness = (HERE / "setup_display_cancel_harness.cpp").read_text()
    for marker, replacement in (("// INSERT_TYPES", types), ("// INSERT_TIMEOUT_STATE", state),
                                ("// INSERT_TIMEOUT_FUNCTIONS", time_functions),
                                ("// INSERT_PRODUCTION", functions)):
        require(harness.count(marker) == 1, f"bad harness marker: {marker}")
        harness = harness.replace(marker, replacement)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-setup-display-cancel-") as temporary:
        directory = Path(temporary)
        unit = directory / "display_cancel.cpp"
        unit.write_text(generate())
        for wifi in (0, 1):
            for gpio in (0, 1):
                output = directory / f"display-cancel-wifi{wifi}-gpio{gpio}"
                # Existing OLED_TRANSACTION call sites contain feature #ifs;
                # retain those shipping macro bodies instead of rewriting them.
                command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                           "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-unused-variable",
                           "-DENABLE_OLED_DISPLAY=1", f"-DENABLE_WIFI={wifi}",
                           f"-DENABLE_GPIO_ENCODER={gpio}", str(unit), "-o", str(output)]
                if args.sanitize:
                    command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                                    "-fno-omit-frame-pointer", "-g"]
                subprocess.run(command, check=True, timeout=120)
                subprocess.run([str(output)], check=True, timeout=30)


if __name__ == "__main__":
    main()
