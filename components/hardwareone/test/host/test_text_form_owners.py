#!/usr/bin/env python3
"""Three text forms each have one owner.

1. An encrypted setting value is "AES:<32 hex IV>:<hex ciphertext>".
   System_Settings.h spells the prefix and its offsets (kDeviceCipherPrefix,
   kDeviceCipherSeparatorAt, kDeviceCipherMinLen); everything else asks
   isDeviceCipherText().
2. The micSource setting holds "auto", "pdm" or "g2". micPreferredSource()
   (System_Microphone.cpp) is the one reader of the word; other code compares
   the AudioSource it returns.
3. The slugs `oledmode` accepts are the rows of kOledModeSlugs
   (OLED_Utils.cpp); modeFromSlug() walks the table.

Reads the working tree. --component points the test at another component
directory.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

from test_shared_text_helpers import blank_comments, sources

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

CIPHER_OWNER = "System_Settings.h"
CIPHER_PREFIX = re.compile(r'"AES:"')
MIC_WORD_COMPARE = re.compile(r'\bmicSource(?:\.c_str\(\))?\s*(?:==|!=)\s*"|'
                              r'\bstr(?:case)?cmp\s*\([^,()]*micSource[^,()]*,\s*"')
SLUG_COMPARE = re.compile(r'\bslug\s*==\s*"')


def measure(component: Path) -> tuple[int, list[str]]:
    failures: list[str] = []
    texts = {relative: blank_comments(text, keep_strings=True)
             for relative, text in sources(component, (".h", ".cpp", ".c", ".inc"))}

    def report(relative: str, code: str, match: re.Match, why: str) -> None:
        number = code.count("\n", 0, match.start()) + 1
        failures.append(f"{relative}:{number} {why}\n    {code.splitlines()[number - 1].strip()[:120]}")

    owner = texts.get(CIPHER_OWNER, "")
    if len(re.findall(r'^constexpr char kDeviceCipherPrefix\[\] = "AES:";$', owner, re.M)) != 1:
        failures.append(f"{CIPHER_OWNER} must define kDeviceCipherPrefix once")
    if "inline bool isDeviceCipherText(const char* text) {" not in owner:
        failures.append(f"{CIPHER_OWNER} must define isDeviceCipherText(const char*)")
    for relative, code in sorted(texts.items()):
        if relative != CIPHER_OWNER:
            for match in CIPHER_PREFIX.finditer(code):
                report(relative, code, match, 'spells "AES:"; ask isDeviceCipherText() or use '
                                              f"the constants of {CIPHER_OWNER}")
        for match in MIC_WORD_COMPARE.finditer(code):
            report(relative, code, match, "compares the micSource word; compare micPreferredSource()")

    microphone = texts.get("System_Microphone.cpp", "")
    if microphone.count("AudioSource micPreferredSource() {") != 1:
        failures.append("System_Microphone.cpp must define micPreferredSource() once")

    oled = texts.get("OLED_Utils.cpp", "")
    start = oled.find("kOledModeSlugs[] = {")
    rows = len(re.findall(r'^\s*\{ "[a-z0-9_]+",\s+OLED_\w+ \},$', oled[start:oled.find("\n};", start)], re.M)) \
        if start >= 0 else 0
    if rows < 40:
        failures.append(f"OLED_Utils.cpp: kOledModeSlugs has {rows} rows; the table or the extractor broke")
    for match in SLUG_COMPARE.finditer(oled):
        report("OLED_Utils.cpp", oled, match, "compares a mode slug with a literal; add a kOledModeSlugs row")
    return len(texts), failures


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    component = args.component.resolve()

    checked, failures = measure(component)
    if not checked:
        raise SystemExit(f"text form owners: no sources under {component}")
    for failure in failures:
        print(f"FAIL {failure}", file=sys.stderr)
    if failures:
        raise SystemExit(f"text form owners: {len(failures)} violation(s)")
    print(f"text form owners: {checked} source files; cipher prefix, mic source word and mode slugs "
          f"each read in one place")


if __name__ == "__main__":
    main()
