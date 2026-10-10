#!/usr/bin/env python3
"""Compile display state with its real guards and no physical input backend."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=shutil.which("c++"))
    args = parser.parse_args()
    component = Path(__file__).resolve().parents[2]
    header = (component / "OLED_Display.h").read_text()
    state_api = header[header.index("struct NavEvents {"):header.index("// Menu navigation state")]
    source = (component / "OLED_Utils.cpp").read_text()
    start = source.index("// Gamepad Input for OLED Menu Navigation")
    end = source.index("// OLED File Browser (128x64 optimized)", start)
    # Keep the production preprocessor boundaries intact: the original bug
    # compiled this whole API away with the physical controller implementation.
    section = source[start:end]
    harness = '''
#include <assert.h>
#include <stdint.h>
#define ENABLE_OLED_INPUT 0
#define ENABLE_BONDED_MODE 1
#define BROADCAST_PRINTF(...) ((void)0)
struct Settings { bool bondModeEnabled; } gSettings{true};
struct Peer { bool initialized; bool bondPeerOnline; } peer{true, true};
Peer* gEspNow = &peer;
int invalidated = 0, dirty = 0;
void invalidateDynamicMenu() { ++invalidated; }
void oledMarkDirty() { ++dirty; }
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
    assert(oledRemoteSourceAvailable());
    oledCycleDataSource();
    assert(gDataSource == DataSource::REMOTE && gDataSourceIndicatorVisible);
    oledCycleDataSource();
    assert(gDataSource == DataSource::BOTH);
    oledCycleDataSource();
    assert(gDataSource == DataSource::LOCAL && invalidated == 3 && dirty == 3);
    peer.bondPeerOnline = false;
    gDataSource = DataSource::REMOTE;
    assert(!oledRemoteSourceAvailable());
    oledCycleDataSource();
    assert(gDataSource == DataSource::LOCAL && !gDataSourceIndicatorVisible);
    assert(invalidated == 4);
}
'''
    with tempfile.TemporaryDirectory(prefix="hw1-oled-without-input-") as temp:
        directory = Path(temp)
        unit = directory / "test.cpp"
        unit.write_text(harness + state_api + section + checks)
        binary = directory / "test"
        subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        str(unit), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
