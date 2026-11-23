#!/usr/bin/env python3
"""Execute the production System_SealedLog.cpp (B11) over the shared fake VFS.

Log files on the card are sealed with the device key. The module that opens,
marks and writes them is compiled here unchanged (its header and source, their
includes dropped) against fake_vfs.h; the device key and the cipher are the toy
ones of sealed_log_host.h. Checked: the mark first in a new or empty file; a
file found ours read once per mount; a file that is not this device's (another
key id, no mark, a mark with no id, a torn mark) moved aside unchanged to the
first free "<name>.foreign-<n>" and never appended to or removed; nothing
created without the key; one sealed row per line, a line over 1023 bytes split
and never dropped, a caller's smaller row limit; a short write leaving whole
rows and no plain byte; openFresh; writeMark; lines put together across pieces
(LineStream); and readWindow: whole rows by stored offset, the mark skipped, a
plain schema line passed through, rows that do not open, another device's file,
the ends of the file and a failed read. Since the review of 2026-11-22 also:
a write that the handle accepted but the card lost is caught by the size after
the flush (settle) and linesLanded says what did arrive; a torn last row gets
its line end before the next row; nothing is judged or moved without the key;
a file is moved aside only in the log trees and the oldest set-aside name is
reused when all are taken; a row opens only at a path of the class it was
sealed for (a transcript copied to another account's folder stays closed); and
a plain line where only sealed rows belong reads "[unsealed line]".

The real cipher and key are covered by test_capture_crypto_key.py; the writers
around this module by their own suites (capped logs, sensor rows, the
automation log, transcripts).
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_fake_vfs import string_double
from test_web_batch_handlers import require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def generate() -> str:
    strip = lambda text: re.sub(r"(?m)^#(include|ifndef SYSTEM_SEALED_LOG_H|define SYSTEM_SEALED_LOG_H)[^\n]*\n", "", text)
    header = strip((COMPONENT / "System_SealedLog.h").read_text())
    header = header[:header.rindex("#endif")]
    source = strip((COMPONENT / "System_SealedLog.cpp").read_text())
    crypto_header = (COMPONENT / "System_CaptureCrypto.h").read_text()
    macros = "\n".join(line for line in re.findall(r"^#define CAPCRYPT_.*$", crypto_header, re.MULTILINE)
                       if "CAPCRYPT_MARK_LINE_MAX" not in line)  # sealed_log_host.h defines that one
    harness = (HERE / "sealed_log_harness.cpp").read_text()
    for marker, text in {"// INSERT_CAPCRYPT_MACROS": macros, "// INSERT_SEALED_LOG": header + "\n" + source}.items():
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, text)
    return harness


def source_contract() -> None:
    source = re.sub(r"//[^\n]*", "", (COMPONENT / "System_SealedLog.cpp").read_text())
    # Explicit PSRAM at the source, nothing in internal RAM, and no file call
    # that goes around the guarded VFS.
    require("EXT_RAM_BSS_ATTR State sState;" in source, "the module's state is in external BSS")
    require(source.count("ps_alloc(") == 4 and source.count("AllocPref::PreferPsram") == 4 and "malloc(" not in source,
            "the row, plain, carry and read buffers are ps_alloc blocks")
    for banned in ("LittleFS.", "SD.open", "SD.remove", "fopen(", "VFS::open(", "VFS::remove(", "VFS::rename("):
        require(banned not in source, f"System_SealedLog.cpp must not use {banned}")
    # No plain write path: every byte written is the mark or a sealed row.
    writes = re.findall(r"\.write\(([^;]*);", source)
    require(len(writes) == 3 and all("mark" in w or "row" in w or "&lineEnd, 1" in w for w in writes),
            "the module writes the mark line, sealed rows and the line end of a torn row, nothing else")
    require("captureCryptoSealLine(path.c_str(), line, take, row, CAPCRYPT_MAX_SEALED)" in source,
            "every row goes through the seal, bound to the class of its file's path")
    require("captureCryptoOpenLine(path.c_str(), line, textLength, plain, CAPCRYPT_MAX_ROW + 1)" in source,
            "a row is opened with the class of the path it is read at")
    require("if (!sealedlog::inLogTree(path.c_str())) return false;" in source,
            "a file is moved aside only inside the log trees")
    require(re.search(r"Head inspect\([^)]*\) \{\s*char ownId\[[^\]]*\];\s*if \(!captureCryptoEnsureKey\(\) \|\| "
                      r"!captureCryptoKeyIdHex\(ownId\)\) return Head::Unreadable;", source) is not None,
            "nothing is judged without the device key")
    require("return !file.getWriteError();" in source and
            "const bool landed = !file.getWriteError() && file.size() == expectedStored;" in source,
            "a write counts only when the flushed file has the expected size")
    header = (COMPONENT / "System_CaptureCrypto.h").read_text()
    for name in ("CAPCRYPT_MAX_ROW", "CAPCRYPT_MAX_SEALED", "CAPCRYPT_MARK_LINE_MAX"):
        require(re.search(rf"^#define {name}\s+\d+$", header, re.M) is not None, f"{name} is a plain number")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-sealed-log-") as directory:
        work = Path(directory)
        (work / "Arduino.h").write_text(string_double())
        unit, binary = work / "sealed_log.cpp", work / "sealed_log"
        unit.write_text(generate())
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wno-unused-parameter",
                   "-Wno-unused-function", "-I", directory, "-I", str(HERE), "-I", str(COMPONENT),
                   str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=180)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
        require(result.returncode == 0, f"sealed log failed:\n{result.stdout}\n{result.stderr}")
        print(result.stdout.strip())
    source_contract()
    print("sealed log: source contract passed")


if __name__ == "__main__":
    main()
