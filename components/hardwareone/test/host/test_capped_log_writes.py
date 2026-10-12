#!/usr/bin/env python3
"""Execute the production capped-log append/rotation and orphan recovery.

Real Arduino String and Print exercise CRLF and write-count behavior. The file,
VFS, allocation and lock boundaries are controlled; this does not qualify media
or power-loss durability. Optional saved sources establish unchanged output and
characterize the old false-success/data-loss faults separately.
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
CORE = COMPONENT.parent / "arduino/cores/esp32"


def generate(overlay: Path | None) -> str:
    source = (overlay or COMPONENT) / "System_Filesystem.cpp"
    production = source.read_text()
    markers = ["bool appendLineWithCap("]
    if "static void recoverLogOrphanAt(" in production:
        markers.append("static void recoverLogOrphanAt(")
    markers.append("void cleanupLogOrphan(")
    functions = "\n\n".join(extract_block(production, marker) for marker in markers)
    harness = (HERE / "capped_log_writes_harness.cpp").read_text()
    require(harness.count("// INSERT_PRODUCTION") == 1, "one production insertion marker")
    return f"#define BASELINE {int(overlay is not None)}\n" + harness.replace("// INSERT_PRODUCTION", functions)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path, help="saved component sources; also runs current")
    parser.add_argument("--evidence", type=Path, help="retain generated source and output")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-capped-logs-") as temp:
        work = args.evidence.resolve() if args.evidence else Path(temp)
        work.mkdir(parents=True, exist_ok=True)
        string = (CORE / "WString.cpp").read_text().replace('#include "Arduino.h"', '#include <cstdio>')
        string = string.replace('#include "esp32-hal-log.h"', '#define log_e(...) ((void)0)\n#define log_w(...) ((void)0)')
        (work / "WString.cpp").write_text(string)
        printing = (CORE / "Print.cpp").read_text().replace('#include "Arduino.h"', '#include <cstdarg>')
        (work / "Print.cpp").write_text(printing)
        conversions = (CORE / "stdlib_noniso.c").read_text().replace('#include "esp_system.h"', '')
        conversions += "\nchar *itoa(int v,char*s,int b){return ltoa(v,s,b);}\nchar *utoa(unsigned int v,char*s,int b){return ultoa(v,s,b);}\n"
        (work / "stdlib_noniso.c").write_text(conversions)
        sanitize = (["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                     "-fno-omit-frame-pointer", "-g"] if args.sanitize else [])
        flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                 "-Wno-unused-parameter", "-I", str(CORE), *sanitize]
        subprocess.run([shutil.which("cc"), "-std=c11", *sanitize, "-I", str(CORE),
                        "-c", str(work / "stdlib_noniso.c"), "-o", str(work / "conversions.o")], check=True)
        for source in ("WString", "Print"):
            subprocess.run([args.cxx, *flags, "-Wno-sign-compare", "-c", str(work / f"{source}.cpp"),
                            "-o", str(work / f"{source}.o")], check=True)
        transcripts = {}
        versions = [("baseline", args.baseline)] if args.baseline else []
        for label, source in [*versions, ("current", None)]:
            unit, binary = work / f"{label}.cpp", work / label
            unit.write_text(generate(source))
            subprocess.run([args.cxx, *flags, str(unit), str(work / "WString.o"),
                            str(work / "Print.o"), str(work / "conversions.o"), "-o", str(binary)], check=True, timeout=120)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            (work / f"{label}-transcript.txt").write_text(result.stdout)
            (work / f"{label}-assertions.log").write_text(result.stderr)
            require(result.returncode == 0, f"{label}: {result.stderr}")
            transcripts[label] = result.stdout
            print(f"{label}: {result.stderr.strip()}")
        if args.baseline:
            require(transcripts["baseline"] == transcripts["current"], "unchanged baseline/current outputs differ")
            print("Normal output is unchanged; saved baseline reproduces old append, copy and promotion faults.")


if __name__ == "__main__":
    main()
