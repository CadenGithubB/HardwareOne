#!/usr/bin/env python3
"""A feature that is compiled out is described by its own header.

The rule (CONTRIBUTING.md, "Keep interfaces small"):

1. The header of a feature has two branches on the feature's flag. The `#else`
   branch holds what the rest of the program may still name when the feature is
   off: inline functions that report "not available" and inline variables for
   the state. A definition that cannot be inline sits in the `#else` branch of
   the feature's own source file.
2. There is no central stub file. `System_SensorStubs.h` and
   `System_SensorStubs.cpp` are gone and no file takes their place.
3. A file that names a feature includes the feature's header without a
   condition. `#if ENABLE_X` around the include of the header that owns
   `ENABLE_X` hides the disabled branch and brings the missing names back.
4. A state name of the disabled branch (`inline bool gXRunning = false;`) is
   declared in the owning header only. An `extern` for it in another file
   compiles today and misses the next change of the type.

OWNERS lists the headers that follow the rule and the flag each one owns. A new
optional feature adds its header here.

Reads the working tree. --component points the test at another component
directory.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

from test_shared_text_helpers import blank_comments, sources

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

OWNERS: dict[str, str] = {
    "i2csensor_mlx90640.h": "ENABLE_THERMAL_SENSOR",
    "i2csensor_vl53l4cx.h": "ENABLE_TOF_SENSOR",
    "i2csensor_bno055.h": "ENABLE_IMU_SENSOR",
    "i2csensor_apds9960.h": "ENABLE_APDS_SENSOR",
    "i2csensor_pa1010d.h": "ENABLE_GPS_SENSOR",
    "i2csensor_rda5807.h": "ENABLE_FM_RADIO",
    "i2csensor_sths34pf80.h": "ENABLE_PRESENCE_SENSOR",
    "i2csensor_ds3231.h": "ENABLE_RTC_SENSOR",
    "i2csensor_seesaw.h": "ENABLE_GAMEPAD_SENSOR",
    "i2csensor_encoder.h": "ENABLE_ENCODER",
    "System_Microphone.h": "ENABLE_MICROPHONE",
    "System_Camera_DVP.h": "ENABLE_CAMERA_SENSOR",
    "System_WiFi.h": "ENABLE_WIFI",
    "System_ESPNow.h": "ENABLE_ESPNOW",
    "OLED_Display.h": "ENABLE_OLED_DISPLAY",
    "WebServer_Server.h": "ENABLE_HTTP_SERVER",
    "WebServer_Utils.h": "ENABLE_HTTP_SERVER",
    "WebServer_Handle.h": "ENABLE_HTTP_SERVER",
}

REMOVED_FILES = ("System_SensorStubs.h", "System_SensorStubs.cpp")
CENTRAL_STUB_NAME = re.compile(r"(?i)stubs?\.(?:h|hpp|cpp|c|inc)$")
DIRECTIVE = re.compile(r"^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif)\b[ \t]*([^\n]*)$", re.M)
GUARDED_INCLUDE = re.compile(
    r'^[ \t]*#[ \t]*if[ \t]+(\w+)[ \t]*\n[ \t]*#[ \t]*include[ \t]+"([^"\n]+)"[ \t]*\n[ \t]*#[ \t]*endif\b',
    re.M)
INLINE_VARIABLE = re.compile(
    r"^[ \t]*inline[ \t]+(?!constexpr\b)[\w:<>\*& \t]+?[ \t\*&](\w+)[ \t]*(?:=[^;()]*|\{[^;()]*\})?;", re.M)


def disabled_branches(code: str, flag: str) -> list[str]:
    """Text of every `#else` branch of an `#if <flag>` in this header."""
    stack: list[tuple[bool, int | None]] = []   # (opened by `#if <flag>`, start of its #else)
    found = []
    for match in DIRECTIVE.finditer(code):
        word, rest = match.group(1), match.group(2).strip()
        if word in ("if", "ifdef", "ifndef"):
            stack.append((word == "if" and rest == flag, None))
        elif word == "else" and stack and stack[-1][0]:
            stack[-1] = (True, match.end())
        elif word == "elif" and stack and stack[-1][0]:
            stack[-1] = (False, None)
        elif word == "endif" and stack:
            mine, start = stack.pop()
            if mine and start is not None:
                found.append(code[start:match.start()])
    return found


def measure(component: Path):
    failures: list[str] = []
    texts = {relative: blank_comments(text, keep_strings=True)
             for relative, text in sources(component, (".h", ".cpp", ".c", ".inc"))}

    # 2. No central stub file, and nothing includes the removed one.
    for relative in sorted(texts):
        name = relative.rsplit("/", 1)[-1]
        if name in REMOVED_FILES or CENTRAL_STUB_NAME.search(name):
            failures.append(f"central stub file: {relative}; put each stub in the #else branch "
                            f"of the header that owns the feature")
        for removed in REMOVED_FILES:
            if re.search(r'#[ \t]*include[ \t]+"[^"\n]*%s"' % re.escape(removed), texts[relative]):
                failures.append(f"{relative} includes {removed}, which is removed")

    # 1. Each owning header has a disabled branch on its own flag.
    state_names: dict[str, str] = {}
    for header, flag in OWNERS.items():
        if header not in texts:
            failures.append(f"{header} is listed in OWNERS and does not exist")
            continue
        branches = disabled_branches(texts[header], flag)
        if not branches:
            failures.append(f"{header} has no #else branch for #if {flag}; a build without the "
                            f"feature needs its disabled branch there")
        for branch in branches:
            for name in INLINE_VARIABLE.findall(branch):
                state_names[name] = header

    # 3. No include of an owning header behind its own flag.
    for relative in sorted(texts):
        for match in GUARDED_INCLUDE.finditer(texts[relative]):
            flag, included = match.group(1), match.group(2).rsplit("/", 1)[-1]
            if OWNERS.get(included) == flag:
                number = texts[relative].count("\n", 0, match.start()) + 1
                failures.append(f"{relative}:{number} includes {included} only when {flag} is set; "
                                f"include it without the condition, it has its own disabled branch")

    # 4. A disabled-branch state name is declared in its owning header only.
    if state_names:
        declared = re.compile(r"^[ \t]*extern\b[^;()\n]*\b(%s)\b[^;()\n]*;"
                              % "|".join(map(re.escape, sorted(state_names))), re.M)
        for relative in sorted(texts):
            for match in declared.finditer(texts[relative]):
                owner = state_names[match.group(1)]
                if relative.rsplit("/", 1)[-1] == owner:
                    continue
                number = texts[relative].count("\n", 0, match.start()) + 1
                failures.append(f"{relative}:{number} declares {match.group(1)}; include {owner}, "
                                f"which owns the name in both branches")
    return len(texts), len(state_names), failures


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    component = args.component.resolve()

    checked, names, failures = measure(component)
    if not checked:
        raise SystemExit(f"disabled feature branches: no sources under {component}")
    for failure in failures:
        print(f"FAIL {failure}", file=sys.stderr)
    if failures:
        raise SystemExit(f"disabled feature branches: {len(failures)} violation(s)")
    print(f"disabled feature branches: {checked} source files, {len(OWNERS)} owning headers, "
          f"{names} disabled-state names, no central stub file")


if __name__ == "__main__":
    main()
