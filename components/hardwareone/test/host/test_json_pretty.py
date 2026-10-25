#!/usr/bin/env python3
"""One JSON re-indenter for every file viewer.

json_pretty_harness.cpp runs the shared formatter (System_JsonPretty.h): valid
JSON comes out in ArduinoJson's serializeJsonPretty layout (empty {} and []
on one line, escapes and UTF-8 untouched), whitespace and CRLF in the file
don't change the result, a buffer boundary anywhere gives the same output,
malformed or truncated JSON is laid out as far as it goes without underflow,
and deep nesting stops adding indent. It also runs the OLED browser's own
fbFillView body builder (Pretty View = re-indented then wrapped at the OLED
width, JSON View = the stored text wrapped, truncation when the body or the
line table runs out) and its action rows for JSON, text and other files.

Source contracts: the web viewer's Pretty mode, the G2 lens Files page, the
OLED browser and serial `fileview` all include System_JsonPretty.h and format
through JsonPrettyFormatter (fileview's own JSON/raw cases run in
fileread_psram), none keeps a private indenter or re-serializes through
ArduinoJson, and the OLED's "Pretty View" row reaches fbFillView with
pretty=true through LOAD_PRETTY_VIEW.

Production sources come from --component (default: this component), so the
suite replays against a pre-change tree.
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
JSON_INCLUDE = COMPONENT.parent / "hardwareone_libs" / "ArduinoJson" / "src"
HEADER = "System_JsonPretty.h"


def fill(harness: str, replacements: dict[str, str]) -> str:
    for marker, replacement in replacements.items():
        require(harness.count(marker) == 1, f"expected one insertion marker: {marker}")
        harness = harness.replace(marker, replacement)
    return harness


def one(pattern: str, source: str, what: str) -> str:
    found = re.findall(pattern, source, flags=re.M)
    require(len(found) == 1, f"one {what} in OLED_Mode_FileBrowser.cpp")
    return found[0]


def harness_unit(oled: str) -> str:
    start = re.search(r"^static constexpr int\s+FB_VIEW_COLS\b.*$", oled, flags=re.M)
    end = re.search(r"^static bool\s+sFbViewTruncated = false;$", oled, flags=re.M)
    require(bool(start and end) and start.start() < end.end(), "OLED viewer state block")
    return fill((HERE / "json_pretty_harness.cpp").read_text(), {
        "// INSERT_OLED_VIEW_STATE": oled[start.start():end.end()],
        "// INSERT_OLED_ACTION_ROWS_MAX": one(r"^static constexpr int FB_ACTION_ROWS_MAX = \d+;$",
                                              oled, "FB_ACTION_ROWS_MAX"),
        "// INSERT_OLED_FILL_VIEW": extract_block(oled, "static void fbFillView("),
        # The trailing space skips the forward declaration (which ends "]);").
        "// INSERT_OLED_ACTION_ROWS": extract_block(
            oled, "static int fbBuildActionRows(const char* labels[FB_ACTION_ROWS_MAX]) "),
        "INSERT_OLED_JSON": one(r"^\s*sFbActionJson = ([^;]+);", oled, "sFbActionJson assignment"),
    })


def source_contracts(component: Path) -> list[str]:
    failures = []
    page = (component / "G2_Page_Files.cpp").read_text()
    oled = (component / "OLED_Mode_FileBrowser.cpp").read_text()
    server = (component / "WebServer_Server.cpp").read_text()
    filesystem = (component / "System_Filesystem.cpp").read_text()
    fileview = extract_block(filesystem, "const char* cmd_fileview(")
    for name, source in (("G2_Page_Files.cpp", page), ("OLED_Mode_FileBrowser.cpp", oled),
                         ("WebServer_Server.cpp", server), ("System_Filesystem.cpp", filesystem)):
        if f'#include "{HEADER}"' not in source:
            failures.append(f"{name}: does not include the shared re-indenter {HEADER}")
        if "JsonPrettyFormatter" not in source:
            failures.append(f"{name}: does not format JSON through JsonPrettyFormatter")
    for name, source in (("G2_Page_Files.cpp", page), ("OLED_Mode_FileBrowser.cpp", oled)):
        if "serializeJsonPretty" in source or "deserializeJson" in source:
            failures.append(f"{name}: re-serializes through ArduinoJson instead of the re-indenter")

    if "fileKindForName(path.c_str()) == FileKind::Json" not in fileview:
        failures.append("cmd_fileview: JSON is not chosen by the shared file classifier")

    view = extract_block(server, "esp_err_t handleFileView(")
    if "if (isJson) {" in view and "// Default: non-JSON" in view:
        json_branch = view[view.index("if (isJson) {"):view.index("// Default: non-JSON")]
        if "pretty.put(gFileReadBuf[i], emit);" not in json_branch:
            failures.append("handleFileView: Pretty mode does not feed the shared re-indenter")
        for private in ("emitIndent", "inString", "indent++"):
            if private in json_branch:
                failures.append(f"handleFileView: private indenter state ({private}) remains")
    else:
        failures.append("handleFileView: JSON branch not found")

    if 'strcmp(pick, "Pretty View") == 0' in oled:
        at = oled.index('strcmp(pick, "Pretty View") == 0')
        if "FileBrowserPendingAction::LOAD_PRETTY_VIEW" not in oled[at:at + 160]:
            failures.append("OLED action menu: Pretty View does not request LOAD_PRETTY_VIEW")
    else:
        failures.append("OLED action menu: no Pretty View dispatch")
    prepare = extract_block(oled, "void prepareFileBrowserData(")
    case = prepare.find("case FileBrowserPendingAction::LOAD_PRETTY_VIEW:")
    fill_call = prepare.find("fbFillView(content.c_str(), content.length(), pretty);")
    if not 0 <= case < fill_call:
        failures.append("prepareFileBrowserData: LOAD_PRETTY_VIEW does not reach fbFillView")
    elif "== FileBrowserPendingAction::LOAD_PRETTY_VIEW" not in prepare[case:fill_call]:
        failures.append("prepareFileBrowserData: pretty flag not taken from LOAD_PRETTY_VIEW")
    return failures


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

    failures = source_contracts(component)
    for failure in failures:
        print(f"FAIL {failure}", file=sys.stderr)

    oled = (component / "OLED_Mode_FileBrowser.cpp").read_text()
    if (component / HEADER).exists() and "static void fbFillView(" in oled:
        flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 "-I", str(component), "-I", str(JSON_INCLUDE)]
        if args.sanitize:
            flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                      "-fno-omit-frame-pointer", "-g"]
        with tempfile.TemporaryDirectory(prefix="hw1-json-pretty-") as directory:
            source = Path(directory) / "json_pretty.cpp"
            executable = Path(directory) / "json_pretty"
            source.write_text(harness_unit(oled))
            compiled = subprocess.run([args.cxx, *flags, str(source), "-o", str(executable)],
                                      timeout=180)
            if compiled.returncode != 0:
                failures.append("json_pretty: compile failed")
            elif subprocess.run([str(executable)], timeout=60).returncode != 0:
                failures.append("json_pretty: harness checks failed")
    else:
        failures.append(f"json_pretty: {HEADER} or OLED fbFillView absent, harness not run")
        print(f"FAIL {failures[-1]}", file=sys.stderr)

    if failures:
        print(f"json pretty FAILED ({len(failures)}): " + "; ".join(failures), file=sys.stderr)
        sys.exit(1)
    print("json pretty: shared re-indenter, OLED viewer and viewer contracts passed")


if __name__ == "__main__":
    main()
