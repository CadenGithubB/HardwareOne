#!/usr/bin/env python3
"""Model load/unload goes through the admin command, and the backend owns the context cap.

The harness compiles the shipping handleLLMLoad/handleLLMUnload (and their
reply helper) from WebPage_LLM.cpp, cmd_llm_load/cmd_llm_unload from
System_LLMCommands.cpp, CommandArgs from System_Command.cpp, and
llmBackendSelect/llmBackendUnload with the context policy and command builder
from System_LLMBackend.cpp, with vendored ArduinoJson. The command executor is a
double that refuses the admin rows to a non-admin (this runner pins that
llmload and llmunload are requiresAdmin rows) and otherwise runs the real
command handlers; the engine and the model registry are recording doubles.

It checks that a user-role web session is refused and never reaches the engine,
that the submitted command carries the model id, that an admin load gets
llmMaxContext with the body's max_ctx only able to lower it, that
llmBackendSelect applies llmMaxContext when called with 0 (CLI, BLE, G2, OLED,
boot autostart), and that an id which cannot travel as one quoted token is
refused. A source guard checks that WebPage_LLM.cpp and OLED_Mode_LLM.cpp no
longer call llmBackendSelect/llmBackendUnload and submit the commands instead.
Not covered: the real authorizeCommand (its own suites), the OLED screen flow
and the engine's KV sizing.
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
JSON_INCLUDE = COMPONENT.parent / "hardwareone_libs" / "ArduinoJson" / "src"

CASES = ["user_load_refused", "user_unload_refused", "admin_load_through_command",
         "admin_unload_through_command", "bad_id_rejected", "select_applies_setting",
         "command_context_argument", "build_load_command"]


def read(name: str) -> str:
    return (COMPONENT / name).read_text()


def unique_block(source: str, marker: str) -> str:
    require(source.count(marker) == 1, f"marker is not unique: {marker}")
    return extract_block(source, marker)


def strip_comments(source: str) -> str:
    """Drop // and /* */ comments, keeping string and character literals."""
    out, pos, state = [], 0, "code"
    while pos < len(source):
        char, following = source[pos], source[pos + 1:pos + 2]
        if state == "line":
            if char == "\n":
                state = "code"
                out.append(char)
        elif state == "comment":
            if char == "*" and following == "/":
                state = "code"
                pos += 1
        elif state in ("string", "character"):
            out.append(char)
            if char == "\\":
                out.append(following)
                pos += 1
            elif char == ('"' if state == "string" else "'"):
                state = "code"
        elif char == "/" and following == "/":
            state = "line"
            pos += 1
        elif char == "/" and following == "*":
            state = "comment"
            pos += 1
        else:
            if char == '"':
                state = "string"
            elif char == "'":
                state = "character"
            out.append(char)
        pos += 1
    return "".join(out)


def admin_rows(commands: str) -> None:
    for name in ("llmload", "llmunload"):
        row = re.search(r'\{\s*"' + name + r'"\s*,\s*"[^"]*"\s*,\s*(true|false)\s*,', commands)
        require(row is not None, f"no {name} registry row")
        require(row.group(1) == "true", f"{name} must be a requiresAdmin row")


def generated() -> str:
    backend_h, backend = read("System_LLMBackend.h"), read("System_LLMBackend.cpp")
    types_h, commands = read("System_LLMTypes.h"), read("System_LLMCommands.cpp")
    command_h, command = read("System_Command.h"), read("System_Command.cpp")
    web, utils = read("WebPage_LLM.cpp"), read("WebServer_Utils.h")
    admin_rows(commands)

    types = re.findall(r"^#define LLM_SETTING_MAX_CONTEXT\s+\d+", types_h, re.M)
    types += re.findall(r"^#define LLM_(?:MODEL_\w+_LEN|STORAGE_\w+)\s+\d+", backend_h, re.M)
    require(len(types) >= 4, "LLM size and storage constants")
    types += [unique_block(backend_h, "enum class LlmBackendKind") + ";",
              unique_block(backend_h, "struct LlmModelDesc") + ";",
              unique_block(command_h, "class CommandArgs") + ";"]
    auth_start = utils.index("#define WEB_AUTH_OR_RETURN(req, ctx)")
    types.append(utils[auth_start:utils.index("\n\n", auth_start)])
    require(command.count("const String CommandArgs::empty_;") == 1, "CommandArgs::empty_")
    types.append("const String CommandArgs::empty_;")
    types += [unique_block(command, marker) for marker in (
        "CommandArgs::CommandArgs(const String& input)", "void CommandArgs::parse(",
        "const String& CommandArgs::arg(", "int CommandArgs::argInt(")]

    have_build = "bool llmBuildLoadCommand(" in backend
    backend_parts = []
    if "static int llmEffectiveMaxContext(" in backend:
        backend_parts.append(unique_block(backend, "static int llmEffectiveMaxContext("))
    if have_build:
        backend_parts.append(unique_block(backend, "bool llmBuildLoadCommand("))
    backend_parts += [unique_block(backend, "bool llmBackendSelect("),
                      unique_block(backend, "void llmBackendUnload(")]
    # The shipping default argument lives on the header declaration.
    backend_parts.insert(0, "bool llmBackendSelect(const char* id, char* errOut, size_t errCap, int maxCtx = 0);")

    command_parts = [unique_block(commands, "static const char* cmd_llm_load("),
                     unique_block(commands, "static const char* cmd_llm_unload(")]
    web_parts = []
    if "static esp_err_t sendLLMCommandReply(" in web:
        web_parts.append(unique_block(web, "static esp_err_t sendLLMCommandReply("))
    web_parts += [unique_block(web, "static esp_err_t handleLLMLoad("),
                  unique_block(web, "static esp_err_t handleLLMUnload(")]

    harness = (HERE / "llm_load_policy_harness.cpp").read_text()
    for marker, body in (("// INSERT_TYPES", "\n\n".join(types)),
                         ("// INSERT_BACKEND", "\n\n".join(backend_parts)),
                         ("// INSERT_COMMANDS", "\n\n".join(command_parts)),
                         ("// INSERT_WEB", "\n\n".join(web_parts))):
        require(harness.count(marker) == 1, f"bad harness marker: {marker}")
        harness = harness.replace(marker, body)
    return f"#define HAVE_BUILD_LOAD_COMMAND {int(have_build)}\n" + harness


def source_guard() -> None:
    """Surfaces submit llmload/llmunload; only the command handlers call the backend."""
    web = strip_comments(read("WebPage_LLM.cpp"))
    oled = strip_comments(read("OLED_Mode_LLM.cpp"))
    for name, text in (("WebPage_LLM.cpp", web), ("OLED_Mode_LLM.cpp", oled)):
        for call in ("llmBackendSelect(", "llmBackendUnload(", "llmLoadModel(", "llmUnload("):
            require(call not in text, f"{name} calls {call} directly")
    load = unique_block(web, "static esp_err_t handleLLMLoad(")
    unload = unique_block(web, "static esp_err_t handleLLMUnload(")
    require("executeUnifiedWebCommand(" in load and "llmBuildLoadCommand(" in load,
            "handleLLMLoad must submit llmload through the web command executor")
    require("executeUnifiedWebCommand(" in unload and '"llmunload"' in unload,
            "handleLLMUnload must submit llmunload through the web command executor")
    require("gSettings.llmMaxContext" not in load, "the context cap belongs to the backend")
    require("submitOLEDCommandForSession(" in oled, "the OLED must submit through the display executor")
    require("llmBuildLoadCommand(" in oled and '"llmunload"' in oled,
            "the OLED picker and Unload row must submit llmload/llmunload")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    failures = []
    with tempfile.TemporaryDirectory(prefix="hw1-llm-load-policy-") as temporary:
        directory = Path(temporary)
        unit, binary = directory / "llm_load_policy.cpp", directory / "llm_load_policy"
        unit.write_text(generated())
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-parameter",
                   "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
                   "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0", "-DARDUINOJSON_ENABLE_PROGMEM=0",
                   "-I", str(JSON_INCLUDE), str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=180)
        for case in CASES:
            result = subprocess.run([str(binary), case], timeout=60,
                                    capture_output=True, text=True)
            print(result.stdout, end="")
            if result.returncode != 0:
                print(f"FAIL {case}: {result.stderr.strip().splitlines()[-1] if result.stderr.strip() else result.returncode}")
                failures.append(case)
    try:
        source_guard()
        print("PASS source_guard")
    except AssertionError as error:
        print(f"FAIL source_guard: {error}")
        failures.append("source_guard")
    require(not failures, "failed: " + ", ".join(failures))


if __name__ == "__main__":
    main()
