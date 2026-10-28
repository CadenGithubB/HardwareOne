#!/usr/bin/env python3
"""Run the shared LLM home menu rows, and check both surfaces build from them.

Compiles llm_home_menu_harness.cpp against System_LLMHomeMenuCore.h as is.
Static checks: G2_Glasses.cpp and OLED_Mode_LLM.cpp both take their home rows
from llmHomeMenuRowsNow and keep no private copy of the row labels, so the
glasses and the local display cannot drift apart again.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
ROW_LABELS = ('"Open chat"', '"Ask (Mic / Keys)"', '"Ask (guided)"', '"Re-run last"',
              '"Select Model"', '"Open Terminal"')


def static_checks() -> None:
    g2 = (COMPONENT / "G2_Glasses.cpp").read_text()
    oled = (COMPONENT / "OLED_Mode_LLM.cpp").read_text()
    for name, body in (("G2_Glasses.cpp g2ShowLlmMenu", extract_block(g2, "static void g2ShowLlmMenu() ")),
                       ("OLED_Mode_LLM.cpp populateHomeMenu",
                        extract_block(oled, "static void populateHomeMenu() "))):
        require("llmHomeMenuRowsNow(" in body, f"{name} must build its rows from llmHomeMenuRowsNow")
        for label in ROW_LABELS:
            require(label not in body, f"{name} keeps a private row label {label}")
    require("LLM_HOME_" not in g2, "G2_Glasses.cpp still has its private home-action enum")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-llm-home-") as directory:
        binary = Path(directory) / "test"
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-I", str(COMPONENT), str(HERE / "llm_home_menu_harness.cpp"), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-g"]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
    static_checks()
    print("llm_home_menu: both surfaces build their home rows from the shared core")


if __name__ == "__main__":
    main()
