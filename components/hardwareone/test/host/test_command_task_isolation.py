#!/usr/bin/env python3
"""Execute shipping code that runs beside the command executor on other tasks.

Two cases, each compiled from command_task_isolation_harness.cpp:

- broadcast: broadcastOutputCore (System_Debug.cpp) and the validate-state
  accessor currentCommandValidateOnly (System_Utils.cpp). One thread runs a
  validate pass the way cmd_exec does (per-task CommandContext plus the
  handler mirror gCLIValidateOnly); another task broadcasts meanwhile. Only
  the validating task's output may be dropped.
- password: userChangePasswordCore and cmd_user_changepassword
  (System_User.cpp) with RETURN_VALID_IF_VALIDATE_CSTR (System_Utils.h) and
  USER_CHANGE_PASSWORD_MESSAGE_MAX (System_User.h). The status is the return
  value, the message lands in the caller's buffer, and the shared debug
  buffer is never touched.

Both cases always run; the suite fails if either fails.
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


def read(name: str) -> str:
    return (COMPONENT / name).read_text()


def extract_macro(source: str, name: str) -> str:
    lines = source[source.index(f"#define {name}("):].splitlines()
    result = []
    for line in lines:
        result.append(line)
        if not line.endswith("\\"):
            return "\n".join(result)
    raise AssertionError(f"unterminated macro {name}")


def fill(harness: str, inserts: dict[str, str]) -> str:
    for marker, value in inserts.items():
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, value)
    return harness


def broadcast_source() -> str:
    debug_h, debug, utils = read("System_Debug.h"), read("System_Debug.cpp"), read("System_Utils.cpp")
    routes = re.findall(r"(?m)^#define (?:MSG_ROUTE_\w+|DEBUG_MSG_SIZE) .*$", debug_h)
    require(len(routes) >= 9, "MSG_ROUTE_* and DEBUG_MSG_SIZE come from System_Debug.h")
    production = []
    if "bool currentCommandValidateOnly()" in utils:
        production.append(extract_block(utils, "bool currentCommandValidateOnly()"))
    production.append(extract_block(debug, "static void broadcastOutputCore("))
    harness = (HERE / "command_task_isolation_harness.cpp").read_text()
    return fill(harness, {"// INSERT_ROUTES": "\n".join(routes),
                          "// INSERT_BROADCAST_PRODUCTION": "\n\n".join(production)})


def password_source() -> str:
    user, user_h, utils_h = read("System_User.cpp"), read("System_User.h"), read("System_Utils.h")
    constant = re.findall(r"(?m)^constexpr size_t USER_CHANGE_PASSWORD_MESSAGE_MAX = \d+;$", user_h)
    require(len(constant) == 1, "System_User.h has no USER_CHANGE_PASSWORD_MESSAGE_MAX "
            "(userChangePasswordCore has no caller-buffer contract)")
    require("bool userChangePasswordCore(" in user,
            "userChangePasswordCore does not return a status (it returns the shared debug buffer)")
    production = "\n\n".join([
        extract_macro(utils_h, "RETURN_VALID_IF_VALIDATE_CSTR"),
        extract_block(user, "bool userChangePasswordCore("),
        extract_block(user, "const char* cmd_user_changepassword(")])
    harness = (HERE / "command_task_isolation_harness.cpp").read_text()
    return fill(harness, {"// INSERT_CONSTANTS": constant[0], "// INSERT_PASSWORD_PRODUCTION": production})


def run_case(name: str, source_fn, args, temporary: Path) -> str | None:
    try:
        source = source_fn()
    except (AssertionError, ValueError) as error:
        return f"{name}: cannot build from the sources: {error}"
    unit, binary = temporary / f"{name}.cpp", temporary / name
    unit.write_text(source)
    command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-pthread",
               f"-DHW1_CASE_{name.upper()}", "-I", str(HERE / "web_batch_stubs"),
               str(unit), "-o", str(binary)]
    if args.sanitize:
        command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-omit-frame-pointer", "-g"]
    compiled = subprocess.run(command, capture_output=True, text=True, timeout=180)
    if compiled.returncode != 0:
        return f"{name}: compile failed:\n{compiled.stderr[-3000:]}"
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
    if result.returncode != 0:
        return f"{name}: failed:\n{result.stdout}\n{result.stderr}"
    count = re.search(r"(?m)^checks=(\d+)$", result.stderr)
    if count is None:
        return f"{name}: missing completed check count"
    print(f"{result.stdout.strip()} ({count[1]} checks)", flush=True)
    return None


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    failures = []
    with tempfile.TemporaryDirectory(prefix="hw1-command-task-isolation-") as temporary:
        for name, source_fn in (("broadcast", broadcast_source), ("password", password_source)):
            failure = run_case(name, source_fn, args, Path(temporary))
            if failure:
                failures.append(failure)
    require(not failures, "\n\n".join(failures))
    print("command task isolation passed", flush=True)


if __name__ == "__main__":
    main()
