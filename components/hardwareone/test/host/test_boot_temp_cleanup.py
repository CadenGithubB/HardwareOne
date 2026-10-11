#!/usr/bin/env python3
"""Run production boot temp cleanup and directory traversal with file faults.

The cleanup helper, VFS DirectoryReader and FsLockGuard are extracted unchanged;
Arduino String is compiled from the vendored core. File/VFS, the mutex scheduler
and log sinks are controlled boundaries. This is not SD or flash qualification.
--baseline characterizes the saved pre-helper cleanup and compares ordinary
leaf-name deletion results while demonstrating its known cleanup defects.
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


def generate(component: Path, baseline: bool) -> str:
    source = (component / "System_Filesystem.cpp").read_text()
    if "static void cleanupBootTemporaryFiles(" in source:
        cleanup = extract_block(source, "static void cleanupBootTemporaryFiles(")
    else:
        require(baseline, "shipping boot cleanup helper is required")
        body = extract_block(source, "// Boot-time cleanup: remove orphaned .tmp files")
        cleanup = "static void cleanupBootTemporaryFiles() " + body[body.index("{"):]
    mutex_header = (COMPONENT / "System_Mutex.h").read_text()
    mutex = (COMPONENT / "System_Mutex.cpp").read_text()
    locks = extract_block(mutex_header, "struct FsLockGuard") + ";\n"
    locks += "\n".join(extract_block(mutex, marker) for marker in (
        "FsLockGuard::FsLockGuard(const char* owner)\n",
        "FsLockGuard::FsLockGuard(const char* owner, TickType_t timeoutTicks)",
        "FsLockGuard::~FsLockGuard()"))
    header = (COMPONENT / "System_VFS.h").read_text()
    vfs = (COMPONENT / "System_VFS.cpp").read_text()
    reader = "namespace VFS {\n" + "\n".join(
        extract_block(header, marker) + ";" for marker in (
            "struct DirectoryEntry", "class DirectoryReader"))
    start = vfs.index("DirectoryReader::DirectoryReader(")
    end = vfs.index("\nstatic FS* fsForPath(", start)
    reader += "\n" + vfs[start:end] + "\n}\n"
    harness = (HERE / "boot_temp_cleanup_harness.cpp").read_text()
    for marker, replacement in (("// INSERT_LOCKS", locks),
                                ("// INSERT_READER", reader),
                                ("// INSERT_CLEANUP", cleanup)):
        require(harness.count(marker) == 1, "one insertion marker: " + marker)
        harness = harness.replace(marker, replacement)
    return f"#define BASELINE {int(baseline)}\n" + harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path, help="saved component directory; also runs current")
    parser.add_argument("--evidence", type=Path, help="retain generated code and reports")
    args = parser.parse_args()
    require(bool(args.cxx), "a C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-boot-temp-") as temporary:
        work = args.evidence.resolve() if args.evidence else Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        string = (CORE / "WString.cpp").read_text().replace('#include "Arduino.h"', '#include <cstdio>')
        string = string.replace('#include "esp32-hal-log.h"', '#define log_e(...) ((void)0)\n#define log_w(...) ((void)0)')
        (work / "WString.cpp").write_text(string)
        conversions = (CORE / "stdlib_noniso.c").read_text().replace('#include "esp_system.h"', '')
        conversions += "\nchar *itoa(int v,char*s,int b){return ltoa(v,s,b);}\nchar *utoa(unsigned int v,char*s,int b){return ultoa(v,s,b);}\n"
        (work / "stdlib_noniso.c").write_text(conversions)
        sanitizer = (["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                      "-fno-omit-frame-pointer", "-g"] if args.sanitize else [])
        flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                 "-Wno-unused-parameter", "-I", str(CORE), *sanitizer]
        subprocess.run([shutil.which("cc"), "-std=c11", *sanitizer, "-I", str(CORE),
                        "-c", str(work / "stdlib_noniso.c"), "-o", str(work / "conversions.o")], check=True, timeout=60)
        subprocess.run([args.cxx, *flags, "-Wno-sign-compare", "-c", str(work / "WString.cpp"),
                        "-o", str(work / "WString.o")], check=True, timeout=60)
        transcripts: dict[tuple[str, int], str] = {}
        for label, component in ([('baseline', args.baseline)] if args.baseline else []) + [('current', COMPONENT)]:
            for llm in (0, 1):
                unit = work / f"{label}-{llm}.cpp"
                binary = work / f"{label}-{llm}"
                unit.write_text(generate(component, label == "baseline"))
                subprocess.run([args.cxx, *flags, f"-DENABLE_LLM_SOURCE_ONBOARD={llm}", str(unit),
                                str(work / "WString.o"), str(work / "conversions.o"), "-o", str(binary)],
                               check=True, timeout=90)
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
                (work / f"{label}-{llm}.log").write_text(result.stderr)
                (work / f"{label}-{llm}.txt").write_text(result.stdout)
                require(result.returncode == 0, f"{label} LLM={llm}: {result.stderr}")
                transcripts[label, llm] = result.stdout
                print(result.stderr.strip())
        if args.baseline:
            for llm in (0, 1):
                require(transcripts['baseline', llm] == transcripts['current', llm],
                        f"ordinary leaf-name cleanup changed for LLM={llm}")
            print("Ordinary deletion parity verified; baseline defects characterized separately.")


if __name__ == "__main__":
    main()
