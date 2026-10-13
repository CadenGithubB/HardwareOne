#!/usr/bin/env python3
"""Exercise production G2/OLED logging selection renderers and input handlers.

The shared catalog, UI functions, row declarations and command-cookie type
come from production. Display, input, logger reads and executor boundaries
are stubbed; owner mutation and authorization are covered separately.

A second unit (sensorlog_config_list_harness.cpp) draws the OLED "Configure"
list with the shipping scroll code (OLEDScrollState, the scroll primitives and
both renderers from OLED_Utils) through the real handleLoggingModeInput and
render block, and checks every frame against the 128x64 panel: rows inside the
content area, every glyph on the panel, a visible scroll indicator while rows
are hidden, and as many rows as the content area holds. `--component <dir>`
runs both units against saved sources (the base tree draws the list in
split-pane mode with listWidth 128, so its scroll arrows land at x=128).
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

SCROLL_FUNCTIONS = ("void oledScrollInit(", "bool oledScrollAddItem(", "void oledScrollClear(",
                    "void oledScrollClampSelection(", "void oledScrollUp(", "void oledScrollDown(",
                    "bool oledScrollHandleNav(", "void oledScrollSetSplitPane(",
                    "void oledScrollRender(", "void oledScrollRenderSimple(")


def logging_types(read) -> list[str]:
    owner = read("System_SensorLogging.h")
    oled_utils = read("OLED_Utils.h")
    catalog_start = owner.index("#define LOG_THERMAL")
    count_start = owner.index("inline constexpr size_t kSensorLogSensorCount")
    catalog = owner[catalog_start:owner.index(";", count_start) + 1]
    return [catalog,
            extract_block(owner, "enum SensorLogFormat") + ";",
            extract_block(oled_utils, "enum LoggingMenuState") + ";"]


def oled_logging(read) -> list[str]:
    """Row constants, the input handler and the config-list render block."""
    oled = read("OLED_Mode_Logging.cpp")
    declarations = oled[oled.index("static constexpr int loggingIntervalRow"):
                        oled.index("// Helper to draw menu items")]
    render_case = extract_block(oled, "case LOG_MENU_SENSOR_CONFIG:")
    return [declarations,
            extract_block(oled, "static bool handleLoggingModeInput("),
            "static void renderSensorConfig() { switch (loggingCurrentState) {\n" +
            render_case + "\ndefault: break;\n} }"]


def splice(harness: str, inserts) -> str:
    for marker, body in inserts:
        require(harness.count(marker) == 1, f"bad marker: {marker}")
        harness = harness.replace(marker, body)
    return harness


def generated(read) -> str:
    g2 = read("G2_Glasses.cpp")
    public = read("G2_Glasses.h")
    bridge = read("G2_HijackCmd.h")
    types = logging_types(read)
    types[2:2] = [extract_block(public, "enum G2HijackPage") + ";",
                  extract_block(bridge, "struct G2CmdCookie") + ";"]
    g2_declarations = g2[g2.index("#define G2_LOG_ROW_LEN"):
                         g2.index("// Rows: 0=\"<- System\"")]
    oled = oled_logging(read)
    production = [g2_declarations, oled[0],
                  extract_block(g2, "static size_t g2BuildLoggingRows("),
                  extract_block(g2, "static void g2LoggingHandleTap("),
                  *oled[1:]]
    return splice((HERE / "sensorlog_ui_harness.cpp").read_text(),
                  (("// INSERT_TYPES", "\n\n".join(types)),
                   ("// INSERT_PRODUCTION", "\n\n".join(production))))


def generated_config_list(read) -> str:
    utils_header = read("OLED_Utils.h")
    utils = read("OLED_Utils.cpp")
    cap = re.search(r"(?m)^#define OLED_SCROLL_MAX_ITEMS\s+\d+\s*$", utils_header)
    require(cap is not None, "OLED_Utils.h: OLED_SCROLL_MAX_ITEMS missing")
    types = logging_types(read) + [cap.group(0),
                                   extract_block(utils_header, "struct OLEDScrollItem") + ";",
                                   extract_block(utils_header, "struct OLEDScrollState") + ";"]
    scroll = []
    for marker in SCROLL_FUNCTIONS:
        require(utils.count(marker) == 1, f"OLED_Utils.cpp: marker missing or not unique: {marker}")
        scroll.append(extract_block(utils, marker))
    return splice((HERE / "sensorlog_config_list_harness.cpp").read_text(),
                  (("// INSERT_TYPES", "\n\n".join(types)),
                   ("// INSERT_SCROLL", "\n\n".join(scroll)),
                   ("// INSERT_PRODUCTION", "\n\n".join(oled_logging(read)))))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="production component directory (point at a saved tree for a base run)")
    parser.add_argument("--source-dir", type=Path,
                        help="overlay directory; a file present here wins over --component")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")

    def read(name: str) -> str:
        overlay = args.source_dir / name if args.source_dir else None
        return (overlay if overlay and overlay.exists() else args.component / name).read_text()

    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-Wno-unused-parameter"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    failed = []
    with tempfile.TemporaryDirectory(prefix="hw1-sensorlog-ui-") as directory:
        for name, source in (("selection", generated(read)),
                             ("config-list", generated_config_list(read))):
            unit, executable = Path(directory) / f"{name}.cpp", Path(directory) / name
            unit.write_text(source)
            subprocess.run([args.cxx, *flags, str(unit), "-o", str(executable)],
                           check=True, timeout=120)
            if subprocess.run([str(executable)], timeout=60).returncode != 0:
                failed.append(name)
    if failed:
        raise SystemExit("sensorlog UI: FAILED (" + ", ".join(failed) + ")")


if __name__ == "__main__":
    main()
