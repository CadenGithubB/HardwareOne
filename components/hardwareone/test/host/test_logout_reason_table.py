#!/usr/bin/env python3
"""The logout reason table replaces its oldest entry, also after millis() wraps.

storeLogoutReason (WebServer_Server.cpp) keeps one reason per client address in
a table of MAX_LOGOUT_REASONS rows. With the table full it looked for the row
whose raw millis() stamp was below `now`; once millis() had wrapped no stamp
was, the index stayed -1 and two Strings were assigned one element before the
table. The production function is compiled with LogoutReason and
MAX_LOGOUT_REASONS from WebServer_Server.h against a settable 32-bit millis();
the table sits between two guard rows. A full table whose stamps lie before the
wrap, and one with stamps on both sides of it, must replace the oldest row by
age and leave the guards alone.

Options: --cxx <compiler>, --sanitize (ASan + UBSan), --component <dir> to run
the same cases against another components/hardwareone directory.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
STORE = "void storeLogoutReason(const String& ip, const String& reason)"


def generated(component: Path) -> str:
    header = (component / "WebServer_Server.h").read_text()
    server = (component / "WebServer_Server.cpp").read_text()
    capacity = re.search(r"^#define MAX_LOGOUT_REASONS \d+$", header, re.M)
    require(capacity is not None, "WebServer_Server.h defines MAX_LOGOUT_REASONS")
    require(server.count(STORE + " {") == 1, "one storeLogoutReason definition")
    types = capacity.group(0) + "\n" + extract_block(header, "struct LogoutReason") + ";"
    harness = (HERE / "logout_reason_table_harness.cpp").read_text()
    for marker, text in (("// INSERT_TYPES", types),
                         ("// INSERT_PRODUCTION", extract_block(server, STORE))):
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, text)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-I", str(HERE / "web_batch_stubs")]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-logout-reason-table-") as directory:
        source = Path(directory) / "logout_reason_table.cpp"
        binary = Path(directory) / "logout_reason_table"
        source.write_text(generated(args.component.resolve()))
        subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)], check=True, timeout=180)
        subprocess.run([str(binary)], check=True, timeout=60)
    print("the logout reason table replaces its oldest entry across the millis() wrap", flush=True)


if __name__ == "__main__":
    main()
