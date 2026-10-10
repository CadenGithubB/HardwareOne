#!/usr/bin/env python3
"""Exercise production G2/OLED logging selection renderers and input handlers.

The shared catalog, UI functions, row declarations and command-cookie type
come from production. Display, input, logger reads and executor boundaries
are stubbed; owner mutation and authorization are covered separately.
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


def generated() -> str:
    owner = (COMPONENT / "System_SensorLogging.h").read_text()
    g2 = (COMPONENT / "G2_Glasses.cpp").read_text()
    oled = (COMPONENT / "OLED_Mode_Logging.cpp").read_text()
    public = (COMPONENT / "G2_Glasses.h").read_text()
    bridge = (COMPONENT / "G2_HijackCmd.h").read_text()
    oled_utils = (COMPONENT / "OLED_Utils.h").read_text()
    catalog_start = owner.index("#define LOG_THERMAL")
    count_start = owner.index("inline constexpr size_t kSensorLogSensorCount")
    catalog = owner[catalog_start:owner.index(";", count_start) + 1]
    types = [catalog,
             extract_block(owner, "enum SensorLogFormat") + ";",
             extract_block(public, "enum G2HijackPage") + ";",
             extract_block(bridge, "struct G2CmdCookie") + ";",
             extract_block(oled_utils, "enum LoggingMenuState") + ";"]
    g2_declarations = g2[g2.index("#define G2_LOG_ROW_LEN"):
                         g2.index("// Rows: 0=\"<- System\"")]
    oled_declarations = oled[oled.index("static constexpr int loggingIntervalRow"):
                             oled.index("// Helper to draw menu items")]
    production = [g2_declarations, oled_declarations,
                  extract_block(g2, "static size_t g2BuildLoggingRows("),
                  extract_block(g2, "static void g2LoggingHandleTap("),
                  extract_block(oled, "static bool handleLoggingModeInput(")]
    render_case = extract_block(oled, "case LOG_MENU_SENSOR_CONFIG:")
    production.append("static void renderSensorConfig() { switch (loggingCurrentState) {\n" +
                      render_case + "\ndefault: break;\n} }")
    harness = (HERE / "sensorlog_ui_harness.cpp").read_text()
    for marker, body in (("// INSERT_TYPES", "\n\n".join(types)),
                         ("// INSERT_PRODUCTION", "\n\n".join(production))):
        require(harness.count(marker) == 1, f"bad marker: {marker}")
        harness = harness.replace(marker, body)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-Wno-unused-parameter"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-sensorlog-ui-") as directory:
        unit, executable = Path(directory) / "test.cpp", Path(directory) / "test"
        unit.write_text(generated())
        subprocess.run([args.cxx, *flags, str(unit), "-o", str(executable)],
                       check=True, timeout=120)
        subprocess.run([str(executable)], check=True, timeout=60)


if __name__ == "__main__":
    main()
