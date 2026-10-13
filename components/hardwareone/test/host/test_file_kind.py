#!/usr/bin/env python3
"""One file classification and one rename/delete authority for the two on-device file browsers.

file_kind_harness.cpp runs the shared classifier (FileBrowser_FileKind.h) and,
on one table of names, the G2 lens Files page's file-tap branch (extracted from
g2FilesHandleTap) beside the OLED browser's "View" decision (its
sFbActionViewable assignment): the lens must offer a text/JSON view exactly
when the OLED offers View (a .log or .md file opens in both).

oled_file_command_harness.cpp runs the OLED browser's rename/delete helpers
against FileManager and executor doubles: both submit the user-level
`filerename` / one-shot `filedelete ... confirm` commands (the lens' path) with
the selected entry's full path, re-scan only after the executor's "OK", and
submit nothing for an unresolved selection or a quote. The handlers'
path-policy refusal ("File does not exist or access denied") gets its own
result and toast (fbToastMutate), not the generic "... failed".

Source contracts: the OLED browser no longer calls FileManager::renameItem /
deleteItem or keeps its own extension list, its prepare pass routes Rename and
Delete through those helpers, the input path still stops guests first, and the
G2 page has no private extension predicates.

Production sources come from --component (default: this component), so the
suite replays against a pre-fix tree.
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

KIND_HEADER = "FileBrowser_FileKind.h"
G2_PREDICATES = ("isBmpFilename", "isJpgFilename", "isJsonFilename", "isTxtFilename",
                 "isCsvFilename")
OLED_HELPERS = ("static FbMutateResult fbSubmitMutate(",
                "static FbMutateResult fbRenameSelected(",
                "static FbMutateResult fbDeleteSelected(",
                "static void fbToastMutate(")


def defines(source: str, names: tuple[str, ...]) -> str:
    lines = []
    for name in names:
        found = re.findall(rf"^#define {name}\b.*$", source, flags=re.M)
        require(len(found) == 1, f"one #define {name}")
        lines.append(found[0])
    return "\n".join(lines)


def fill(harness: str, replacements: dict[str, str]) -> str:
    for marker, replacement in replacements.items():
        require(harness.count(marker) == 1, f"expected one insertion marker: {marker}")
        harness = harness.replace(marker, replacement)
    return harness


def g2_file_tap(page: str) -> str:
    """The `else` block after `if (e.isFolder)` in g2FilesHandleTap."""
    handler = extract_block(page, "void g2FilesHandleTap(")
    require(handler.count("if (e.isFolder)") == 1, "one folder branch in g2FilesHandleTap")
    folder = extract_block(handler, "if (e.isFolder)")
    rest = handler[handler.index(folder) + len(folder):].lstrip()
    require(rest.startswith("else"), "g2FilesHandleTap: an else branch for files")
    return extract_block(rest, "else")[len("else"):].strip()


def kind_unit(component: Path) -> str:
    page = (component / "G2_Page_Files.cpp").read_text()
    oled = (component / "OLED_Mode_FileBrowser.cpp").read_text()
    manager = (component / "System_FileManager.h").read_text()
    classifiers = [extract_block(page, f"static bool {name}(")
                   for name in G2_PREDICATES if f"static bool {name}(" in page]
    mapper = "static FilesChooserKind filesChooserKindFor("
    if mapper in page:
        classifiers.append(extract_block(page, mapper))
    viewable = re.findall(r"^\s*sFbActionViewable = ([^;]+);", oled, flags=re.M)
    require(len(viewable) == 1, "one sFbActionViewable assignment in OLED_Mode_FileBrowser.cpp")
    oled_classifiers = ""
    if "static bool fbIsViewableExt(" in oled:
        oled_classifiers = extract_block(oled, "static bool fbIsViewableExt(")
    return fill((HERE / "file_kind_harness.cpp").read_text(), {
        "// INSERT_FILE_ENTRY": "\n".join((
            defines(manager, ("FILE_MANAGER_MAX_PATH", "FILE_MANAGER_MAX_NAME")),
            extract_block(manager, "struct FileEntry") + ";")),
        "// INSERT_G2_CHOOSER_KIND": extract_block(page, "enum FilesChooserKind") + ";",
        "// INSERT_G2_CLASSIFIERS": "\n\n".join(classifiers),
        "// INSERT_G2_FILE_TAP": g2_file_tap(page),
        "// INSERT_OLED_CLASSIFIERS": oled_classifiers,
        "INSERT_OLED_VIEWABLE": viewable[0],
    })


def command_unit(oled: str) -> str:
    pieces = [extract_block(oled, "enum class FbMutateResult") + ";"]
    pieces += [extract_block(oled, marker) for marker in OLED_HELPERS]
    return fill((HERE / "oled_file_command_harness.cpp").read_text(), {
        "// INSERT_OLED_FILE_COMMANDS": "\n\n".join(pieces),
    })


def source_contracts(page: str, oled: str) -> list[str]:
    failures = []
    for name, source in (("G2_Page_Files.cpp", page), ("OLED_Mode_FileBrowser.cpp", oled)):
        if f'#include "{KIND_HEADER}"' not in source:
            failures.append(f"{name}: does not include the shared classifier {KIND_HEADER}")
    private = [name for name in G2_PREDICATES if f"static bool {name}(" in page]
    if private:
        failures.append("G2_Page_Files.cpp: private extension predicates " + ", ".join(private))
    if "fbIsViewableExt(" in oled:
        failures.append("OLED_Mode_FileBrowser.cpp: private extension list fbIsViewableExt")
    for call in ("renameItem(", "deleteItem("):
        if call in oled:
            failures.append(f"OLED_Mode_FileBrowser.cpp: calls FileManager::{call[:-1]} directly "
                            "instead of the authorized command")
    missing = [marker for marker in OLED_HELPERS if marker not in oled]
    if missing:
        failures.append("OLED_Mode_FileBrowser.cpp: no command helpers " + ", ".join(missing))
    prepare = extract_block(oled, "void prepareFileBrowserData(")
    guard = prepare.find("OLEDFileBrowserCtxGuard ctxGuard;")
    for call in ("fbRenameSelected(newName)", "fbDeleteSelected()"):
        at = prepare.find(call)
        if not 0 <= guard < at:
            failures.append(f"prepareFileBrowserData: no {call} under OLEDFileBrowserCtxGuard")
    if 'strcmp(pick, "Rename")' in oled:
        start = oled.index('strcmp(pick, "Rename")')
        menu = oled[start:oled.index('"< Back"', start)]
        rename_gate = menu.find("oledGuestBlocksMutate()")
        rename = menu.find("sFbRenameActive = true")
        delete_gate = menu.find("oledGuestBlocksMutate()", rename + 1) if rename >= 0 else -1
        confirm = menu.find("oledConfirmRequest(")
        if not 0 <= rename_gate < rename < delete_gate < confirm:
            failures.append("OLED action menu: Rename/Delete no longer stop guests first")
    else:
        failures.append("OLED action menu: no Rename dispatch found")
    return failures


def compile_and_run(cxx: str, flags: list[str], unit: str, name: str, temp: Path,
                    include: Path) -> str | None:
    source = temp / f"{name}.cpp"
    source.write_text(unit)
    executable = temp / name
    compiled = subprocess.run([cxx, *flags, "-I", str(include), str(source), "-o", str(executable)],
                              timeout=180)
    if compiled.returncode != 0:
        return f"{name}: compile failed"
    ran = subprocess.run([str(executable)], timeout=60)
    if ran.returncode != 0:
        return f"{name}: exit {ran.returncode}"
    return None


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
    page = (component / "G2_Page_Files.cpp").read_text()
    oled = (component / "OLED_Mode_FileBrowser.cpp").read_text()
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]

    failures = source_contracts(page, oled)
    for failure in failures:
        print(f"FAIL {failure}", file=sys.stderr)
    with tempfile.TemporaryDirectory(prefix="hw1-file-kind-") as directory:
        temp = Path(directory)
        result = compile_and_run(args.cxx, flags, kind_unit(component), "file_kind", temp, component)
        if result:
            failures.append(result)
        if all(marker in oled for marker in OLED_HELPERS) and "enum class FbMutateResult" in oled:
            result = compile_and_run(args.cxx, flags, command_unit(oled), "oled_file_command",
                                     temp, component)
            if result:
                failures.append(result)
        else:
            failures.append("oled_file_command: OLED rename/delete helpers absent, not run")
            print("FAIL oled_file_command: OLED rename/delete helpers absent, not run",
                  file=sys.stderr)
    if failures:
        print(f"file kind FAILED ({len(failures)}): " + "; ".join(failures), file=sys.stderr)
        sys.exit(1)
    print("file kind: shared classifier, browser agreement, OLED command path and contracts passed")


if __name__ == "__main__":
    main()
