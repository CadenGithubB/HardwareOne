#!/usr/bin/env python3
"""Run the shipping OLED Map actions and the waypoint owner's save reporting.

oled_map_actions_harness.cpp: the Map mode's state block, track scan, menu
dispatch, gpsMapInputHandler and the head of displayGPSMap (through map
centering) are extracted unchanged. The Map owners, keyboard, file picker and
VFS are doubles that record the identity installed at each call and refuse the
anonymous identity, as checkPerm does. HAL_Input's real button mapping
(tables, inputGetButtonMask, inputIsButtonPressed, INPUT_CHECK) and NavEvents
are extracted too, and the unit is built per input device: the seesaw gamepad
(ENABLE_GPS_SENSOR 0 and 1), the ANO rotary encoder and the GPIO wheel with the
P4X-EYE keys, each fed the press bits, wheel detents and nav events the OLED
input router hands a mode on that device.

map_waypoint_save_harness.cpp: WaypointManager's mutators, cmd_waypoint,
cmd_waypointfile, CommandArgs and requireQuotedToken are extracted unchanged;
saveWaypoints() is a double whose result each case chooses.

--component points both units at another component tree (e.g. the base) to
show which checks fail there.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_mqtt_boolean_commands import extract_macro
from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def between(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def define_lines(source: str, *prefixes: str) -> str:
    return "\n".join(line for line in source.splitlines()
                     if line.startswith(tuple(f"#define {p}" for p in prefixes)))


def plain_define_lines(source: str, *prefixes: str) -> str:
    # Single-line #defines only (a continued macro would swallow the next line).
    return "\n".join(line for line in define_lines(source, *prefixes).splitlines()
                     if not line.rstrip().endswith("\\"))


def map_actions_unit(component: Path) -> str:
    oled = (component / "OLED_Mode_Map.cpp").read_text()
    maps_h = (component / "System_Maps.h").read_text()
    user = (component / "System_User.h").read_text()
    display = (component / "OLED_Display.h").read_text()
    file_manager = (component / "System_FileManager.h").read_text()
    input_h = (component / "HAL_Input.h").read_text()
    input_cpp = (component / "HAL_Input.cpp").read_text()
    seesaw_h = (component / "i2csensor_seesaw.h").read_text()
    ano_h = (component / "i2csensor_ano_encoder.h").read_text()
    types = [
        extract_block(input_h, "enum InputButton") + ";",
        extract_block(input_h, "enum InputControllerType") + ";",
        plain_define_lines(seesaw_h, "JOYSTICK_", "GAMEPAD_BUTTON_"),
        plain_define_lines(ano_h, "ANO_BTN_", "ANO_VIRT_START"),
        extract_block(display, "struct NavEvents") + ";",
        extract_block(user, "enum CommandSource") + ";",
        extract_block(user, "struct AuthContext") + ";",
        define_lines(file_manager, "FILE_MANAGER_MAX_PATH", "FILE_MANAGER_MAX_NAME"),
        extract_block(file_manager, "struct FileEntry") + ";",
        extract_block(display, "enum OLEDMode") + ";",
        extract_block(display, "struct FilePickerRequest") + ";",
        extract_block(maps_h, "enum MapFeatureType") + ";",
        define_lines(maps_h, "HWMAP_FTYPE_", "HWMAP_FEATURE_HEADER_SIZE", "LAYER_",
                     "MAX_WAYPOINT", "WAYPOINT_"),
        extract_block(maps_h, "struct Waypoint") + ";",
    ]
    guard = []
    if "class OLEDMapCtxGuard" in oled:
        guard = [extract_block(oled, "static AuthContext oledMapAuthContext("),
                 extract_block(oled, "class OLEDMapCtxGuard") + ";"]
    display_head = extract_block(oled, "static void displayGPSMap(")
    display_head = display_head[:display_head.index("  // Initialize async renderer on first call")] + "}\n"
    production = [
        between(oled, "// Map menu state (accessed by footer system)", "// Draw map info overlay"),
        extract_block(oled, "struct RouteInfo") + ";",
        extract_block(oled, "static bool isGPSDataLine("),
        extract_block(oled, "static void probeTrackFile("),
        extract_block(oled, "static void scanTrackFiles("),
        # goBackToMainMenu plus whatever helpers sit before the submenu dispatch.
        between(oled, "// Go back to main menu from submenu",
                "// Execute submenu action based on current submenu type and selection"),
        extract_block(oled, "static void executeSubmenuAction("),
        extract_block(oled, "static void executeMainMenuAction("),
        extract_block(oled, "static void executeMapMenuAction("),
        extract_block(oled, "static bool gpsMapInputHandler("),
        display_head,
    ]
    # HAL_Input owns the logical-button mapping; the harness picks the
    # controller, these are the shipping tables and lookups.
    input_layer = [
        between(input_cpp, "// Gamepad Seesaw button mappings", "void inputAbstractionInit()"),
        extract_block(input_cpp, "uint32_t inputGetButtonMask("),
        extract_block(input_cpp, "bool inputIsButtonPressed("),
        plain_define_lines(input_h, "INPUT_CHECK", "INPUT_MASK"),
    ]
    harness = (HERE / "oled_map_actions_harness.cpp").read_text()
    for marker in ("// INSERT_TYPES", "// INSERT_INPUT", "// INSERT_GUARD", "// INSERT_PRODUCTION"):
        require(harness.count(marker) == 1, f"missing {marker} marker")
    return (harness.replace("// INSERT_TYPES", "\n\n".join(types))
            .replace("// INSERT_INPUT", "\n\n".join(input_layer))
            .replace("// INSERT_GUARD", "\n\n".join(guard))
            .replace("// INSERT_PRODUCTION", "\n\n".join(production)))


def waypoint_save_unit(component: Path) -> str:
    maps = (component / "System_Maps.cpp").read_text()
    maps_h = (component / "System_Maps.h").read_text()
    command_h = (component / "System_Command.h").read_text()
    command = (component / "System_Command.cpp").read_text()
    filesystem = (component / "System_Filesystem.cpp").read_text()
    utils_h = (component / "System_Utils.h").read_text()
    types = [
        define_lines(maps_h, "MAX_WAYPOINT", "WAYPOINT_"),
        extract_block(maps_h, "struct Waypoint") + ";",
        extract_block(maps_h, "class WaypointManager") + ";",
        extract_block(command_h, "class CommandArgs") + ";",
        extract_macro(utils_h, "RETURN_VALID_IF_VALIDATE_CSTR"),
    ]
    production = ["const String CommandArgs::empty_;"]
    production += [extract_block(command, marker) for marker in (
        "CommandArgs::CommandArgs(", "void CommandArgs::parse()", "const String& CommandArgs::arg(",
        "int CommandArgs::argInt(", "float CommandArgs::argFloat(", "String CommandArgs::remaining(")]
    production += [
        extract_block(filesystem, "const char* requireQuotedToken("),
        extract_block(maps, "static void sanitizeWaypointTextCopy("),
        between(maps, "Waypoint* WaypointManager::_waypoints = nullptr;", "bool WaypointManager::ensureStorage()"),
        extract_block(maps, "bool WaypointManager::ensureStorage()"),
        # addWaypoint (both overloads) through selectTarget: every table mutator.
        between(maps, "int WaypointManager::addWaypoint(float lat, float lon, const char* name)",
                "bool WaypointManager::getDistanceBearingToIndex("),
        extract_block(maps, "const char* cmd_waypoint("),
        extract_block(maps, "const char* cmd_waypointfile("),
    ]
    harness = (HERE / "map_waypoint_save_harness.cpp").read_text()
    for marker in ("// INSERT_TYPES", "// INSERT_PRODUCTION"):
        require(harness.count(marker) == 1, f"missing {marker} marker")
    return (harness.replace("// INSERT_TYPES", "\n\n".join(types))
            .replace("// INSERT_PRODUCTION", "\n\n".join(production)))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="component source tree to extract from (default: this checkout)")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(HERE),
             "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-const-variable",
             "-Wno-unused-but-set-variable", "-Wno-unused-parameter"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    component = args.component.resolve()
    # MAP_TEST_DEVICE: 0 seesaw gamepad, 1 ANO encoder, 2 GPIO wheel + P4X-EYE keys.
    map_unit = map_actions_unit(component)
    builds = [(f"map_actions_{name}_gps{gps}", map_unit,
               [f"-DMAP_TEST_DEVICE={device}", f"-DENABLE_GPS_SENSOR={gps}"])
              for name, device, gps in (("seesaw", 0, 0), ("seesaw", 0, 1), ("ano", 1, 1), ("gpio", 2, 0))]
    builds.append(("waypoint_save", waypoint_save_unit(component), []))
    failed = []
    with tempfile.TemporaryDirectory(prefix="hw1-oled-map-actions-") as directory:
        for name, source, defines in builds:
            label = " ".join([name, *defines])
            unit = Path(directory) / f"{name}.cpp"
            binary = Path(directory) / name
            unit.write_text(source)
            subprocess.run([args.cxx, *flags, *defines, str(unit), "-o", str(binary)],
                           check=True, timeout=180)
            print(f"== {label}", flush=True)
            if subprocess.run([str(binary)], timeout=60).returncode != 0:
                failed.append(label)
    require(not failed, "failing: " + ", ".join(failed))
    print("test_oled_map_actions: all units passed")


if __name__ == "__main__":
    main()
