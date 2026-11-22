#!/usr/bin/env python3
"""Exercise production scheduler ownership, cache parsing and poll policy.

The full rule/command execution pass is a boundary double that invokes the real
cache rebuild. ArduinoJson, lifecycle, invalidation, due checks and the polling
decision come directly from firmware; filesystem, clock and RTOS are controlled
host boundaries.

The same program checks the owner's automation summaries (one case per trigger
type, built from parseOneTrigger). A second program compiles the whole G2
Automations page beside lens/command/file doubles: the lens shows the owner's
trigger text and taps act on the automation and state the rows showed. A third
builds the disabled public API.

Production sources come from --component (default: this component), so the
suite replays against a pre-fix tree. Every program runs even if an earlier one
fails; the exit status reports any failure.
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

from test_g2_page_publish import boundary_types, list_publish_helper
from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
SUMMARY_API = "bool automationCopySummaries("

# The filesystem lock, as the store's mutation owner and the scheduler's pass
# entry use it (System_Mutex.h): reentrant per task, `held` false on reentry
# and on a missed try-lock, one real mutex so two threads exclude each other.
# It is never taken under the scheduler's state lock.
FS_LOCK_DOUBLE = r"""
static std::timed_mutex fsMutexDouble;
static thread_local unsigned fsLockDepth = 0;
static int fsMutexToken = 0;
static void* gFsMutex = &fsMutexToken;
static std::atomic<unsigned> fsTryMisses{0};
static bool isFsLockedByCurrentTask() { return fsLockDepth != 0; }
struct FsLockGuard {
  bool held = false;
  explicit FsLockGuard(const char* owner = nullptr) : FsLockGuard(owner, portMAX_DELAY) {}
  FsLockGuard(const char*, TickType_t timeoutTicks) {
    CHECK(stateLockDepth == 0);
    if (!gFsMutex || fsLockDepth != 0) return;
    if (timeoutTicks == portMAX_DELAY) fsMutexDouble.lock();
    else if (!fsMutexDouble.try_lock()) { ++fsTryMisses; return; }
    held = true;
    fsLockDepth = 1;
  }
  ~FsLockGuard() {
    if (!held) return;
    fsLockDepth = 0;
    fsMutexDouble.unlock();
  }
  FsLockGuard(const FsLockGuard&) = delete;
  FsLockGuard& operator=(const FsLockGuard&) = delete;
};
"""


def owner_summary(component: Path) -> str:
    """The header's summary types plus the owner's trigger parser and summaries.

    Returns "" for a tree whose owner has no summary list (its pages parse the
    file themselves); the scheduler program then refuses to build.
    """
    owner = (component / "System_Automation.cpp").read_text()
    header = (component / "System_Automation.h").read_text()
    if SUMMARY_API not in owner:
        return ""
    start = header.index("enum class AutomationTriggerKind")
    end = header.index("\n", header.index("bool automationFindSummary(", start))
    types = header[start:end + 1]
    begin = owner.index("static constexpr int MAX_TRIGGERS = 4;")
    finish = owner.index("// Compute the next firing time at or after", begin)
    section = owner[begin:finish]
    require(SUMMARY_API in section and "static bool parseOneTrigger(" in section,
            "owner summary section holds the trigger parser and the summary list")
    return types + "\n" + section


def generated(component: Path) -> str:
    owner = (component / "System_Automation.cpp").read_text()
    header = (component / "System_Automation.h").read_text()
    # The file-format check (a tree without one keeps the old start) and the
    # cache owner. The format constant comes from its header line.
    format_marker = "// automations.json format version\n"
    start = owner.find(format_marker)
    if start < 0:
        start = owner.index("// In-RAM cache of per-automation scheduling metadata")
    end = owner.index("// Update the nextAt field", start)
    constant = re.search(r"^constexpr uint32_t kAutomationsFileVersion = \d+;$", header, re.M)
    cache = (constant.group(0) + "\n" if constant else "") + owner[start:end]
    # Firmware builds verify the ESP ABI budget. LP64 host long/time_t make
    # the same fields wider; that architecture assertion is not host behavior.
    cache, removed = re.subn(
        r'static_assert\(sizeof\(AutomationsCacheEntry\) == 16,\s*'
        r'"AutomationsCacheEntry must remain within its 16-byte entry budget"\);',
        "// ESP-specific cache size assertion checked by firmware builds.", cache)
    require(removed == 1, "cache ABI assertion extraction changed")
    functions = "\n".join(extract_block(owner, signature) for signature in (
        "void notifyAutomationScheduler()", "bool startAutomationScheduler()",
        "void stopAutomationScheduler()", "bool automationSchedulerRunning()",
        "const char* automationSetSystemEnabled(bool enable)",
        # The pass-entry filesystem try-lock (a tree without it claims a pass
        # without looking at the lock).
        *(("static bool automationPassFilesystemFree()",)
          if "static bool automationPassFilesystemFree()" in owner else ()),
        "void automationTick()"))
    tick = extract_block(owner, "static void schedulerTickMinute()")
    begin = tick.index("  static uint32_t sEventCursor")
    # The comment that follows the drain; older trees word it differently.
    drain_end = next(marker for marker in ("  // The clock was set while interval",
                                           "  // Before time() counts at all only event fires",
                                           "  // Without synced time only event fires") if marker in tick)
    drain = tick[begin:tick.index(drain_end, begin)]
    require("gAutomationEventsPending.exchange(false);" in drain,
            "event drain extraction holds the pending-flag exchange")
    events = (component / "System_Events.cpp").read_text()
    ring = "\n".join(extract_block(events, signature) for signature in (
        "uint32_t systemEventLatestSeq()", "int systemEventFetchSince("))
    # G-70: the post-fire path, which keeps a short interval's next run in RAM.
    # The next run with the clock owner's answer; a tree without it has the
    # public function only, and the clock cases then do not compile.
    next_run = "static time_t automationNextRunTime(const char* automationJson, time_t fromTime, bool clockSet) "
    # The post-fire writeback goes through the store's mutation owner on a
    # tree that has one (mutateAutomationsJson, in the cache section).
    reschedule_mutation = "static AutomationsMutation automationRescheduleMutation("
    reschedule = "\n".join(extract_block(owner, signature) for signature in (
        "static time_t nextFire(const Trigger& s, time_t from)",
        "static bool onFired(Trigger& s, time_t firedAt)",
        *((reschedule_mutation,) if reschedule_mutation in owner else ()),
        "static void rescheduleAfterFire(long id, const char* automationJson, time_t firedAt, bool notify) ",
        *((next_run,) if next_run in owner else ()),
        "time_t computeNextRunTime(const char* automationJson, time_t fromTime) "))
    # System_Clock's "is this a real epoch", which the due cache and the pass ask.
    # It goes in as ClockOwner; the harness's Clock::isValidEpoch asks it
    # unless a case counts small numbers as a set clock (clockForcedSet).
    clock = (component / "System_Clock.cpp").read_text()
    threshold = re.search(r"^static constexpr int kSyncedTmYearThreshold = \d+;", clock, re.M)
    require(threshold is not None, "System_Clock.cpp holds the year threshold")
    clock_owner = ("namespace ClockOwner {\n" + threshold.group(0) + "\n" +
                   extract_block(clock, "bool isValidEpoch(time_t t)") + "\n}  // namespace ClockOwner")
    require("automationVolatileSet(" in reschedule and "if (notify) notifyAutomationScheduler();" in reschedule,
            "rescheduleAfterFire keeps short intervals in RAM and notifies without a write")
    # The uptime schedule's own cases need its table; the regression cases
    # before them run against any tree.
    uptime_table = "struct AutomationUptimeSchedule {" in owner
    # The carry rule (an interval in its first countdown is not carried) and
    # automationUptimeForget: their table cases need the new API; the
    # clock-set behaviour cases run against any tree with the table.
    carry_rule = "static bool carries(const Entry& e)" in owner
    summary = owner_summary(component)
    require(bool(summary), "System_Automation.cpp has no owner automation summary "
            "(automationCopySummaries); pages still parse automations.json themselves")
    harness = (HERE / "automation_scheduler_harness.cpp").read_text()
    if reschedule_mutation in owner:
        reschedule = extract_block(owner, "struct AutomationRescheduleContext") + ";\n" + reschedule
    for marker, replacement in {"// INSERT_CLOCK_OWNER": clock_owner,
                                "// INSERT_CACHE_OWNER": FS_LOCK_DOUBLE + cache,
                                "// INSERT_SCHEDULER_API": functions,
                                "// INSERT_EVENT_RING": ring,
                                "// INSERT_PENDING_EVENT_DRAIN": drain,
                                "// INSERT_TRIGGER_SUMMARY": summary,
                                "// INSERT_RESCHEDULE": reschedule}.items():
        require(harness.count(marker) == 1, f"exactly one insertion marker: {marker}")
        harness = harness.replace(marker, replacement)
    return (("#define HAVE_UPTIME_SCHEDULE 1\n" if uptime_table else "") +
            ("#define HAVE_UPTIME_CARRY_RULE 1\n" if carry_rule else "") + harness)


def g2_page(component: Path) -> str:
    page = (component / "G2_Page_Automations.cpp").read_text()
    production = re.sub(r"^#include[^\n]*\n", "", page, flags=re.M)
    harness = (HERE / "g2_automations_page_harness.cpp").read_text()
    for marker, replacement in {"// INSERT_BOUNDARY_TYPES": boundary_types(component),
                                "// INSERT_OWNER_SUMMARY": owner_summary(component),
                                "// INSERT_LIST_PUBLISH_HELPER": list_publish_helper(component),
                                "// INSERT_AUTOMATIONS_PAGE": production}.items():
        require(harness.count(marker) == 1, f"exactly one insertion marker: {marker}")
        harness = harness.replace(marker, replacement)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    sanitizer = parser.add_mutually_exclusive_group()
    sanitizer.add_argument("--sanitize", action="store_true")
    sanitizer.add_argument("--thread-sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="component directory to read production sources from")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component.resolve()
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
    failures = []

    def build_and_run(name: str, generate, work: Path) -> None:
        source, binary = work / f"{name}.cpp", work / name
        try:
            source.write_text(generate(component))
        except AssertionError as error:
            failures.append(f"{name}: {error}")
            print(f"{name}: {error}", file=sys.stderr, flush=True)
            return
        if subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)],
                          timeout=180).returncode != 0:
            failures.append(f"{name}: compile failed")
            return
        ran = subprocess.run([str(binary)], timeout=60)
        if ran.returncode != 0:
            failures.append(f"{name}: exit {ran.returncode}")

    with tempfile.TemporaryDirectory(prefix="hw1-automation-scheduler-") as directory:
        work = Path(directory)
        build_and_run("scheduler", generated, work)
        build_and_run("g2_automations_page", g2_page, work)
        source, binary = work / "test.cpp", work / "test"
        shutil.copyfile(component / "System_Automation.h", work / "System_Automation.h")
        shutil.copyfile(component / "System_CommandRegistry.h", work / "System_CommandRegistry.h")
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
        if subprocess.run([args.cxx, *flags, "-I", str(work), str(source), "-o", str(binary)],
                          timeout=120).returncode != 0:
            failures.append("disabled API: compile failed")
        elif subprocess.run([str(binary)], timeout=60).returncode != 0:
            failures.append("disabled API: failed")
        else:
            print("automation scheduler: disabled public API passed without platform dependencies",
                  flush=True)
    if failures:
        print("automation scheduler FAILED: " + "; ".join(failures), file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
