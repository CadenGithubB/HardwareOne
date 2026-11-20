#!/usr/bin/env python3
"""The top-level README stays a short overview that matches the tree.

README.md is the overview and getting started; the reference sections live in
docs/README_DETAILS.md. Nothing else checks either file, so a wrong path or a
renamed heading would ship silently. This test pins:

- the title form RELEASING.md names (`# Hardware One v<PROJECT_VER>`);
- the length (an overview, not a reference);
- the getting-started commands and the tools and saved configuration they name;
- the boards table against boards/*.defaults;
- the mesh protocol name (custom ESP-NOW v1);
- every relative link and #anchor in both files;
- the two headings other documents link to.

Reads the working tree. --repo points the test at another checkout.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
MAX_README_LINES = 120
DETAILS = "docs/README_DETAILS.md"
REQUIRED_COMMANDS = (
    "tools/vendor_arduino.sh",
    "python3 tools/build_config.py new",
    "python3 tools/build_config.py build <name>",
    "tools/build_board.sh <board> --config <name> -p PORT flash monitor",
)
PINNED_HEADINGS = ("### Local speech-to-text model (ESP32-P4)", "### Support matrix")
LINK = re.compile(r"\]\(([^)\s]+)\)")
ATTRIBUTE = re.compile(r'(?:src|srcset|href)="([^"]+)"')


def slug(heading: str) -> str:
    heading = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", heading.strip().lower().replace("`", ""))
    return re.sub(r"[^\w\- ]", "", heading).replace(" ", "-")


def anchors(path: Path) -> set[str]:
    found: set[str] = set()
    seen: dict[str, int] = {}
    fenced = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.lstrip().startswith("```"):
            fenced = not fenced
        if fenced:
            continue
        heading = re.match(r"\s*(?:>\s*)?#{1,6}\s+(.*?)\s*$", line)
        if heading:
            name = slug(heading.group(1))
            count = seen.get(name, 0)
            seen[name] = count + 1
            found.add(name if count == 0 else f"{name}-{count}")
        found.update(re.findall(r'<a (?:name|id)="([^"]+)"', line))
    return found


def unfenced(lines: list[str]) -> list[str]:
    """The lines outside ``` code blocks (a `# comment` in a block is not a heading)."""
    kept = []
    fenced = False
    for line in lines:
        if line.lstrip().startswith("```"):
            fenced = not fenced
            continue
        if not fenced:
            kept.append(line)
    return kept


def link_errors(repo: Path, relative: str) -> list[str]:
    errors = []
    path = repo / relative
    text = path.read_text(encoding="utf-8")
    for target in LINK.findall(text) + ATTRIBUTE.findall(text):
        if target.startswith(("http://", "https://", "mailto:")):
            continue
        file_part, _, fragment = target.partition("#")
        full = (path.parent / file_part).resolve() if file_part else path
        if not full.exists():
            errors.append(f"{relative}: link target does not exist: {target}")
        elif fragment and full.suffix == ".md" and fragment not in anchors(full):
            errors.append(f"{relative}: no heading for the anchor in: {target}")
    return errors


def measure(repo: Path) -> list[str]:
    errors: list[str] = []
    readme = (repo / "README.md").read_text(encoding="utf-8")
    details_path = repo / DETAILS
    if not details_path.is_file():
        return [f"{DETAILS} is missing"]
    details = details_path.read_text(encoding="utf-8")
    lines = readme.splitlines()

    version = re.search(r'^\s*set\(PROJECT_VER "([0-9.]+)"\)', (repo / "CMakeLists.txt").read_text(encoding="utf-8"), re.M)
    if not version:
        errors.append("CMakeLists.txt: no plain PROJECT_VER line")
    else:
        titles = [line for line in unfenced(lines) if line.startswith("# ")]
        wanted = f"# Hardware One v{version.group(1)}"
        if titles != [wanted]:
            errors.append(f"README.md: title must be exactly {wanted!r} (RELEASING.md), found {titles}")
    if re.search(r"\bv?\d+\.\d+", details.splitlines()[0]):
        errors.append(f"{DETAILS}: the title carries a version; RELEASING.md keeps four version sites")

    if len(lines) > MAX_README_LINES:
        errors.append(f"README.md has {len(lines)} lines; the overview stays within {MAX_README_LINES} "
                      f"(move reference text to {DETAILS})")

    for command in REQUIRED_COMMANDS:
        if command not in readme:
            errors.append(f"README.md: getting started no longer shows `{command}`")
    for tool in sorted(set(re.findall(r"\btools/[\w/.-]+\.(?:py|sh)\b", readme))):
        if not (repo / tool).is_file():
            errors.append(f"README.md names {tool}, which does not exist")
    wizard = (repo / "tools/build_config.py").read_text(encoding="utf-8")
    if '("new",' not in wizard:
        errors.append("tools/build_config.py no longer has the `new` guided setup the README leads with")
    for name in sorted(set(re.findall(r"build_config\.py build ([a-z0-9][a-z0-9-]*)", readme))):
        if not (repo / "configurations" / f"{name}.json").is_file():
            errors.append(f"README.md builds the saved configuration {name}, which does not exist")

    boards = {p.name[:-len(".defaults")] for p in (repo / "boards").glob("*.defaults")
              if not p.name.endswith(".ota.defaults")}
    section = readme.split("\n## Boards\n", 1)[-1].split("\n## ", 1)[0]
    listed = set(re.findall(r"^\|[^|]*\|\s*`([a-z0-9_]+)`\s*\|\s*$", section, re.M))
    if listed != boards:
        errors.append(f"README.md boards table differs from boards/*.defaults: "
                      f"missing {sorted(boards - listed)}, unknown {sorted(listed - boards)}")

    if "custom ESP-NOW v1" not in readme:
        errors.append("README.md: the mesh protocol is named `custom ESP-NOW v1`")
    for relative, text in (("README.md", readme), (DETAILS, details)):
        for stale in re.findall(r"(?i)ESP-NOW V[2-9]\b|mesh v4", text):
            errors.append(f"{relative}: stale protocol name {stale!r}; the name is custom ESP-NOW v1")
        errors.extend(link_errors(repo, relative))

    for heading in PINNED_HEADINGS:
        if heading not in details.splitlines():
            errors.append(f"{DETAILS}: heading {heading!r} is linked from other documents and must stay")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=None)
    args = parser.parse_args()
    repo = args.repo.resolve() if args.repo else HERE.parents[3]
    errors = measure(repo)
    for error in errors:
        print(f"FAIL {error}")
    if errors:
        return 1
    print("PASS README overview: title, length, commands, boards table, protocol name, links")
    return 0


if __name__ == "__main__":
    sys.exit(main())
