#!/usr/bin/env python3
"""One table of known tasks, read by both stack reports.

`taskstats` (reportAllTaskStacks) and the memory monitor's periodic stack
sample each kept their own table of task name, handle and stack size, plus a
parallel list of "alive" flags in the same order. The copies had drifted: the
monitor's had no espnow_tx row, so the periodic sample never looked at that
task. The table now has one owner, collectKnownTaskRows() in
System_TaskUtils.cpp.

The production function is compiled with KnownTaskRow, kKnownTaskMax and the
stack size constants of System_TaskUtils.h against stand-in handles and
running flags, once with ESP-NOW and once without: every row is returned, a
row is alive exactly while its owner says so (a stale handle is not), each row
carries its own stack size, the ESP-NOW rows carry the watermark their owners
sampled, and a short buffer is not overrun. Source contracts: both reports
walk collectKnownTaskRows; no second table, parallel flag list or row-position
test (`taskIdx == 1`) remains; every row's name is the name a task is created
with.

Options: --cxx <compiler>, --sanitize (ASan + UBSan), --component <dir> to run
the same checks against another components/hardwareone directory.
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

OWNER = "size_t collectKnownTaskRows(KnownTaskRow* rows, size_t cap)"
ROW = re.compile(r'^\s*add\(\{\s*("[a-z_0-9]+"|INPUT_TASK_NAME)\s*,', re.M)


def strip_comments(text: str) -> str:
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)


def row_names(owner: str) -> list[str]:
    return ROW.findall(owner)


def production(component: Path) -> tuple[str, int]:
    header = (component / "System_TaskUtils.h").read_text()
    source = (component / "System_TaskUtils.cpp").read_text()
    require(source.count(OWNER + " {") == 1, "System_TaskUtils.cpp defines collectKnownTaskRows once")
    owner = extract_block(source, OWNER)
    # The stack sizes the table names, as the header defines them.
    used = sorted(set(re.findall(r"\b\w+_STACK_WORDS\b", owner)))
    stacks = []
    for constant in used:
        line = re.findall(rf"(?m)^constexpr uint32_t {constant} = \d+;", header)
        require(len(line) == 1, f"System_TaskUtils.h defines {constant} once")
        stacks.append(line[0])
    require(len(stacks) >= 13, "the table names a stack size per row")
    maximum = re.findall(r"(?m)^constexpr size_t kKnownTaskMax = \d+;", header)
    require(len(maximum) == 1, "one kKnownTaskMax in System_TaskUtils.h")
    names = row_names(owner)
    pieces = [*stacks, extract_block(header, "struct KnownTaskRow") + ";", maximum[0],
              f"#define HW1_KNOWN_TASK_ROWS {len(names)}u", owner]
    return "\n".join(pieces), len(names)


def created_under(name: str, sources: dict[str, str]) -> bool:
    """A task is created (or its stack recorded) under this name somewhere."""
    creation = re.compile(r"\b(?:xTaskCreate\w*|taskStackRecord)\s*\([^;]*?" + re.escape(name) + r"\s*,")
    return any(creation.search(text) for text in sources.values())


def check_sources(component: Path) -> None:
    source = (component / "System_TaskUtils.cpp").read_text()
    monitor = (component / "System_MemoryMonitor.cpp").read_text()
    owner = extract_block(source, OWNER)
    names = row_names(owner)
    require(len(names) == len(set(names)) and len(names) >= 13, f"table rows: {names}")
    require('"espnow_tx"' in names, "the table has the espnow_tx row")

    # Both reports walk the owner's rows.
    report = strip_comments(extract_block(source, "void reportAllTaskStacks()"))
    sample = strip_comments(extract_block(monitor, "void sampleMemoryState(bool forceFullScan)"))
    for where, body in (("reportAllTaskStacks", report), ("sampleMemoryState", sample)):
        require(body.count("collectKnownTaskRows(") == 1, f"{where} reads the table through its owner")
        require("taskAlive" not in body, f"{where} keeps no parallel list of alive flags")
        require(not re.search(r"\b(?:taskIdx|ktIdx|warnIdx)\s*==\s*\d", body),
                f"{where} picks no row by its position")
        require(".ownerSampled" in body, f"{where} asks the row whether its owner sampled the watermark")

    # No second table: outside the owner a row name is spelled only where the
    # task is created.
    for name in names:
        if name == "INPUT_TASK_NAME":
            continue
        require(name not in strip_comments(monitor),
                f"System_MemoryMonitor.cpp spells the task name {name}: a second table")
        require(strip_comments(report).count(name) == 0,
                f"reportAllTaskStacks spells the task name {name}: a second table")

    # A row is only useful under the name the task really has.
    sources = {path.name: strip_comments(path.read_text())
               for path in sorted(component.glob("*.cpp")) if path.name != "System_TaskUtils.cpp"}
    sources["System_TaskUtils.cpp"] = strip_comments(source.replace(owner, ""))
    for name in names:
        require(created_under(name, sources), f"no task is created under the row name {name}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    harness = (HERE / "known_task_table_harness.cpp").read_text()
    require(harness.count("// INSERT_PRODUCTION") == 1, "one insertion marker")
    unit_text, rows = production(args.component)
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-known-task-table-") as directory:
        source = Path(directory) / "known_tasks.cpp"
        source.write_text(harness.replace("// INSERT_PRODUCTION", unit_text))
        for espnow in (1, 0):
            binary = Path(directory) / f"known_tasks_{espnow}"
            subprocess.run([args.cxx, *flags, f"-DENABLE_ESPNOW={espnow}", str(source), "-o", str(binary)],
                           check=True, timeout=180)
            subprocess.run([str(binary)], check=True, timeout=60)
    check_sources(args.component)
    print(f"known task table: {rows} rows, one owner, both reports read it", flush=True)


if __name__ == "__main__":
    main()
