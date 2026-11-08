#!/usr/bin/env python3
"""Commands and wizard pages named in the public guides exist in the firmware.

docs/USERGUIDE.md and docs/QUICKSTART.md are hand-written. This suite reads them
next to the real sources (nothing is compiled) and checks that:

- every command line in a USERGUIDE code block resolves against the
  CommandEntry registry (tools/command_registry.py parse_commands(), the same
  tables docs/COMMAND_REFERENCE.md is generated from) with the firmware's lookup
  rule: case-insensitive, whole-word, longest registered prefix
  (System_CommandLookupCore.h). Indented continuation lines, `--- section ---`
  separators, `(note)` lines and the documented non-command forms (automation
  `IF`/`NAME:`/`SCHEDULE:` syntax, `debug<flagname>` placeholders and the
  UART-only `dictate` line) are not commands;
- a `(admin)` / `(super admin)` annotation on such a line matches the
  registry's admin / super-admin flag;
- every `debug...` flag named in the USERGUIDE debug-flag table is registered;
- every command named in QUICKSTART's "Step 4 - Access the UI" resolves;
- QUICKSTART's Step 3 wizard table lists the page titles of
  System_SetupWizard.cpp (getWizardPageTitle) in kPageOrder order.

Not covered: inline command names in prose outside those places, and whether a
command's arguments match its usage text.
"""

from __future__ import annotations

from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
REPO = HERE.parents[3]
sys.path.insert(0, str(REPO))

from tools import command_registry  # noqa: E402

USERGUIDE = REPO / "docs" / "USERGUIDE.md"
QUICKSTART = REPO / "docs" / "QUICKSTART.md"
WIZARD = COMPONENT / "System_SetupWizard.cpp"

# Documented forms that are deliberately not CLI commands.
NOT_COMMANDS = (
    re.compile(r"^IF\s"),            # automation condition syntax
    re.compile(r"^(NAME|SCHEDULE):"),  # automation file fields
    re.compile(r"^debug<flagname>"),  # placeholder for the debug flag table
    re.compile(r"^dictate\s.*UART-only"),  # host-link verb, documented as not a CLI command
)


class Report:
    def __init__(self) -> None:
        self.failures = 0
        self.total = 0

    def check(self, name: str, ok: bool, detail: str = "") -> None:
        self.total += 1
        if ok:
            print(f"PASS {name}")
        else:
            self.failures += 1
            print(f"FAIL {name}" + (f"   [{detail}]" if detail else ""))


def registry() -> list[dict]:
    """Registry rows in registration order (first of equal names wins)."""
    rows = []
    for array_rows in command_registry.parse_commands().values():
        rows.extend(array_rows)
    return rows


def resolve(line: str, rows: list[dict]) -> dict | None:
    """System_CommandLookupCore.h resolve(): longest whole-word prefix."""
    text = line.strip()
    best, best_len = None, 0
    for row in rows:
        name = row["name"]
        n = len(name)
        if n <= best_len or n > len(text):
            continue
        if text[:n].lower() != name.lower():
            continue
        if n != len(text) and not text[n].isspace():
            continue
        best, best_len = row, n
    return best


def code_block_lines(text: str):
    """Yield (line_number, raw_line) for lines inside ``` fences."""
    inside = False
    for number, raw in enumerate(text.split("\n"), 1):
        if raw.startswith("```"):
            inside = not inside
            continue
        if inside:
            yield number, raw


def privilege(row: dict) -> str:
    if row["superadmin"]:
        return "super admin"
    return "admin" if row["admin"] else "none"


def annotation(line: str) -> str | None:
    low = line.lower()
    if re.search(r"\(super[ -]?admin\)", low):
        return "super admin"
    if "(admin)" in low:
        return "admin"
    return None


def section(text: str, heading: str) -> str:
    start = text.find(heading)
    if start < 0:
        return ""
    rest = text[start + len(heading):]
    end = re.search(r"^#{1,3} ", rest, re.M)
    return rest[:end.start()] if end else rest


def wizard_titles() -> list[str]:
    src = WIZARD.read_text(encoding="utf-8", errors="replace")
    order_m = re.search(r"kPageOrder\[\]\s*=\s*\{(.*?)\};", src, re.S)
    title_m = re.search(r"getWizardPageTitle\(SetupWizardPage page\)\s*\{(.*?)\n\}", src, re.S)
    if not order_m or not title_m:
        return []
    order = re.findall(r"WIZARD_PAGE_[A-Z_]+", order_m.group(1))
    titles = dict(re.findall(r"case (WIZARD_PAGE_[A-Z_]+): return \"([^\"]+)\";",
                             title_m.group(1)))
    return [titles.get(page, f"<no title for {page}>") for page in order]


def main() -> int:
    report = Report()
    rows = registry()
    report.check("command registry parsed", len(rows) > 500, str(len(rows)))
    by_name = {}
    for row in rows:
        by_name.setdefault(row["name"].lower(), row)

    guide = USERGUIDE.read_text(encoding="utf-8")

    unresolved, wrong_priv = [], []
    last = None
    for number, raw in code_block_lines(guide):
        stripped = raw.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if raw[0].isspace():
            # Continuation of the previous entry: its annotation belongs to it.
            want = annotation(stripped)
            if want and last and privilege(last) != want:
                wrong_priv.append(f"{number}: {last['name']} is {privilege(last)}, doc says {want}")
            continue
        if stripped.startswith("---") or stripped.startswith("("):
            continue
        if any(p.search(stripped) for p in NOT_COMMANDS):
            continue
        row = resolve(stripped, rows)
        if row is None:
            unresolved.append(f"{number}: {stripped[:60]}")
            last = None
            continue
        last = row
        want = annotation(stripped)
        if want and privilege(row) != want:
            wrong_priv.append(f"{number}: {row['name']} is {privilege(row)}, doc says {want}")
    report.check("every USERGUIDE code-block command resolves to a registered command",
                 not unresolved, "; ".join(unresolved))
    report.check("USERGUIDE (admin)/(super admin) annotations match the registry",
                 not wrong_priv, "; ".join(wrong_priv))

    debug_table = section(guide, "## Debug Flags")
    flags = re.findall(r"^\| `(debug[A-Za-z0-9_]+)` \|", debug_table, re.M)
    report.check("USERGUIDE debug-flag table found", len(flags) > 10, str(len(flags)))
    missing = [f for f in flags if f.lower() not in by_name]
    report.check("every USERGUIDE debug flag is a registered command",
                 not missing, ", ".join(missing))

    quick = QUICKSTART.read_text(encoding="utf-8")
    step4 = section(quick, "### Step 4 - Access the UI")
    spans = [s for s in re.findall(r"`([^`]+)`", step4)
             if re.match(r"^[A-Za-z][A-Za-z0-9]+(\s|$)", s)]
    report.check("QUICKSTART Step 4 names commands", len(spans) >= 5, str(spans))
    bad = [s for s in spans if resolve(s, rows) is None]
    report.check("every command in QUICKSTART Step 4 resolves", not bad, ", ".join(bad))

    titles = wizard_titles()
    report.check("wizard page order and titles parsed", len(titles) >= 5, str(titles))
    step3 = section(quick, "### Step 3 - Advanced wizard (Advanced mode only)")
    documented = re.findall(r"^\| \d+ \| \*\*([^*]+)\*\* \|", step3, re.M)
    report.check("QUICKSTART wizard table lists the firmware's pages in kPageOrder order",
                 documented == titles, f"doc={documented} firmware={titles}")

    print(f"{report.total - report.failures}/{report.total} public-doc command checks passed")
    return 1 if report.failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
