#!/usr/bin/env python3
"""ESP-NOW test commands that change live state, and both writers of the
heartbeat mode, need admin.

Reads System_ESPNow.cpp as text (nothing is compiled): the rows of
espNowCommands[] and, for each row it judges, the handler's own body.

- A test command (its name contains "test", or its help text starts with
  "TEST") is admin unless its handler changes nothing: no memset of a global,
  no assignment to a global, no topology-stream mutator, no send. The open
  ones are exactly `testfilelock`, which prints the file-transfer slot table.
  `teststreams`, `testconcurrent` and `testcleanup` rewrite the topology
  collection another session may be using (FEAT-11).
- Every command that writes gSettings.meshHeartbeatBroadcast carries the same
  flag, and that flag is admin: `espnowhbmode` (its handler calls setSetting on
  it) and the command named by the setting's SettingEntry row
  (`espnowheartbeatbroadcast`).

Not covered: the general rule for the other open rows that save a setting or
transmit (an owner decision), and whether the executor enforces the flag
(test_command_executor_admission.py and the role suites own that).

--component DIR reads another tree (the tree before this change fails five
checks).
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent

ROW = re.compile(r'^\s*\{\s*"(?P<name>[A-Za-z0-9_]+)",\s*"(?P<help>(?:[^"\\]|\\.)*)",\s*'
                 r'(?P<admin>true|false),\s*(?P<handler>\w+)', re.M)

# What "changes live state" means for a test command's handler.
WRITES = (
    ("memset of a global", re.compile(r"\bmemset\(\s*&?g[A-Z]\w*")),
    ("assignment to a global",
     re.compile(r"\bg[A-Z]\w*(?:(?:\.|->)\w+|\[[^\]\n]*\])*\s*(?:=(?!=)|\+=|-=|\+\+|--)")),
    ("topology-stream mutator",
     re.compile(r"\b(?:findOrCreateTopoStreamLocked|finalizeTopologyStreamLocked|"
                r"cleanupStaleTopoStreams)\(")),
    ("send", re.compile(r"\b(?:espnowtx::send\w*|v4_send\w*|esp_now_send|sendV4\w*)\(")),
)
OPEN_TEST_COMMANDS = {"testfilelock"}
HEARTBEAT_FIELD = "gSettings.meshHeartbeatBroadcast"


def handler_body(source: str, handler: str) -> str | None:
    marker = f"const char* {handler}(const String& "
    return extract_block(source, marker) if source.count(marker) == 1 else None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--component", type=Path, default=HERE.parents[1])
    args = parser.parse_args()
    source = (args.component / "System_ESPNow.cpp").read_text()

    table = extract_block(source, "extern const CommandEntry espNowCommands[] =")
    rows = {m["name"]: m for m in ROW.finditer(table)}
    failures = total = 0

    def check(name: str, ok: bool, detail: str = "") -> None:
        nonlocal failures, total
        total += 1
        if ok:
            print(f"PASS {name}")
        else:
            failures += 1
            print(f"FAIL {name}{': ' + detail if detail else ''}")

    check("espNowCommands[] parses", len(rows) >= 100, f"{len(rows)} rows")

    tests = {name: row for name, row in rows.items()
             if "test" in name or row["help"].startswith("TEST")}
    check("the test commands are found", {"teststreams", "testconcurrent", "testcleanup",
                                          "testfilelock"} <= set(tests), ", ".join(sorted(tests)))
    for name, row in sorted(tests.items()):
        body = handler_body(source, row["handler"])
        if body is None:
            check(f"{name}: one handler definition", False, row["handler"])
            continue
        writes = [label for label, pattern in WRITES if pattern.search(body)]
        if row["admin"] == "true":
            check(f"{name} is admin", True)
        else:
            check(f"{name} is open and changes nothing", not writes,
                  "handler has " + ", ".join(writes) + " but the row is open")
    open_tests = {name for name, row in tests.items() if row["admin"] == "false"}
    check("the open test commands are exactly testfilelock", open_tests == OPEN_TEST_COMMANDS,
          ", ".join(sorted(open_tests)))

    # Every command that writes the heartbeat mode: handlers that set the
    # field, plus the command its SettingEntry row names.
    writers = set()
    for name, row in rows.items():
        body = handler_body(source, row["handler"])
        if body and re.search(r"setSetting\(\s*" + re.escape(HEARTBEAT_FIELD) + r"\s*,", body):
            writers.add(name)
    entry = re.search(r"\{[^{}\n]*&" + re.escape(HEARTBEAT_FIELD) + r"\b[^{}\n]*\}", source)
    key = re.findall(r'"(\w+)"', entry[0])[-1] if entry else ""
    check("the heartbeat setting names its command", key in rows, key or "no SettingEntry row")
    if key in rows:
        writers.add(key)
    check("two commands write the heartbeat mode",
          writers == {"espnowhbmode", "espnowheartbeatbroadcast"}, ", ".join(sorted(writers)))
    for name in sorted(writers):
        check(f"{name} is admin", rows[name]["admin"] == "true",
              "writes meshHeartbeatBroadcast from an open row")

    print(f"espnow command privileges: {total - failures}/{total} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
