#!/usr/bin/env python3
"""Every path that enters an OLED mode asks the same question.

`oledmode thermal` used to answer OK on a build without the thermal camera and
leave the screen blank: the command checked only the slug, and the display task
asked only the mode's own entry rule (oledModeRuleAllows), which a mode that is
not compiled does not have. The launcher asked getMenuAvailability(), which
also knows the build and the hardware. The saved default mode checked nothing.

The rule (OLED_Display.h):

1. getMenuAvailability() is the one question "can this mode be entered now";
   oledModeRuleAllows() is called from it alone.
2. oledModeInBuild() is the build test any task may ask. cmd_oledmode refuses
   a mode that is not in the build before it posts anything, and the display
   task asks getMenuAvailability() when it applies the request.
3. The saved default mode is read by savedDefaultOledMode() alone, which falls
   back to the status screen for a mode that is not in the build.
4. modeFromSlug() is called by cmd_oledmode and savedDefaultOledMode() only.

Reads the working tree. --component points the test at another component
directory.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

from test_shared_text_helpers import blank_comments, sources
from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def calls(code: str, name: str) -> list[int]:
    lines = []
    for match in re.finditer(r"(?<![\w.])%s\s*\(" % re.escape(name), code):
        head = code[max(0, code.rfind("\n", 0, match.start())):match.start()]
        if re.search(r"^\s*(?:static\s+)?(?:const\s+)?[\w:<>\*&]+\s+$", head.split("\n")[-1]):
            continue  # a declaration or definition, not a call
        lines.append(code.count("\n", 0, match.start()) + 1)
    return lines


def owner_of(code: str, line: int) -> str:
    """The function a line sits in: the last top-level signature above it."""
    head = code.split("\n")[:line]
    for text in reversed(head):
        match = re.match(r"^(?:static\s+)?[\w:<>\*& ]+?\b(\w+)\s*\([^;]*$", text)
        if match and not text.startswith((" ", "\t", "#", "}")):
            return match.group(1)
    return "?"


def measure(component: Path) -> tuple[int, list[str]]:
    failures: list[str] = []
    texts = {relative: blank_comments(text, keep_strings=True)
             for relative, text in sources(component, (".h", ".cpp", ".c", ".inc"))}

    allowed = {"oledModeRuleAllows": {"getMenuAvailability"},
               "modeFromSlug": {"cmd_oledmode", "savedDefaultOledMode"}}
    for name, owners in allowed.items():
        for relative, code in sorted(texts.items()):
            for line in calls(code, name):
                owner = owner_of(code, line)
                if owner not in owners:
                    failures.append(f"{relative}:{line} calls {name}() from {owner}(); "
                                    f"only {', '.join(sorted(owners))} may")

    oled = texts.get("OLED_Utils.cpp", "")
    command = extract_block(oled, "const char* cmd_oledmode(") if "const char* cmd_oledmode(" in oled else ""
    if not command or "oledModeInBuild(target)" not in command or \
            command.index("oledModeInBuild(target)") > command.index("oledPostOwnerRequest("):
        failures.append("cmd_oledmode must refuse a mode that is not in the build before it posts")
    default = extract_block(oled, "OledMode savedDefaultOledMode()") if "OledMode savedDefaultOledMode()" in oled else ""
    if "oledModeInBuild(mode)" not in default:
        failures.append("savedDefaultOledMode must fall back for a mode that is not in the build")
    owner = oled[oled.find("request.kind == OledOwnerRequest::CliMode && !shouldBlockForDisplayAuth()"):]
    owner = owner[:owner.find("requestOledMode(request.mode")]
    if "getMenuAvailability(request.mode" not in owner:
        failures.append("the display task must ask getMenuAvailability() when it applies oledmode")
    for relative, code in sorted(texts.items()):
        if relative != "OLED_Utils.cpp" and "gSettings.oledDefaultMode" in code and "modeFromSlug" in code:
            failures.append(f"{relative} resolves gSettings.oledDefaultMode itself; use savedDefaultOledMode()")
    return len(texts), failures


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    component = args.component.resolve()

    checked, failures = measure(component)
    if not checked:
        raise SystemExit(f"oled mode entry: no sources under {component}")
    for failure in failures:
        print(f"FAIL {failure}", file=sys.stderr)
    if failures:
        raise SystemExit(f"oled mode entry: {len(failures)} violation(s)")
    print(f"oled mode entry: {checked} source files; one availability question, one slug reader")


if __name__ == "__main__":
    main()
