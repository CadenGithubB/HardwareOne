#!/usr/bin/env python3
"""Run the file catch-up ledger store (System_CatchupLedger.cpp) over the fake VFS.

Extracts the store between CATCHUP_LEDGER_HOST_BEGIN and _END and the
declarations of System_CatchupLedger.h, compiles them with
catchup_ledger_harness.cpp, the shared fake_vfs.h, the real ArduinoJson and
the real pure core, and checks: create, load and save of ledger.json (schema 1,
staged, UTC); system-log rotation (more than one step between two walks),
sensor rotation and the TimeAnchors promotion keep their ids, two boot files
with equal heads never swap; a trim note shifts the base and keeps the id, a
restart gives a new id, a trim without its note gives a new id; a card taken
out and put back keeps the id; the walk budget, with the FS lock taken per
folder and released between folders; the whitelist (never-offered paths) and
the sealing rule, including a hand-edited ledger that tries to unseal a file;
`delivered` saved lazily, by the 5-minute rule and by a flush; range reads
with head, shrink and missing checks; and the scopes every guarded call ran
under.

Source rules: the trim and restart hooks do no I/O, logging or allocation;
the store allocates only through its PSRAM adapter.

Not covered: real FreeRTOS, the PSRAM allocator, LittleFS/FAT timing (the
0.5 s target is measured on the bench), and the server (test_espnow_catchup_serve.py).
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
BEGIN = "\n// CATCHUP_LEDGER_HOST_BEGIN\n"
END = "\n// CATCHUP_LEDGER_HOST_END\n"


def store_text() -> str:
    source = (COMPONENT / "System_CatchupLedger.cpp").read_text()
    require(source.count(BEGIN) == 1 and source.count(END) == 1, "one host region in System_CatchupLedger.cpp")
    return source[source.index(BEGIN):source.index(END)]


def header_text() -> str:
    header = (COMPONENT / "System_CatchupLedger.h").read_text()
    start = header.index("#if ENABLE_ESPNOW_SEND")
    end = header.index("#else  // !ENABLE_ESPNOW_SEND")
    body = header[start + len("#if ENABLE_ESPNOW_SEND"):end]
    return re.sub(r"^#include[^\n]*\n", "", body, flags=re.M)


def code_only(text: str) -> str:
    """The text without comments and string literals (rules look at code)."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return re.sub(r'"(?:\\.|[^"\\\n])*"', '""', text)


def source_rules(store: str) -> list[str]:
    failures = []
    code = code_only(store)
    for hook in ("void catchupNoteTrim(", "void catchupNoteRestart("):
        body = code_only(extract_block(store, hook))
        for banned in ("VFS::", "File ", "CL_WARN", "CL_DEBUG", "alloc", "String", "Serial", "printf", "new "):
            if banned in body:
                failures.append(f"{hook.strip('(')} uses {banned!r}: the hook runs inside the log writer")
    for banned in (r"\bmalloc\(", r"\bnew\s+[A-Za-z_]", r"std::vector", r"std::string", r"\bps_alloc\("):
        if re.search(banned, code):
            failures.append(f"the store uses {banned}: allocate through clAlloc (PSRAM) only")
    scopes = re.findall(r'VFS::systemAuth\("([^"]+)"\)', store)
    if scopes != ["catchup.read.battery"]:
        failures.append(f"only battery reads may run unscoped, found {scopes}")
    return failures


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    store = store_text()
    failures = source_rules(store)
    harness = (HERE / "catchup_ledger_harness.cpp").read_text()
    for marker in ("// INSERT_HEADER", "// INSERT_LEDGER"):
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
    unit_text = harness.replace("// INSERT_HEADER", header_text()).replace("// INSERT_LEDGER", store)
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
             "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
             "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0", "-DARDUINOJSON_ENABLE_PROGMEM=0",
             "-I", str(HERE / "web_batch_stubs"), "-I", str(HERE), "-I", str(COMPONENT),
             "-I", str(COMPONENT.parent / "hardwareone_libs/ArduinoJson/src")]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-catchup-ledger-") as temp:
        work = Path(temp)
        unit, binary = work / "catchup_ledger.cpp", work / "catchup_ledger"
        unit.write_text(unit_text)
        if subprocess.run([args.cxx, *flags, str(unit), "-o", str(binary)], timeout=240).returncode != 0:
            failures.append("the ledger harness does not compile")
        else:
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=120)
            print((result.stdout + result.stderr).strip())
            if result.returncode != 0:
                failures.append("the ledger harness failed")
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
