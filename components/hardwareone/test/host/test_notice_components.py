#!/usr/bin/env python3
"""NOTICE.md names every component the dependency locks pull in.

Readiness G-05/DELTA-6. Reads each `dependencies.lock*` at the repository root
and checks that the name of every locked registry component (`  <owner>/<name>:`)
appears in NOTICE.md (case-insensitive), so a new managed component cannot
ship without its licence row. Nothing is written. --repo points the check at
another checkout.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]
LOCKED = re.compile(r"(?m)^  ([A-Za-z0-9_.-]+)/([A-Za-z0-9_.-]+):")


def missing(repo: Path) -> list[str]:
    notice = (repo / "NOTICE.md").read_text(encoding="utf-8").lower()
    locks = sorted(repo.glob("dependencies.lock*"))
    if not locks:
        return ["no dependencies.lock* at the repository root"]
    found: list[str] = []
    for lock in locks:
        for owner, name in LOCKED.findall(lock.read_text(encoding="utf-8")):
            if name.lower() not in notice:
                found.append(f"{lock.name}: {owner}/{name} is not named in NOTICE.md")
    return sorted(set(found))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=REPO)
    args = parser.parse_args()
    found = missing(args.repo.resolve())
    if found:
        for line in found:
            print(line, file=sys.stderr)
        return 1
    print("notice components: every locked component is named in NOTICE.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
