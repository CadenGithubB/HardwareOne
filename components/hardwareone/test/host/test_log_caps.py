#!/usr/bin/env python3
"""Capped-log sizes by LittleFS size (UP-03) and the per-log policy (S-8).

Compiles System_LogCapsCore.h as it is (it is pure) and checks:

1. logCapsFor for every partition table's LittleFS size, read from the tables'
   size column, against the same arithmetic done here, with the numbers of
   every layout pinned (128, 1856, 2284, 5696, 7808, 8704, 9068, 10092 and
   24512 KiB): all capped logs together
   stay within a quarter of the partition (eight logs at the 4 KiB floor are
   exactly a quarter of the 128 KiB table), no cap exceeds today's constant,
   and a partition whose quarter holds today's sum keeps every cap.
2. Which logs are written through and which keep the copy rotation.
3. Source rule: every appendLineWithCap call site in the component names one
   of the eight known logs with one cap each, and those caps add up to the
   header's kNominalTotal. A new capped log, or a changed cap, fails here
   until the header's table (and so the quarter bound) is updated.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
REPO = COMPONENT.parents[1]

HARNESS = r'''
#include "System_LogCapsCore.h"
#include <cstdio>
#include <cstdlib>
static_assert(logcaps::kNominalTotal == 2823442, "today's caps add up to 2,823,442 bytes");
static_assert(logcaps::logCapsFor(0).total() == logcaps::kNominalTotal, "an unknown size leaves every cap");
static_assert(logcaps::scaledCap(100, 128 * 1024) == 100, "a cap below the floor is never raised");
static_assert(logcaps::generationBytes(573041) == 286520, "a generation is half the cap");
static_assert(logcaps::kBatchFlushBytes < logcaps::kBatchCapacity && logcaps::kBatchDueMs < logcaps::kBatchMaxAgeMs &&
              logcaps::kBatchDueMs + logcaps::kBatchTickMs <= logcaps::kBatchMaxAgeMs,
              "the log task writes a batch before it is 2 s old");
static_assert(logcaps::batchAged(1000, 4294967000u, 1296) && !logcaps::batchAged(1000, 4294967000u, 1297),
              "ages are wrap-safe");
int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const logcaps::LogCaps c = logcaps::logCapsFor(static_cast<size_t>(std::strtoull(argv[i], nullptr, 10)));
    std::printf("%s %zu %zu %zu %zu %zu %zu %zu %zu\n", argv[i], c.login, c.i2c, c.errors, c.systemEvents,
                c.events, c.audit, c.crashHistory, c.total());
  }
  const char* names[] = {"/system/sys_logs/successful_login.log", "/system/sys_logs/failed_login.log",
                         "/system/sys_logs/i2c_errors.log", "/system/sys_logs/errors.log",
                         "/system/sys_logs/system-events.log", "/system/sys_logs/events.log",
                         "/system/sys_logs/command-audit.log", "/system/sys_logs/crash-history.log",
                         "/sd/system/sys_logs/errors.log", "/sd/system/sys_logs/failed_login.log",
                         "/system/sys_logs/events.log.1", "errors.log", "", "/system/sys_logs/system-events.log.1"};
  for (const char* name : names) {
    std::printf("policy %s %d %d\n", name[0] ? name : "-", logcaps::writesThrough(name) ? 1 : 0,
                logcaps::followedByCatchup(name) ? 1 : 0);
  }
  std::printf("policy null %d %d\n", logcaps::writesThrough(nullptr) ? 1 : 0, logcaps::followedByCatchup(nullptr) ? 1 : 0);
  return 0;
}
'''

NOMINAL = {"login": 696969, "i2c": 64 * 1024, "errors": 256 * 1024, "systemEvents": 256 * 1024,
           "events": 256 * 1024, "audit": 500 * 1024, "crashHistory": 64 * 1024}
ORDER = ("login", "i2c", "errors", "systemEvents", "events", "audit", "crashHistory")
NOMINAL_TOTAL = 2 * NOMINAL["login"] + sum(NOMINAL[k] for k in ORDER[1:])
FLOOR = 4096

# Pinned per-layout numbers (KiB -> login, 256 KB logs, audit, 64 KB logs).
PINNED = {
    128: (4096, 4096, 4096, 4096),
    1856: (113927, 45000, 84607, 13835),
    2284: (141131, 55132, 104548, 16247),
    5696: (357998, 135900, 263520, 35478),
    7808: (492237, 185895, 361923, 47381),
    8704: (549187, 207105, 403670, 52431),
    9068: (572323, 215721, 420629, 54483),
    10092: (637408, 239961, 468339, 60254),
    24512: (696969, 262144, 512000, 65536),
}

LOGS = {
    "LOG_OK_FILE": "/system/sys_logs/successful_login.log",
    "LOG_FAIL_FILE": "/system/sys_logs/failed_login.log",
    "LOG_I2C_FILE": "/system/sys_logs/i2c_errors.log",
    "LOG_ERROR_FILE": "/system/sys_logs/errors.log",
    "LOG_EVENTS_FILE": "/system/sys_logs/system-events.log",
    "LOG_EVENT_STREAM_FILE": "/system/sys_logs/events.log",
}
EXPECTED_CAPS = {
    "/system/sys_logs/successful_login.log": NOMINAL["login"],
    "/system/sys_logs/failed_login.log": NOMINAL["login"],
    "/system/sys_logs/i2c_errors.log": NOMINAL["i2c"],
    "/system/sys_logs/errors.log": NOMINAL["errors"],
    "/system/sys_logs/system-events.log": NOMINAL["systemEvents"],
    "/system/sys_logs/events.log": NOMINAL["events"],
    "/system/sys_logs/command-audit.log": NOMINAL["audit"],
    "/system/sys_logs/crash-history.log": NOMINAL["crashHistory"],
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def expected_caps(total: int) -> dict:
    # Each log keeps its floor; what the quarter holds above the eight floors
    # is shared in proportion to what each log has above its floor.
    budget, floors = total // 4, 8 * FLOOR
    if total == 0 or budget >= NOMINAL_TOTAL:
        return dict(NOMINAL)
    if budget <= floors:
        return {k: min(v, FLOOR) for k, v in NOMINAL.items()}
    return {k: FLOOR + (v - FLOOR) * (budget - floors) // (NOMINAL_TOTAL - floors) for k, v in NOMINAL.items()}


def littlefs_sizes() -> dict:
    sizes = {}
    for table in sorted((REPO / "partitions").glob("partitions_*.csv")):
        for line in table.read_text().splitlines():
            fields = [field.strip() for field in line.split("#", 1)[0].split(",")]
            if len(fields) >= 5 and fields[0] == "littlefs":
                sizes[table.name] = int(fields[4], 0)
    require(len(sizes) >= 9, "every partition table has a littlefs row")
    return sizes


def balanced_args(text: str, start: int) -> list:
    """The top-level arguments of the call whose '(' is at `start`."""
    depth, args, current, quote, i = 0, [], "", "", start
    while i < len(text):
        ch = text[i]
        if quote:
            current += ch
            if ch == "\\":
                current += text[i + 1]
                i += 1
            elif ch == quote:
                quote = ""
        elif ch in "\"'":
            quote = ch
            current += ch
        elif ch == "(":
            depth += 1
            if depth > 1:
                current += ch
        elif ch == ")":
            depth -= 1
            if depth == 0:
                args.append(current.strip())
                return args
            current += ch
        elif ch == "," and depth == 1:
            args.append(current.strip())
            current = ""
        else:
            current += ch
        i += 1
    raise AssertionError("unbalanced call")


def source_caps() -> None:
    logging_h = (COMPONENT / "System_Logging.h").read_text()
    constants = {name: eval(value, {"__builtins__": {}})  # noqa: S307 - arithmetic on integer literals only
                 for name, value in re.findall(r"constexpr size_t (LOG_\w+) = ([0-9 *]+);", logging_h)}
    debug = (COMPONENT / "System_Debug.cpp").read_text()
    for name, path in LOGS.items():
        require(f'extern const char* const {name} = "{path}";' in debug, f"{name} names {path}")
    seen = {}
    calls = 0
    for source in sorted(COMPONENT.glob("*.cpp")) + sorted(COMPONENT.glob("*.h")):
        text = re.sub(r"//[^\n]*", "", source.read_text(errors="replace"))
        for match in re.finditer(r"\b(appendLineWithCap|appendTimestampedLine)\s*\(", text):
            before = text[max(0, match.start() - 40):match.start()]
            if re.search(r"\b(bool|void|inline bool|static void|return)\s*$", before):
                continue  # a declaration, a definition, or the String form's forward
            args = balanced_args(text, match.end() - 1)
            target, cap = args[0], args[-1]
            if target in ("path", "logFile"):
                # appendTimestampedLine's own forward and the auth log's choice
                # between the two login logs (both LOG_CAP_BYTES).
                require(cap in ("capBytes", "LOG_CAP_BYTES"), f"{source.name}: {match.group(0)}{args}")
                continue
            path = LOGS.get(target) or (target[1:-1] if target.startswith('"') else None)
            require(path is not None, f"{source.name}: capped-log call with an unknown log: {target}")
            value = constants.get(cap)
            if value is None:
                require(re.fullmatch(r"[0-9 *]+", cap) is not None, f"{source.name}: cap of {path} is not a constant: {cap}")
                value = eval(cap, {"__builtins__": {}})  # noqa: S307
            require(seen.setdefault(path, value) == value, f"{path} is given two caps: {seen[path]} and {value}")
            calls += 1
    require(calls >= 15, f"found only {calls} capped-log call sites")
    require(seen == EXPECTED_CAPS,
            "the capped logs and their caps differ from System_LogCapsCore.h's table:\n"
            f"  source: {sorted(seen.items())}\n  header: {sorted(EXPECTED_CAPS.items())}")
    require(sum(seen.values()) == NOMINAL_TOTAL, "the call sites' caps add up to kNominalTotal")
    print(f"log caps: {calls} call sites name {len(seen)} logs, {sum(seen.values())} bytes nominal")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    sizes = littlefs_sizes()
    totals = sorted({0, *sizes.values(), *(kib * 1024 for kib in PINNED), 4 * NOMINAL_TOTAL, 4 * NOMINAL_TOTAL - 4,
                     64 * 1024, 16 * 1024 * 1024})
    with tempfile.TemporaryDirectory(prefix="hw1-log-caps-") as temp:
        work = Path(temp)
        unit, binary = work / "log_caps.cpp", work / "log_caps"
        unit.write_text(HARNESS)
        flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(COMPONENT)]
        if args.sanitize:
            flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        subprocess.run([args.cxx, *flags, str(unit), "-o", str(binary)], check=True, timeout=120)
        out = subprocess.run([str(binary), *map(str, totals)], capture_output=True, text=True, check=True, timeout=30).stdout
    rows, policy = {}, {}
    for line in out.splitlines():
        fields = line.split()
        if fields[0] == "policy":
            policy[fields[1]] = (fields[2] == "1", fields[3] == "1")
        else:
            rows[int(fields[0])] = dict(zip((*ORDER, "total"), map(int, fields[1:])))
    for total in totals:
        want = expected_caps(total)
        got = rows[total]
        require({k: got[k] for k in ORDER} == want, f"logCapsFor({total}): {got} != {want}")
        require(got["total"] == 2 * want["login"] + sum(want[k] for k in ORDER[1:]), "total() adds the two login logs")
        require(all(got[k] <= NOMINAL[k] for k in ORDER), "no cap exceeds today's constant")
        if total >= 128 * 1024:
            require(got["total"] <= total // 4, f"{total}: the capped logs fit in a quarter of the partition")
            if total < 4 * NOMINAL_TOTAL:
                require(got["total"] > total // 4 - 8, f"{total}: the quarter is used (rounding aside)")
        elif total > 0:
            require(got["total"] == 8 * FLOOR, "below 128 KiB every log has its floor")
        if total >= 4 * NOMINAL_TOTAL:
            require(got["total"] == NOMINAL_TOTAL, "a large partition keeps every cap")
    for kib, (login, mid, audit, small) in PINNED.items():
        got = rows[kib * 1024]
        require((got["login"], got["errors"], got["systemEvents"], got["events"], got["audit"], got["i2c"],
                 got["crashHistory"]) == (login, mid, mid, mid, audit, small, small), f"{kib} KiB: {got}")
    require(rows[128 * 1024]["total"] == 128 * 1024 // 4, "eight logs at the floor are a quarter of the 128 KiB table")
    for name, size in sizes.items():
        require(rows[size]["total"] <= size // 4, f"{name}: caps exceed a quarter of its {size // 1024} KiB")
    through = {name for name, (w, _) in policy.items() if w}
    followed = {name for name, (_, f) in policy.items() if f}
    require(through == {"/system/sys_logs/successful_login.log", "/system/sys_logs/failed_login.log",
                        "/system/sys_logs/crash-history.log", "/sd/system/sys_logs/failed_login.log"},
            f"written through: {sorted(through)}")
    require(followed == {"/system/sys_logs/errors.log", "/system/sys_logs/events.log",
                         "/system/sys_logs/crash-history.log", "/sd/system/sys_logs/errors.log", "errors.log"},
            f"followed by catch-up: {sorted(followed)}")
    # The followed set is the ledger's fixed list (System_CatchupLedger.cpp).
    ledger = (COMPONENT / "System_CatchupLedger.cpp").read_text()
    for name in ("events.log", "errors.log", "crash-history.log"):
        require(f'/system/sys_logs/{name}"' in ledger, f"the ledger follows {name}")
    for name in ("system-events.log", "command-audit.log", "i2c_errors.log", "successful_login.log", "failed_login.log"):
        require(f'/system/sys_logs/{name}"' not in ledger, f"the ledger does not follow {name}")
    source_caps()
    print(f"log caps: {len(totals)} partition sizes, {len(sizes)} partition tables within a quarter, policy and call sites checked")


if __name__ == "__main__":
    main()
