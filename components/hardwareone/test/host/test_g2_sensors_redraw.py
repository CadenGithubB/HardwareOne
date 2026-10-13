#!/usr/bin/env python3
"""Sensors page completions redraw through the lens applier, never inline.

The cam_pwr post-hook (after every camera power operation, from any source)
and the openmic/closemic completion on cmd_exec used to re-render the CAM/MIC
detail on their own task after a level/page check, writing the Sensors level
and hijack page around the render. A tap between that check and the render
(CAM detail "Settings >", or "<- Hardware") then left the lens on the new page
while taps routed to the Sensors DETAIL rows (camera power toggle,
cameraautostart). g2_sensors_redraw_harness.cpp compiles the whole production
page (includes stripped; camera and microphone compiled in) beside a lens
double with swap admission, a generation-checked Redraw queue and task-context
tags, and runs that interleaving: no render on cam_pwr or cmd_exec, the
navigated page keeps its routing, and an unchanged view is still refreshed.
It also covers out-of-range list taps (no CAM-detail "recovery") and the live
view's level committed only after the live page started.

Production sources come from --component (default: this component), so the
same suite replays the regression against a pre-fix tree.
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

from test_g2_page_publish import boundary_types, fill, list_publish_helper
from test_web_batch_handlers import require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
HARNESS = HERE / "g2_sensors_redraw_harness.cpp"


def sensors_prototypes(component: Path) -> str:
    header = (component / "G2_Page_Sensors.h").read_text()
    start = header.index("#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES")
    end = header.index("#else  // !(ENABLE_BLUETOOTH && ENABLE_G2_GLASSES)", start)
    body = header[start:end].split("\n", 1)[1]
    require("void g2SensorsHandleTap(uint32_t idx);" in body, "Sensors tap prototype")
    return body


def sensors_unit(component: Path) -> str:
    page = (component / "G2_Page_Sensors.cpp").read_text()
    for marker in ("static void g2SensorsOnCameraPowerDone(",
                   "static void onMicAutostartRuntimeDone(",
                   "void g2RegisterSensorsCameraPowerHook("):
        require(page.count(marker) == 1, f"G2_Page_Sensors.cpp: one {marker}")
    production = re.sub(r"^#include[^\n]*\n", "", page, flags=re.M)
    return fill(HARNESS, {
        "// INSERT_BOUNDARY_TYPES": boundary_types(component),
        "// INSERT_SENSORS_PROTOTYPES": sensors_prototypes(component),
        "// INSERT_LIST_PUBLISH_HELPER": list_publish_helper(component),
        "// INSERT_SENSORS_PAGE": production,
    })


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="component directory to read production sources from")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component.resolve()
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    # Older trees keep a write-only function-static roster count in
    # g2ShowSensorsMenu, which clang reports (GCC does not); it is not this
    # suite's subject, so only their replays silence that warning.
    if "gSensorsListCount" in (component / "G2_Page_Sensors.cpp").read_text():
        flags.append("-Wno-unused-but-set-variable")
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-g2-sensors-redraw-") as directory:
        unit = Path(directory) / "sensors.cpp"
        executable = Path(directory) / "sensors"
        unit.write_text(sensors_unit(component))
        compiled = subprocess.run([args.cxx, *flags, str(unit), "-o", str(executable)],
                                  timeout=180)
        if compiled.returncode != 0:
            print("G2 Sensors redraw FAILED: compile failed", file=sys.stderr)
            sys.exit(1)
        ran = subprocess.run([str(executable)], timeout=60)
        if ran.returncode != 0:
            print(f"G2 Sensors redraw FAILED: exit {ran.returncode}", file=sys.stderr)
            sys.exit(1)
    print("G2 Sensors redraw: harness passed")


if __name__ == "__main__":
    main()
