#!/usr/bin/env python3
"""OLED list navigation: one step per nav event, and per-module editor windows.

Splices production code into oled_list_nav_harness.cpp:
  * OLED_Mode_Automations.cpp: AUTO_LIST_MAX, struct AutomationRenderData, the
    autoRenderData declaration, autoListUp/autoListDown;
  * OLED_Mode_FileBrowser.cpp: enum class FileBrowserPendingAction, the
    pending-action slot, oledFileBrowserUp/Down/Select/Back;
  * System_Settings.h / OLED_SettingsEditor.h types (the extractors shared with
    test_oled_remote_settings.py) and, from OLED_SettingsEditor.cpp, the OLED
    predicate wrappers, the module source, initSettingsEditor,
    displaySettingsEditor, settingsEditorDown/Select/Back and
    openSettingsEditorForModule.
File-scope state those functions use on the tree under test (a per-mode
debounce stamp and interval, where present) comes along, so the same cases run
against a saved tree.

Cases: nav events 40 ms apart (a briskly turned ANO wheel, quick dpad presses)
each move the Automations and Files lists one row, and A/B right after a move
still register, because the input layer already paces nav events; the settings
editor draws every row of a 4-row module without a scroll arrow after a module
with no editable row, and its window follows the cursor. `--component <dir>`
runs the cases against saved sources (the base tree drops events that follow
the previous one within 200 ms and keeps the item scroll offset in a function
static that goes negative and carries into the next module).
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

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_web_batch_handlers import extract_block, require  # noqa: E402
from test_oled_remote_settings import editor_types, settings_types  # noqa: E402


def unique(source: str, marker: str, name: str) -> str:
    require(source.count(marker) == 1, f"{name}: marker missing or not unique: {marker}")
    return extract_block(source, marker)


def optional_lines(source: str, names: tuple[str, ...]) -> list[str]:
    """File-scope declarations of `names` that exist on this tree."""
    lines = []
    for name in names:
        match = re.search(r"(?m)^(?:EXT_RAM_BSS_ATTR )?static [^\n(]*\b" + name + r"\b[^\n]*;[^\n]*$", source)
        if match:
            lines.append(match[0])
    return lines


def automations(read) -> str:
    source = read("OLED_Mode_Automations.cpp")
    name = "OLED_Mode_Automations.cpp"
    limit = re.search(r"(?m)^#define AUTO_LIST_MAX\s+\d+\s*$", source)
    require(limit is not None, f"{name}: AUTO_LIST_MAX missing")
    state = re.search(r"(?m)^EXT_RAM_BSS_ATTR static AutomationRenderData autoRenderData;\s*$", source)
    require(state is not None, f"{name}: autoRenderData declaration missing")
    return "\n\n".join([limit[0], unique(source, "struct AutomationRenderData", name) + ";", state[0],
                        *optional_lines(source, ("autoLastInput", "AUTO_DEBOUNCE")),
                        unique(source, "static void autoListUp(", name),
                        unique(source, "static void autoListDown(", name)])


def file_browser(read) -> str:
    source = read("OLED_Mode_FileBrowser.cpp")
    name = "OLED_Mode_FileBrowser.cpp"
    slot = re.search(r"(?m)^static FileBrowserPendingAction fileBrowserPendingAction\b[^\n]*;\s*$", source)
    require(slot is not None, f"{name}: pending-action slot missing")
    return "\n\n".join([unique(source, "enum class FileBrowserPendingAction", name) + ";", slot[0],
                        *optional_lines(source, ("oledFileBrowserLastInput", "OLED_FILE_BROWSER_DEBOUNCE")),
                        *(unique(source, marker, name) for marker in (
                            "void oledFileBrowserUp(", "void oledFileBrowserDown(",
                            "void oledFileBrowserSelect(", "void oledFileBrowserBack("))])


# (marker, stand-in for a tree without it; None = required on every tree)
EDITOR_FUNCTIONS = (
    ("static bool isSettingVisible(", None),
    ("static bool isEditableEntry(", None),
    ("static bool entryHasEnumOptions(", None),
    ("static int enumOptionCount(", None),
    ("static bool enumOptionAt(", None),
    ("static int enumOptionIndexForCurrent(", None),
    ("static bool editorIsRemote(", ""),
    ("static const SettingsModule** editorModules(", ""),
    ("void initSettingsEditor(", None),
    ("static void editorDropForeignModule(", ""),
    ("void displaySettingsEditor(", None),
    ("void settingsEditorDown(", None),
    ("void settingsEditorSelect(", None),
    ("bool openSettingsEditorForModule(", None),
    ("void settingsEditorBack(", None),
)


def editor(read) -> str:
    source = read("OLED_SettingsEditor.cpp")
    parts = []
    for marker, standin in EDITOR_FUNCTIONS:
        if marker in source:
            parts.append(unique(source, marker, "OLED_SettingsEditor.cpp"))
        else:
            require(standin is not None, f"OLED_SettingsEditor.cpp: missing {marker}")
            parts.append(standin)
    return "\n\n".join(parts)


def generated(read) -> str:
    harness = (HERE / "oled_list_nav_harness.cpp").read_text()
    for marker, body in (("// INSERT_AUTOMATIONS", automations(read)),
                         ("// INSERT_FILE_BROWSER", file_browser(read)),
                         ("// INSERT_SETTINGS_TYPES", settings_types(read)),
                         ("// INSERT_EDITOR_TYPES", editor_types(read)),
                         ("// INSERT_EDITOR", editor(read))):
        require(harness.count(marker) == 1, f"harness marker missing/duplicated: {marker}")
        harness = harness.replace(marker, body)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("clang++") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=HERE.parents[1],
                        help="production component directory (point at a saved tree for a base run)")
    parser.add_argument("--source-dir", type=Path,
                        help="overlay directory; a file present here wins over --component")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")

    def read(name: str) -> str:
        overlay = args.source_dir / name if args.source_dir else None
        return (overlay if overlay and overlay.exists() else args.component / name).read_text()

    # settingsEditorDown/Select keep a module-list local that only some cases read.
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-unused-variable"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-oled-list-nav-") as directory:
        unit, executable = Path(directory) / "test.cpp", Path(directory) / "test"
        unit.write_text(generated(read))
        subprocess.run([args.cxx, *flags, str(unit), "-o", str(executable)], check=True, timeout=300)
        run = subprocess.run([str(executable)], timeout=60)
    if run.returncode != 0:
        raise SystemExit("oled_list_nav: FAILED")


if __name__ == "__main__":
    main()
