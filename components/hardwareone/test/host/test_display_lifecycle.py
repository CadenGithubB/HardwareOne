#!/usr/bin/env python3
"""Compile the real display HAL with controlled I2C, driver and GPIO boundaries.

Like the HAL Bluetooth test, copy the unchanged implementation/header beside
stub dependency headers so quoted project includes resolve to host substitutes.
The checks cover ownership and retry behavior; physical display, bus timing and
rail settling still need hardware qualification.
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


def check_ui_boundary() -> None:
    source = (COMPONENT / "OLED_Utils.cpp").read_text()
    for marker, operation in (
        ("bool initOLEDDisplay()", "displayInit"),
        ("bool earlyOLEDInit()", "displayInit"),
        ("void stopOLEDDisplay()", "displayStop"),
        ("void applyOLEDBrightness()", "displaySetBrightness"),
        ("void oledDisplayOff()", "displaySetPower"),
        ("void oledDisplayOn()", "displaySetPower"),
        ("void oledPrepareForSleep()", "displayPrepareForSleep"),
        ("void oledResumeFromSleep()", "displayResumeFromSleep"),
    ):
        body = extract_block(source, marker)
        require(re.search(r"\b" + operation + r"\s*\(", body) is not None,
                f"{marker} must delegate hardware lifecycle to {operation}")
        require(re.search(r"(?:->\s*(?:begin|end|ssd1306_command)\s*\(|"
                          r"\bi2cProbeAddress\s*\(|\bnew\s+Adafruit_SSD1306\b|"
                          r"\bdigitalWrite\s*\(\s*I2C2_POWER_PIN)", body) is None,
                f"{marker}: hardware initialization/power belongs in HAL_Display")


def compile_and_run(cxx: str, sanitize: bool, display_type: int, power_pin: bool) -> None:
    with tempfile.TemporaryDirectory(prefix="hw1-display-hal-") as directory:
        work = Path(directory)
        for name in ("HAL_Display.cpp", "HAL_Display.h", "HAL_Display_P4EyePixels.h", "OLED_Layout.h"):
            shutil.copy2(COMPONENT / name, work / name)
        for name in ("display_lifecycle_platform.h", "display_lifecycle_harness.cpp"):
            shutil.copy2(HERE / name, work / name)
        for name in ("System_I2C.h", "System_Settings.h", "OLED_Display.h", "Wire.h",
                     "Arduino.h", "Adafruit_SSD1306.h", "HAL_Display_P4Eye.h"):
            (work / name).write_text('#pragma once\n#include "display_lifecycle_platform.h"\n')
        config = """#pragma once
#include <cstdint>
#define DISPLAY_TYPE_NONE 0
#define DISPLAY_TYPE_SSD1306 1
#define DISPLAY_TYPE_ST7789 2
#define DISPLAY_TYPE_ILI9341 3
#define DISPLAY_TYPE_ST7789_P4_EYE 4
"""
        config += f"#define DISPLAY_TYPE {display_type}\n"
        if power_pin:
            config += "#define I2C2_POWER_PIN 39\n"
        (work / "System_BuildConfig.h").write_text(config)
        command = [cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-I", str(work), str(work / "HAL_Display.cpp"),
                   str(work / "display_lifecycle_harness.cpp"), "-o", str(work / "checks")]
        if sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-g"]
        subprocess.run(command, check=True)
        cases = (("0", "3d"), ("0", "3c"), ("1", "3d"), ("1", "3c")) if display_type == 1 else ((),)
        for arguments in cases:
            subprocess.run([str(work / "checks"), *arguments], check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    for display_type, power_pin in ((1, False), (1, True), (4, False), (0, False)):
        compile_and_run(args.cxx, args.sanitize, display_type, power_pin)
    check_ui_boundary()
    print("display_lifecycle: SSD1306, power-gated SSD1306, P4 canvas and disabled builds pass")


if __name__ == "__main__":
    main()
