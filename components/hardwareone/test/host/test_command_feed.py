#!/usr/bin/env python3
"""Execute the shipping command-origin prefix and command-feed writer.

originPrefix (its forward declaration with the default suffix, and its
definition) and appendCommandToFeed come from HardwareOne.cpp unchanged, in
source order. The CM5/live-audio classifiers, redactCmdForAudit and the web
mirror are recorded boundaries.
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


def generate() -> str:
    source = (COMPONENT / "HardwareOne.cpp").read_text()
    declaration = re.findall(r"static String originPrefix\([^;{]*\);", source)
    require(len(declaration) == 1, "HardwareOne.cpp forward-declares originPrefix once")
    definition = re.search(r"static String originPrefix\([^;{]*\)\s*\{", source)
    require(definition is not None, "HardwareOne.cpp defines originPrefix")
    require(source.count("originPrefix(source, ctx.auth.user, ctx.auth.ip)") == 1,
            "the command-reply caller uses the default (empty) suffix")
    production = "\n\n".join((declaration[0],
                              extract_block(source, "void appendCommandToFeed("),
                              extract_block(source[definition.start():], "static String originPrefix(")))
    harness = (HERE / "command_feed_harness.cpp").read_text()
    require(harness.count("// INSERT_PRODUCTION") == 1, "one insertion marker")
    return harness.replace("// INSERT_PRODUCTION", production)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-command-feed-") as temporary:
        unit, binary = Path(temporary) / "test.cpp", Path(temporary) / "test"
        unit.write_text(generate())
        # originPrefix truncates into its 128-byte buffer on purpose; GCC would
        # flag that bounded snprintf under -Wall (clang does not).
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-function", "-Wno-format-truncation",
                   "-I", str(HERE / "web_batch_stubs"), str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=120)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
        require(result.returncode == 0, f"command feed failed:\n{result.stdout}\n{result.stderr}")
        count = re.search(r"(?m)^checks=(\d+)$", result.stderr)
        require(count is not None, "missing completed check count")
        print(f"{result.stdout.strip()} ({count[1]} checks)", flush=True)


if __name__ == "__main__":
    main()
