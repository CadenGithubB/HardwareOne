#!/usr/bin/env python3
"""A new automation id always fits the signed long every reader takes it as.

Ids were millis(): after 24.9 days of uptime a number above 2^31 - 1, which
automations.json keeps as written but as<long>() reads as 0 and String::toInt
as 2147483647. Such an automation could not be addressed by id, was not
rescheduled after it fired, and a second one was skipped as its duplicate.

System_AutomationId.h (the range owner: automationIdSeed, automationIdAdvance,
automationIdTextAboveRange) is compiled as it is, and the production
sanitizeAutomationsJson is run with the real ArduinoJson: new ids fit at every
uptime; a duplicate found on day 35 gets an id in range; a stored id above the
range is given a new one and the rest of the automation is unchanged; a file
whose ids are in range, 2147483647 included, is not touched. Source rules:
both places that make an id use the owner, and the scheduler pass leaves an
automation whose id is above the range out and queues the sanitizer.

Options: --cxx <compiler>, --sanitize (ASan + UBSan), --component <dir> to run
the same cases against another components/hardwareone directory.
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
FUNCTIONS = ("bool automationIdExistsInJson(const String& json, unsigned long id)",
             "bool sanitizeAutomationsJson(String& jsonRef)")


def definition(source: str, marker: str) -> str:
    """The block of a definition; a forward declaration of the same name is skipped."""
    return extract_block(source[source.index(marker + " {"):], marker)


def rule_failures(automation: str) -> list[str]:
    failures = []
    code = re.sub(r"//[^\n]*", "", automation)
    for pattern, what in ((r"\bid\s*=\s*millis\(\)", "an id taken straight from millis()"),
                          (r"\bnewId\s*=\s*\(unsigned long\)\s*millis\(\)", "an id taken straight from millis()"),
                          (r"\b(?:id|newId)\s*\+=", "an id stepped without the range owner")):
        for match in re.finditer(pattern, code):
            failures.append(f"System_Automation.cpp: {what}: {match.group(0)!r}")
    if len(re.findall(r"=\s*automationIdSeed\(\(uint32_t\)millis\(\)\);", code)) != 2:
        failures.append("System_Automation.cpp: both places that make an id start from automationIdSeed(millis())")
    if len(re.findall(r"=\s*automationIdAdvance\(", code)) != 2:
        failures.append("System_Automation.cpp: both places step a taken id with automationIdAdvance()")
    tick = definition(automation, "static void schedulerTickMinute()")
    guard = re.search(r"if \(automationIdTextAboveRange\(idStr\.c_str\(\)\)\) \{(.*?)\n    \}", tick, re.S)
    if not guard or "queueSanitize = true;" not in guard.group(1) or "continue;" not in guard.group(1):
        failures.append("System_Automation.cpp: the scheduler pass skips an id above the range and queues the sanitizer")
    elif tick.index("automationIdTextAboveRange(") > tick.index("// Duplicate-id guard"):
        failures.append("System_Automation.cpp: the range check comes before the duplicate guard")
    return failures


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component.resolve()
    require((component / "System_AutomationId.h").exists(),
            "System_AutomationId.h, the owner of the automation id range, exists")
    automation = (component / "System_Automation.cpp").read_text()
    harness = (HERE / "automation_ids_harness.cpp").read_text()
    require(harness.count("// INSERT_PRODUCTION") == 1, "one insertion marker")
    harness = harness.replace("// INSERT_PRODUCTION",
                              "\n\n".join(definition(automation, function) for function in FUNCTIONS))
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wno-unused-function",
             "-Wno-unused-parameter", "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1",
             "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0", "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0",
             "-DARDUINOJSON_ENABLE_PROGMEM=0", "-I", str(component), "-I", str(HERE / "web_batch_stubs"),
             "-I", str(COMPONENT.parent / "hardwareone_libs/ArduinoJson/src")]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    failures = []
    with tempfile.TemporaryDirectory(prefix="hw1-automation-ids-") as directory:
        source = Path(directory) / "automation_ids.cpp"
        binary = Path(directory) / "automation_ids"
        source.write_text(harness)
        if subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)], timeout=180).returncode != 0:
            failures.append("the automation id harness does not compile")
        elif subprocess.run([str(binary)], timeout=60).returncode != 0:
            failures.append("the automation id harness failed")
    failures += rule_failures(automation)
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        sys.exit(1)
    print("a new automation id fits a signed long at every uptime; a stored one above the range is renumbered",
          flush=True)


if __name__ == "__main__":
    main()
