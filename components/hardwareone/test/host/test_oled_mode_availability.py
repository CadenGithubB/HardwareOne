#!/usr/bin/env python3
"""Launcher availability applies each OLED mode module's own entry rule.

Extracts from the production tree and compiles with oled_mode_availability_harness.cpp:
  * OLED_Display.h: the mode enum, the OLED*Func callback typedefs, MAX_OLED_MODES,
    struct OLEDModeEntry and enum class MenuAvailability;
  * OLED_Utils.cpp: the mode registry (statics, registerOLEDMode,
    registerOLEDModes, findOLEDMode) and getMenuAvailability, with
    oledModeFeatureState and oledModeRuleAllows where the tree has them;
  * the real availFuncs: isCLIInputAvailable (OLED_Mode_CLIInput.cpp),
    isSetPatternAvailable (OLED_Mode_SetPattern.cpp), isLogoutModeAvailable
    (OLED_Mode_Auth.cpp), gpsOLEDModeAvailable (i2csensor_pa1010d_oled.h) and
    thermalOLEDModeAvailable (i2csensor_mlx90640_oled.h).
The harness registers them through the production registry and checks that a
module's refusal makes its launcher row unavailable with the module's reason
(CLI Input and Gamepad PW "Login required" without a named display session,
Logout "Not logged in"), while the central classification keeps its class and
reason (GPS/thermal No HW and Off, not built rows) and modes without a rule are
unchanged.
A source check requires requestOLEDModePrepared() to be declared in
OLED_Display.h beside requestOLEDMode() and in no other header or mode file.
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

REGISTRY_STATICS = (
    r"(?m)^[^\n;]*\bstatic const OLEDModeEntry\* oledModeRegistry\[[^\n;]*;",
    r"(?m)^[^\n;]*\bstatic size_t oledModeRegistrySize\b[^\n;]*;",
)
REGISTRY_FUNCTIONS = (
    "void registerOLEDMode(",
    "void registerOLEDModes(",
    "const OLEDModeEntry* findOLEDMode(",
)
# Present on trees where getMenuAvailability consults the module rule.
AVAILABILITY_HELPERS = (
    "static MenuAvailability oledModeFeatureState(",
    "bool oledModeRuleAllows(",
)
RULES = (
    ("OLED_Mode_CLIInput.cpp", "static bool isCLIInputAvailable("),
    ("OLED_Mode_SetPattern.cpp", "static bool isSetPatternAvailable("),
    ("OLED_Mode_Auth.cpp", "static bool isLogoutModeAvailable("),
    ("i2csensor_pa1010d_oled.h", "static bool gpsOLEDModeAvailable("),
    ("i2csensor_mlx90640_oled.h", "static bool thermalOLEDModeAvailable("),
)


def read_source(component: Path, source_dir: Path | None, name: str) -> str:
    overlay = source_dir / name if source_dir else None
    path = overlay if overlay and overlay.exists() else component / name
    return path.read_text(encoding="utf-8")


def find_definition(source: str, signature: str) -> int:
    """Offset of the definition of `signature` (skipping declarations and calls), or -1."""
    boundary = r"\b" if signature[-1].isalnum() else ""
    match = re.search(r"(?m)^" + re.escape(signature) + boundary + r"[^;{]*\{", source)
    return match.start() if match else -1


def definition(source: str, signature: str) -> str:
    start = find_definition(source, signature)
    require(start >= 0, f"production definition not found: {signature}")
    return extract_block(source[start:], signature)


def single(source: str, pattern: str, what: str) -> str:
    match = re.search(pattern, source)
    require(match is not None, f"not found: {what}")
    return match.group(0)


def build_unit(component: Path, source_dir: Path | None) -> str:
    display_h = read_source(component, source_dir, "OLED_Display.h")
    oled = read_source(component, source_dir, "OLED_Utils.cpp")

    mode_enum = definition(display_h, "enum OLEDMode").replace(
        "enum OLEDMode", "enum OLEDMode : int", 1)
    typedefs = re.findall(r"(?m)^typedef [^;\n]*\(\*OLED\w+Func\)[^;\n]*;", display_h)
    require(len(typedefs) >= 4, "OLED_Display.h: the OLED*Func callback typedefs were not found")
    types = "\n".join([
        mode_enum + ";",
        *typedefs,
        single(display_h, r"(?m)^#define MAX_OLED_MODES\s+\d+", "MAX_OLED_MODES"),
        definition(display_h, "struct OLEDModeEntry") + ";",
        definition(display_h, "enum class MenuAvailability") + ";",
        "MenuAvailability getMenuAvailability(OLEDMode mode, String* outReason);",
    ])

    registry = [single(oled, pattern, pattern) for pattern in REGISTRY_STATICS]
    registry += [definition(oled, signature) for signature in REGISTRY_FUNCTIONS]
    registry += [definition(oled, signature) for signature in AVAILABILITY_HELPERS
                 if find_definition(oled, signature) >= 0]
    registry.append(definition(oled, "MenuAvailability getMenuAvailability("))

    rules = [definition(read_source(component, source_dir, name), signature)
             for name, signature in RULES]

    harness = (HERE / "oled_mode_availability_harness.cpp").read_text(encoding="utf-8")
    for marker, text in (("// INSERT_TYPES", types),
                         ("// INSERT_REGISTRY", "\n\n".join(registry)),
                         ("// INSERT_RULES", "\n\n".join(rules))):
        require(harness.count(marker) == 1, f"harness marker missing: {marker}")
        harness = harness.replace(marker, text)
    return harness


def prototype_failures(component: Path, source_dir: Path | None) -> list[str]:
    """requestOLEDModePrepared() is declared once, in OLED_Display.h beside requestOLEDMode()."""
    failures = []
    display_h = read_source(component, source_dir, "OLED_Display.h")
    request = display_h.find("void requestOLEDMode(")
    prepared = display_h.find("void requestOLEDModePrepared(")
    if prepared < 0:
        failures.append("OLED_Display.h does not declare requestOLEDModePrepared()")
    elif request < 0 or not 0 < prepared - request < 800:
        failures.append("OLED_Display.h: requestOLEDModePrepared() is not declared right after "
                        "requestOLEDMode()")
    names = {path.name for path in component.glob("*.cpp")} | {path.name for path in component.glob("*.h")}
    if source_dir:
        names |= {path.name for path in source_dir.glob("*.cpp")}
        names |= {path.name for path in source_dir.glob("*.h")}
    for name in sorted(names):
        if name in ("OLED_Display.h", "OLED_Utils.cpp") or name.startswith("._"):
            continue
        try:
            text = read_source(component, source_dir, name)
        except (OSError, UnicodeDecodeError):
            continue
        if re.search(r"(?m)^\s*void requestOLEDModePrepared\(", text):
            failures.append(f"{name} declares requestOLEDModePrepared() itself; "
                            "it belongs in OLED_Display.h")
    return failures


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=HERE.parents[1])
    parser.add_argument("--source-dir", type=Path)
    args = parser.parse_args()
    component = args.component.resolve()
    source_dir = args.source_dir.resolve() if args.source_dir else None

    failures = prototype_failures(component, source_dir)
    unit_text = build_unit(component, source_dir)
    with tempfile.TemporaryDirectory(prefix="hw1-oled-mode-availability-") as directory:
        work = Path(directory)
        unit = work / "oled_mode_availability_unit.cpp"
        binary = work / "oled_mode_availability"
        unit.write_text(unit_text, encoding="utf-8")
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-function", "-Wno-unused-parameter", str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True)
        result = subprocess.run([str(binary)], check=False)
        if result.returncode != 0:
            failures.append("availability harness checks failed (see FAIL lines above)")
    for failure in failures:
        print(f"FAIL: {failure}")
    if failures:
        raise SystemExit("oled_mode_availability: FAILED")
    print("oled_mode_availability: OK")


if __name__ == "__main__":
    main()
