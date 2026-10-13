#!/usr/bin/env python3
"""Execute production transport-session snapshots and lazy bypass creation.

The complete boot-local namespace, registry and serial/display lifecycle
functions are extracted unchanged. The host replaces only Arduino String,
critical sections, clock/notification boundaries and the FreeRTOS mutex that
serializes each transport's lifecycle writer (a contended take runs a hook
that plays the holder). No threaded fixture reads or writes the plain bool
authentication policy.

Source contract, checked before anything is compiled: the two writers are
FreeRTOS mutex handles (priority inheritance), not std::atomic_flag spin
loops, and no writer window in System_User.cpp or HardwareOne.cpp (a
Begin/End pair or a NamedSourceUseGuard scope) contains console output, a
display push, a log append or a users.json read.
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

# Calls that return with a lifecycle writer held, and the guard object whose
# lifetime holds one. The retired spin-lock spelling stays listed so an old
# source fails the contract instead of slipping past it.
WRITER_BEGIN_CALLS = ("serialTransportSessionBeginDelivery(",
                      "localDisplayTransportSessionBeginDelivery(",
                      "localDisplayTransportSessionBeginUiSync(",
                      "serialTransportSessionClearAndBeginDelivery(")
WRITER_GUARD_DECLARATION = re.compile(r"\bNamedSourceUseGuard\s+\w+\s*\(")
# I/O that must never run while a writer is held: console writes, the display
# push, the auth-log append and roster (users.json) reads, plus the output
# drain wait and the shared mirror that fan out to other sinks.
WRITER_WINDOW_FORBIDDEN = ("Serial.", "displayUpdate(", "appendLineWithCap(",
                           "recordLoginAttempt(", "updateUserLastSeen(",
                           "isAdminUser(", "readRosterJson(", "readText(",
                           "writeText", "broadcastOutput(", "BROADCAST_PRINTF(",
                           "debugWaitOutputDrained(")


def strip_comments_and_literals(source: str) -> str:
    """Blank comments and string/char literal bodies, keeping every newline."""
    out, pos, state = [], 0, "code"
    while pos < len(source):
        char = source[pos]
        following = source[pos + 1: pos + 2]
        if state == "line":
            out.append(char if char == "\n" else " ")
            if char == "\n":
                state = "code"
        elif state == "comment":
            out.append(char if char == "\n" else " ")
            if char == "*" and following == "/":
                out.append(" ")
                pos += 1
                state = "code"
        elif state in ("string", "character"):
            quote = '"' if state == "string" else "'"
            if char == "\\":
                out.append("  ")
                pos += 1
            elif char == quote:
                out.append(char)
                state = "code"
            else:
                out.append(char if char == "\n" else " ")
        elif char == "/" and following in ("/", "*"):
            out.append("  ")
            pos += 1
            state = "line" if following == "/" else "comment"
        else:
            out.append(char)
            if char in ('"', "'"):
                state = "string" if char == '"' else "character"
        pos += 1
    return "".join(out)


def enclosing_block_end(code: str, start: int) -> int:
    """Index just past the closing brace of the innermost block around start."""
    depth = 0
    for pos in range(start, len(code)):
        if code[pos] == "{":
            depth += 1
        elif code[pos] == "}":
            if depth == 0:
                return pos
            depth -= 1
    return len(code)


def writer_windows(code: str):
    """Yield (start, end) spans of code executed while a writer may be held."""
    for call in WRITER_BEGIN_CALLS:
        for match in re.finditer(re.escape(call), code):
            line_start = code.rfind("\n", 0, match.start()) + 1
            prefix = code[line_start:match.start()].strip()
            if prefix in ("bool", "TransportSessionEpoch", "void"):
                continue  # the definition or a declaration, not a call
            yield match.start(), enclosing_block_end(code, match.end())
    for match in WRITER_GUARD_DECLARATION.finditer(code):
        yield match.start(), enclosing_block_end(code, match.end())


def check_writer_contract(production: str) -> None:
    require("std::atomic_flag" not in production and "test_and_set" not in production
            and "taskYIELD" not in production,
            "session writers must be FreeRTOS mutexes, not std::atomic_flag spin loops")
    for name in ("sSerialSessionWriter", "sDisplaySessionWriter"):
        require(re.search(r"\bSemaphoreHandle_t\s+" + name + r"\s*=\s*\n?\s*xSemaphoreCreateMutexStatic\(",
                          production) is not None,
                f"{name} must be a SemaphoreHandle_t created with xSemaphoreCreateMutexStatic")
    require("xSemaphoreTake(" in production and "xSemaphoreGive(" in production,
            "writers must be taken and given through the FreeRTOS mutex API")
    for name in ("System_User.cpp", "HardwareOne.cpp"):
        text = (COMPONENT / name).read_text()
        code = strip_comments_and_literals(text)
        windows = list(writer_windows(code))
        require(name != "System_User.cpp" or windows, "expected writer windows in System_User.cpp")
        for start, end in windows:
            window = code[start:end]
            for token in WRITER_WINDOW_FORBIDDEN:
                offset = window.find(token)
                require(offset < 0,
                        f"{name}:{code.count(chr(10), 0, start + offset) + 1}: {token.rstrip('(')} inside the writer "
                        f"window opened at line {code.count(chr(10), 0, start) + 1}")


def generate(source: str) -> str:
    header = (COMPONENT / "System_User.h").read_text()
    types = extract_block(header, "enum CommandSource") + ";\n"
    for marker in ("using TransportSessionEpoch =", "inline constexpr TransportSessionEpoch kNoTransportSessionEpoch =",
                   "static constexpr size_t kPublicUsernameMaxLen ="):
        begin = header.index(marker)
        types += header[begin:header.index(";", begin) + 1] + "\n"
    begin = source.index("namespace {", source.index("// Boot-local transport session generations"))
    namespace = extract_block(source[begin:], "namespace")
    begin = source.index("TransportSessionEpoch transportSessionOpen(CommandSource source)")
    end_marker = "TransportSessionEpoch localDisplayTransportSessionSnapshot("
    end = source.index(end_marker) + len(extract_block(source, end_marker))
    production = namespace + "\n" + source[begin:end]
    check_writer_contract(production)
    require(production.count("serialTransportSessionSnapshot(") == 1, "expected intact serial wrapper")
    require(production.count("localDisplayTransportSessionSnapshot(") == 1, "expected intact display wrapper")
    # The harness stands in for gSettings with the two policy flags the helper
    # reads; tie that stand-in to the real declarations and defaults.
    settings = (COMPONENT / "System_Settings.h").read_text()
    for field in ("serialRequireAuth", "localDisplayRequireAuth"):
        require(re.search(r"\bbool\s+" + field + r";", settings) is not None,
                f"System_Settings.h declares bool {field}")
        require(f"{field}(true)" in settings, f"{field} defaults to true, like the stand-in")
    harness = (HERE / "session_snapshot_harness.cpp").read_text()
    for marker, value in (("// INSERT_TYPES", types), ("// INSERT_PRODUCTION", production)):
        require(harness.count(marker) == 1, f"missing insertion point {marker}")
        harness = harness.replace(marker, value)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path, help="saved pre-change System_User.cpp, kept outside repository")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    sources = [("current", COMPONENT / "System_User.cpp")]
    if args.baseline:
        sources.append(("baseline", args.baseline))
    transcripts = {}
    with tempfile.TemporaryDirectory(prefix="hw1-session-snapshot-") as temporary:
        directory = Path(temporary)
        for label, source in sources:
            unit, binary = directory / f"{label}.cpp", directory / label
            unit.write_text(generate(source.read_text()))
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-Wno-unused-function", "-DENABLE_HTTP_SERVER=0",
                       "-I", str(HERE / "web_batch_stubs"), str(unit), "-o", str(binary)]
            if args.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                                "-fno-omit-frame-pointer", "-g"]
            subprocess.run(command, check=True, timeout=120)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            require(result.returncode == 0, f"{label} failed:\n{result.stdout}\n{result.stderr}")
            count = re.search(r"(?m)^checks=(\d+)$", result.stderr)
            require(count is not None, f"{label}: missing completed assertion count")
            transcripts[label] = result.stdout
            print(f"session snapshot {label}: {count[1]} assertions passed; "
                  "writers are FreeRTOS mutexes and no writer window does I/O", flush=True)
        if args.baseline:
            require(transcripts["current"] == transcripts["baseline"],
                    f"snapshot behavior changed:\ncurrent:\n{transcripts['current']}\nbaseline:\n{transcripts['baseline']}")
            print("session snapshot: all state/lifetime transcripts byte-identical to baseline", flush=True)


if __name__ == "__main__":
    main()
