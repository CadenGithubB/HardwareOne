#!/usr/bin/env python3
"""Source contract: OLED leftovers the two review sets could only clear after the merge.

1. The unified actions mode's placeholder file is gone: OLED_Mode_UnifiedMenu.cpp
   does not exist and the component CMakeLists.txt does not list it; the empty
   "displayMemoryStats() moved to ..." section of System_MemoryMonitor.cpp is
   gone too (that function was deleted; the page is displayMemoryStatsRendered).
2. OLED_Utils.cpp carries no `#if !ENABLE_OLED_DISPLAY` stubs of
   oledBootModeActive / shouldBlockForDisplayAuth (their comment named a
   tgRequireAuth reader that no longer exists). Each is defined once, inside the
   display build, and every firmware reference outside a declaration sits under
   `#if ENABLE_OLED_DISPLAY`, so a DISPLAY_TYPE=0 build needs no stub. The
   CMakeLists comments no longer give those stubs as OLED_Utils.cpp's reason to
   stay unconditional.
3. OLED_Mode_Network.cpp credits getTotalFailedLoginCount() to System_User's
   credential attempt table (all transports), not to WebServer_Server.h, whose
   include comment names what the file still takes from it.
4. The file browsers describe `filerename` / `filedelete` as they are
   registered: user-level commands (System_Filesystem.cpp registers both
   without the admin flag), with no "admin-only" left in their comments.
Reads source text only; nothing is compiled.
--component points the suite at another tree's component directory.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent

DISPLAY_SYMBOLS = ("oledBootModeActive", "shouldBlockForDisplayAuth")
DECLARATIONS = (re.compile(r"^\s*extern\s+bool\s+oledBootModeActive\s*;\s*$"),
                re.compile(r"^\s*bool\s+shouldBlockForDisplayAuth\s*\(\s*\)\s*;\s*$"))


def strip_comments(text: str) -> str:
    """Blank // and /* */ comments, keeping newlines (string contents kept)."""
    out, pos, n = [], 0, len(text)
    while pos < n:
        if text.startswith("//", pos):
            end = text.find("\n", pos)
            pos = n if end < 0 else end
            continue
        if text.startswith("/*", pos):
            end = text.find("*/", pos + 2)
            chunk = text[pos:n if end < 0 else end + 2]
            out.append("\n" * chunk.count("\n"))
            pos = n if end < 0 else end + 2
            continue
        if text[pos] in "\"'":
            quote, end = text[pos], pos + 1
            while end < n and text[end] != quote and text[end] != "\n":
                end += 2 if text[end] == "\\" else 1
            out.append(text[pos:end + 1])
            pos = end + 1
            continue
        out.append(text[pos])
        pos += 1
    return "".join(out)


def comments_only(text: str) -> str:
    """The // and /* */ comment text of a source."""
    return "\n".join(re.findall(r"//[^\n]*|/\*.*?\*/", text, flags=re.S))


def display_guarded_lines(code: str) -> list[bool]:
    """Per line: inside a branch that is compiled only with ENABLE_OLED_DISPLAY."""
    guarded, stack = [], []  # stack of (condition, in_else)
    for line in code.splitlines():
        stripped = line.strip()
        directive = re.match(r"#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", stripped)
        if directive:
            kind, condition = directive.group(1), directive.group(2).strip()
            if kind in ("if", "ifdef", "ifndef"):
                stack.append((condition if kind == "if" else "", False))
            elif kind == "elif" and stack:
                stack[-1] = (condition, False)
            elif kind == "else" and stack:
                stack[-1] = (stack[-1][0], True)
            elif kind == "endif" and stack:
                stack.pop()
        inside = False
        for condition, in_else in stack:
            compact = condition.replace(" ", "")
            if "||" in compact:
                continue  # either side may compile it
            negative = "!ENABLE_OLED_DISPLAY" in compact
            positive = not negative and re.search(r"(?<![!\w])ENABLE_OLED_DISPLAY\b", compact) is not None
            if (positive and not in_else) or (negative and in_else):
                inside = True
        guarded.append(inside)
    return guarded


def firmware_sources(component: Path) -> list[Path]:
    found = []
    for pattern in ("*.cpp", "*.h", "*.c"):
        for path in component.rglob(pattern):
            parts = path.relative_to(component).parts
            if "test" in parts or path.name.startswith("._"):
                continue
            found.append(path)
    return sorted(found)


def unified_menu_failures(component: Path) -> list[str]:
    failures = []
    if (component / "OLED_Mode_UnifiedMenu.cpp").exists():
        failures.append("OLED_Mode_UnifiedMenu.cpp still exists (it only held a placeholder comment)")
    cmake = (component / "CMakeLists.txt").read_text(encoding="utf-8")
    listed = [line.strip() for line in cmake.splitlines()
              if not line.strip().startswith("#") and "OLED_Mode_UnifiedMenu.cpp" in line]
    if listed:
        failures.append("components/hardwareone/CMakeLists.txt still lists OLED_Mode_UnifiedMenu.cpp")
    memory = (component / "System_MemoryMonitor.cpp").read_text(encoding="utf-8")
    if re.search(r"\bdisplayMemoryStats\s*\(", memory):
        failures.append("System_MemoryMonitor.cpp still names the deleted displayMemoryStats()")
    return failures


def display_stub_failures(component: Path) -> list[str]:
    failures = []
    oled_text = (component / "OLED_Utils.cpp").read_text(encoding="utf-8")
    if re.search(r"(?m)^\s*#\s*if\s+!\s*ENABLE_OLED_DISPLAY\b", oled_text):
        failures.append("OLED_Utils.cpp still has an `#if !ENABLE_OLED_DISPLAY` stub block")
    if "tgRequireAuth still references" in oled_text:
        failures.append("OLED_Utils.cpp still claims tgRequireAuth references the display stubs")
    oled_code = strip_comments(oled_text)
    oled_guard = display_guarded_lines(oled_code)
    definitions = {
        "oledBootModeActive": re.compile(r"(?m)^\s*bool\s+oledBootModeActive\s*=[^;]*;"),
        "shouldBlockForDisplayAuth": re.compile(r"(?m)^\s*bool\s+shouldBlockForDisplayAuth\s*\(\s*\)\s*\{"),
    }
    for name, pattern in definitions.items():
        found = list(pattern.finditer(oled_code))
        if len(found) != 1:
            failures.append(f"OLED_Utils.cpp defines {name} {len(found)} times, want 1")
            continue
        line = oled_code.count("\n", 0, found[0].start())
        if not oled_guard[line]:
            failures.append(f"OLED_Utils.cpp:{line + 1}: {name} is defined outside the display build")

    for path in firmware_sources(component):
        code = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        guard = display_guarded_lines(code)
        for number, line in enumerate(code.splitlines()):
            for name in DISPLAY_SYMBOLS:
                if not re.search(r"\b" + name + r"\b", line):
                    continue
                if any(pattern.match(line) for pattern in DECLARATIONS):
                    continue
                if not guard[number]:
                    failures.append(f"{path.relative_to(component)}:{number + 1}: {name} is used "
                                    "outside `#if ENABLE_OLED_DISPLAY` (a DISPLAY_TYPE=0 build would need a stub)")

    cmake = (component / "CMakeLists.txt").read_text(encoding="utf-8")
    if "!ENABLE_OLED_DISPLAY stub implementations" in re.sub(r"\s*\n\s*#\s*", " ", cmake):
        failures.append("components/hardwareone/CMakeLists.txt still keeps OLED_Utils.cpp for the "
                        "removed !ENABLE_OLED_DISPLAY stub implementations")
    return failures


def failed_login_comment_failures(component: Path) -> list[str]:
    failures = []
    network = (component / "OLED_Mode_Network.cpp").read_text(encoding="utf-8")
    include = re.search(r'(?m)^#include\s+"WebServer_Server\.h"[^\n]*$', network)
    if include is None:
        failures.append("OLED_Mode_Network.cpp: WebServer_Server.h include not found")
    elif "getTotalFailedLoginCount" in include.group(0):
        failures.append("OLED_Mode_Network.cpp: the WebServer_Server.h include still claims "
                        "getTotalFailedLoginCount() (System_User.h declares it)")
    call = network.find("getTotalFailedLoginCount()", include.end() if include else 0)
    if call < 0:
        failures.append("OLED_Mode_Network.cpp: getTotalFailedLoginCount() call not found")
    else:
        # The // comment block directly above the line that reads the counter.
        lines = network[:call].splitlines()
        comment = []
        for line in reversed(lines[:-1]):
            if not line.strip().startswith("//"):
                break
            comment.insert(0, line.strip()[2:].strip())
        text = " ".join(comment)
        if "WebServer_Server" in text:
            failures.append("OLED_Mode_Network.cpp: the failed-login counter is still credited to WebServer_Server")
        if "every transport" not in text and "all transports" not in text:
            failures.append("OLED_Mode_Network.cpp: the failed-login counter comment must say it counts "
                            "failed credential attempts on every transport")
    header = (component / "System_User.h").read_text(encoding="utf-8")
    if not re.search(r"\buint32_t\s+getTotalFailedLoginCount\s*\(\s*\)\s*;", header):
        failures.append("System_User.h no longer declares getTotalFailedLoginCount()")
    return failures


def file_verb_comment_failures(component: Path) -> list[str]:
    failures = []
    registry = (component / "System_Filesystem.cpp").read_text(encoding="utf-8")
    for verb in ("filedelete", "filerename"):
        row = re.search(r'\{\s*"' + verb + r'"\s*,\s*"(?:[^"\\]|\\.)*"\s*,\s*(true|false)\s*,', registry)
        if row is None:
            failures.append(f"System_Filesystem.cpp: registry row for {verb} not found")
        elif row.group(1) != "false":
            failures.append(f"System_Filesystem.cpp: {verb} is admin-flagged again; "
                            "the browsers' comments and toasts assume user-level verbs")
    for name in ("OLED_Mode_FileBrowser.cpp", "G2_Page_Files.cpp"):
        comments = comments_only((component / name).read_text(encoding="utf-8"))
        for match in re.finditer(r"admin-only[^\n]*", comments):
            context = comments[max(0, match.start() - 120):match.end() + 120]
            if "filerename" in context or "filedelete" in context or "commands" in context:
                failures.append(f"{name}: a comment still calls the file verbs admin-only: "
                                f"{match.group(0).strip()[:70]}")
    return failures


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", help="accepted for a uniform runner interface; unused")
    parser.add_argument("--sanitize", action="store_true", help="accepted for a uniform runner interface; unused")
    parser.add_argument("--component", type=Path, default=HERE.parents[1])
    args = parser.parse_args()
    component = args.component.resolve()

    sections = (
        ("unified actions mode file", unified_menu_failures),
        ("display-off stubs", display_stub_failures),
        ("failed-login counter comment", failed_login_comment_failures),
        ("file verb comments", file_verb_comment_failures),
    )
    failures = []
    for label, check in sections:
        failures += [f"{label}: {failure}" for failure in check(component)]
    if failures:
        print("OLED merge leftovers FAILED:")
        for failure in failures:
            print("  " + failure)
        raise SystemExit(1)
    print(f"OLED merge leftovers: {len(sections)} source contracts passed")


if __name__ == "__main__":
    main()
