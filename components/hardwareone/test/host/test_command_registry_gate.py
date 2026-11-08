#!/usr/bin/env python3
"""The command-registry drift gate passes on the tree.

Runs the two release gates of tools/command_registry.py as a maintainer would:

- `audit` exits 0: no duplicate command names (tables in mutually exclusive
  #if/#else branches, such as System_OTA.cpp's two otaCommands[], count as
  one), no unregistered or dangling arrays, no command string in code that
  resolves to nothing, and every SettingEntry cmdKey binds to a command.
- `reference --check` exits 0: docs/COMMAND_REFERENCE.md is byte-identical to
  a fresh render from the CommandEntry tables, so a registry change that was
  not followed by `python3 tools/command_registry.py reference` fails here.

Each gate's own output is printed on failure. Reads sources only; nothing is
compiled and nothing is written.

--repo points the suite at another checkout; that checkout's tool, sources and
reference are used (the pre-fix tree fails both gates).
"""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]

GATES = (
    ("audit", ["audit"]),
    ("reference --check", ["reference", "--check"]),
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, default=REPO)
    args = parser.parse_args()
    tool = args.repo / "tools" / "command_registry.py"
    if not tool.is_file():
        print(f"FAIL: {tool} not found")
        return 1

    failures = 0
    for label, argv in GATES:
        run = subprocess.run([sys.executable, "-B", str(tool), *argv], cwd=args.repo,
                             capture_output=True, text=True, timeout=120)
        if run.returncode == 0:
            last = run.stdout.strip().splitlines()[-1:] or [""]
            print(f"PASS: command_registry.py {label} ({last[0]})")
            continue
        failures += 1
        print(f"FAIL: command_registry.py {label} exited {run.returncode}")
        for line in (run.stdout + run.stderr).rstrip().splitlines():
            print(f"    {line}")

    print(f"command registry gate: {len(GATES) - failures}/{len(GATES)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
