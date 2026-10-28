#!/usr/bin/env python3
"""Run the OLED LLM chat view's reply reader as it ships.

Splices the reader's constants, state and functions out of OLED_Mode_LLM.cpp
into oled_llm_reader_harness.cpp, which supplies a scripted chat module, and
runs it for the 128x64 OLED (4 visible lines) and the P4X-EYE canvas (9).
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
COMPONENT = HERE.parents[1]


def region(source: str, start: str, end_line: str) -> str:
    """From `start` through the line containing `end_line`."""
    first = source.find(start)
    require(first >= 0, f"missing production marker: {start!r}")
    last = source.find(end_line, first)
    require(last >= 0, f"missing production marker: {end_line!r}")
    return source[first:source.index("\n", last) + 1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    oled = (COMPONENT / "OLED_Mode_LLM.cpp").read_text()
    # The screen redraws only when marked dirty: displayLLM must ask for the
    # next frame whenever llmScreenIsLive() says something is still changing.
    display = extract_block(oled, "static void displayLLM() ")
    require("if (llmScreenIsLive()) oledMarkDirty();" in display,
            "displayLLM must request the next frame while the screen is live")
    cmd_enum = re.search(r"^enum : uint8_t \{ LLM_CMD_NONE[^\n]*$", oled, re.M)
    cmd_flight = re.search(r"^static uint8_t sCmdInFlight[^\n]*$", oled, re.M)
    ui_state = re.search(r"^static LLMUIState sUIState[^\n]*$", oled, re.M)
    require(bool(cmd_enum and cmd_flight and ui_state), "missing LLM screen state declarations")
    definitions = [
        region(oled, "static const int LLM_CHARS", "static const int LLM_READER_COLS"),
        extract_block(oled, "enum class LLMUIState") + ";",
        ui_state.group(), cmd_enum.group(), cmd_flight.group(),
        region(oled, "#define LLM_READER_MAX_LINES", "static int          sReaderTop"),
    ] + [extract_block(oled, marker) for marker in (
        "static size_t foldKeepingNewlines(", "static void rebuildReader(",
        "static void syncReader(", "static void readerLine(", "static void scrollReader(",
        "static bool llmScreenIsLive(",
    )]
    harness = (HERE / "oled_llm_reader_harness.cpp").read_text()
    harness = harness.replace("// INSERT_PRODUCTION_DEFINITIONS", "\n\n".join(definitions))
    with tempfile.TemporaryDirectory(prefix="hw1-oled-llm-reader-") as directory:
        source = Path(directory) / "test.cpp"
        source.write_text(harness)
        for name, height in (("oled-128x64", 43), ("p4x-eye", 99)):
            binary = Path(directory) / name
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       f"-DOLED_CONTENT_HEIGHT={height}", "-I", str(COMPONENT),
                       str(source), "-o", str(binary)]
            if args.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-g"]
            subprocess.run(command, check=True)
            print(f"{name}:", flush=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
