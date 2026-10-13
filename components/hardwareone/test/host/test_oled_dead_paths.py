#!/usr/bin/env python3
"""Dead OLED code paths stay deleted, and the live paths that replaced them stay.

1. Source contract over every top-level *.cpp/*.h of the component, with comments
   and string/char literals stripped: no code may name the deleted paths (the
   dynamic remote menu and its remote command input, the DataSource selection and
   its R/B indicator, the OLED_UI Dialog and Progress components with the
   oledUIHandleInput/oledUIModalActive entry points, the old display functions
   superseded by the *Rendered versions and the undefined web-stats externs, four
   unused helpers, the four helpers those deletions left without a caller
   (hasRemoteSettings and the oledDrawBox/oledDrawButton/oledDrawTextCentered
   primitives), LOG_MENU_VIEWER, and the unified actions mode). The live
   replacements (oledUIRender, the *Rendered displays, the mode registry and
   back-navigation) and the live drawing helpers (oledDrawIcon,
   oledDrawLevelBars) must still be defined, so the contract cannot pass
   vacuously.
2. Compiled with oled_dead_paths_harness.cpp: the production enum OLEDMode
   (OLED_Display.h) with modeFromSlug and slugFromMode (OLED_Utils.cpp).
   `oledmode unified` names no mode, no mode's slug resolves to a different mode,
   and "actions"/"imuactions" still open IMU actions.
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

REMOVED = {
    "dynamic remote menu": (
        "buildDynamicMenu", "gDynamicMenuItems", "gDynamicMenuItemCount", "invalidateDynamicMenu",
        "getFilteredMenuItemCount", "OLEDMenuItemEx", "MAX_DYNAMIC_MENU_ITEMS", "loadRemoteMenuItems",
        "loadCachedManifest", "addSubmenuHeader", "startRemoteCommandInput",
        "isRemoteCommandInputActive", "cancelRemoteCommandInput", "completeRemoteCommandInput",
        "gRemoteCommandInputActive",
    ),
    "data source selection": (
        "DataSource", "gDataSource", "gDataSourceIndicatorVisible", "oledCycleDataSource",
        "oledGetDataSourceLabel", "oledRemoteSourceAvailable",
    ),
    "OLED_UI dialog/progress": (
        "OledDialog", "gOledDialog", "oledDialogOK", "oledDialogYesNo", "oledDialogCustom",
        "oledDialogClose", "oledDialogActive", "oledDialogHandleInput", "oledDialogRender",
        "OledProgress", "gOledProgress", "oledProgressShow", "oledProgressUpdate",
        "oledProgressLabel", "oledProgressClose", "oledProgressActive", "oledProgressRender",
        "oledUIHandleInput", "oledUIModalActive", "OledUICallback", "OledUIButton",
        "oledUIButtonLabel",
    ),
    "superseded display function": (
        "displaySystemStatus", "displayMemoryStats", "displayWebStats", "displayConnectedSensors",
        "gServerStartTime", "gTotalSessions", "gFailedLoginAttempts",
    ),
    "unused helper": (
        "forceSettingsModeLink", "resetOLEDFileBrowser", "getRegisteredOLEDModes",
        "getPreviousOLEDMode",
    ),
    "helper left without a caller": (
        "hasRemoteSettings", "oledDrawBox", "oledDrawButton", "oledDrawTextCentered",
    ),
    "unreachable logging state": ("LOG_MENU_VIEWER",),
    "unified actions mode": (
        "OLED_UNIFIED_MENU", "UnifiedMenuItem", "displayUnifiedMenu", "handleUnifiedMenuInput",
        "unifiedMenuAvailable", "buildUnifiedMenu",
    ),
}

KEPT = (
    "oledUIRender", "oledToastRender", "oledPairingRibbonRender", "displaySystemStatusRendered",
    "displayMemoryStatsRendered", "displayWebStatsRendered", "displayConnectedSensorsRendered",
    "findOLEDMode", "popOLEDMode", "oledMenuBack", "modeFromSlug", "slugFromMode",
    "oledDrawIcon", "oledDrawLevelBars",
)


def source_names(component: Path, source_dir: Path | None) -> list[str]:
    names = set()
    for root in (component, source_dir):
        if root is None:
            continue
        for pattern in ("*.cpp", "*.h"):
            names |= {path.name for path in root.glob(pattern) if not path.name.startswith("._")}
    return sorted(names)


def read_source(component: Path, source_dir: Path | None, name: str) -> str:
    overlay = source_dir / name if source_dir else None
    path = overlay if overlay and overlay.exists() else component / name
    return path.read_text(encoding="utf-8", errors="replace")


def code_only(source: str) -> str:
    """Source with comments removed and string/char literal contents blanked."""
    out = []
    pos, state = 0, "code"
    while pos < len(source):
        char = source[pos]
        following = source[pos + 1:pos + 2]
        if state == "code":
            previous = source[pos - 1:pos] if pos else ""
            if char == "R" and following == '"' and not (previous.isalnum() or previous == "_"):
                opening = source.find("(", pos + 2)
                delimiter = source[pos + 2:opening] if opening >= 0 else ""
                end = source.find(")" + delimiter + '"', opening) if opening >= 0 else -1
                if opening >= 0 and end >= 0 and len(delimiter) <= 16:
                    out.append('R""' + "\n" * source.count("\n", pos, end))
                    pos = end + len(delimiter) + 2
                    continue
            if char == "/" and following == "/":
                state = "line"
                pos += 2
                continue
            if char == "/" and following == "*":
                state = "block"
                pos += 2
                out.append(" ")
                continue
            if char in "\"'":
                state = "string" if char == '"' else "char"
            out.append(char)
        elif state == "line":
            if char == "\n":
                state = "code"
                out.append(char)
        elif state == "block":
            if char == "*" and following == "/":
                state = "code"
                pos += 2
                continue
            if char == "\n":
                out.append(char)
        else:
            if char == "\\":
                pos += 2
                continue
            if char == ('"' if state == "string" else "'"):
                state = "code"
                out.append(char)
            elif char == "\n":
                state = "code"
                out.append(char)
        pos += 1
    return "".join(out)


def contract_failures(component: Path, source_dir: Path | None) -> list[str]:
    failures = []
    names = source_names(component, source_dir)
    require(len(names) > 50, f"too few production sources under {component}")
    code = {name: code_only(read_source(component, source_dir, name)) for name in names}
    for group, identifiers in REMOVED.items():
        for identifier in identifiers:
            pattern = re.compile(r"\b" + re.escape(identifier) + r"\b")
            hits = []
            for name, text in code.items():
                lines = [text.count("\n", 0, match.start()) + 1 for match in pattern.finditer(text)]
                if lines:
                    hits.append(f"{name}:{','.join(str(line) for line in lines[:4])}")
            if hits:
                failures.append(f"{group}: `{identifier}` is still in the code ({'; '.join(hits[:4])})")
    everything = "\n".join(code.values())
    for identifier in KEPT:
        if not re.search(r"\b" + re.escape(identifier) + r"\s*\([^;{)]*\)\s*\{", everything):
            failures.append(f"live path `{identifier}` is no longer defined")
    return failures


def definition(source: str, signature: str) -> str:
    match = re.search(r"(?m)^" + re.escape(signature) + r"[^;{]*\{", source)
    require(match is not None, f"production definition not found: {signature}")
    return extract_block(source[match.start():], signature)


def build_unit(component: Path, source_dir: Path | None) -> str:
    display_h = read_source(component, source_dir, "OLED_Display.h")
    oled = read_source(component, source_dir, "OLED_Utils.cpp")
    mode_enum = definition(display_h, "enum OLEDMode")
    closing = mode_enum.rstrip().rfind("}")
    body = mode_enum[:closing].rstrip()
    if not body.endswith(","):
        body += ","
    mode_enum = body + "\n  OLED_HARNESS_MODE_END\n};"
    slugs = "\n\n".join(definition(oled, signature) for signature in (
        "OLEDMode modeFromSlug(", "const char* slugFromMode("))
    harness = (HERE / "oled_dead_paths_harness.cpp").read_text(encoding="utf-8")
    for marker, text in (("// INSERT_ENUM", mode_enum), ("// INSERT_SLUGS", slugs)):
        require(harness.count(marker) == 1, f"harness marker missing: {marker}")
        harness = harness.replace(marker, text)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=HERE.parents[1])
    parser.add_argument("--source-dir", type=Path)
    args = parser.parse_args()
    component = args.component.resolve()
    source_dir = args.source_dir.resolve() if args.source_dir else None

    failures = contract_failures(component, source_dir)
    unit_text = build_unit(component, source_dir)
    with tempfile.TemporaryDirectory(prefix="hw1-oled-dead-paths-") as directory:
        work = Path(directory)
        unit = work / "oled_dead_paths_unit.cpp"
        binary = work / "oled_dead_paths"
        unit.write_text(unit_text, encoding="utf-8")
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True)
        result = subprocess.run([str(binary)], check=False)
        if result.returncode != 0:
            failures.append("slug harness checks failed (see FAIL lines above)")
    for failure in failures:
        print(f"FAIL: {failure}")
    if failures:
        raise SystemExit("oled_dead_paths: FAILED")
    print("oled_dead_paths: OK")


if __name__ == "__main__":
    main()
