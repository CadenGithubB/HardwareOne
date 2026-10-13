#!/usr/bin/env python3
"""OLED display commands from cmd_exec reach main-loop-owned state only on the owner frame.

Extracts from the production tree and compiles with an owner-task double
(oled_owner_requests_harness.cpp):
  * OLED_Utils.cpp: the owner/session-boundary statics and forward declarations
    at the top, oledBindOwnerTask, oledIsOwnerTask, requestOLEDMode, the
    owner-request queue (from `struct OledOwnerRequest {` up to cmd_oledmode;
    absent on trees without it), cmd_oledmode, cmd_oledtext, cmd_oledclear,
    cmd_oledanim and oledUpdate (the owner frame);
  * OLED_Mode_SetPattern.cpp: the flow statics, the owner-side restart step
    when present, and cmd_setpattern (setgamepadpassword);
  * OLED_Display.h: the mode enum.
The harness calls the commands and requestOLEDMode() as cmd_exec and checks that
customOLEDText, the mode, the back stack, onEnter hooks, the display-role read,
the framebuffer, the animation and the pattern-flow state change only when the
owner runs oledUpdate(), with the same side effects as before. `oledmode` to a
mode whose own entry rule refuses (oledModeRuleAllows) shows the unavailable page
with the module's reason and pushes the previous screen; the display-auth gate
keeps precedence. A source check confirms that updateOLEDDisplay() drains the
requests right after its first session-boundary apply.
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

OWNER_FUNCTIONS = ("static void oledBindOwnerTask(", "static bool oledIsOwnerTask(")
COMMANDS = (
    "const char* cmd_oledmode(",
    "const char* cmd_oledtext(",
    "const char* cmd_oledclear(",
    "const char* cmd_oledanim(",
    "void oledUpdate(",
)
OWNER_TOP = "static std::atomic<uint32_t> sOledSessionBoundaryRequested"
REQUEST_SPAN = "struct OledOwnerRequest {"
PATTERN_RESTART = "static void setPatternRestartFromCommand("


def read_source(component: Path, source_dir: Path | None, name: str) -> str:
    overlay = source_dir / name if source_dir else None
    path = overlay if overlay and overlay.exists() else component / name
    return path.read_text(encoding="utf-8")


def definition_start(source: str, signature: str) -> int:
    """Offset of the definition of `signature`, skipping declarations and calls."""
    boundary = r"\b" if signature[-1].isalnum() else ""
    match = re.search(re.escape(signature) + boundary + r"[^;{]*\{", source)
    require(match is not None, f"production definition not found: {signature}")
    return match.start()


def definition(source: str, signature: str) -> str:
    return extract_block(source[definition_start(source, signature):], signature)


def owner_top(oled: str) -> str:
    start = oled.index(OWNER_TOP)
    end = oled.index("\n#endif", start)
    return oled[start:end]


def request_span(oled: str) -> str:
    """The owner-request queue beside the OLED commands (empty when absent)."""
    start = oled.find(REQUEST_SPAN)
    if start < 0:
        return ""
    end = definition_start(oled, "const char* cmd_oledmode(")
    require(start < end, "struct OledOwnerRequest must precede cmd_oledmode()")
    return oled[start:end]


def pattern_static(source: str, declaration: str) -> str:
    match = re.search(rf"(?m)^{re.escape(declaration)}\b[^;]*;", source)
    require(match is not None, f"OLED_Mode_SetPattern.cpp static not found: {declaration}")
    return match.group(0)


def build_unit(component: Path, source_dir: Path | None) -> str:
    display_h = read_source(component, source_dir, "OLED_Display.h")
    oled = read_source(component, source_dir, "OLED_Utils.cpp")
    pattern = read_source(component, source_dir, "OLED_Mode_SetPattern.cpp")

    mode_enum = definition(display_h, "enum OLEDMode").replace(
        "enum OLEDMode", "enum OLEDMode : int", 1)
    owner = "\n\n".join([owner_top(oled)] + [definition(oled, s) for s in OWNER_FUNCTIONS])
    transition = "\n\n".join([definition(oled, "void requestOLEDMode("), request_span(oled)] +
                             [definition(oled, s) for s in COMMANDS])
    setpattern = [definition(pattern, "enum PatternSetupStep") + ";",
                  pattern_static(pattern, "static PatternSetupStep sStep"),
                  pattern_static(pattern, "static String sFirstPattern")]
    if PATTERN_RESTART in pattern:
        setpattern.append(definition(pattern, PATTERN_RESTART))
    setpattern.append(definition(pattern, "static const char* cmd_setpattern("))

    harness = (HERE / "oled_owner_requests_harness.cpp").read_text(encoding="utf-8")
    for marker, text in (("// INSERT_TYPES", mode_enum + ";"),
                         ("// INSERT_OWNER", owner),
                         ("// INSERT_TRANSITION", transition),
                         ("// INSERT_SETPATTERN", "\n\n".join(setpattern))):
        require(harness.count(marker) == 1, f"harness marker missing: {marker}")
        harness = harness.replace(marker, text)
    return harness


def frame_drain_failures(component: Path, source_dir: Path | None) -> list[str]:
    """updateOLEDDisplay() drains posted requests right after the session boundary."""
    body = definition(read_source(component, source_dir, "OLED_Utils.cpp"),
                      "void updateOLEDDisplay(")
    boundary = body.find("oledApplyPendingSessionBoundary();")
    running = body.find("if (!gOledRunning")
    drain = body.find("oledApplyPendingOwnerRequests();", max(boundary, 0))
    if boundary < 0 or running < 0:
        return ["updateOLEDDisplay(): session-boundary apply or running check not found"]
    if drain < 0 or drain > running:
        return ["updateOLEDDisplay() does not apply the requests other tasks posted right after "
                "its first session-boundary apply (before the running check)"]
    between = body[boundary + len("oledApplyPendingSessionBoundary();"):drain]
    if re.search(r"\b\w+\s*\(", re.sub(r"//[^\n]*", "", between)):
        return ["updateOLEDDisplay(): nothing may run between the session-boundary apply and "
                "the request drain"]
    return []


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=HERE.parents[1])
    parser.add_argument("--source-dir", type=Path)
    args = parser.parse_args()
    component = args.component.resolve()

    unit_text = build_unit(component, args.source_dir)
    failures = frame_drain_failures(component, args.source_dir)
    with tempfile.TemporaryDirectory(prefix="hw1-oled-owner-requests-") as directory:
        work = Path(directory)
        unit = work / "oled_owner_requests_unit.cpp"
        binary = work / "oled_owner_requests"
        unit.write_text(unit_text, encoding="utf-8")
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-function", str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True)
        result = subprocess.run([str(binary)], check=False)
        if result.returncode != 0:
            failures.append("owner-task harness checks failed (see FAIL lines above)")
    for failure in failures:
        print(f"FAIL: {failure}")
    if failures:
        raise SystemExit("oled_owner_requests: FAILED")
    print("oled_owner_requests: OK")


if __name__ == "__main__":
    main()
