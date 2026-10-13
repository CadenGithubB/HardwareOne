#!/usr/bin/env python3
"""Feed well-formed, truncated and oversized tokenizer blobs to the real loader.

The production tokenizer structs, loadTokenizerFromFile with its merge map,
freeTokenizer, encode and decode are compiled unchanged against an in-memory
File and a PSRAM allocator that truncates sizes to the ESP32's 32-bit size_t.
Every malformed blob must fail the load (no out-of-blob reads, no ids at or
above the vocabulary, no 16-bit merge-key collisions) and leave nothing that
freeTokenizer cannot release. Each case runs in its own process so one ASan
abort does not hide the others. Weight loading and forward() are not covered.
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


def generate() -> str:
    internal = (COMPONENT / "System_LLM_Internal.h").read_text()
    types = "\n".join(extract_block(internal, marker) + ";" for marker in (
        "struct MergeEntry", "struct MergeLookup", "struct PreSplitToken", "struct TokenizerState"))
    source = (COMPONENT / "System_LLM_Tokenizer.cpp").read_text()
    start, end = "// Simple open-addressing hash map", "void freeTokenizer()"
    for marker in (start, end, "int encode(const char* text,", "const char* decode(int prev_token,"):
        require(source.count(marker) == 1, f"one production marker: {marker}")
    loader = source[source.index(start):source.index(end)]
    require("bool loadTokenizerFromFile(File& f)" in loader, "loader inside the extracted region")
    production = "\n".join([loader] + [extract_block(source, marker) for marker in (
        end, "int encode(const char* text,", "const char* decode(int prev_token,")])
    harness = (HERE / "llm_tokenizer_loader_harness.cpp").read_text()
    for marker, text in (("// INSERT_TYPES", types), ("// INSERT_PRODUCTION", production)):
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, text)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-llm-tokenizer-") as directory:
        work = Path(directory)
        source, binary = work / "test.cpp", work / "test"
        source.write_text(generate())
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-function", "-Wno-unused-parameter", str(source), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=120)
        cases = subprocess.run([str(binary), "--list"], check=True, capture_output=True,
                               text=True, timeout=30).stdout.split()
        failed = []
        for case in cases:
            try:
                result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=60)
                ok, detail = result.returncode == 0, (result.stdout + result.stderr).strip()
            except subprocess.TimeoutExpired:
                ok, detail = False, "timed out"
            first = next((line for line in detail.splitlines()
                          if "Assertion" in line or "ERROR: AddressSanitizer" in line
                          or "runtime error" in line), detail.splitlines()[-1] if detail else "")
            print(f"{'PASS' if ok else 'FAIL'} {case}" + ("" if ok else f": {first}"))
            if not ok:
                failed.append(case)
        require(not failed, f"tokenizer loader cases failed: {', '.join(failed)}")


if __name__ == "__main__":
    main()
