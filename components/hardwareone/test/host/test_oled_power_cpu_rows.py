#!/usr/bin/env python3
"""Check the OLED Power > CPU rows fit one line on every chip's clock set.

Splices the production power-mode table and getters (System_Power.cpp) and the
row formatter (OLED_Mode_Power.cpp populatePowerCpuMenu) into
oled_power_cpu_rows_harness.cpp, then builds and runs it for the ESP32/S3
clocks, the P4 before revision 3 and the P4 revision 3. "> Performance
400/100MHz" was 24 glyphs on a 21-glyph line, so the rows carry no unit; the
static check here requires the unit in the screen's header title instead, and
that title to fit beside a full battery/notification group (13 glyphs).
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
CLOCK_SETS = {
    "esp32/s3": [],
    "p4 before rev 3": ["-DCONFIG_IDF_TARGET_ESP32P4=1", "-DCONFIG_ESP32P4_SELECTS_REV_LESS_V3=1"],
    "p4 rev 3": ["-DCONFIG_IDF_TARGET_ESP32P4=1"],
}
# Header right side at its widest: "~100%" (30 px), "|" separator (8 px) and
# bell + count (12 px) leave 78 px of a 128 px header for the title.
TITLE_GLYPHS = (128 - 30 - 8 - 12) // 6


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")

    title = re.search(r'case OLED_POWER_CPU:\s*return "([^"]*)";',
                      (COMPONENT / "OLED_Utils.cpp").read_text())
    require(title is not None, "OLED_Utils.cpp: no header title for OLED_POWER_CPU")
    require("MHz" in title.group(1), f"header title {title.group(1)!r} must carry the rows' unit")
    require(len(title.group(1)) <= TITLE_GLYPHS,
            f"header title {title.group(1)!r} is over {TITLE_GLYPHS} glyphs")

    power = (COMPONENT / "System_Power.cpp").read_text()
    definitions = [extract_block(power, "struct PowerModeConfig") + ";",
                   extract_block(power, "static const PowerModeConfig gPowerModes[] =") + ";"]
    definitions += [extract_block(power, marker) for marker in (
        "const char* getPowerModeName(", "uint32_t getPowerModeCpuFreq(",
        "uint32_t getPowerModeActiveCpuFreq(", "uint32_t getPowerModeIdleCpuFreq(",
    )]
    definitions.append(extract_block((COMPONENT / "OLED_Mode_Power.cpp").read_text(),
                                     "static void populatePowerCpuMenu("))
    harness = (HERE / "oled_power_cpu_rows_harness.cpp").read_text()
    harness = harness.replace("// INSERT_PRODUCTION_DEFINITIONS", "\n\n".join(definitions))

    with tempfile.TemporaryDirectory(prefix="hw1-oled-power-cpu-") as directory:
        source = Path(directory) / "test.cpp"
        source.write_text(harness)
        for name, defines in CLOCK_SETS.items():
            binary = Path(directory) / re.sub(r"\W+", "_", name)
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       *defines, "-I", str(HERE / "i2c_stubs"), "-I", str(COMPONENT),
                       str(source), "-o", str(binary)]
            if args.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-g"]
            subprocess.run(command, check=True)
            print(f"{name}:", flush=True)
            subprocess.run([str(binary)], check=True)
    print(f"OLED CPU power rows fit; header title {title.group(1)!r}")


if __name__ == "__main__":
    main()
