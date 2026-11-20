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
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
CORE = COMPONENT.parent / "arduino/cores/esp32"


def generate(overlay: Path | None, baseline: bool | None = None) -> str:
    """The harness on `overlay`'s sources (default: this tree). `baseline`
    (default: an overlay is given) selects the saved pre-G-39 expectations."""
    if baseline is None:
        baseline = overlay is not None
    source = (overlay or COMPONENT) / "System_Filesystem.cpp"
    production = source.read_text()
    markers = [marker for marker in ("static bool appendCappedLineOnce(", "static bool restartCappedLog(")
               if marker in production]
    markers.append("bool appendLineWithCap(")
    # The String form, a wrapper around the (text, length) form since C1: out of
    # line in System_Filesystem.cpp at first, inline in System_Filesystem.h since.
    wrapper = "bool appendLineWithCap(const char* path, const String& line, size_t capBytes) {"
    header = ((overlay or COMPONENT) / "System_Filesystem.h").read_text()
    string_form = ""
    if "bool appendLineWithCap(const char* path, const char* line, size_t lineLength" in production:
        if wrapper in production:
            markers.append(wrapper[:-2])
        elif "inline " + wrapper in header:
            string_form = extract_block(header, "inline " + wrapper[:-2])
    if "static void recoverLogOrphanAt(" in production:
        markers.append("static void recoverLogOrphanAt(")
    markers.append("void cleanupLogOrphan(")
    functions = "\n\n".join(extract_block(production, marker) for marker in markers)
    if string_form:
        functions += "\n\n" + string_form
    # log_out's capped-log line (System_Debug.cpp): the timestamp prefix and,
    # since C1, the line formatted into its kept PSRAM block.
    debug = ((overlay or COMPONENT) / "System_Debug.cpp").read_text()
    timestamped = "static void appendTimestampedLine(const char* path, const char* text, size_t capBytes) {"
    if timestamped in debug:
        start = debug.index("static constexpr size_t kTimestampPrefixSize")
        definition = debug.index(timestamped)  # not the forward declaration
        end = definition + len(extract_block(debug[definition:], "static void appendTimestampedLine("))
        line_code = "#define HAVE_TIMESTAMPED_LINE 1\n" + debug[start:end]
    else:
        line_code = "#define HAVE_TIMESTAMPED_LINE 0\n" + extract_block(debug, "static String buildTimestampPrefix(")
    harness = (HERE / "capped_log_writes_harness.cpp").read_text()
    require(harness.count("// INSERT_PRODUCTION") == 1, "one production insertion marker")
    require(harness.count("// INSERT_LOG_LINE") == 1, "one log line insertion marker")
    notes = int("catchupNoteTrim(" in production)
    return (f"#define BASELINE {int(baseline)}\n#define HAVE_CATCHUP_NOTES {notes}\n" +
            harness.replace("// INSERT_PRODUCTION", functions).replace("// INSERT_LOG_LINE", line_code))


def source_contract() -> None:
    debug = (COMPONENT / "System_Debug.cpp").read_text()
    task = extract_block(debug, "void debugOutputTask(")
    # The [ERROR], [EVENT] and [EVLOG] lines go through the kept line block.
    for log in ("LOG_ERROR_FILE", "LOG_EVENTS_FILE", "LOG_EVENT_STREAM_FILE"):
        require(f"appendTimestampedLine({log}, msg->text," in task, f"log_out appends {log} through appendTimestampedLine")
    require("buildTimestampPrefix()" not in task and "appendLineWithCap(" not in task,
            "log_out builds no String line of its own")
    utils = (COMPONENT / "System_Utils.cpp").read_text()
    require('appendLineWithCap("/system/sys_logs/command-audit.log", entry, strlen(entry), 500 * 1024);' in utils,
            "the command audit hands its char buffer on without a String")
    fs = (COMPONENT / "System_Filesystem.cpp").read_text()
    body = extract_block(fs, "bool appendLineWithCap(const char* path, const char* line, size_t lineLength,")
    require("String(dest)" not in body and "String(tmpPath)" not in body,
            "appendLineWithCap makes the resolved path's String once per tier")
    # File catch-up (System_CatchupLedger.h): one trim note after the promoted
    # rename, one restart note after the restarted log opened; the hooks do no
    # I/O, logging or allocation (they run inside this writer).
    require(body.count("catchupNoteTrim(dest, tailStart);") == 1 and
            body.index("catchupNoteTrim(") > body.index("renameGuarded(tmpName, destPath, ctx)") and
            body.index("catchupNoteTrim(") > body.index('STACK_TRACEF("appendLineWithCap.rename_failed")'),
            "appendLineWithCap notes the trim once, after the successful rename")
    restart = extract_block(fs, "static bool restartCappedLog(")
    require(restart.count("catchupNoteRestart(dest.c_str());") == 1 and
            restart.index("catchupNoteRestart(") > restart.index("restart_open_failed"),
            "restartCappedLog notes the restart once, after the new log opened")
    ledger_path = COMPONENT / "System_CatchupLedger.cpp"
    if ledger_path.exists():
        ledger = ledger_path.read_text()
        for hook in ("void catchupNoteTrim(", "void catchupNoteRestart("):
            hook_body = re.sub(r"//[^\n]*", "", extract_block(ledger, hook))
            for banned in ("VFS::", "File ", "WARN_", "DEBUG_", "CL_WARN", "CL_DEBUG", "alloc", "Serial", "printf"):
                require(banned not in hook_body, f"{hook.strip('(')} must not use {banned}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path, help="saved component sources; also runs current")
    parser.add_argument("--evidence", type=Path, help="retain generated source and output")
    parser.add_argument("--previous", type=Path,
                        help="a component tree of an earlier commit (same expectations); its transcript must equal this one's")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-capped-logs-") as temp:
        work = args.evidence.resolve() if args.evidence else Path(temp)
        work.mkdir(parents=True, exist_ok=True)
        # The core String with its heap calls counted (arduino_string_counted):
        # the log-line cases count the blocks a line costs.
        string = (CORE / "WString.cpp").read_text().replace('#include "Arduino.h"', '#include <cstdio>')
        string = string.replace('#include "esp32-hal-log.h"', '#define log_e(...) ((void)0)\n#define log_w(...) ((void)0)')
        string = re.sub(r'\b(malloc|realloc|free)\(', lambda m: 'hostString' + m[1].capitalize() + '(', string)
        (work / "WString.cpp").write_text('#include "string_heap_count.h"\n' + string)
        (work / "string_heap_count.h").write_text(
            '#pragma once\n#include <cstddef>\n'
            'struct StringHeapCount { unsigned long made = 0, regrown = 0, freed = 0; };\n'
            'extern StringHeapCount gStringHeap;\n'
            'void* hostStringMalloc(size_t);\nvoid* hostStringRealloc(void*, size_t);\nvoid hostStringFree(void*);\n')
        (work / "string_heap_count.cpp").write_text(
            '#include "string_heap_count.h"\n#include <cstdlib>\nStringHeapCount gStringHeap;\n'
            'void* hostStringMalloc(size_t n) { ++gStringHeap.made; return std::malloc(n); }\n'
            'void* hostStringRealloc(void* p, size_t n) { if (p) ++gStringHeap.regrown; else ++gStringHeap.made; '
            'return std::realloc(p, n); }\n'
            'void hostStringFree(void* p) { if (p) ++gStringHeap.freed; std::free(p); }\n')
        printing = (CORE / "Print.cpp").read_text().replace('#include "Arduino.h"', '#include <cstdarg>')
        (work / "Print.cpp").write_text(printing)
        conversions = (CORE / "stdlib_noniso.c").read_text().replace('#include "esp_system.h"', '')
        conversions += "\nchar *itoa(int v,char*s,int b){return ltoa(v,s,b);}\nchar *utoa(unsigned int v,char*s,int b){return ultoa(v,s,b);}\n"
        (work / "stdlib_noniso.c").write_text(conversions)
        sanitize = (["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                     "-fno-omit-frame-pointer", "-g"] if args.sanitize else [])
        flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                 "-Wno-unused-parameter", "-I", str(work), "-I", str(CORE), *sanitize]
        subprocess.run([shutil.which("cc"), "-std=c11", *sanitize, "-I", str(CORE),
                        "-c", str(work / "stdlib_noniso.c"), "-o", str(work / "conversions.o")], check=True)
        for source in ("WString", "Print", "string_heap_count"):
            subprocess.run([args.cxx, *flags, "-Wno-sign-compare", "-c", str(work / f"{source}.cpp"),
                            "-o", str(work / f"{source}.o")], check=True)
        transcripts = {}
        versions = [("baseline", args.baseline, True)] if args.baseline else []
        versions += [("previous", args.previous, False)] if args.previous else []
        for label, source, saved in [*versions, ("current", None, False)]:
            unit, binary = work / f"{label}.cpp", work / label
            unit.write_text(generate(source, saved))
            subprocess.run([args.cxx, *flags, str(unit), str(work / "WString.o"), str(work / "Print.o"),
                            str(work / "string_heap_count.o"), str(work / "conversions.o"), "-o", str(binary)],
                           check=True, timeout=120)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            (work / f"{label}-transcript.txt").write_text(result.stdout)
            (work / f"{label}-assertions.log").write_text(result.stderr)
            require(result.returncode == 0, f"{label}: {result.stderr}")
            transcripts[label] = result.stdout
            print(f"{label}: {result.stderr.strip()}")
        if args.previous:
            require(transcripts["previous"] == transcripts["current"], "previous/current transcripts differ")
            print(f"previous tree: {len(transcripts['current'].splitlines())} transcript lines equal")
        if args.baseline:
            require(transcripts["baseline"] == transcripts["current"], "unchanged baseline/current outputs differ")
            print("Normal output is unchanged; saved baseline reproduces old append, copy and promotion faults.")
    source_contract()


if __name__ == "__main__":
    main()
