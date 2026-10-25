#!/usr/bin/env python3
"""Compile the OLED session-role gate and the launcher populate with a counting roster.

Extracts from the production tree:
  * OLED_Utils.cpp: everything between the shouldBlockForDisplayAuth() definition
    and oledModeAllowedForGuest() (oledIsGuestSession and its session-role
    helpers), oledModeAllowedForGuest, oledGuestBlocksMutate,
    getMenuAvailability with its oledModeFeatureState classification where the
    tree has it (every ENABLE_* off; the harness registers no mode entry rules)
    and oledApplyPendingSessionBoundary;
  * OLED_Mode_Menu.cpp: the launcher populate (populateMenuScroll,
    mainMenuPopulate and helpers) and the selected-row right-pane decorator;
  * OLED_Display.h / OLED_Utils.h: the mode enum, MenuAvailability and the
    menu/scroll structs.
Fakes count every users.json read (getUserAuthorizationRole, isAdminUser). The
checks: launcher steps read the roster at most once per session and identity
generation; a failed lookup is guest and non-admin; a gIdentityGeneration bump
and a session boundary re-resolve the role; other tasks read live.
--component/--source-dir point the suite at another production tree.
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

OLED_UTILS_FUNCTIONS = (
    "bool oledModeAllowedForGuest(",
    "bool oledGuestBlocksMutate(",
    "static MenuAvailability oledModeFeatureState(",
    "MenuAvailability getMenuAvailability(",
    "static void oledApplyPendingSessionBoundary(",
)
# Absent on trees where getMenuAvailability holds the classification itself.
OPTIONAL_FUNCTIONS = ("static MenuAvailability oledModeFeatureState(",)
MENU_STATICS = ("sMainScroll", "sMainScrollInit", "sCatLabels")
MENU_FUNCTIONS = (
    "static void menuItemRightPaneDraw(",
    "static int menuAvailRank(",
    "static void populateMenuScroll(",
    "void getCategoryItems(",
    "static void mainMenuEnsureInit(",
    "static bool categoryVisibleForGuest(",
    "static void mainMenuPopulate(",
)


def read_source(component: Path, source_dir: Path | None, name: str) -> str:
    overlay = source_dir / name if source_dir else None
    path = overlay if overlay and overlay.exists() else component / name
    return path.read_text(encoding="utf-8")


def definition(source: str, signature: str) -> str:
    """The definition of `signature`, skipping forward declarations and calls."""
    boundary = r"\b" if signature[-1].isalnum() else ""
    match = re.search(re.escape(signature) + boundary + r"[^;{]*\{", source)
    require(match is not None, f"production definition not found: {signature}")
    return extract_block(source[match.start():], signature)


def session_role_span(oled: str) -> str:
    """oledIsGuestSession plus whatever session-role helpers sit beside it."""
    end = oled.index("bool oledModeAllowedForGuest(")
    start = oled.rindex("bool shouldBlockForDisplayAuth()", 0, end)
    guard = extract_block(oled[start:], "bool shouldBlockForDisplayAuth()")
    span = oled[start + len(guard):end]
    require("bool oledIsGuestSession(" in span,
            "oledIsGuestSession() is not between shouldBlockForDisplayAuth() and "
            "oledModeAllowedForGuest()")
    return span


def menu_static(menu: str, name: str) -> str:
    match = re.search(rf"(?m)^[^\n;]*\bstatic\b[^\n;]*\b{name}\b[^\n;]*;", menu)
    require(match is not None, f"OLED_Mode_Menu.cpp static not found: {name}")
    return match.group(0)


def build_unit(component: Path, source_dir: Path | None) -> str:
    display_h = read_source(component, source_dir, "OLED_Display.h")
    utils_h = read_source(component, source_dir, "OLED_Utils.h")
    oled = read_source(component, source_dir, "OLED_Utils.cpp")
    menu = read_source(component, source_dir, "OLED_Mode_Menu.cpp")
    layout = read_source(component, source_dir, "OLED_Layout.h").replace("#pragma once", "")

    max_items = re.search(r"(?m)^#define OLED_SCROLL_MAX_ITEMS\s+\d+", utils_h)
    require(max_items is not None, "OLED_SCROLL_MAX_ITEMS not found in OLED_Utils.h")
    mode_enum = definition(display_h, "enum OLEDMode").replace(
        "enum OLEDMode", "enum OLEDMode : int", 1)
    types = "\n".join([
        max_items.group(0),
        mode_enum + ";",
        definition(display_h, "enum class MenuAvailability") + ";",
        definition(display_h, "struct OLEDMenuItem") + ";",
        definition(utils_h, "struct OLEDScrollItem") + ";",
        definition(utils_h, "struct OLEDScrollState") + ";",
        # Row capacity from the shared layout, as drawing and navigation use it.
        layout,
        definition(utils_h, "inline constexpr int oledVisibleRows("),
    ])
    utils = "\n\n".join([session_role_span(oled)] +
                        [definition(oled, signature) for signature in OLED_UTILS_FUNCTIONS
                         if signature not in OPTIONAL_FUNCTIONS or signature in oled])
    launcher = "\n\n".join([menu_static(menu, name) for name in MENU_STATICS] +
                           [definition(menu, signature) for signature in MENU_FUNCTIONS])

    harness = (HERE / "oled_session_role_harness.cpp").read_text(encoding="utf-8")
    for marker, text in (("// INSERT_TYPES", types), ("// INSERT_OLED_UTILS", utils),
                         ("// INSERT_MENU", launcher)):
        require(marker in harness, f"harness marker missing: {marker}")
        harness = harness.replace(marker, text)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=HERE.parents[1])
    parser.add_argument("--source-dir", type=Path)
    args = parser.parse_args()

    unit_text = build_unit(args.component.resolve(), args.source_dir)
    with tempfile.TemporaryDirectory(prefix="hw1-oled-session-role-") as directory:
        work = Path(directory)
        unit = work / "oled_session_role_unit.cpp"
        binary = work / "oled_session_role"
        unit.write_text(unit_text, encoding="utf-8")
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-function", str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True)
        result = subprocess.run([str(binary)], check=False)
        if result.returncode != 0:
            raise SystemExit("oled_session_role: FAILED")


if __name__ == "__main__":
    main()
