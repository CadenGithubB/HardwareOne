#!/usr/bin/env python3
"""The OLED owner holds the display-session writer for the check only.

Compiles, with oled_commit_window_harness.cpp, from OLED_Utils.cpp:
  * the command-result path, executeOLEDCommandWithResultImpl (with
    oledDisplayEpochLiveUnderWriter where the tree has it);
  * the frame-commit fence, oledFrameCommitAllowed and oledCommitFrame.
The fakes model the display lifecycle writer as a mutex another task's
publisher (remote login, logout, replacement) has to wait for, the live-session
registry, the boundary generation and the panel push.

Checks: the result copy and the panel push run with the writer released, so a
publication is never held off by them; a replacement or logout that lands
during the copy wipes the result and returns false; one that lands during the
push marks the frame dirty; a stale session (boundary pending, other epoch,
user or auth state) pushes nothing and marks dirty; the logged-out login
screen does not re-mark itself dirty; the re-checks open no bypass session
(display auth off); an expected epoch that is no longer the display's is
refused before submitting. A source contract checks that updateOLEDDisplay
commits through oledCommitFrame() and pushes nothing itself. The writer-window
scan over OLED_Utils.cpp lives in session_snapshot.
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

RESULT_PATH = ("static bool oledDisplayEpochLiveUnderWriter(",
               "static bool executeOLEDCommandWithResultImpl(")
COMMIT_FENCE = ("static bool oledFrameCommitAllowed(", "static void oledCommitFrame(")


def read_source(component: Path, source_dir: Path | None, name: str) -> str:
    overlay = source_dir / name if source_dir else None
    path = overlay if overlay and overlay.exists() else component / name
    return path.read_text(encoding="utf-8")


def has_definition(source: str, signature: str) -> bool:
    return re.search(re.escape(signature) + r"[^;{]*\{", source) is not None


def definition(source: str, signature: str) -> str:
    """The definition of `signature`, skipping forward declarations and calls."""
    match = re.search(re.escape(signature) + r"[^;{]*\{", source)
    require(match is not None, f"production definition not found: {signature}")
    return extract_block(source[match.start():], signature)


def code_only(text: str) -> str:
    """Drop // and /* */ comments (enough for the call checks below)."""
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)


def contract_failures(oled: str) -> list[str]:
    failures = []
    update = code_only(definition(oled, "void updateOLEDDisplay("))
    if "displayUpdate(" in update:
        failures.append("updateOLEDDisplay pushes the panel itself instead of through oledCommitFrame()")
    if update.count("oledCommitFrame();") != 1:
        failures.append("updateOLEDDisplay must commit through oledCommitFrame() exactly once")
    for signature in COMMIT_FENCE + RESULT_PATH:
        if not has_definition(oled, signature):
            continue
        body = code_only(definition(oled, signature))
        if "localDisplayTransportSessionSnapshot(" in body:
            failures.append(f"{signature.rstrip('(')}: re-checks through the opening snapshot "
                            "(use transportSessionEpochIsLive)")
    if has_definition(oled, "static void oledCommitFrame("):
        commit = code_only(definition(oled, "static void oledCommitFrame("))
        after_push = commit[commit.find("displayUpdate("):]
        if "BeginUiSync(" in after_push:
            failures.append("oledCommitFrame re-checks through BeginUiSync, which opens a bypass session")
    return failures


def build_unit(oled: str) -> tuple[str, bool]:
    pieces = [definition(oled, signature) for signature in RESULT_PATH
              if signature != RESULT_PATH[0] or has_definition(oled, signature)]
    fence = all(has_definition(oled, signature) for signature in COMMIT_FENCE)
    if fence:
        pieces += [definition(oled, signature) for signature in COMMIT_FENCE]
    harness = (HERE / "oled_commit_window_harness.cpp").read_text(encoding="utf-8")
    require(harness.count("// INSERT_OLED_UTILS") == 1, "harness marker missing: // INSERT_OLED_UTILS")
    harness = harness.replace("// INSERT_OLED_UTILS", "\n\n".join(pieces))
    return f"#define HARNESS_HAS_COMMIT_FENCE {1 if fence else 0}\n" + harness, fence


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=HERE.parents[1])
    parser.add_argument("--source-dir", type=Path)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")

    oled = read_source(args.component.resolve(), args.source_dir, "OLED_Utils.cpp")
    failures = contract_failures(oled)
    unit_text, fence = build_unit(oled)
    if not fence:
        failures.append("frame-commit fence (oledFrameCommitAllowed/oledCommitFrame) absent: "
                        "the panel push is not separated from the writer window")
    with tempfile.TemporaryDirectory(prefix="hw1-oled-commit-window-") as directory:
        work = Path(directory)
        unit = work / "oled_commit_window_unit.cpp"
        binary = work / "oled_commit_window"
        unit.write_text(unit_text, encoding="utf-8")
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-function", str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=120)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
        if result.returncode != 0:
            failures.append("harness failed:\n" + result.stderr.strip())
        count = re.search(r"(?m)^checks=(\d+)$", result.stdout)
    if failures:
        print("oled_commit_window FAILED:")
        for failure in failures:
            print("  " + failure)
        raise SystemExit(1)
    require(count is not None, "missing completed assertion count")
    print(f"OLED commit window: {count[1]} checks passed; the result copy and the panel push "
          "run outside the display-session writer")


if __name__ == "__main__":
    main()
