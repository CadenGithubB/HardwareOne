#!/usr/bin/env python3
"""Small text helpers have one owner each.

The rule (CONTRIBUTING.md, "Keep interfaces small": "Small text jobs have one
owner each"):

1. Number arguments go through `parseIntArg` (System_ArgParse.h), which reads a
   whole token and reports failure. `String::toInt`, `atoi` and `atol` read
   junk as 0 and stop at the first non-digit without saying so.
2. On/off arguments go through `parseBoolArg`, `argIsOn` or `argIsOff`
   (System_Utils.h). No handler lists its own synonyms.
3. A MAC address is formatted and parsed by System_MacText.h.
4. JSON string escaping is done by System_JsonText.h (`jsonEscape`,
   `jsonAppendEscaped`, `jsonQuote` in System_Utils).
5. Text is copied into a buffer with `strlcpy` or `snprintf`. `strcpy`,
   `strcat` and `sprintf` are not used. `strncpy` stays only where a record or
   packet field needs its zero fill.

Checks 1, 2, 3 and the `strncpy` part of 5 are ratchets: the tables below hold
the number of remaining uses per file. A file may not go above its number, and
a file not listed has none. When a file drops below its number the test asks
for the smaller number, so the tables stay exact. What remains:

- toInt/atoi/atol: values that are not command arguments (stored files, web
  form fields, on-screen menus, text already checked digit by digit).
- on/off lists: places that accept extra words on purpose (yes/no, a web form
  value) or parse another protocol.
- strncpy: record and packet fields (about 94), and copies of a counted prefix
  such as `strncpy(d, s, len); d[len] = 0` (about 39). The second group was
  left as it is, not reviewed one by one.

Reads the working tree. --component points the test at another component
directory; --print-allowances prints the tables for the current tree.
"""
from __future__ import annotations

import argparse
from collections import Counter
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
SKIPPED_DIRECTORIES = ("test/", "third_party/", "tools/")

NUMBER_OWNER = "System_ArgParse.h"
MAC_OWNER = "System_MacText.h"
JSON_OWNERS = {"System_JsonText.h", "System_Utils.h", "System_Utils.cpp"}

LOOSE_NUMBER = re.compile(r"\.toInt\s*\(|(?<![\w.])ato[il]\s*\(")
ON_WORDS = {"on", "true", "1", "enable", "enabled"}
OFF_WORDS = {"off", "false", "0", "disable", "disabled"}
WORD_COMPARE = re.compile(
    r'(?:==\s*"([A-Za-z0-9]+)"|equalsIgnoreCase\s*\(\s*"([A-Za-z0-9]+)"\s*\)|'
    r'str(?:case)?cmp\s*\([^()]*,\s*"([A-Za-z0-9]+)"\s*\)\s*==\s*0)')
# Six bytes with a separator; plain hex runs (file tokens, fingerprints) are not MAC text.
MAC_FORMAT = re.compile(r"(?:%02[xX][:-]){5}%02[xX]")
JSON_ESCAPE_DEFINITION = re.compile(
    r"(?m)^[ \t]*(?:static\s+|inline\s+)*[\w:<>&*]+[ \t*&]+"
    r"(\w*(?:[jJ]son_?[eE]scape(?!d)|[eE]scape_?[jJ]son)\w*)\s*\([^;{]*\)\s*(?:const\s*)?\{")
UNBOUNDED_COPY = re.compile(r"(?<![\w.])(strcpy|strcat|sprintf|vsprintf)\s*\(")
STRNCPY = re.compile(r"(?<![\w.])strncpy\s*\(")

# Remaining uses per file. Regenerate with --print-allowances after removing some.
LOOSE_NUMBER_ALLOWED: dict[str, int] = {
    "OLED_FirstTimeSetup.cpp": 1,
    "OLED_Mode_Automations.cpp": 2,
    "OLED_RemoteSettings.cpp": 1,
    "OLED_SettingsEditor.cpp": 1,
    "OLED_SetupWizard.cpp": 2,
    "System_Automation.cpp": 3,
    "System_Camera_DVP.cpp": 1,
    "System_ESPNow.cpp": 1,
    "System_ESPSR.cpp": 1,
    "System_FirstTimeSetup.cpp": 2,
    "System_Microphone.cpp": 1,
    "System_SetupWizard.cpp": 3,
    "System_SetupWizardMode.cpp": 3,
    "WebPage_Bond.cpp": 1,
    "WebPage_LLM.cpp": 4,
    "WebServer_Server.cpp": 1,
}
ON_OFF_LIST_ALLOWED: dict[str, int] = {
    "G2_Glasses.cpp": 2,
    "OLED_RemoteSettings.cpp": 1,
    "System_CLIConfirm.cpp": 1,
    "System_MQTT.cpp": 1,
    "WebServer_Server.cpp": 2,
}
MAC_FORMAT_ALLOWED: dict[str, int] = {
}
STRNCPY_ALLOWED: dict[str, int] = {
    "G2_Glasses.cpp": 2,
    "G2_Page_ESPNow.cpp": 1,
    "G2_Page_Network.cpp": 3,
    "G2_Page_Settings.cpp": 1,
    "G2_Page_TestSuite.cpp": 3,
    "G2_Page_TextEntry.cpp": 1,
    "OLED_ESPNow.cpp": 3,
    "OLED_FirstTimeSetup.cpp": 1,
    "OLED_Mode_FileBrowser.cpp": 1,
    "OLED_Mode_Map.cpp": 1,
    "OLED_Mode_Network.cpp": 7,
    "OLED_Mode_Remote.cpp": 1,
    "OLED_Mode_Sensors.cpp": 1,
    "OLED_Mode_System.cpp": 5,
    "OLED_Utils.cpp": 8,
    "System_Automation.cpp": 10,
    "System_CLIConfirm.cpp": 1,
    "System_Cm5Presence.cpp": 1,
    "System_ESPNow.cpp": 20,
    "System_ESPNow_Files.cpp": 2,
    "System_ESPNow_PairCore.h": 1,
    "System_ESPSR.cpp": 2,
    "System_EdgeImpulse.cpp": 3,
    "System_Events.cpp": 4,
    "System_FileManager.cpp": 3,
    "System_Maps.cpp": 7,
    "WebPage_Bond.cpp": 2,
    "WebPage_Sensors.cpp": 1,
    "WebServer_Server.cpp": 4,
    "i2csensor_pca9685.cpp": 1,
}


def blank_comments(text: str, keep_strings: bool) -> str:
    """Replaces comments (and, unless keep_strings, literal contents) by spaces."""
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
            body = text[i + 1:j] if keep_strings else " " * (j - i - 1)
            out.append(quote + body + (quote if j < n and text[j] == quote else ""))
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def sources(component: Path, suffixes):
    for path in sorted(component.rglob("*")):
        if path.suffix not in suffixes:
            continue
        relative = path.relative_to(component).as_posix()
        if relative.startswith(SKIPPED_DIRECTORIES):
            continue
        yield relative, path.read_text(encoding="utf-8", errors="replace")


def on_off_lists(code_with_strings: str) -> int:
    """Lines that compare against two or more spellings of on, or of off."""
    count = 0
    for line in code_with_strings.split("\n"):
        if '"' not in line:
            continue
        words = {next(w for w in match.groups() if w).lower()
                 for match in WORD_COMPARE.finditer(line)}
        if len(words & ON_WORDS) >= 2 or len(words & OFF_WORDS) >= 2:
            count += 1
    return count


def measure(component: Path):
    loose, on_off, mac, strncpy = Counter(), Counter(), Counter(), Counter()
    hard = []
    checked = 0
    for relative, text in sources(component, (".h", ".cpp")):
        checked += 1
        code = blank_comments(text, keep_strings=False)
        with_strings = blank_comments(text, keep_strings=True)

        if relative.endswith(".cpp"):
            loose[relative] = len(LOOSE_NUMBER.findall(code))
            on_off[relative] = on_off_lists(with_strings)
        if relative != MAC_OWNER:
            mac[relative] = len(MAC_FORMAT.findall(with_strings))
        strncpy[relative] = len(STRNCPY.findall(code))

        if relative not in JSON_OWNERS:
            for match in JSON_ESCAPE_DEFINITION.finditer(code):
                number = code.count("\n", 0, match.start()) + 1
                hard.append(f"private JSON escape function, use {sorted(JSON_OWNERS)[0]}\n"
                            f"    {relative}:{number}: {match.group(1)}")
        for match in UNBOUNDED_COPY.finditer(code):
            number = code.count("\n", 0, match.start()) + 1
            hard.append(f"{match.group(1)} has no bound, use strlcpy or snprintf\n"
                        f"    {relative}:{number}")
    return checked, hard, (loose, on_off, mac, strncpy)


RATCHETS = (
    ("LOOSE_NUMBER_ALLOWED", LOOSE_NUMBER_ALLOWED,
     "toInt/atoi/atol", f"parse command numbers with parseIntArg ({NUMBER_OWNER})"),
    ("ON_OFF_LIST_ALLOWED", ON_OFF_LIST_ALLOWED,
     "own list of on/off spellings", "use parseBoolArg, argIsOn or argIsOff (System_Utils.h)"),
    ("MAC_FORMAT_ALLOWED", MAC_FORMAT_ALLOWED,
     "MAC address format string", f"use the formatters in {MAC_OWNER}"),
    ("STRNCPY_ALLOWED", STRNCPY_ALLOWED,
     "strncpy", "copy text with strlcpy; strncpy only for a zero-filled record field"),
)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component", type=Path, default=COMPONENT)
    parser.add_argument("--print-allowances", action="store_true")
    args = parser.parse_args()
    component = args.component.resolve()

    checked, failures, counters = measure(component)
    if not checked:
        raise SystemExit(f"shared text helpers: no sources under {component}")

    if args.print_allowances:
        for (name, _, _, _), counter in zip(RATCHETS, counters):
            print(f"{name}: dict[str, int] = {{")
            for relative in sorted(counter):
                if counter[relative]:
                    print(f'    "{relative}": {counter[relative]},')
            print("}")
        return

    remaining = 0
    for (name, allowed, what, advice), counter in zip(RATCHETS, counters):
        for relative in sorted(set(counter) | set(allowed)):
            found, limit = counter.get(relative, 0), allowed.get(relative, 0)
            remaining += found
            if found > limit:
                failures.append(f"{what}: {relative} has {found}, allowed {limit}; {advice}")
            elif found < limit:
                failures.append(f"{what}: {relative} has {found}, the table says {limit}; "
                                f"lower {name} to {found}")

    for failure in failures:
        print(f"FAIL {failure}", file=sys.stderr)
    if failures:
        raise SystemExit(f"shared text helpers: {len(failures)} violation(s)")
    print(f"shared text helpers: {checked} source files, {remaining} listed exceptions")


if __name__ == "__main__":
    main()
