#!/usr/bin/env python3
"""executeCommand runs only on the command executor.

Handlers share the executor's task state (the validate mirror, the debug and
capture buffers, static result buffers), so System_Utils.cpp's executeCommand
refuses any task but cmd_exec once the executor queue exists; before that the
early-boot direct path of submitAndExecuteSync is the executor. The harness
compiles the production admission predicate executeCommandCallerAdmitted() and
the opening of executeCommand() through its refusal, and checks: early boot and
cmd_exec are admitted; httpd (or any other task) is refused with an Error
result, false and one log line that names the task but not the command; the
window between creating the queue and the executor task refuses other tasks.
A source check confirms nothing but clearing the output buffer runs before the
refusal.

The ESP-NOW status endpoint (WebPage_ESPNow.cpp handleEspNowStatus), the last
direct caller off the executor, now submits each of its eight fixed status
commands through executeInternalStatusCommand in the same order, after the
same guest-loadable WEB_AUTH_OR_RETURN, and the file has no executeCommand call.

--component / --source-dir point the suite at another copy of the production
sources (e.g. the pre-fix tree, which fails both parts); the harness and this
script are always read from this directory.
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
sys.path.insert(0, str(HERE))
from test_web_batch_handlers import extract_block, require  # noqa: E402

HARNESS = HERE / "command_executor_admission_harness.cpp"
EXECUTE = "bool executeCommand(AuthContext& ctx, const char* cmd, char* out, size_t outSize)"
GUARD = "if (!executeCommandCallerAdmitted())"
STATUS_COMMANDS = [
    "espnowstatus", "espnowmode", "bondstatus", "espnowlist",
    "espnowmeshes listjson", "espnowdeviceinfo", "espnowmeshrole", "espnowmeshstatus",
]


def strip_comments(code: str) -> str:
    code = re.sub(r"/\*.*?\*/", " ", code, flags=re.S)
    return re.sub(r"//[^\n]*", " ", code)


def production(utils: str) -> str:
    require("static bool executeCommandCallerAdmitted()" in utils,
            "System_Utils.cpp has no executeCommandCallerAdmitted(): executeCommand admits any task")
    predicate = extract_block(utils, "static bool executeCommandCallerAdmitted()")
    require(utils.count(EXECUTE) == 1, "one executeCommand definition in System_Utils.cpp")
    body = utils.index("{", utils.index(EXECUTE)) + 1
    require(GUARD in utils[body:], "executeCommand does not call executeCommandCallerAdmitted()")
    guard_at = utils.index(GUARD, body)
    head = utils[body:guard_at]
    require(strip_comments(head).split() == ["out[0]", "=", "'\\0';"],
            "executeCommand runs more than clearing the output before its admission check:\n" + head)
    guard = extract_block(utils[guard_at:], GUARD)
    prefix = ("bool executeCommandPrefix(AuthContext& ctx, const char* cmd, char* out, size_t outSize) {"
              + head + guard + "\n  (void)ctx; (void)cmd;\n  return true;  // reached the handlers\n}\n")
    return predicate + "\n\n" + prefix


def status_endpoint(web: str) -> list[str]:
    failures = []
    if re.search(r"(?<![\w.>])executeCommand\s*\(", strip_comments(web)):
        failures.append("WebPage_ESPNow.cpp still calls executeCommand directly (on httpd)")
    body = extract_block(web, "static esp_err_t handleEspNowStatus(")
    if not body.split("{", 1)[1].lstrip().startswith("WEB_AUTH_OR_RETURN(req, ctx);"):
        failures.append("handleEspNowStatus no longer opens with the guest-loadable WEB_AUTH_OR_RETURN")
    if "executeInternalStatusCommand(CMDS[i], cmdBuf, sizeof(cmdBuf))" not in body:
        failures.append("handleEspNowStatus does not submit CMDS[i] through executeInternalStatusCommand")
    table = re.search(r"static const char\* CMDS\[\] = \{(.*?)\};", body, re.S)
    commands = re.findall(r'"([^"]*)"', table.group(1)) if table else []
    if commands != STATUS_COMMANDS:
        failures.append(f"the status slots changed (the page applies results by slot): {commands}")
    return failures


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("clang++") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=HERE.parents[1])
    parser.add_argument("--source-dir", type=Path,
                        help="overlay directory; a file present here wins over --component")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")

    def read(name: str) -> str:
        overlay = args.source_dir / name if args.source_dir else None
        return (overlay if overlay and overlay.exists() else args.component / name).read_text()

    failures = status_endpoint(read("WebPage_ESPNow.cpp"))
    try:
        unit_source = HARNESS.read_text().replace("// INSERT_PRODUCTION", production(read("System_Utils.cpp")))
    except (AssertionError, ValueError) as error:
        failures.append(f"admission: cannot build from the sources: {error}")
        unit_source = None
    if unit_source is not None:
        with tempfile.TemporaryDirectory(prefix="hw1-command-executor-admission-") as directory:
            unit, binary = Path(directory) / "test.cpp", Path(directory) / "test"
            unit.write_text(unit_source)
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-Wno-gnu-zero-variadic-macro-arguments", str(unit), "-o", str(binary)]
            if args.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                                "-fno-omit-frame-pointer", "-g"]
            compiled = subprocess.run(command, capture_output=True, text=True, timeout=180)
            if compiled.returncode != 0:
                failures.append(f"admission: compile failed:\n{compiled.stderr[-3000:]}")
            else:
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
                sys.stdout.write(result.stdout)
                if result.returncode != 0 or not re.search(r"(?m)^checks=\d+$", result.stderr):
                    failures.append(f"admission: failed:\n{result.stderr}")
    for failure in failures:
        print(f"FAIL {failure}", file=sys.stderr)
    if failures:
        raise SystemExit("command_executor_admission: FAILED")
    print("command_executor_admission: executeCommand admits only cmd_exec and early boot; "
          "the ESP-NOW status endpoint submits")


if __name__ == "__main__":
    main()
