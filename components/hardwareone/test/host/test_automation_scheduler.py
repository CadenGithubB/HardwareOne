#!/usr/bin/env python3
"""Exercise production scheduler ownership, cache parsing and poll policy.

The full rule/command execution pass is a boundary double that invokes the real
cache rebuild. ArduinoJson, lifecycle, invalidation, due checks and the polling
decision come directly from firmware; filesystem, clock and RTOS are controlled
host boundaries.
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


def generated() -> str:
    owner = (COMPONENT / "System_Automation.cpp").read_text()
    start = owner.index("// In-RAM cache of per-automation scheduling metadata")
    end = owner.index("// Update the nextAt field", start)
    cache = owner[start:end]
    # Firmware builds verify the ESP ABI budget. LP64 host long/time_t make
    # the same fields wider; that architecture assertion is not host behavior.
    cache, removed = re.subn(
        r'static_assert\(sizeof\(AutomationsCacheEntry\) == 16,\s*'
        r'"AutomationsCacheEntry must remain within its 16-byte DRAM budget"\);',
        "// ESP-specific cache size assertion checked by firmware builds.", cache)
    require(removed == 1, "cache ABI assertion extraction changed")
    functions = "\n".join(extract_block(owner, signature) for signature in (
        "void notifyAutomationScheduler()", "bool startAutomationScheduler()",
        "void stopAutomationScheduler()", "bool automationSchedulerRunning()",
        "void automationTick()"))
    tick = extract_block(owner, "static void schedulerTickMinute()")
    drain = next(line for line in tick.splitlines()
                 if "gAutomationEventsPending.exchange(false);" in line)
    harness = (HERE / "automation_scheduler_harness.cpp").read_text()
    for marker, replacement in {"// INSERT_CACHE_OWNER": cache,
                                "// INSERT_SCHEDULER_API": functions,
                                "// INSERT_PENDING_EVENT_DRAIN": drain}.items():
        require(harness.count(marker) == 1, f"exactly one insertion marker: {marker}")
        harness = harness.replace(marker, replacement)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    sanitizer = parser.add_mutually_exclusive_group()
    sanitizer.add_argument("--sanitize", action="store_true")
    sanitizer.add_argument("--thread-sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-pthread",
             "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-unused-variable",
             "-Wno-sign-compare", "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1",
             "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0", "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0",
             "-DARDUINOJSON_ENABLE_PROGMEM=0", "-I", str(HERE / "web_batch_stubs"),
             "-I", str(COMPONENT.parent / "hardwareone_libs/ArduinoJson/src")]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    if args.thread_sanitize:
        flags += ["-fsanitize=thread", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-automation-scheduler-") as directory:
        work = Path(directory)
        source, binary = work / "test.cpp", work / "test"
        source.write_text(generated())
        subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)], check=True, timeout=120)
        subprocess.run([str(binary)], check=True, timeout=60)
        shutil.copyfile(COMPONENT / "System_Automation.h", work / "System_Automation.h")
        shutil.copyfile(COMPONENT / "System_CommandRegistry.h", work / "System_CommandRegistry.h")
        (work / "System_BuildConfig.h").write_text("#pragma once\n#define ENABLE_AUTOMATION 0\n")
        source.write_text('''#include "System_Automation.h"
#include <cassert>
void webBatchTestStringAppend() {}
int main() {
  assert(!startAutomationScheduler());
  assert(!automationSchedulerRunning());
  notifyAutomationScheduler();
  automationOnSystemEvent(1);
  automationTick();
  stopAutomationScheduler();
  assert(!automationSchedulerRunning());
}
''')
        subprocess.run([args.cxx, *flags, "-I", str(work), str(source), "-o", str(binary)],
                       check=True, timeout=120)
        subprocess.run([str(binary)], check=True, timeout=60)
    print("automation scheduler: disabled public API passed without platform dependencies", flush=True)


if __name__ == "__main__":
    main()
