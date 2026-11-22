#!/usr/bin/env python3
"""Command replies: one failure rule, and failures that say why.

The reply contract (System_CommandReply.h, CONTRIBUTING.md): a failed command
returns `Error: <reason>`, and commandReplyIsFailure() is the only place that
decides from reply text whether a command failed. The executor applies it and
hands callers a bool.

Three checks:

1. Writers. No firmware source returns a bare "ERROR" or "Error", and no
   firmware source holds a string that starts "ERROR: ". Most displays (OLED,
   glasses, MQTT, the UART link) show only the returned reply, so a bare word
   leaves the user without the reason.
2. Readers. No firmware source compares a reply with the "Error"/"ERROR"
   prefix by hand; it uses the executor's bool or commandReplyIsFailure().
   The exceptions are listed with their reason in READER_EXCEPTIONS.
3. The rule itself, compiled from the real header.

Reads the working tree; compiles one small host program. --component points
the test at another component directory.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
SKIPPED_DIRECTORIES = ("test/", "third_party/", "tools/")

BARE_RETURN = re.compile(r'\breturn\b[^;\n]*"(?:ERROR|Error)"\s*;')
UPPERCASE_TEXT = re.compile(r'"ERROR: ')
PREFIX_COMPARE = re.compile(
    r'(?:strncmp|strncasecmp)\s*\([^;\n]*"(?:Error|ERROR)"\s*,\s*5\s*\)'
    r'|startsWith\(\s*"(?:Error|ERROR)"\s*\)')

# file -> why this prefix comparison is not a second copy of the rule.
READER_EXCEPTIONS = {
    "System_CommandReply.h": "the rule",
    "System_Cm5HostControl.cpp": "writer: gives a direct CM5 callback reply its Error: prefix",
}

RULE_PROGRAM = r'''
#include "System_CommandReply.h"
#include <cassert>
#include <initializer_list>
int main() {
  for (const char* failed : {"Error: no such file", "Error", "ERROR: old peer", "ERROR",
                             "Usage: wifiadd <ssid> <pass>", "Unknown command: frob\nType 'help'"}) {
    assert(commandReplyIsFailure(failed));
  }
  for (const char* succeeded : {"", "OK", "OK: saved", "VALID", "error: lower case is data",
                                "Unknown feature list", " Error: leading space"}) {
    assert(!commandReplyIsFailure(succeeded));
  }
  // A JSON reply's own verdict (CMD-3): "success":false or "ok":false at the
  // top level, or an error object whose first key is "error", is a failure.
  for (const char* failed : {"{\"ok\":false}", "{\"success\":false,\"error\":\"SD card not available\"}",
                             "{\"error\":\"oom\"}", "{\"schema\":1,\"ok\":false,\"error\":\"buffer\"}",
                             " { \"success\" : false }", "{\"ok\":true,\"success\":false}",
                             "{\"success\":true,\"ok\":false}", "{\"error\":{\"code\":3}}",
                             "{\"schema\":1,\"list\":[{\"a\":1}],\"ok\":false}"}) {
    assert(commandReplyIsFailure(failed));
    assert(commandReplyJsonStatesFailure(failed));
  }
  // Not a failure: a success, a nested or quoted false, a status object whose
  // "error" field is the last error of a running service, a null error, text.
  for (const char* succeeded : {"{\"schema\":1,\"ok\":true}", "{\"success\":true,\"error\":\"\"}",
                                "{\"state\":\"idle\",\"error\":\"capture timeout\"}",
                                "{\"error\":null,\"state\":\"idle\"}",
                                "{\"results\":[{\"success\":false}],\"ok\":true}",
                                "{\"peer\":{\"ok\":false},\"success\":true}",
                                "{\"note\":\"\\\"ok\\\":false\"}", "{\"okay\":false}",
                                "{\"success\":falsey}", "[{\"ok\":false}]", "{\"ok\":false",
                                "Saved {\"ok\":false}", "{\"name\":\"x\"}"}) {
    assert(!commandReplyIsFailure(succeeded));
  }
  assert(!commandReplyIsFailure(nullptr));
  assert(commandReplyIsFailure(kCommandReplyOutOfMemory));
  return 0;
}
'''


def sources(component: Path):
    for path in sorted(component.rglob("*")):
        if path.suffix not in (".cpp", ".h"):
            continue
        relative = path.relative_to(component).as_posix()
        if relative.startswith(SKIPPED_DIRECTORIES):
            continue
        yield relative, path.read_text(encoding="utf-8", errors="replace")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    component = args.component.resolve()

    failures = []
    for relative, text in sources(component):
        for number, line in enumerate(text.splitlines(), 1):
            where = f"{relative}:{number}: {line.strip()[:110]}"
            if BARE_RETURN.search(line):
                failures.append(f"bare error reply, return \"Error: <reason>\" instead\n    {where}")
            if UPPERCASE_TEXT.search(line):
                failures.append(f"error text is spelled \"Error: \"\n    {where}")
            if PREFIX_COMPARE.search(line) and relative not in READER_EXCEPTIONS:
                failures.append("reply text judged by hand, use the executor's bool or "
                                f"commandReplyIsFailure()\n    {where}")
    for failure in failures:
        print(f"FAIL {failure}", file=sys.stderr)

    if not args.cxx:
        raise SystemExit("command reply contract: a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-command-reply-") as directory:
        unit, executable = Path(directory) / "rule.cpp", Path(directory) / "rule"
        unit.write_text(RULE_PROGRAM)
        subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                        "-I", str(component), str(unit), "-o", str(executable)],
                       check=True, timeout=120)
        subprocess.run([str(executable)], check=True, timeout=30)

    if failures:
        raise SystemExit(f"command reply contract: {len(failures)} violation(s)")
    print("command reply contract: writers, readers and the rule passed")


if __name__ == "__main__":
    main()
