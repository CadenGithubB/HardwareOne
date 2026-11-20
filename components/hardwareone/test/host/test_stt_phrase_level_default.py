#!/usr/bin/env python3
"""The within-phrase leveler for speech-to-text is off by default and has one saved setting.

Owner decision 2026-11-19: the leveler (System_STT.cpp steps a session's audio
x1..x8 every 0.1 s) raised word errors in steady hiss on the Citrinet bench, so
it ships off. Before 2026-11-20 the field had no settings row and no command:
its only value was the struct initialiser, true. It now has a row in
sttSettingEntries, an editor command and one admin command, `sttphraselevel`.

Two places hold the default and both must say off. The row's default is the
one a boot applies (applyRegisteredDefaults() stamps every registered field
before the saved file is read); the struct initialiser is what code sees
before that. A build that brings back `true` in either place switches the
leveler on again for every device that never ran the command.

Source checks only: the row, the editor line and the command row sit in
System_Settings.cpp inside the same `#if ENABLE_DICTATION || ENABLE_LOCAL_STT`
block as the other speech settings, and the two readers in System_STT.cpp
still gate the leveler on the setting and on that microphone's automatic gain.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re

from test_web_batch_handlers import require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
SPEECH_CONDITION = "ENABLE_DICTATION || ENABLE_LOCAL_STT"


def enclosing_conditions(lines: list[str], index: int) -> list[str]:
    """The stack of #if conditions open at line `index` (0-based)."""
    stack: list[str] = []
    for line in lines[:index]:
        text = line.strip()
        if re.match(r"#\s*if(n?def)?\b", text):
            stack.append(re.sub(r"\s*//.*$", "", re.sub(r"^#\s*", "", text)))
        elif re.match(r"#\s*(else|elif)\b", text):
            require(bool(stack), "an #else without an #if")
            stack[-1] = "not(" + stack[-1] + ")"
        elif re.match(r"#\s*endif\b", text):
            require(bool(stack), "an #endif without an #if")
            stack.pop()
    return stack


def only_line(lines: list[str], needle: str, what: str) -> int:
    hits = [i for i, line in enumerate(lines) if needle in line and not line.lstrip().startswith("//")]
    require(len(hits) == 1, f"System_Settings.cpp has exactly one {what} (found {len(hits)})")
    return hits[0]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", "--component", dest="source_dir", type=Path, default=COMPONENT)
    src = parser.parse_args().source_dir

    settings_h = (src / "System_Settings.h").read_text()
    require("sttPhraseLevel(false)," in settings_h and "sttPhraseLevel(true)" not in settings_h,
            "the struct default of sttPhraseLevel is off")
    require(re.search(r"^\s*bool sttPhraseLevel;", settings_h, flags=re.M) is not None,
            "sttPhraseLevel is a bool settings field")

    lines = (src / "System_Settings.cpp").read_text().splitlines()

    # The row: default 0 (what a boot applies), range 0..1, bound to the one command name.
    row = only_line(lines, "&gSettings.sttPhraseLevel", "settings row for sttPhraseLevel")
    require(re.search(r'\{ "sttphraselevel",\s*SETTING_BOOL,\s*&gSettings\.sttPhraseLevel,\s*0,\s*0,\s*nullptr,\s*0,\s*1,'
                      r'\s*"[^"]+",\s*nullptr,\s*false,\s*nullptr,\s*"sttphraselevel" \},', lines[row]) is not None,
            "the sttphraselevel row is a bool with default 0, bound to the command sttphraselevel")
    table = max(i for i in range(row) if "static const SettingEntry " in lines[i])
    require("sttSettingEntries[]" in lines[table], "the row is in sttSettingEntries")

    editor = only_line(lines, "SETTING_EDITOR_CMD(cmd_set_sttphraselevel,", "editor command for sttphraselevel")
    require(re.search(r'SETTING_EDITOR_CMD\(cmd_set_sttphraselevel,\s*"sttphraselevel"\)', lines[editor]) is not None,
            "the editor command edits the key sttphraselevel")

    command = only_line(lines, '{ "sttphraselevel", "', "command row for sttphraselevel")
    match = re.search(r'\{ "sttphraselevel", "([^"]+)", (true|false), cmd_set_sttphraselevel, "([^"]+)" \},',
                      lines[command])
    require(match is not None, "the sttphraselevel command row calls cmd_set_sttphraselevel")
    require(match.group(2) == "true", "sttphraselevel is an admin command")
    require(match.group(3) == "Usage: sttphraselevel [0|1]", "sttphraselevel takes 0 or 1")
    require("0 = off, the default" in match.group(1), "the help text names off as the default")

    # One name: no second command row or alias reaches the same handler.
    handlers = [line for line in lines if "cmd_set_sttphraselevel" in line]
    require(len(handlers) == 2, "cmd_set_sttphraselevel is defined once and registered once")

    for index, what in ((row, "settings row"), (editor, "editor command"), (command, "command row")):
        require(enclosing_conditions(lines, index) == ["if " + SPEECH_CONDITION],
                f"the sttphraselevel {what} is compiled exactly when the other speech settings are "
                f"(#if {SPEECH_CONDITION})")

    # The readers are unchanged: the setting, and that microphone's automatic gain.
    stt = (src / "System_STT.cpp").read_text()
    reader = re.compile(
        r"const bool phraseLevel = gSettings\.sttPhraseLevel &&\s*"
        r"\(\(source == AUDIO_SRC_LOCAL_PDM && gSettings\.sttAutoGain\) \|\|\s*"
        r"\(source == AUDIO_SRC_G2_LEFT && gSettings\.sttG2AutoGain\)\);")
    require(len(reader.findall(stt)) == 2 and stt.count("gSettings.sttPhraseLevel") == 2,
            "System_STT.cpp reads sttPhraseLevel in its two session loops, gated on that mic's automatic gain")
    print("stt phrase level default: ok")


if __name__ == "__main__":
    main()
