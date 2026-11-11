#!/usr/bin/env python3
"""Shared names are declared once, in a header.

The rule (CONTRIBUTING.md, "Keep interfaces small"): a function or variable that more
than one source file uses is declared in the header of the module that defines
it. A `.cpp` file does not carry its own `extern` copy of another module's
declaration: the compiler cannot check such a copy against the definition, so
it goes stale without an error.

Two checks:

1. No firmware `.cpp` declares a name with `extern`. Allowed are `extern "C"`
   (language linkage), definitions (`extern const T name = ...`, which keeps
   the external linkage a namespace-scope const would lose) and the linker
   script symbols in LINKER_SYMBOLS.
2. Command tables are declared only in System_CommandRegistry.h and settings
   modules only in System_Settings.h.

Reads the working tree. --component points the test at another component
directory.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
SKIPPED_DIRECTORIES = ("test/", "third_party/", "tools/")

# Symbols the linker script provides; no header declares them.
LINKER_SYMBOLS = {
    "_bss_start", "_bss_end", "_noinit_start", "_noinit_end",
    "_ext_ram_bss_start", "_ext_ram_bss_end",
    "_ext_ram_noinit_start", "_ext_ram_noinit_end",
}

COMMAND_TABLE_HOME = "System_CommandRegistry.h"
SETTINGS_MODULE_HOME = "System_Settings.h"
COMMAND_TABLE = re.compile(r"\bextern\s+const\s+(?:struct\s+)?CommandEntry\s+\w+\s*\[\s*\]\s*;")
SETTINGS_MODULE = re.compile(r"\bextern\s+const\s+(?:struct\s+)?SettingsModule\s+\w+\s*;")

EXTERN = re.compile(r"(?m)^[ \t]*extern\b")
LANGUAGE_LINKAGE = re.compile(r'extern\s*"')


def blank_comments_and_strings(text: str) -> str:
    """Replaces comment and literal contents by spaces; keeps line breaks and quotes."""
    out = []
    i, n = 0, len(text)
    while i < n:
        two = text[i:i + 2]
        if two == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(c if c == "\n" else " " for c in text[i:j]))
            i = j
        elif text[i] == "R" and text[i + 1:i + 2] == '"':
            open_paren = text.find("(", i)
            delimiter = text[i + 2:open_paren]
            j = text.find(")" + delimiter + '"', open_paren)
            j = n if j < 0 else j + len(delimiter) + 2
            out.append('"' + "".join(c if c == "\n" else " " for c in text[i + 1:j - 1]) + '"')
            i = j
        elif text[i] in "\"'":
            quote = text[i]
            j = i + 1
            while j < n and text[j] != quote and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(quote + " " * (j - i - 1) + (quote if j < n and text[j] == quote else ""))
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def statement(code: str, start: int) -> str:
    """The text from `start` to the end of the declaration or the start of a body."""
    depth = 0
    for i in range(start, len(code)):
        c = code[i]
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        elif depth == 0 and c in ";{":
            return code[start:i + 1]
    return code[start:]


def is_definition(text: str) -> bool:
    """True for `extern T name = ...`: an initializer outside parentheses."""
    depth = 0
    for c in text:
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        elif c == "=" and depth == 0:
            return True
    return False


def declared_name(text: str) -> str:
    head = text.split("(", 1)[0]
    names = re.findall(r"[A-Za-z_]\w*", head.split("[", 1)[0])
    return names[-1] if names else ""


def sources(component: Path, suffixes):
    for path in sorted(component.rglob("*")):
        if path.suffix not in suffixes:
            continue
        relative = path.relative_to(component).as_posix()
        if relative.startswith(SKIPPED_DIRECTORIES):
            continue
        yield relative, path.read_text(encoding="utf-8", errors="replace")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    component = args.component.resolve()

    failures = []
    checked = 0
    for relative, text in sources(component, (".cpp",)):
        checked += 1
        code = blank_comments_and_strings(text)
        for match in EXTERN.finditer(code):
            declaration = statement(code, match.start())
            if LANGUAGE_LINKAGE.match(declaration.lstrip()):
                continue
            if is_definition(declaration) or declared_name(declaration) in LINKER_SYMBOLS:
                continue
            number = code.count("\n", 0, match.start()) + 1
            shown = " ".join(declaration.split())[:110]
            failures.append("extern declaration in a .cpp file, declare the name in its "
                            f"module's header and include that\n    {relative}:{number}: {shown}")

    for relative, text in sources(component, (".h", ".cpp")):
        code = blank_comments_and_strings(text)
        for pattern, home, what in ((COMMAND_TABLE, COMMAND_TABLE_HOME, "command table"),
                                    (SETTINGS_MODULE, SETTINGS_MODULE_HOME, "settings module")):
            if relative == home:
                continue
            for match in pattern.finditer(code):
                number = code.count("\n", 0, match.start()) + 1
                failures.append(f"{what} declared outside {home}\n"
                                f"    {relative}:{number}: {' '.join(match.group(0).split())}")

    for failure in failures:
        print(f"FAIL {failure}", file=sys.stderr)
    if failures:
        raise SystemExit(f"extern declarations: {len(failures)} violation(s)")
    if not checked:
        raise SystemExit(f"extern declarations: no .cpp files under {component}")
    print(f"extern declarations: {checked} source files declare shared names through headers")


if __name__ == "__main__":
    main()
