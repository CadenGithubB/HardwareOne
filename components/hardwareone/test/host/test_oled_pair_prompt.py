#!/usr/bin/env python3
"""Run the production OLED incoming-pair-request prompt against a refusing owner.

Extracts from the production tree:
  * OLED_ESPNow.cpp: the owner-command helper `oledEspNowRunOwnerCommand` when
    the file has one, and everything after the `sPairingSel` static through the
    end of `oledEspNowPollPairRequest()` (the prompt statics, the Accept/Reject
    callbacks and the poll);
  * OLED_Utils.cpp: the shipping confirm overlay (state, request, close, input,
    session-boundary reset);
  * OLED_Display.h: the mode enum and NavEvents.
The pending request, the owner commands espnowaccept/espnowreject behind a
switchable admin refusal, the cached session role and the toast are doubles.
The checks: the prompt opens once per request (sender MAC plus arrival time)
even when the owner refuses the answer, the refusal is toasted, the prompt never
opens for an identity that cannot answer (login screen, AuthBypass, non-admin),
the answer names the shown sender, a session boundary re-shows an unanswered
request, and idle frames read nothing.
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
HELPER = "static bool oledEspNowRunOwnerCommand("
POLL = "void oledEspNowPollPairRequest("
CONFIRM_FUNCTIONS = (
    "bool oledConfirmRequest(",
    "bool oledConfirmIsActive(",
    "static void oledConfirmResetForSessionBoundary(",
    "static void oledConfirmClose(",
    "static bool oledConfirmHandleInput(",
)


def read_source(component: Path, source_dir: Path | None, name: str) -> str:
    overlay = source_dir / name if source_dir else None
    path = overlay if overlay and overlay.exists() else component / name
    return path.read_text(encoding="utf-8")


def definition(source: str, signature: str) -> str:
    """The definition of `signature`, skipping forward declarations and calls."""
    match = re.search(re.escape(signature) + r"[^;{]*\{", source)
    require(match is not None, f"production definition not found: {signature}")
    return extract_block(source[match.start():], signature)


def prompt_span(oled: str) -> str:
    """The prompt statics, callbacks and poll that follow the pairing-row static."""
    anchor = re.search(r"(?m)^static int sPairingSel\b[^\n]*\n", oled)
    require(anchor is not None, "OLED_ESPNow.cpp: the sPairingSel static is missing")
    poll = definition(oled[anchor.end():], POLL)
    end = oled.index(poll, anchor.end()) + len(poll)
    span = oled[anchor.end():end]
    require("oledConfirmRequest(" in span, "the poll no longer opens the confirm dialog")
    return span


def build_unit(component: Path, source_dir: Path | None) -> str:
    oled = read_source(component, source_dir, "OLED_ESPNow.cpp")
    utils = read_source(component, source_dir, "OLED_Utils.cpp")
    display = read_source(component, source_dir, "OLED_Display.h")
    types = [extract_block(display, marker) + ";" for marker in ("enum OLEDMode", "struct NavEvents")]
    state = re.search(r"(?m)^static OLEDConfirmState gOledConfirmState\b[^\n]*;", utils)
    require(state is not None, "OLED_Utils.cpp: gOledConfirmState is missing")
    confirm = [extract_block(utils, "struct OLEDConfirmState") + ";", state.group(0)]
    confirm += [definition(utils, signature) for signature in CONFIRM_FUNCTIONS]
    # The shared refusal helper exists only after the A2/A12 fixes; a tree
    # without it still compiles here and is judged by the behavioral cases.
    helper = definition(oled, HELPER) if HELPER in oled else ""
    harness = (HERE / "oled_pair_prompt_harness.cpp").read_text(encoding="utf-8")
    for marker, body in (("// INSERT_TYPES", "\n\n".join(types)),
                         ("// INSERT_CONFIRM", "\n\n".join(confirm)),
                         ("// INSERT_HELPER", helper),
                         ("// INSERT_PROMPT", prompt_span(oled))):
        require(harness.count(marker) == 1, f"missing/duplicate marker: {marker}")
        harness = harness.replace(marker, body)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=HERE.parents[1],
                        help="production component directory (overlay for base runs)")
    parser.add_argument("--source-dir", type=Path,
                        help="directory whose files override the component's")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-Wno-unused-parameter", "-Wno-unused-function"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    unit_text = build_unit(args.component.resolve(), args.source_dir)
    with tempfile.TemporaryDirectory(prefix="hw1-oled-pair-prompt-") as directory:
        unit, executable = Path(directory) / "test.cpp", Path(directory) / "test"
        unit.write_text(unit_text, encoding="utf-8")
        subprocess.run([args.cxx, *flags, str(unit), "-o", str(executable)],
                       check=True, timeout=120)
        result = subprocess.run([str(executable)], timeout=60)
    require(result.returncode == 0, "OLED pair prompt: behavioral checks failed (see FAIL lines)")
    print("OLED pair prompt: once per request, only for an identity that may answer it")


if __name__ == "__main__":
    main()
