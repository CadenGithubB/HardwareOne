#!/usr/bin/env python3
"""Clock values and intervals stored as uint32_t in JSON are read back as uint32_t.

ArduinoJson's `value | default` reads with the type of the default. Boot
anchors are written `(uint32_t)` and were read through `| 0`, an int: a
millisAtSync above 2^31 (a sync after 24.9 days of uptime) came back as 0, and
from January 2038 on every epochAtSync would have. An automation's intervalMs
was read the same way, so an interval above 24.8 days was "no interval".

The production parseBootAnchors (System_User.cpp) and parseOneTrigger with
nextFire (System_Automation.cpp) are compiled with the real ArduinoJson and
run on stored values on both sides of 2^31; every value below 2^31 must read
back as before. The production resolveUserTimestamp (with formatEpochAsIso8601)
is run on a user created and an anchor synced on both sides of 2^31 and of the
wrap: the two millis() stamps are 32-bit, so their difference is taken as
uint32_t and read as int32_t. A subtraction of two longs overflowed on the
device when the stamps straddled 2^31, and on a host with 64-bit long it calls
a creation just before the wrap a previous boot. Source rule: no read of intervalMs, epochAtSync or
millisAtSync goes through an int default.

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
TRIGGER_PARTS = ("struct Trigger", "static uint8_t daysCsvToMask(const char* csv)",
                 "static bool parseOneTrigger(JsonVariantConst trig, Trigger& out)",
                 "static time_t nextFire(const Trigger& s, time_t from)")
# (file, JSON key, number of reads of it through a uint32_t default)
READS = (("System_User.cpp", "epochAtSync", 1), ("System_User.cpp", "millisAtSync", 1),
         ("System_User.cpp", "ntpAnchorId", 1), ("System_Automation.cpp", "intervalMs", 4))


def definition(source: str, marker: str) -> str:
    """The block of a definition; a forward declaration of the same name is skipped."""
    return extract_block(source[source.index(marker + " {"):], marker)


def generated(component: Path) -> str:
    user = (component / "System_User.cpp").read_text()
    automation = (component / "System_Automation.cpp").read_text()
    anchors = "\n\n".join([
        definition(user, "struct BootAnchor") + ";",
        definition(user, "static int parseBootAnchors(const String& usersJson, BootAnchor* anchors, int maxCount)"),
        definition(user, "static bool formatEpochAsIso8601(time_t epoch, char* buf, size_t bufSize)"),
        definition(user, "static bool resolveUserTimestamp(JsonObject user, unsigned long createdMs,\n"
                         "                                 const BootAnchor& anchor)")])
    triggers = "\n\n".join(definition(automation, part) + (";" if part.startswith("struct ") else "")
                           for part in TRIGGER_PARTS)
    harness = (HERE / "stored_time_values_harness.cpp").read_text()
    for marker, text in (("// INSERT_BOOT_ANCHORS", anchors), ("// INSERT_TRIGGERS", triggers)):
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, text)
    return harness


def rule_failures(component: Path) -> list[str]:
    failures = []
    for name, key, count in READS:
        source = (component / name).read_text()
        reads = re.findall(rf'\["{key}"\]\s*\|\s*([^;]+);', source)
        unsigned = [default for default in reads if default.strip() == "uint32_t(0)"]
        if len(unsigned) != count or len(reads) != count:
            failures.append(f'{name}: ["{key}"] is read through these defaults: {reads}; '
                            f"expected {count} read(s), each through uint32_t(0)")
    return failures


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component.resolve()
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wno-unused-function",
             "-Wno-unused-parameter", "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1",
             "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0", "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0",
             "-DARDUINOJSON_ENABLE_PROGMEM=0", "-I", str(HERE / "web_batch_stubs"),
             "-I", str(COMPONENT.parent / "hardwareone_libs/ArduinoJson/src")]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    failures = []
    with tempfile.TemporaryDirectory(prefix="hw1-stored-time-values-") as directory:
        source = Path(directory) / "stored_time_values.cpp"
        binary = Path(directory) / "stored_time_values"
        source.write_text(generated(component))
        if subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)], timeout=180).returncode != 0:
            failures.append("the stored time values harness does not compile")
        elif subprocess.run([str(binary)], timeout=60).returncode != 0:
            failures.append("the stored time values harness failed")
    failures += rule_failures(component)
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        sys.exit(1)
    print("clock values and intervals above 2^31 read back from JSON as they were written", flush=True)


if __name__ == "__main__":
    main()
