#!/usr/bin/env python3
""""Is the clock set" is asked of System_Clock, not answered with a hand threshold.

Clock::isValidEpoch (the year 2020 or later) is the owner. Five places kept a
weaker test of their own: `epoch < 100000` (the mesh time-sync command, the
certificate expiry line, the validity of a new certificate, the expiry check
when HTTPS starts), which turns true after 28 hours of uptime without any time
source, and `now > 0` in the automation scheduler, which is true one second
after boot.

Run as production code: the real Clock::isValidEpoch and Clock::isSynced
(System_Clock.cpp) against a settable clock: not set at any time before 2020,
55 hours after a boot without a time source included, and set from 2020 on.
The mesh time command that used the weak test is run as production code by
espnow_time_status (System_ESPNow_MeshTime.cpp: it refuses and sends nothing
while the clock is not set, and asks the clock owner through mtClockSet). The
automation scheduler's part runs in automation_scheduler (its next-run
computation and its due cache). Source rules: no comparison of a time with
100000 is left in the firmware, the places ask Clock::isValidEpoch or
Clock::isSynced, and the scheduler pass takes its next run from
automationNextRunTime with the clock owner's answer.

The tests that stay, each for a stated reason, are listed in KEPT: a new one
elsewhere fails the rule until it is listed with its reason.

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

# (file, function, calls of Clock::isValidEpoch in it)
ASKERS = (
    ("System_WiFi.cpp", "const char* cmd_certinfo(const String& argsInput)", 1),
    ("System_WiFi.cpp", "const char* cmd_certgen(const String& argsInput)", 1),
)
# Own tests that stay: (file, the text, how often, why).
KEPT = (
    ("HardwareOne.cpp", "timeinfo.tm_year >= 120", 1,
     "reads the tm it has just filled inside a 10 ms window; its comment says why it does not call Clock::isSynced"),
    ("R1_HealthHistoryStore.cpp", "now >= 1609459200", 1,
     "the retention pass's own floor (2021), with its own host harness; not one of the weak tests"),
    ("System_User.cpp", "time(nullptr) > 0", 1,
     "harmless: resolvePendingUserCreationTimes resolves from stored boot anchors, whatever the clock reads"),
    ("System_FirstTimeSetup.cpp", "time(nullptr) > 0", 1,
     "harmless: the same callee"),
)
HAND_TESTS = (
    r"\b\w*(?:now|epoch|Epoch|Now)\w*\s*(?:<|>|<=|>=)\s*100000\b",
    r"\btm_year\s*(?:<|>=)\s*1\d\d\b",
    r"\b\w*(?:now|Now)\w*\s*(?:<|>|<=|>=)\s*1[5-7]\d{8}\b",
    r"\btime\((?:nullptr|NULL)\)\s*(?:>|<=)\s*0\b",
)

HARNESS = r"""
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

static time_t gClock = 0;
static time_t testTime(time_t*) { return gClock; }

namespace Clock {
#define time testTime
// INSERT_CLOCK
#undef time
}  // namespace Clock

static int failures = 0, checks = 0;
static void check(bool ok, const std::string& what) {
  ++checks;
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

int main() {
  // The clock counts from 1970 after a boot without a time source.
  for (time_t clock : {time_t{0}, time_t{5}, time_t{99999}, time_t{100000}, time_t{100001}, time_t{200000},
                       time_t{86400} * 365, time_t{946684800} /* 2000 */, time_t{1577836799} /* 2019-12-31 */}) {
    gClock = clock;
    check(!Clock::isValidEpoch(clock), "isValidEpoch says not set at " + std::to_string(clock));
    check(!Clock::isSynced(), "isSynced says not set with the clock at " + std::to_string(clock));
  }
  for (time_t clock : {time_t{1577923200} /* 2020-01-02 */, time_t{1763000000}, time_t{2200000000}}) {
    gClock = clock;
    check(Clock::isValidEpoch(clock), "isValidEpoch says set at " + std::to_string(clock));
    check(Clock::isSynced(), "isSynced says set with the clock at " + std::to_string(clock));
  }
  check(Clock::isValidEpoch(time_t{1577836800}), "2020-01-01 00:00 UTC is the first valid second");
  if (failures != 0) {
    std::fprintf(stderr, "%d of %d clock question checks failed\n", failures, checks);
    return 1;
  }
  std::printf("clock set question: %d checks passed\n", checks);
  return 0;
}
"""


def strip_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", lambda match: "\n" * match.group(0).count("\n"), source, flags=re.S)
    return re.sub(r"//[^\n]*", "", source)


def definition(source: str, marker: str) -> str:
    """The block of a definition; a forward declaration of the same name is skipped."""
    return extract_block(source[source.index(marker + " {"):], marker)


def rule_failures(component: Path) -> list[str]:
    failures = []
    for name, function, count in ASKERS:
        body = strip_comments(definition((component / name).read_text(), function))
        found = body.count("Clock::isValidEpoch(")
        if found != count:
            failures.append(f"{name}: {function} asks Clock::isValidEpoch {found} time(s), expected {count}")
    mesh_time = (component / "System_ESPNow_MeshTime.cpp").read_text()
    if "static bool mtClockSet() { return Clock::isSynced(); }" not in mesh_time:
        failures.append("System_ESPNow_MeshTime.cpp: mesh time asks Clock::isSynced whether the clock is set")
    server = strip_comments((component / "WebServer_Server.cpp").read_text())
    if not re.search(r"time_t now = time\(NULL\);\s*if \(Clock::isValidEpoch\(now\)\) \{", server):
        failures.append("WebServer_Server.cpp: the certificate expiry check at HTTPS start asks Clock::isValidEpoch")
    automation = (component / "System_Automation.cpp").read_text()
    tick = strip_comments(definition(automation, "static void schedulerTickMinute()"))
    if "const bool clockSet = Clock::isValidEpoch(now);" not in tick:
        failures.append("System_Automation.cpp: the scheduler pass asks Clock::isValidEpoch whether the clock is set")
    if "automationNextRunTime(obj.c_str(), now, clockSet)" not in tick or "computeNextRunTime(" in tick:
        failures.append("System_Automation.cpp: the scheduler pass takes its next run from automationNextRunTime "
                        "with the clock owner's answer")
    cache = strip_comments(definition(automation, "static void rebuildAutomationsCache()"))
    if "Clock::isValidEpoch(now)" not in cache or "!waitsForClock" not in cache:
        failures.append("System_Automation.cpp: the due cache leaves a time-of-day trigger out while the clock is not set")
    kept = {(name, text): count for name, text, count, _ in KEPT}
    for path in sorted(list(component.glob("*.cpp")) + list(component.glob("*.h"))):
        if path.name in ("System_Clock.cpp", "System_Clock.h"):
            continue  # the owner
        code = strip_comments(path.read_text())
        for pattern in HAND_TESTS:
            for match in re.finditer(pattern, code):
                text = match.group(0)
                allowed = [key for key in kept if key[0] == path.name and key[1].endswith(text)]
                if allowed and kept[allowed[0]] > 0:
                    kept[allowed[0]] -= 1
                    continue
                line = code.count("\n", 0, match.start()) + 1
                failures.append(f"{path.name}:{line}: a hand test for a set clock: {text!r}; "
                                "ask Clock::isValidEpoch or Clock::isSynced")
    for (name, text), left in kept.items():
        if left:
            failures.append(f"{name}: the listed test {text!r} is gone; remove it from KEPT")
    return failures


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component.resolve()
    clock = (component / "System_Clock.cpp").read_text()
    threshold = re.search(r"^static constexpr int kSyncedTmYearThreshold = \d+;", clock, re.M)
    require(threshold is not None, "System_Clock.cpp holds the year threshold")
    harness = HARNESS.replace("// INSERT_CLOCK", threshold.group(0) + "\n" +
                              extract_block(clock, "bool isSynced()") + "\n" +
                              extract_block(clock, "bool isValidEpoch(time_t t)"))
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wno-unused-variable"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    failures = []
    with tempfile.TemporaryDirectory(prefix="hw1-clock-set-question-") as directory:
        source = Path(directory) / "clock_set_question.cpp"
        binary = Path(directory) / "clock_set_question"
        source.write_text(harness)
        if subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)], timeout=180).returncode != 0:
            failures.append("the clock question harness does not compile")
        elif subprocess.run([str(binary)], timeout=60).returncode != 0:
            failures.append("the clock question harness failed")
    failures += rule_failures(component)
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        print(f"clock set question: {len(failures)} failure(s)", file=sys.stderr)
        sys.exit(1)
    print("'is the clock set' is asked of System_Clock; four own tests are kept, each with its reason", flush=True)


if __name__ == "__main__":
    main()
