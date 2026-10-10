#!/usr/bin/env python3
"""Compile production Map HTTP/OLED routing, snapshots and web session delivery.

The HTTP handler retains authentication, task identity, live actions and track
data responses; only filesystem listing is excluded. OLED retains its guest
guard, complete Tracks action branch and track status drawing. Snapshot/executor
boundaries are doubles; Map lifetime and authorization have their owner suites.
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
    web = (COMPONENT / "WebPage_Maps.cpp").read_text()
    oled = (COMPONENT / "OLED_Mode_Map.cpp").read_text()
    utils = (COMPONENT / "WebServer_Utils.h").read_text()
    command = (COMPONENT / "System_Utils.cpp").read_text()
    maps = (COMPONENT / "System_Maps.h").read_text()
    auth_start = utils.index("#define WEB_AUTH_OR_RETURN(req, ctx)")
    auth_end = utils.index("\n\n", auth_start)
    handler = extract_block(web, "esp_err_t handleGPSTracksAPI(")
    # Keep every query operation verbatim, including stream output. The
    # unrelated directory traversal suffix remains outside this harness.
    handler = handler[:handler.index("  // No query params - list available GPS log files")]
    handler += "\n  return ESP_OK;\n}"
    action = extract_block(oled, "static void executeSubmenuAction(")
    guard = action[:action.index("  char maps[8][96];")]
    branch = action[action.index("    case MENU_CAT_TRACKS:"):action.index("    case MENU_CAT_INFO:")]
    action = guard + "\n  switch (submenuType) {\n" + branch + "  }\n}"
    production = [utils[auth_start:auth_end],
                  extract_block(command, "bool executeUnifiedWebCommand("),
                  extract_block(web, "static esp_err_t handleGPSLiveTrackAction("),
                  handler, action, extract_block(oled, "static void drawTrackStatus(")]
    harness = (HERE / "map_live_ui_harness.cpp").read_text()
    require(harness.count("// INSERT_PRODUCTION") == 1, "missing production marker")
    require(harness.count("// INSERT_TRACK_TYPES") == 1, "missing track types marker")
    types = maps[maps.index("#define MAX_TRACK_POINTS"):maps.index("// GPS Track management")]
    return (harness.replace("// INSERT_TRACK_TYPES", types)
            .replace("// INSERT_PRODUCTION", "\n\n".join(production)))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-Wno-unused-parameter", "-I", str(HERE / "web_batch_stubs"),
             "-I", str(COMPONENT.parent / "hardwareone_libs/ArduinoJson/src")]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-map-live-ui-") as directory:
        unit, binary = Path(directory) / "test.cpp", Path(directory) / "test"
        unit.write_text(generated())
        for gps in (0, 1):
            subprocess.run([args.cxx, *flags, f"-DENABLE_GPS_SENSOR={gps}", str(unit),
                            "-o", str(binary)], check=True, timeout=120)
            subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
