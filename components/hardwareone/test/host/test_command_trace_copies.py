#!/usr/bin/env python3
"""Copies that exist only for a debug trace are made only when it is written.

Every command was redacted for traces that are usually off: the two submit
functions (submitAndExecuteSync, submitCommandAsync) and cmd_exec each ran
redactCmdForAudit on the line and kept the result for DEBUG_CMD_FLOW lines
(and, in the sync submit, the timeout error). broadcastOutput made a full
redacted copy of every command result (up to CMD_RESULT_MAX) for a
50-character DEBUG_CMD_FLOW trace. Each copy is now made inside the trace's
arguments, which are evaluated only when the trace is on, or, in the sync
submit, the first time a trace or the timeout error needs it.

The production submitAndExecuteSync, submitCommandAsync,
releaseSyncExecReqOwner and redactOutputForLog (System_Utils.cpp) and
broadcastOutput with broadcastOutputRouted, its body (HardwareOne.cpp) run on
the production command types and the real Arduino String with counted heap
calls. With the trace off: no
redaction, and no String block in broadcastOutput (the old tree makes them);
with it on, and on a full queue or a timeout: the same lines as before. The
queued and Bluetooth copies of a result are unchanged, a private STT result
included. cmd_exec's loop cannot run on the host; a source check keeps its
redaction inside the trace's arguments, and another keeps the production
DEBUG_CMD_FLOWF lazy: its arguments are evaluated only when the trace is on.

Not changed, and pinned elsewhere: the transports' own redaction,
executeCommand's (test_command_resolution_contract.py), and the web feed's
second redaction in appendCommandToFeed, which is not idempotent ("login"
gives "login ***", and again "login *** ***").

Options: --cxx <compiler>, --sanitize, --component <dir> to run against
another components/hardwareone directory.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from arduino_string_host import arduino_string_counted
from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def generate(component: Path) -> str:
    utils = (component / "System_Utils.cpp").read_text()
    main = (component / "HardwareOne.cpp").read_text()
    pieces = [
        extract_block(utils, "void releaseSyncExecReqOwner(ExecReq* request)"),
        extract_block(utils, "bool submitAndExecuteSync(const Command& cmd, String& out,\n"),
        extract_block(utils, "bool submitCommandAsync(const Command& cmd,"),
        extract_block(utils, "String redactOutputForLog(const String& output)"),
    ]
    # The empty-line reply the submit function names (a tree before it has none).
    empty = re.search(r'static constexpr char kReplyEmptyCommand\[\] =\s*"[^"\n]*";', utils)
    if empty:
        pieces.insert(0, empty[0])
    # broadcastOutput's body, which takes the route mask; a tree before it has none.
    routed = "static void broadcastOutputRouted(const String& s, const CommandContext& ctx,"
    if routed in main:
        pieces.append(extract_block(main, routed))
    pieces.append(extract_block(main, "void broadcastOutput(const String& s, const CommandContext& ctx) "))
    harness = (HERE / "command_trace_copies_harness.cpp").read_text()
    marker = "// INSERT_PRODUCTION"
    require(harness.count(marker) == 1, "one insertion marker")
    return harness.replace(marker, "\n\n".join(pieces))


def source_contract(component: Path) -> None:
    main = (component / "HardwareOne.cpp").read_text()
    executor = extract_block(main, "static void commandExecTask(void* pv) {")
    trace = re.search(r'DEBUG_CMD_FLOWF\("\[cmd_exec\] exec [^;]*;', executor)
    require(trace is not None, "cmd_exec traces the command it runs")
    require("redactCmdForAudit(String(r->line)).c_str()" in trace[0],
            "cmd_exec redacts the line inside the trace's arguments")
    require(executor.count("redactCmdForAudit(") == 1,
            "cmd_exec makes no redacted copy outside its trace")
    # The saving rests on the production trace macro evaluating its arguments
    # only when the trace is on; the harness has its own lazy copy of it.
    debug = (component / "System_Debug.h").read_text()
    require(re.search(r"(?m)^#define DEBUG_CMD_FLOWF\(fmt, \.\.\.\) "
                      r"DEBUGF_QUEUE_DEBUG\(DEBUG_CMD_FLOW, fmt, ##__VA_ARGS__\)$", debug) is not None,
            "DEBUG_CMD_FLOWF is DEBUGF_QUEUE_DEBUG on DEBUG_CMD_FLOW")
    queue = re.search(r"(?ms)^#define DEBUGF_QUEUE_DEBUG\(flag, fmt, \.\.\.\) \\\n(.*?)\n\n", debug)
    body = " ".join(queue.group(1).replace("\\", " ").split()) if queue else ""
    require(body == "do { if (isDebugOutputEnabled(flag)) { debugQueuePrintf(flag, fmt, ##__VA_ARGS__); } } while (0)",
            "DEBUGF_QUEUE_DEBUG evaluates its arguments only inside if (isDebugOutputEnabled(flag))")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    sanitize = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", "-g"] if args.sanitize else []
    with tempfile.TemporaryDirectory(prefix="hw1-command-trace-copies-") as directory:
        d = Path(directory)
        includes, sources = arduino_string_counted(d, sanitize)
        unit, binary = d / "test.cpp", d / "test"
        unit.write_text(generate(args.component))
        subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        "-Wno-sign-compare", "-Wno-format-truncation", *sanitize, *includes,
                        "-I", str(args.component), str(unit), *sources, "-o", str(binary)],
                       check=True, timeout=180)
        subprocess.run([str(binary)], check=True, timeout=30)
    source_contract(args.component)
    print("command trace copies source contract passed")


if __name__ == "__main__":
    main()
