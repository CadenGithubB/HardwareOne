#!/usr/bin/env python3
"""Elapsed time since a millis() stamp is an unsigned 32-bit difference.

`(long)(now - stamp) < interval` is negative when the stamp is between 24.9 and
49.7 days old, so "not yet" stays true: the sensor logger took no sample after
a pause of that length, the size-triggered rotation did not run, the timed
vitals read stayed closed and old status lines came back. The unsigned form has
no such window.

Run as production code against a settable 32-bit clock: g2HealthTick,
healthPollStatusLine and g2HealthLoggingStatusLine (G2_Health.cpp) and
g2RingPollVitalForLogging (G2_Ring.cpp), each with a stamp that is 25, 30 and
49 days old and across the wrap. Source rules cover every site of the change:
no signed difference is left for the listed stamps, each is compared as
`(uint32_t)(now - stamp)`, and the three stamps another task renews are copied
before the clock is read, so the stamp is never ahead of it.

Options: --cxx <compiler>, --sanitize (ASan + UBSan), --component <dir> to run
the same cases and rules against another components/hardwareone directory.
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

HEALTH_STATE = (
    r"^static uint32_t sLastSyncMs = 0;$",
    r"^static bool sRefreshSupportedLast = false;$",
    r"^enum : uint8_t \{\n  POLL_UI_IDLE = 0,\n(?:  POLL_UI_\w+,\n)+\};$",
    r"^static uint8_t sPollUi = POLL_UI_IDLE;$",
    r"^static uint32_t sPollUiMs = 0;$",
    r"^static char     sLoggingUiMsg\[40\];$",
    r"^static uint32_t sLoggingUiMs = 0;$",
    r"^static constexpr uint32_t kLoggingUiHoldMs = \d+;$",
)
HEALTH_FUNCTIONS = ("void g2HealthTick(void)",
                    "static const char* healthPollStatusLine(const char* fallback)",
                    "const char* g2HealthLoggingStatusLine(void)")
RING_POLL = "void g2RingPollVitalForLogging(void)"

# (file, function holding the test or None for the whole file, the name subtracted
#  from the clock, number of unsigned comparisons with it)
ELAPSED = (
    ("System_SensorLogging.cpp", None, "lastTickMs", 1),
    ("System_SensorLogging.cpp", None, "lastHeartbeatMs", 1),
    ("System_SensorLogging.cpp", None, "lastTruncateMs", 1),
    ("System_SensorLogging.cpp", None, "lastSummaryMs", 1),
    ("System_SensorLogging.cpp", "void healthLoggingTick()", "lastBurstMs", 1),
    ("System_SensorLogging.cpp", "void healthLoggingTick()", "sHtMineLastReseekMs", 1),
    ("System_SensorLogging.cpp", "void healthLoggingTick()", "sHtMineLastMs", 2),
    ("G2_Health.cpp", "void g2HealthTick(void)", "sLastSyncMs", 1),
    ("G2_Health.cpp", "static const char* healthPollStatusLine(const char* fallback)", "shownMs", 1),
    ("G2_Health.cpp", "const char* g2HealthLoggingStatusLine(void)", "shownMs", 1),
    ("G2_Glasses.cpp", None, "sLastFailDumpMs", 1),
    ("G2_Ring.cpp", RING_POLL, "lastMs", 1),
)
# Stamps another task renews: (file, function, stamp, the copy that is compared).
COPIED_FIRST = (
    ("System_SensorLogging.cpp", "void healthLoggingTick()", "sHtMineLastBurstMs", "lastBurstMs"),
    ("G2_Health.cpp", "static const char* healthPollStatusLine(const char* fallback)", "sPollUiMs", "shownMs"),
    ("G2_Health.cpp", "const char* g2HealthLoggingStatusLine(void)", "sLoggingUiMs", "shownMs"),
)
# No signed difference may be taken against these names anywhere in their file.
STAMPS = (
    ("System_SensorLogging.cpp", ("lastTickMs", "lastHeartbeatMs", "lastTruncateMs", "lastSummaryMs",
                                  "sHtMineLastBurstMs", "lastBurstMs", "sHtMineLastReseekMs", "sHtMineLastMs")),
    ("G2_Health.cpp", ("sLastSyncMs", "sPollUiMs", "sLoggingUiMs", "shownMs")),
    ("G2_Glasses.cpp", ("sLastFailDumpMs",)),
)


def strip_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", " ", source, flags=re.S)
    return re.sub(r"//[^\n]*", "", source)


def scope(source: str, function: str | None) -> str:
    return strip_comments(extract_block(source, function) if function else source)


def rule_failures(component: Path) -> list[str]:
    failures = []
    sources = {}

    def read(name: str) -> str:
        if name not in sources:
            sources[name] = (component / name).read_text()
        return sources[name]

    clock = r"(?:millis\(\)|\w*[nN]ow\w*)"
    for name, function, stamp, count in ELAPSED:
        try:
            code = scope(read(name), function)
        except ValueError:
            failures.append(f"{name}: {function} not found")
            continue
        found = len(re.findall(rf"\(uint32_t\)\s*\(\s*{clock}\s*-\s*{re.escape(stamp)}\s*\)\s*(?:<|>=)", code))
        if found != count:
            failures.append(f"{name}: {found} unsigned elapsed comparison(s) against {stamp}, expected {count}")
    for name, function, stamp, copy in COPIED_FIRST:
        try:
            code = scope(read(name), function)
        except ValueError:
            failures.append(f"{name}: {function} not found")
            continue
        made = re.search(rf"const uint32_t {copy} = {stamp};", code)
        used = re.search(rf"\(uint32_t\)\s*\(\s*millis\(\)\s*-\s*{copy}\s*\)", code)
        if not made or not used or made.start() > used.start():
            failures.append(f"{name}: {stamp} is not copied before millis() is read in {function}")
    for name, stamps in STAMPS:
        code = strip_comments(read(name))
        for stamp in stamps:
            for match in re.finditer(rf"\((?:long|int32_t|int)\)\s*\(\s*[\w.()]+\s*-\s*{re.escape(stamp)}\s*\)", code):
                failures.append(f"{name}: signed elapsed time: {match.group(0)!r}")
    ring = scope(read("G2_Ring.cpp"), RING_POLL)
    for match in re.finditer(r"\((?:long|int32_t|int)\)\s*\(\s*now\s*-\s*lastMs\s*\)", ring):
        failures.append(f"G2_Ring.cpp: signed elapsed time: {match.group(0)!r}")
    return failures


def device_long(text: str) -> str:
    """`long` is 32 bits on the device and 64 here; spell it as what the device compiles.

    Without this a `(long)(now - stamp)` in a tree under test never goes
    negative on the host and the fault it carries on the device stays hidden.
    """
    text = re.sub(r"\bunsigned long\b", "uint32_t", text)
    return re.sub(r"\blong\b", "int32_t", text)


def generated(component: Path) -> str:
    health = (component / "G2_Health.cpp").read_text()
    ring = (component / "G2_Ring.cpp").read_text()
    ring_header = (component / "G2_Ring.h").read_text()
    state = []
    for pattern in HEALTH_STATE:
        match = re.search(pattern, health, re.M)
        require(match is not None, f"G2_Health.cpp state line: {pattern}")
        state.append(match.group(0))
    count = re.search(r"^static constexpr uint8_t G2_RING_POLL_VITAL_COUNT = \d+;$", ring_header, re.M)
    require(count is not None, "G2_Ring.h defines G2_RING_POLL_VITAL_COUNT")
    harness = (HERE / "elapsed_time_harness.cpp").read_text()
    for marker, text in (
            ("// INSERT_HEALTH_STATE", "\n".join(state)),
            ("// INSERT_HEALTH_FUNCTIONS", "\n\n".join(extract_block(health, f) for f in HEALTH_FUNCTIONS)),
            ("// INSERT_RING", count.group(0) + "\n" + extract_block(ring, RING_POLL))):
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, device_long(text))
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component.resolve()
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wno-unused-function",
             "-Wno-unused-variable"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    failures = []
    with tempfile.TemporaryDirectory(prefix="hw1-elapsed-time-") as directory:
        source = Path(directory) / "elapsed_time.cpp"
        binary = Path(directory) / "elapsed_time"
        source.write_text(generated(component))
        if subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)], timeout=180).returncode != 0:
            failures.append("the elapsed-time harness does not compile")
        elif subprocess.run([str(binary)], timeout=60).returncode != 0:
            failures.append("the elapsed-time harness failed")
    failures += rule_failures(component)
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        print(f"elapsed time: {len(failures)} failure(s)", file=sys.stderr)
        sys.exit(1)
    comparisons = sum(count for _, _, _, count in ELAPSED)
    print(f"elapsed time: four production functions and {comparisons} comparisons at {len(ELAPSED)} stamps passed",
          flush=True)


if __name__ == "__main__":
    main()
