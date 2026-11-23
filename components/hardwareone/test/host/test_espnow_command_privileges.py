#!/usr/bin/env python3
"""ESP-NOW test commands that change live state need admin, and the commands
the 1.0 clean-out removed stay removed.

Reads System_ESPNow.cpp as text (nothing is compiled): the rows of
espNowCommands[] and, for each row it judges, the handler's own body.

- A test command (its name contains "test", or its help text starts with
  "TEST") is admin unless its handler changes nothing: no memset of a global,
  no assignment to a global, no topology-stream mutator, no send.
- The 1.0 clean-out (ESP-NOW 1.0 step 1) removed the test commands
  `teststreams`, `testconcurrent`, `testcleanup` and `testfilelock`,
  `bondtestsensor`, unencrypted `espnowpair`, and the heartbeat mode
  (`espnowhbmode`, `espnowheartbeatbroadcast` and the setting
  meshHeartbeatBroadcast, which nothing read): no row, no handler and no
  setting field remain.

Not covered: the general rule for the other open rows that save a setting or
transmit (an owner decision), and whether the executor enforces the flag
(test_command_executor_admission.py and the role suites own that).

--component DIR reads another tree.
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
REMOVED_COMMANDS = {
    "teststreams": "cmd_test_streams", "testconcurrent": "cmd_test_concurrent",
    "testcleanup": "cmd_test_cleanup", "testfilelock": "cmd_test_filelock",
    "bondtestsensor": "cmd_bond_testsensor", "espnowpair": "cmd_espnow_pair",
    "espnowhbmode": "cmd_espnow_hbmode", "espnowheartbeatbroadcast": "cmd_espnow_heartbeatbroadcast",
}
REMOVED_FIELD = "meshHeartbeatBroadcast"


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

    header = (args.component / "System_ESPNow.h").read_text()
    for name, handler in sorted(REMOVED_COMMANDS.items()):
        check(f"{name} has no row", name not in rows)
        check(f"{handler} is gone", re.search(r"\b" + handler + r"\b", source + header) is None)
    settings = (args.component / "System_Settings.h").read_text()
    check("the heartbeat mode setting is gone",
          REMOVED_FIELD not in source and REMOVED_FIELD not in settings and '"heartbeatBroadcast"' not in source)

    print(f"espnow command privileges: {total - failures}/{total} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
