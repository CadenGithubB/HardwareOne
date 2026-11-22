#!/usr/bin/env python3
"""Execute shipping code that runs beside the command executor on other tasks.

Two cases, each compiled from command_task_isolation_harness.cpp:

- broadcast: broadcastOutputCore (System_Debug.cpp) and the validate-state
  accessor currentCommandValidateOnly (System_Utils.cpp). One thread runs a
  validate pass the way cmd_exec does (per-task CommandContext plus the
  handler mirror gCliValidateOnly); another task broadcasts meanwhile. Only
  the validating task's output may be dropped.
- password: userChangePasswordCore and cmd_user_changepassword
  (System_User.cpp) with RETURN_VALID_IF_VALIDATE_CSTR (System_Utils.h) and
  USER_CHANGE_PASSWORD_MESSAGE_MAX (System_User.h). The status is the return
  value, the message lands in the caller's buffer, and the shared debug
  buffer is never touched. The password rule (isValidPublicPassword and
  publicPasswordRuleReply) is the production one, and a source contract
  checks that every password a person sets goes through it (UA-6).

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
from reply_words_source import reply_prelude

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
    # The one password rule, as production states it (System_User.h/.cpp).
    limits = re.findall(r"(?m)^static constexpr size_t kPublicPassword(?:Min|Max)Len = \d+;$", user_h)
    require(len(limits) == 2, "System_User.h states the password length limits")
    production = "\n\n".join([
        extract_macro(utils_h, "RETURN_VALID_IF_VALIDATE_CSTR"),
        "\n".join(limits),
        extract_block(user, "bool isValidPublicPassword("),
        extract_block(user, "const char* publicPasswordRuleReply("),
        extract_block(user, "bool userChangePasswordCore("),
        extract_block(user, "const char* cmd_user_changepassword(")])
    harness = (HERE / "command_task_isolation_harness.cpp").read_text()
    return fill(harness, {"// INSERT_CONSTANTS": constant[0], "// INSERT_PASSWORD_PRODUCTION": production})


def password_rule_contract() -> None:
    """One password rule (UA-6): every place a person sets a password asks
    isValidPublicPassword (System_User.cpp), directly or through its reply form
    publicPasswordRuleReply; none checks a bare length of its own."""
    user = read("System_User.cpp")
    for marker in ("bool userChangePasswordCore(", "const char* cmd_user_resetpassword(",
                   "const char* cmd_user_add("):
        require("publicPasswordRuleReply(" in extract_block(user, marker),
                f"{marker} applies the shared password rule")
    for marker in ("bool adminCreateUser(", "static AccountRequestResult checkAccountRequestFields(",
                   "const char* publicPasswordRuleReply("):
        require("isValidPublicPassword(" in extract_block(user, marker),
                f"{marker} applies the shared password rule")
    for name, count in (("OLED_Mode_ChangePassword.cpp", 1), ("System_FirstTimeSetup.cpp", 2),
                        ("WebServer_Server.cpp", 1), ("System_ESPNow.cpp", 1)):
        require(read(name).count("isValidPublicPassword(") == count,
                f"{name}: {count} password check(s) through the shared rule")
    g2 = read("G2_Page_Users.cpp")
    require("len >= kPublicPasswordMinLen && len <= kPublicPasswordMaxLen" in g2,
            "G2 user forms check the shared password limits")
    for name in ("System_User.cpp", "OLED_Mode_ChangePassword.cpp", "System_FirstTimeSetup.cpp",
                 "G2_Page_Users.cpp", "WebServer_Server.cpp"):
        bare = re.findall(r"(?i)pass\w*\.length\(\)\s*<\s*\d|rest\.length\(\)\s*<\s*\d", read(name))
        require(not bare, f"{name}: a bare password length check: {bare}")


def run_case(name: str, source_fn, args, temporary: Path) -> str | None:
    try:
        source = source_fn()
    except (AssertionError, ValueError) as error:
        return f"{name}: cannot build from the sources: {error}"
    unit, binary = temporary / f"{name}.cpp", temporary / name
    unit.write_text(reply_prelude(COMPONENT) + source)
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
    password_rule_contract()
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
