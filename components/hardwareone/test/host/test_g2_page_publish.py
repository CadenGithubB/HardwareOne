#!/usr/bin/env python3
"""Refused lens renders must not change what the visible G2 list rows do.

g2ShowListPage can refuse a render (another swap in flight, allocation
failure, lost admission, full lens queue) and the previous rows then stay on
the lens. Each harness compiles shipping G2 page code and drives taps with a
g2ShowListPage double that can refuse:

* Files (g2_files_publish_harness.cpp): the chooser, delete-confirm and info
  overlay functions plus the real tap dispatcher.
* Settings (g2_settings_publish_harness.cpp): the level/row-map state, row
  builders, edit and commit machinery, menu entry and tap dispatcher.
* LED (g2_led_publish_harness.cpp): the whole page.

Production pieces come from --component (default: this component), so the
same suite replays the regression against a pre-fix tree. Every harness runs
even if an earlier one fails; the exit status reports any failure.
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

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
HELPER_MARKER = "template <typename Publish>"


def list_publish_helper(component: Path) -> str:
    """G2_Page_Common.h's g2ShowListPageAndPublish, or "" on a tree without it.

    Harnesses strip page includes; pages that use the helper fail to compile
    without this text, so an absent helper cannot pass silently.
    """
    common = (component / "G2_Page_Common.h").read_text()
    if HELPER_MARKER not in common:
        return ""
    helper = extract_block(common, HELPER_MARKER)
    require("inline bool g2ShowListPageAndPublish(" in helper,
            "the publish template is the list publish helper")
    return helper


def paginator(component: Path) -> str:
    common = (component / "G2_Page_Common.h").read_text()
    return "\n\n".join((
        extract_block(common, "struct G2Paginator") + ";",
        extract_block(common, "inline G2Paginator g2PaginatorPrepare("),
        extract_block(common, "inline size_t g2PaginatorWriteChrome("),
    ))


def boundary_types(component: Path) -> str:
    glasses = (component / "G2_Glasses.h").read_text()
    hijack = (component / "G2_HijackCmd.h").read_text()
    return "\n\n".join((
        extract_block(glasses, "enum G2HijackPage") + ";",
        extract_block(glasses, "enum G2OverlayKind") + ";",
        extract_block(hijack, "struct G2CmdCookie") + ";",
    ))


def text_entry_types(component: Path) -> str:
    header = (component / "G2_Page_TextEntry.h").read_text()
    typedefs = []
    for name in ("TextEntryCommitFn", "TextEntryCancelFn", "TextEntryAbandonFn"):
        found = re.search(rf"^typedef void \(\*{name}\)\([^)]*\);$", header, flags=re.M)
        require(found is not None, f"typedef {name}")
        typedefs.append(found[0])
    return "\n".join(typedefs) + "\n" + extract_block(header, "struct TextEntryConfig") + ";"


def defines(source: str, names: tuple[str, ...]) -> str:
    lines = []
    for name in names:
        found = re.findall(rf"^#define {name}\b.*$", source, flags=re.M)
        require(len(found) == 1, f"one #define {name}")
        lines.append(found[0])
    return "\n".join(lines)


def files_unit(component: Path) -> str:
    page = (component / "G2_Page_Files.cpp").read_text()
    manager = (component / "System_FileManager.h").read_text()
    start = re.search(r"^static bool\s+gFilesChooserActive\b", page, flags=re.M)
    require(start is not None, "Files chooser state")
    end = page.index("// Capacity calls use VFS", start.start())
    state = page[start.start():end]
    overlay = re.findall(r"^static const uint32_t kFileInfoOverlayMs = \d+;$", page, flags=re.M)
    require(len(overlay) == 1, "one info overlay duration")
    # buildChooserPath returns bool once it reports an unresolved path.
    chooser_path = next((marker for marker in ("static bool buildChooserPath(",
                                               "static void buildChooserPath(")
                         if marker in page), None)
    require(chooser_path is not None, "buildChooserPath definition")
    functions = "\n\n".join(extract_block(page, marker) for marker in (
        chooser_path,
        "static void filesSubmitMutate(",
        "static void showDeleteConfirm(",
        "static void showFileChooser(",
        "static void showFileInfo(",
        "void g2FilesHandleTap(",
    ))
    return fill(HERE / "g2_files_publish_harness.cpp", {
        "// INSERT_BOUNDARY_TYPES": boundary_types(component),
        "// INSERT_TEXT_ENTRY_TYPES": text_entry_types(component),
        "// INSERT_FILE_MANAGER": "\n".join((
            defines(manager, ("FILE_MANAGER_MAX_PATH", "FILE_MANAGER_MAX_NAME")),
            extract_block(manager, "struct FileEntry") + ";")),
        "// INSERT_FILES_STATE": "\n".join((
            defines(page, ("FILES_VISIBLE_ENTRIES", "FILES_MAX_ROWS", "FILES_ROW_LEN")),
            overlay[0], state)),
        "// INSERT_LIST_PUBLISH_HELPER": list_publish_helper(component),
        "// INSERT_FILES_FUNCTIONS": functions,
    })


def settings_unit(component: Path) -> str:
    page = (component / "G2_Page_Settings.cpp").read_text()
    settings = (component / "System_Settings.h").read_text()
    start = page.index("enum SettingsLevel")
    end = page.index("// Level 2 JSON", start)
    body = page[start:end]
    entry = extract_block(page, "void g2ShowSettingsMenu(")
    taps = extract_block(page, "void g2SettingsHandleTap(")
    # System_Settings.h forward-declares SettingEntry first; take the definition.
    definition = settings[settings.index("struct SettingEntry {"):]
    types = "\n\n".join((
        extract_block(settings, "enum SettingType") + ";",
        "typedef bool (*ConnectionCheckFunc)();",
        extract_block(definition, "struct SettingEntry") + ";",
        extract_block(settings, "struct SettingsModule") + ";",
    ))
    return fill(HERE / "g2_settings_publish_harness.cpp", {
        "// INSERT_BOUNDARY_TYPES": boundary_types(component),
        "// INSERT_TEXT_ENTRY_TYPES": text_entry_types(component),
        "// INSERT_SETTINGS_TYPES": types,
        "// INSERT_PAGINATOR": paginator(component),
        "// INSERT_LIST_PUBLISH_HELPER": list_publish_helper(component),
        "// INSERT_SETTINGS_PAGE": body + "\n\n" + entry + "\n\n" + taps,
    })


def led_unit(component: Path) -> str:
    page = (component / "G2_Page_LED.cpp").read_text()
    production = re.sub(r"^#include[^\n]*\n", "", page, flags=re.M)
    return fill(HERE / "g2_led_publish_harness.cpp", {
        "// INSERT_BOUNDARY_TYPES": boundary_types(component),
        "// INSERT_PAGINATOR": paginator(component),
        "// INSERT_LIST_PUBLISH_HELPER": list_publish_helper(component),
        "// INSERT_LED_PAGE": production,
    })


def fill(harness_path: Path, replacements: dict[str, str]) -> str:
    harness = harness_path.read_text()
    for marker, body in replacements.items():
        require(harness.count(marker) == 1, f"{harness_path.name}: one marker {marker}")
        harness = harness.replace(marker, body)
    return harness


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
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    failures = []
    with tempfile.TemporaryDirectory(prefix="hw1-g2-page-publish-") as directory:
        for name, build in (("files", files_unit), ("settings", settings_unit),
                            ("led", led_unit)):
            unit = Path(directory) / f"{name}.cpp"
            executable = Path(directory) / name
            unit.write_text(build(component))
            compiled = subprocess.run([args.cxx, *flags, str(unit), "-o", str(executable)],
                                      timeout=180)
            if compiled.returncode != 0:
                failures.append(f"{name}: compile failed")
                continue
            ran = subprocess.run([str(executable)], timeout=60)
            if ran.returncode != 0:
                failures.append(f"{name}: exit {ran.returncode}")
    if failures:
        print("G2 page publish FAILED: " + "; ".join(failures), file=sys.stderr)
        sys.exit(1)
    print("G2 page publish: files, settings and LED harnesses passed")


if __name__ == "__main__":
    main()
