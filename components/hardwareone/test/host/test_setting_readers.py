#!/usr/bin/env python3
"""Every power and camera setting must have a consumer, not only a command.

A source check over the shipping component sources. For each row of
powerSettingEntries and cameraSettingEntries it finds the gSettings field and
requires at least one read of that field outside the settings table, outside
setSetting() writes, and outside the setting's own set/echo command (the
cmd_<cmdKey> function for a one-word cmdKey). A setting that is only stored and
echoed back advertises behaviour nothing performs. It also requires that the
power command does not promise a battery-driven mode switch unless something
calls the auto-mode check. It does not prove a reader uses the value correctly;
the module suites cover behaviour.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
TABLES = (
    ("System_Power.cpp", "static const SettingEntry powerSettingEntries[]"),
    ("System_Camera_DVP.cpp", "static const SettingEntry cameraSettingEntries[]"),
)


def sources() -> dict[str, str]:
    files = {}
    for path in sorted(COMPONENT.rglob("*")):
        if path.suffix in (".cpp", ".h", ".c") and "test" not in path.relative_to(COMPONENT).parts:
            files[str(path.relative_to(COMPONENT))] = path.read_text(errors="replace")
    return files


def rows(table: str) -> list[tuple[str, str | None]]:
    found = []
    for line in table.splitlines():
        field = re.search(r"&gSettings\.(\w+)", line)
        if not field:
            continue
        strings = re.findall(r'"((?:[^"\\]|\\.)*)"', line)
        command = strings[-1] if strings and re.fullmatch(r"[a-z0-9_]+", strings[-1]) else None
        found.append((field.group(1), command))
    return found


def unread(files: dict[str, str]) -> list[str]:
    missing = []
    for filename, marker in TABLES:
        source = files[filename]
        require(source.count(marker) == 1, f"one settings table: {marker}")
        table = extract_block(source, marker)
        entries = rows(table)
        require(entries, f"no rows parsed from {marker}")
        for field, command in entries:
            pattern = re.compile(rf"\bgSettings\.{field}\b")
            readers = 0
            for name, text in files.items():
                if name == filename:
                    text = text.replace(table, "")
                    owner = f"const char* cmd_{command}(" if command else None
                    if owner and text.count(owner) == 1:
                        text = text.replace(extract_block(text, owner), "")
                for line in text.splitlines():
                    if pattern.search(line) and not re.search(rf"setSetting\(\s*gSettings\.{field}\b", line):
                        readers += 1
            if readers == 0:
                missing.append(f"{filename}: {field} (command {command or '-'})")
    return missing


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", help="accepted for the shared ctest arguments; unused")
    parser.add_argument("--sanitize", action="store_true", help="accepted; unused")
    parser.parse_args()
    files = sources()
    problems = ["setting with no consumer: " + item for item in unread(files)]
    calls = sum(len(re.findall(r"\bcheckAutoPowerMode\s*\(", text)) for text in files.values())
    definitions = len(re.findall(r"\bvoid\s+checkAutoPowerMode\s*\(", "\n".join(files.values())))
    if calls and calls <= definitions:
        problems.append("checkAutoPowerMode is declared but never called")
    power = files["System_Power.cpp"]
    if calls <= definitions:
        if "Will switch to PowerSaver" in power:
            problems.append("power auto promises a switch that nothing performs")
        if "not available" not in extract_block(power, "const char* cmd_power("):
            problems.append("power auto must report that it is unavailable")
    require(not problems, "; ".join(problems))
    print(f"PASS every power/camera setting has a consumer; auto-mode calls={calls}")


if __name__ == "__main__":
    main()
