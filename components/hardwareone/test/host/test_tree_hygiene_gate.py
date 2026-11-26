#!/usr/bin/env python3
"""No account home path and no private LAN address in the tracked tree.

Release gate (RELEASING.md, "Gate: before any tag", tracked tree hygiene).
Searches every tracked text file except the vendored libraries for

- a home directory path of the form /Users/<name>, and
- an address in 192.168.0.0/16,

and fails on any hit outside the short list of ranges the firmware, the tools
and the docs use on purpose (ALLOWED_SUBNETS below). A bench address pasted
into a doc, a capture or a script fails here. Add a range to the list only
with the reason it is not somebody's network.

Not covered: a Wi-Fi network name or a password. Those cannot be named in a
tracked test; the commit hook (secret-guard) and review cover them.

Reads the index with `git grep`; nothing is compiled and nothing is written.
--repo points the gate at another checkout.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]

# Third octet of 192.168.x.y -> why it is in the tree.
ALLOWED_SUBNETS = {
    0: "generic example address",
    1: "generic example address",
    4: "ESP-IDF SoftAP default (192.168.4.1)",
    77: "recovery updater SoftAP (tools/updater/main/Kconfig.projbuild)",
    100: "host-test fixture",
}
ALLOWED_HOME_NAMES: frozenset[str] = frozenset()

PATTERN = r"/Users/[A-Za-z0-9._-]+|192\.168\.[0-9]+"
PATHSPEC = (".", ":!components/hardwareone_libs")


def findings(repo: Path) -> list[str]:
    result = subprocess.run(
        ["git", "-C", str(repo), "grep", "-nIE", PATTERN, "--", *PATHSPEC],
        capture_output=True, text=True, errors="replace")
    if result.returncode == 1:
        return []
    if result.returncode != 0:
        raise RuntimeError(f"git grep failed in {repo}: {result.stderr.strip()}")
    bad: list[str] = []
    for line in result.stdout.splitlines():
        path, number, text = (line.split(":", 2) + ["", ""])[:3]
        for match in re.finditer(PATTERN, text):
            found = match.group(0)
            if found.startswith("/Users/"):
                allowed = found[len("/Users/"):] in ALLOWED_HOME_NAMES
            else:
                allowed = int(found.rsplit(".", 1)[1]) in ALLOWED_SUBNETS
            if not allowed:
                bad.append(f"{path}:{number}: {found}")
    return bad


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, default=REPO)
    args = parser.parse_args()
    try:
        bad = findings(args.repo.resolve())
    except RuntimeError as error:
        print(f"FAIL: {error}")
        return 1
    if bad:
        print(f"FAIL: {len(bad)} home path or private address hit(s) in the tracked tree:")
        for entry in bad:
            print("  " + entry)
        print("Replace each with a placeholder, or add the range to ALLOWED_SUBNETS with its reason.")
        return 1
    print("tree hygiene: no home path, no private address outside the allowed ranges")
    return 0


if __name__ == "__main__":
    sys.exit(main())
