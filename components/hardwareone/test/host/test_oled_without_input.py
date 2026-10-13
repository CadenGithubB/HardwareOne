#!/usr/bin/env python3
"""Compile display state with its real guards and no physical input backend.

The display-state API (OLED_Display.h, NavEvents) and the gamepad input section
of OLED_Utils.cpp compile with ENABLE_OLED_INPUT 0 and stay neutral: no
navigation events, no joystick delta, no pressed buttons, no input processed.
That state carries no bonded data-source selection (removed with the dynamic
remote menu). --component/--source-dir point the suite at another production tree.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
DATA_SOURCE_API = ("DataSource", "gDataSource", "gDataSourceIndicatorVisible", "oledCycleDataSource",
                   "oledGetDataSourceLabel", "oledRemoteSourceAvailable", "invalidateDynamicMenu")


def read_source(component, source_dir, name):
    overlay = source_dir / name if source_dir else None
    path = overlay if overlay and overlay.exists() else component / name
    return path.read_text()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=HERE.parents[1])
    parser.add_argument("--source-dir", type=Path)
    args = parser.parse_args()
    component = args.component.resolve()
    source_dir = args.source_dir.resolve() if args.source_dir else None
    header = read_source(component, source_dir, "OLED_Display.h")
    state_api = header[header.index("struct NavEvents {"):header.index("// Menu navigation state")]
    source = read_source(component, source_dir, "OLED_Utils.cpp")
    start = source.index("// Gamepad Input for OLED Menu Navigation")
    end = source.index("// OLED File Browser (128x64 optimized)", start)
    # Keep the production preprocessor boundaries intact: the original bug
    # compiled this whole API away with the physical controller implementation.
    section = source[start:end]
    leftovers = [name for name in DATA_SOURCE_API
                 if re.search(r"\b" + name + r"\b", state_api + section)]
    if leftovers:
        raise SystemExit("FAIL: the display state still carries the dead bonded data-source "
                         "selection: " + ", ".join(leftovers))
    harness = '''
#include <assert.h>
#include <stdint.h>
#define ENABLE_OLED_INPUT 0
'''
    checks = '''
int main() {
    gNavEvents = {true, true, true, true, 12, -8, 3};
    updateInputState();
    assert(!gNavEvents.up && !gNavEvents.down && !gNavEvents.left && !gNavEvents.right);
    assert(gNavEvents.deltaX == 0 && gNavEvents.deltaY == 0 && gNavEvents.wheelDelta == 0);
    int x = 12, y = -8;
    getJoystickDelta(x, y);
    assert(x == 0 && y == 0 && getNewlyPressedButtons() == 0);
    assert(!processOLEDInput());
    tryAutoStartInputForMenu();
    assert(gNavEvents.deltaX == 0 && gNavEvents.wheelDelta == 0);
}
'''
    with tempfile.TemporaryDirectory(prefix="hw1-oled-without-input-") as temp:
        directory = Path(temp)
        unit = directory / "test.cpp"
        unit.write_text(harness + state_api + section + checks)
        binary = directory / "test"
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
    print("oled_without_input: OK")


if __name__ == "__main__":
    main()
