#!/usr/bin/env python3
"""Run serial, OLED and session-owned CLI Wi-Fi wizard page transitions.

Input/scans and shared navigation are controlled boundaries. The three shipping
adapters must accept/skip/back through that navigation without finishing early;
setup_menu_layout separately runs the real navigation/registry/board matrix.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def generate() -> str:
    setup = (COMPONENT / "System_SetupWizard.cpp").read_text()
    oled = (COMPONENT / "OLED_SetupWizard.cpp").read_text()
    mode = (COMPONENT / "System_SetupWizardMode.cpp").read_text()
    header = (COMPONENT / "System_SetupWizard.h").read_text()
    types = "\n".join([
        extract_block(header, "enum SetupWizardPage") + ";",
        extract_block(header, "struct SetupWizardResult") + ";",
        extract_block(mode, "enum class WizardSubMode") + ";",
        extract_block(mode, "static struct") + " sWizard;",
        extract_block(mode, "struct WizardRender") + ";",
        extract_block((COMPONENT / "System_CLIMode.h").read_text(), "enum CLIModeInputResult") + ";",
    ])
    functions = "\n\n".join([
        extract_block(setup, "bool wizardHandleCancelInput("),
        extract_block(setup, "static String waitForWizardInput("),
        extract_block(setup, "static String waitForWizardSecret("),
        extract_block(setup, "static void handleSerialWiFiPage("),
        extract_block(oled, "bool renderWiFiPage("),
        extract_block(mode, "static WizardSubMode subModeForPage("),
        extract_block(mode[mode.rindex("static CLIModeInputResult dispatchWiFi("):],
                      "static CLIModeInputResult dispatchWiFi("),
        extract_block(mode, "static CLIModeInputResult wizardMode_onInput("),
    ])
    # This existing String shim contains no setup policy.
    string = extract_block((HERE / "setup_mqtt_application_harness.cpp").read_text(), "class String") + ";"
    harness = (HERE / "setup_wifi_navigation_harness.cpp").read_text()
    for marker, replacement in (("// INSERT_STRING", string), ("// INSERT_TYPES", types),
                                ("// INSERT_PRODUCTION", functions)):
        require(harness.count(marker) == 1, f"bad insertion marker: {marker}")
        harness = harness.replace(marker, replacement)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="hw1-setup-wifi-") as temporary:
        directory = Path(temporary)
        unit = directory / "wifi.cpp"
        unit.write_text(generate())
        for wifi in (0, 1):
            binary = directory / f"wifi-{wifi}"
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-Wno-unused-function", "-Wno-unused-parameter", f"-DENABLE_WIFI={wifi}",
                       str(unit), "-o", str(binary)]
            if args.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                                "-fno-omit-frame-pointer", "-g"]
            subprocess.run(command, check=True, timeout=120)
            subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
