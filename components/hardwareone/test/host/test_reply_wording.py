#!/usr/bin/env python3
"""The words of a command reply are read in one place: System_CommandReply.h.

The rule (CONTRIBUTING.md, "Display text and reply text"):

1. A command handler returns text. The reply leads with "OK" (the executor's
   stamp on a success), "SUCCESS" (a handler's own confirmation), "Error"
   (a failure and its reason), "Usage:" or "Unknown command:". The executor's
   privilege refusals are kCommandReplyAdminRequired and
   kCommandReplySuperAdminRequired; a structured reply carries "ok":true or
   "ok":false.
2. System_CommandReply.h spells these words. Code that has to read a reply
   asks its functions: commandReplyIsFailure, commandReplyIsError,
   commandReplyHasOkStamp, commandReplyIsBareOk, commandReplyConfirms,
   commandReplyIsAdminRefusal, commandReplyBody, commandReplyReason,
   commandReplyJsonOk, commandReplyJsonFailed.
3. A page that needs one particular reply of a module compares it with a
   constant the module's header defines (kGpsTrackReplyCleared,
   kFileReplyMissingOrDenied, kPasswordReplyWrongCurrent, kSettingsReplySaved), which the
   module
   also returns. It does not search the reply for a phrase.

The test fails on a string comparison or search against one of the words in
any other file, and on a search for "successfully".

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
OWNER = "System_CommandReply.h"

WORD = (r'"(?:OK|SUCCESS|Error|ERROR|Usage:|Unknown command|\\"ok\\"|Admin access|'
        r'Super-admin|[^"\n]*successfully)')
READS = (
    re.compile(r'\b(?:strn?cmp|strn?casecmp|strstr|strcasestr)\s*\([^,()]+(?:\([^()]*\))?[^,()]*,\s*' + WORD),
    re.compile(r'(?:\.|->)\s*(?:startsWith|indexOf|equals|equalsIgnoreCase)\s*\(\s*' + WORD),
    re.compile(r'(?:==|!=)\s*' + WORD),
)
FUNCTIONS = ("commandReplyIsFailure", "commandReplyIsError", "commandReplyHasOkStamp",
             "commandReplyIsBareOk", "commandReplyConfirms", "commandReplyIsAdminRefusal",
             "commandReplyJsonOk", "commandReplyJsonFailed")
TEXT_FUNCTIONS = ("commandReplyBody", "commandReplyReason")
CONSTANTS = {
    "kCommandReplyOkStamp": "OK: ",
    "kCommandReplyErrorLead": "Error: ",
    "kCommandReplyAdminRequired": "Error: Admin access required",
    "kCommandReplySuperAdminRequired": "Error: Super-admin access required",
}
# A module's own reply that a page compares: (header, constant, text).
MODULE_REPLIES = (
    ("System_Maps.h", "kGpsTrackReplyCleared", "GPS track cleared"),
    ("System_Filesystem.h", "kFileReplyMissingOrDenied", "Error: File does not exist or access denied"),
    ("System_User.h", "kPasswordReplyWrongCurrent", "Error: Current password incorrect"),
    ("System_User.h", "kPasswordReplyMismatch", "Error: New passwords do not match"),
    ("System_Settings.h", "kSettingsReplySaved", "Settings saved"),
    ("System_Settings.h", "kSettingsReplySavedNoChange", "Settings saved — no changed files"),
)


def measure(component: Path):
    failures: list[str] = []
    texts = dict(sources(component, (".h", ".cpp", ".c", ".inc")))
    owner = texts.get(OWNER)
    if owner is None:
        return 0, [f"{OWNER} not found under {component}"]
    for name in FUNCTIONS:
        if len(re.findall(r"^inline bool %s\(const char\* reply\) \{" % name, owner, re.M)) != 1:
            failures.append(f"{OWNER} must define {name}(const char* reply) once")
    for name in TEXT_FUNCTIONS:
        if len(re.findall(r"^inline const char\* %s\(const char\* reply\) \{" % name, owner, re.M)) != 1:
            failures.append(f"{OWNER} must define {name}(const char* reply) once")
    for name, value in CONSTANTS.items():
        if re.findall(r'^constexpr char %s\[\] = "([^"\n]*)";$' % name, owner, re.M) != [value]:
            failures.append(f'{OWNER} must define {name} once as "{value}"')
    for header, name, value in MODULE_REPLIES:
        found = re.findall(r'^constexpr char %s\[\] = "([^"\n]*)";$' % name, texts.get(header, ""), re.M)
        if found != [value]:
            failures.append(f'{header} must define {name} once as "{value}"')
        for relative, text in texts.items():
            if relative == header:
                continue
            code = blank_comments(text, keep_strings=True)
            if f'"{value}"' in code:
                number = code.count("\n", 0, code.index(f'"{value}"')) + 1
                failures.append(f'{relative}:{number} spells "{value}"; use {name} of {header}')

    checked = 0
    for relative, text in sorted(texts.items()):
        if relative == OWNER:
            continue
        checked += 1
        code = blank_comments(text, keep_strings=True)
        starts = {match.start() for pattern in READS for match in pattern.finditer(code)}
        for start in sorted(starts):
            number = code.count("\n", 0, start) + 1
            line = code.splitlines()[number - 1].strip()
            failures.append(f"{relative}:{number} reads a reply word; ask a function of {OWNER}\n"
                            f"    {line[:120]}")
    return checked, failures


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    component = args.component.resolve()

    checked, failures = measure(component)
    if not checked:
        raise SystemExit(f"reply wording: no sources under {component}")
    for failure in failures:
        print(f"FAIL {failure}", file=sys.stderr)
    if failures:
        raise SystemExit(f"reply wording: {len(failures)} violation(s)")
    print(f"reply wording: {checked} source files read no reply word outside {OWNER}")


if __name__ == "__main__":
    main()
