#!/usr/bin/env python3
"""The log writer sends each line to the serial port and the log file in one
write of its own buffer, not through Print::printf.

Print::printf (Arduino core Print.cpp) formats into a 64-byte stack buffer
and, for a longer line (most log lines, with their timestamp), mallocs a
block of the line's length, formats again, writes and frees
(census-string-apis-08). debugOutputTask (System_Debug.cpp) did that once
for the serial port and once for the system log file per line.
writeLogLine formats into a buffer sized for a 255-character message with its
prefix and makes the same single write; a line that does not fit (a category
tag over 47 characters) still goes through printf.

1. Runs the production writeLogLine against the real Print.cpp, compiled on
   the host with its malloc counted: for timestamps 0 to 4294967295, no tag,
   short and 47-character tags, and messages from empty to 255 characters
   (the 64-byte boundary, '%' text, UTF-8) the bytes equal Print::printf's,
   both make one write, printf makes its block for a line of 64 bytes or
   more and writeLogLine none; an overlong line takes printf's path.
2. Source contract: debugOutputTask writes the serial line and both file
   lines through writeLogLine and has no printf of a log line left.

--component points the test at another copy of components/hardwareone.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from arduino_string_host import CORE, arduino_string
from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def print_counted(directory: Path, flags) -> tuple[list[str], list[str]]:
    """arduino_string() plus Print.cpp with its malloc/free counted."""
    includes, inputs = arduino_string(directory, flags)
    (directory / "print_heap_count.h").write_text(
        "#pragma once\n#include <cstddef>\n"
        "struct PrintHeapCount { unsigned long made = 0, freed = 0; };\n"
        "extern PrintHeapCount gPrintHeap;\n"
        "void* hostPrintMalloc(size_t);\nvoid hostPrintFree(void*);\n")
    (directory / "print_heap_count.cpp").write_text(
        '#include "print_heap_count.h"\n#include <cstdlib>\nPrintHeapCount gPrintHeap;\n'
        "void* hostPrintMalloc(size_t n) { ++gPrintHeap.made; return std::malloc(n); }\n"
        "void hostPrintFree(void* p) { if (p) ++gPrintHeap.freed; std::free(p); }\n")
    source = (CORE / "Print.cpp").read_text()
    source = re.sub(r"\b(malloc|free)\(", lambda m: "hostPrint" + m[1].capitalize() + "(", source)
    (directory / "Print.cpp").write_text('#include <cstdarg>\n#include "print_heap_count.h"\n' + source)
    return includes, inputs + [str(directory / "Print.cpp"), str(directory / "print_heap_count.cpp")]


def production(component: Path) -> tuple[str, str]:
    header = (component / "System_Debug.h").read_text()
    size = re.search(r"(?m)^#define DEBUG_MSG_SIZE \d+", header)
    require(size is not None, "System_Debug.h defines DEBUG_MSG_SIZE")
    source = (component / "System_Debug.cpp").read_text()
    capacity = re.search(r"(?m)^static constexpr size_t kLogLineCapacity = [^;]+;$", source)
    require(capacity is not None and "static __attribute__((noinline)) void writeLogLine(" in source,
            "System_Debug.cpp has writeLogLine and its buffer capacity")
    return size[0], capacity[0] + "\n" + extract_block(
        source, "static __attribute__((noinline)) void writeLogLine(")


def source_contract(component: Path) -> None:
    source = (component / "System_Debug.cpp").read_text()
    task = extract_block(source, "void debugOutputTask(void* parameter) {")
    require("writeLogLine(Serial, msg->timestamp, nullptr, msg->text);" in task,
            "the serial line goes through writeLogLine")
    # B11: the tag is chosen once (the category name, or none) and the line
    # goes through writeLogLine on flash, or through systemLogCardWriteLocked
    # (one sealed row) when the file is on the card. Nothing plain is written
    # to a card file: every writeLogLine on the file sits behind a flash-path
    # test.
    require("? getDebugCategoryName(msg->category) : nullptr;" in task and
            task.count("writeLogLine(gSystemLogFile, msg->timestamp, cat, msg->text);") == 2 and
            task.count("systemLogCardWriteLocked(msg->timestamp, cat, msg->text)") == 1,
            "the system log file line goes through writeLogLine on flash and systemLogCardWriteLocked on the card")
    sealed_branch = task.index("if (SealedLog::appliesTo(gSystemLogActivePath)) {")
    fallback = task.index("writeLogLine(gSystemLogFile, msg->timestamp, cat, msg->text);")
    require(sealed_branch < task.index("systemLogCardWriteLocked(") < task.index("systemLogCardFailoverLocked();") < fallback,
            "the plain writer runs only for a file that is not on the card, after the card file was given up")
    failover = extract_block(source, "static void systemLogCardFailoverLocked() ")
    require("gSystemLogFile.close();  // still the card: nothing plain goes there" in failover and
            failover.index("gSystemLogFile.close();  // still the card: nothing plain goes there") <
            failover.index("gSystemLogFile.write("),
            "the lines the card did not take are written plain only to a file that is not on the card")
    sealed = extract_block(source, "static __attribute__((noinline)) bool systemLogCardWriteLocked(")
    require("SealedLog::writeLines(gSystemLogFile, gSystemLogActivePath, line," in sealed and ".write(" not in sealed and
            "printf(" not in sealed.replace("snprintf(", ""),
            "the sealed line is written through SealedLog only")
    # Review of 2026-11-22: the line is formatted in external RAM (the log
    # task's stack is 6 KB and the seal runs above this frame), a card flush
    # counts only when the file then has the size written, and the lines of an
    # unverified stretch are kept so a pulled card loses none.
    require("EXT_RAM_BSS_ATTR static char line[kLogLineCapacity];" in sealed and "char line[kLogLineCapacity];\n" not in
            sealed.replace("EXT_RAM_BSS_ATTR static char line[kLogLineCapacity];\n", ""),
            "the sealed line's buffer is not on the log task's stack")
    require("else if (!systemLogCardSettleLocked()) systemLogCardFailoverLocked();" in task,
            "a card flush is verified, and a failed one gives the card file up")
    settle = extract_block(source, "static bool systemLogCardSettleLocked() ")
    require("SealedLog::settle(gSystemLogFile, gSystemLogActivePath, card.stored)" in settle, "the flush is verified by size")
    require("sealedlog::linesLanded(card.pending, keptBytes, sealedlog::kMaxRowBytes, have - card.settled)" in failover and
            'VFS::noteLogCardFileFailure(gSystemLogPath.c_str(), "debug.log");' in failover,
            "the failover reports this log's card file and writes on flash only what the card did not take")
    require('Serial.printf("[%lu] %s\\n", msg->timestamp' not in task and "gSystemLogFile.printf(" not in task,
            "debugOutputTask prints no log line with printf")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component.resolve()
    size, function = production(component)
    harness = (HERE / "log_line_writes_harness.cpp").read_text()
    for marker, text in (("// INSERT_DEBUG_MSG_SIZE", size), ("// INSERT_PRODUCTION", function)):
        require(harness.count(marker) == 1, f"one {marker}")
        harness = harness.replace(marker, text)
    sanitize = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", "-g"] if args.sanitize else []
    with tempfile.TemporaryDirectory(prefix="hw1-log-line-writes-") as directory:
        d = Path(directory)
        includes, sources = print_counted(d, sanitize)
        unit, binary = d / "test.cpp", d / "test"
        unit.write_text(harness)
        subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                        "-Wno-deprecated-declarations", "-Wno-format-truncation", "-Wno-sign-compare", *sanitize, *includes,
                        str(unit), *sources, "-o", str(binary)], check=True, timeout=180)
        subprocess.run([str(binary)], check=True, timeout=30)
    source_contract(component)
    print("log line writes: serial and file lines are written in one write without a heap block")


if __name__ == "__main__":
    main()
