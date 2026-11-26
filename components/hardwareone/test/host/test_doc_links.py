#!/usr/bin/env python3
"""Every relative link in the tracked Markdown resolves; no absolute file links.

Readiness G-22. Reads every tracked *.md file (the vendored libraries,
managed components and experiments excepted), drops fenced code blocks and
inline code, and checks each [text](target) link:

- a URL (scheme:...) or an in-page anchor (#...) is not checked;
- an absolute path (/private/tmp/..., a home folder) fails: a cloner cannot
  follow it, and it leaks the build machine's layout;
- a relative path must name a tracked file or a folder that holds one.

A file the build or a tool makes (not tracked) is named in `code`, not linked.
Reads the index with `git ls-files`; nothing is written. --repo points the
check at another checkout.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import unquote

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]

SKIPPED_PREFIXES = ("components/hardwareone_libs/", "managed_components/")
LINK = re.compile(r"\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
FENCE = re.compile(r"```.*?```", re.S)
CODE_SPAN = re.compile(r"`[^`\n]*`")


def tracked(repo: Path) -> list[str]:
    out = subprocess.run(["git", "-C", str(repo), "ls-files", "-z"], check=True,
                         capture_output=True).stdout.decode("utf-8")
    return [name for name in out.split("\0") if name]


def problems(repo: Path) -> list[str]:
    files = tracked(repo)
    known = set(files)
    for name in files:
        parent = name
        while "/" in parent:
            parent = parent.rsplit("/", 1)[0]
            known.add(parent)
    found: list[str] = []
    for name in files:
        if not name.endswith(".md") or name.startswith(SKIPPED_PREFIXES) or "/managed_components/" in name:
            continue
        text = (repo / name).read_text(encoding="utf-8", errors="replace")
        text = FENCE.sub(lambda m: "\n" * m.group(0).count("\n"), text)
        for number, line in enumerate(text.split("\n"), 1):
            for match in LINK.finditer(CODE_SPAN.sub("", line)):
                target = match.group(1)
                if re.match(r"^[A-Za-z][A-Za-z0-9+.-]*:", target) or target.startswith("#"):
                    continue
                path = unquote(target.split("#", 1)[0])
                if not path:
                    continue
                if path.startswith("/"):
                    found.append(f"{name}:{number}: absolute link {target}")
                    continue
                resolved = os.path.normpath(os.path.join(os.path.dirname(name), path))
                if resolved not in known:
                    found.append(f"{name}:{number}: no tracked file at {target}")
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=REPO)
    args = parser.parse_args()
    found = problems(args.repo.resolve())
    if found:
        print("Markdown links a cloner cannot follow:", file=sys.stderr)
        for line in found:
            print("  " + line, file=sys.stderr)
        return 1
    print("doc links: every relative link in the tracked Markdown resolves")
    return 0


if __name__ == "__main__":
    sys.exit(main())
