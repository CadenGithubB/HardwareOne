#!/usr/bin/env python3
"""Execute the production web login slot choice with the ESP32's 32-bit millis.

The harness compiles, from WebServer_Server.cpp on every run, setSession,
findFreeSessionIndex, pruneExpiredSessions, the session cookie writer and the
session fence with its slot invalidate/publish helpers, plus SessionEntry from
WebServer_Server.h. `unsigned long` and `long` in that text are mapped to
uint32_t/int32_t, the ESP32's widths, so millis() arithmetic wraps as on the
device. httpd, tokens, transport epochs, events and logout reasons are
recording doubles.

It checks that with the two-slot table full a new login replaces the session
that expires first, at early uptime and across the millis() wrap, and that the
replaced session is signed out like any other logout: SYSEVT_LOGOUT with
"evicted", a logout reason for its browser (unless it is the address signing
in), its socket closed (unless it carries this login), its transport epoch
closed and its slot generation bumped; a revoked session waiting out its
notice grace period is replaced with its socket closed but no second event or
reason. Not covered: the cookie reaching a
browser, handleLogin, or the SSE delivery of the logout reason.
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

FENCE_START = "std::atomic<TransportSessionEpoch> sWebSessionEpoch[MAX_SESSIONS];"
FENCE_CLASS = "class WebSessionFenceGuard"
FENCE_HELPERS = ("void webInvalidateSessionSlotLocked(", "void webInvalidateSessionSlot(",
                 "bool webPublishSessionSlot(")
PRODUCTION = ("int findFreeSessionIndex(", "void pruneExpiredSessions(",
              "static esp_err_t writeSessionCookie(", "String setSession(")


def unique(source: str, marker: str, name: str) -> str:
    require(source.count(marker) == 1, f"{name}: marker is not unique: {marker}")
    return extract_block(source, marker)


def esp32_widths(text: str) -> str:
    """unsigned long / long are 32 bits on the ESP32 (ILP32)."""
    text = re.sub(r"\bunsigned long\b", "uint32_t", text)
    text = re.sub(r"\(long\)", "(int32_t)", text)
    return re.sub(r"static_cast<long>", "static_cast<int32_t>", text)


def generate() -> tuple[str, bool]:
    server = (COMPONENT / "WebServer_Server.cpp").read_text()
    header = (COMPONENT / "WebServer_Server.h").read_text()

    define = re.search(r"^#define MAX_SESSIONS\s+\d+\s*$", header, re.M)
    require(define is not None, "MAX_SESSIONS definition")
    types = define.group(0) + "\n" + unique(header, "struct SessionEntry", "WebServer_Server.h") + ";"

    require(server.count(FENCE_START) == 1 and server.count(FENCE_CLASS) == 1, "one session fence")
    start = server.index(FENCE_START)
    prelude = server[start:server.index(FENCE_CLASS, start)]
    fence = [prelude + unique(server, FENCE_CLASS, "WebServer_Server.cpp") + ";"]
    fence += [unique(server, marker, "WebServer_Server.cpp") for marker in FENCE_HELPERS]
    production = [unique(server, marker, "WebServer_Server.cpp") for marker in PRODUCTION]

    harness = (HERE / "web_session_eviction_harness.cpp").read_text()
    for marker, text in (("// INSERT_TYPES", types), ("// INSERT_FENCE", "\n\n".join(fence)),
                         ("// INSERT_PRODUCTION", "\n\n".join(production))):
        require(harness.count(marker) == 1, f"one insertion marker {marker}")
        harness = harness.replace(marker, esp32_widths(text))
    return harness, "sWebSessionSlotGeneration" in prelude


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--case", help="run one harness case by name")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    unit_text, has_generation = generate()
    with tempfile.TemporaryDirectory(prefix="hw1-web-session-eviction-") as temporary:
        directory = Path(temporary)
        (directory / "Arduino.h").write_text((HERE / "web_batch_stubs" / "Arduino.h").read_text())
        unit = directory / "web_session_eviction.cpp"
        unit.write_text(unit_text)
        binary = directory / "web_session_eviction"
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-parameter", "-Wno-unused-function",
                   f"-DHAVE_SLOT_GENERATION={int(has_generation)}",
                   "-I", str(directory), str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=180)
        subprocess.run([str(binary)] + ([args.case] if args.case else []), check=True, timeout=60)


if __name__ == "__main__":
    main()
