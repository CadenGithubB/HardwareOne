#!/usr/bin/env python3
"""Compile shipping CLI batch handlers with host boundary mocks.

The single-command and batch handlers, web context types/preparation, marker
helpers and settings cleanup class are extracted from production on every run. The JSON implementation is the real
vendored ArduinoJson with its Arduino String adapter enabled. Only hardware,
HTTP, command execution, identity, and allocation backends are mocked; this
does not execute ESP-IDF, Arduino's real heap policy, or a device task race.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
JSON_INCLUDE = COMPONENT.parent / "hardwareone_libs" / "ArduinoJson" / "src"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def mac_text(component: Path = COMPONENT) -> str:
    """System_MacText.h (MAC_FMT, MAC_ARGS, macToDisplay, macToPathToken,
    macEquals, macParse) as text to place in a generated translation unit."""
    return (component / "System_MacText.h").read_text().replace("#pragma once\n", "", 1)


def json_text(component: Path = COMPONENT) -> str:
    """System_JsonText.h (jsonEscapeChar, jsonEscapedLength, jsonEscapeInto) as
    text to place in a generated translation unit."""
    return (component / "System_JsonText.h").read_text().replace("#pragma once\n", "", 1)


# parseBoolArg for a test String that has c_str() only (same words, no trimming).
ON_OFF_DOUBLE = r'''
#include <strings.h>
inline int parseBoolArg(const String& arg) {
  for (const char* word : {"on", "true", "1", "enable"}) if (strcasecmp(arg.c_str(), word) == 0) return 1;
  for (const char* word : {"off", "false", "0", "disable"}) if (strcasecmp(arg.c_str(), word) == 0) return 0;
  return -1;
}
[[maybe_unused]] inline bool argIsOn(const String& arg) { return parseBoolArg(arg) == 1; }
[[maybe_unused]] inline bool argIsOff(const String& arg) { return parseBoolArg(arg) == 0; }
'''


def arg_text(component: Path = COMPONENT, replies: bool = True, on_off=True) -> str:
    """The command-argument parsers as text to place in a generated translation
    unit: System_ArgParse.h (parseIntArg); with `replies` the two shared
    refusals from System_CommandReply.h (leave them out when the unit includes
    that header); with `on_off` parseBoolArg, argIsOn and argIsOff from
    System_Utils.h, which need a String type declared before this text
    (`on_off="double"` gives ON_OFF_DOUBLE instead)."""
    parts = [(component / "System_ArgParse.h").read_text().replace("#pragma once\n", "", 1)]
    if replies:
        reply_h = (component / "System_CommandReply.h").read_text()
        for name in ("kCommandReplyNotANumber", "kCommandReplyNotOnOff"):
            start = reply_h.index(f"constexpr char {name}[]")
            parts.append("[[maybe_unused]] " + reply_h[start:reply_h.index(";", start) + 1])
    if on_off == "double":
        parts.append(ON_OFF_DOUBLE)
    elif on_off:
        utils_h = (component / "System_Utils.h").read_text()
        parts.append(extract_block(utils_h, "inline int parseBoolArg(const String& arg)"))
        for name in ("argIsOn", "argIsOff"):
            parts.append(extract_block(utils_h, f"inline bool {name}(const String& arg)"))
    return "\n".join(parts) + "\n"


def with_arg_text(unit: str, component: Path = COMPONENT, on_off=False) -> str:
    """A generated translation unit with arg_text() placed where it compiles:
    after the unit's own String class when it defines one, otherwise after the
    #include lines that precede its first definition. The shared refusals are
    left out when the unit includes System_CommandReply.h itself."""
    text = arg_text(component, replies='#include "System_CommandReply.h"' not in unit, on_off=on_off)
    own_string = re.search(r"(?m)^(?:class|struct) String\b[^;{]*\{", unit)
    if own_string:
        end = own_string.start() + len(extract_block(unit[own_string.start():], own_string.group(0)[:-1].rstrip()))
        end = unit.index(";", end) + 1
    else:
        first_body = unit.find("{")
        includes = [m.end() for m in re.finditer(r"(?m)^#include[^\n]*\n", unit[:first_body if first_body >= 0 else len(unit)])]
        end = includes[-1] if includes else 0
    return unit[:end] + "\n" + text + unit[end:]


def extract_block(source: str, marker: str) -> str:
    """Extract an actual C++ definition, ignoring braces in comments/literals."""
    start = source.index(marker)
    opening = source.index("{", start + len(marker))
    pos, depth, state = opening, 0, "code"
    while pos < len(source):
        char = source[pos]
        following = source[pos + 1 : pos + 2]
        if state == "line":
            if char == "\n":
                state = "code"
        elif state == "comment":
            if char == "*" and following == "/":
                state = "code"
                pos += 1
        elif state in ("string", "character"):
            if char == "\\":
                pos += 1
            elif char == ('"' if state == "string" else "'"):
                state = "code"
        elif char == "/" and following == "/":
            state = "line"
            pos += 1
        elif char == "/" and following == "*":
            state = "comment"
            pos += 1
        elif char in ('"', "'"):
            state = "string" if char == '"' else "character"
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start : pos + 1]
        pos += 1
    raise AssertionError(f"unterminated production definition: {marker}")



def request_body_readers(component: Path, allocating: bool = True) -> str:
    """The shared request-body readers, for a harness whose handler calls them.

    allocating=False leaves out the reader that owns its buffer (and with it the
    need for a ps_alloc stand-in)."""
    header = (component / "WebServer_Utils.h").read_text()
    source = (component / "WebServer_Utils.cpp").read_text()
    pieces = [
        extract_block(header, "enum class WebBodyStatus") + ";",
        "WebBodyStatus webRecvRequestBody(httpd_req_t* req, char* buf, size_t bufSize, size_t* outLen = nullptr);",
        extract_block(source, "WebBodyStatus webRecvRequestBody("),
    ]
    if allocating:
        pieces += [
            extract_block(header, "struct WebBodyBuffer") + ";",
            extract_block(source, "WebBodyBuffer webReadRequestBody("),
        ]
    return "\n".join(pieces)


def web_command_types() -> str:
    """Use shipping context fields/defaults instead of a reduced test copy."""
    user = (COMPONENT / "System_User.h").read_text()
    command = (COMPONENT / "System_CommandTypes.h").read_text()
    debug = (COMPONENT / "System_Debug.h").read_text()
    routes = "\n".join(line for line in debug.splitlines()
                       if line.startswith("#define MSG_ROUTE_"))
    return "\n\n".join(["struct httpd_req_t;", routes,
        *(extract_block(user, marker) + ";" for marker in
          ("enum CommandSource", "struct AuthContext")),
        *(extract_block(command, marker) + ";" for marker in
          ("enum CommandOrigin", "enum CommandContextBehavior", "struct CommandContext", "struct Command "))])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path, help="directory holding pre-change source snapshots")
    args = parser.parse_args()
    if not args.cxx:
        raise SystemExit("a host C++17 compiler is required")
    local = ((args.baseline / "WebServer_Server.before.cpp") if args.baseline else
             COMPONENT / "WebServer_Server.cpp").read_text()
    bond = (COMPONENT / "WebPage_Bond.cpp").read_text()
    utils = ((args.baseline / "System_Utils.before.cpp") if args.baseline else
             COMPONENT / "System_Utils.cpp").read_text()
    web_utils = (COMPONENT / "WebServer_Utils.cpp").read_text()
    web_utils_header = (COMPONENT / "WebServer_Utils.h").read_text()
    limits = (COMPONENT / "System_CommandLimits.h").read_text()
    for marker in ("WebBodyStatus webRecvRequestBody(", "WebBodyBuffer webReadRequestBody(",
                   "esp_err_t webSendBodyReadError("):
        require(web_utils.count(marker) == 1, f"shared request-body reader missing: {marker}")
    cmd_input_max = re.search(r"static constexpr size_t CMD_INPUT_MAX = \d+;", limits)
    require(cmd_input_max is not None, "CMD_INPUT_MAX definition not found")
    # The default arguments live on the header declarations; carry them over.
    reader_declarations = re.findall(r"^(?:WebBodyStatus webRecvRequestBody|esp_err_t webSendBodyReadError)\(.*;$",
                                     web_utils_header, re.MULTILINE)
    require(len(reader_declarations) == 2, "shared reader declarations missing from WebServer_Utils.h")
    definitions = [
        "#define HAVE_WEB_CONTEXT_HELPER " + str(int(not args.baseline)),
        cmd_input_max.group(0),
        extract_block(web_utils_header, "enum class WebBodyStatus") + ";",
        extract_block(web_utils_header, "struct WebBodyBuffer") + ";",
        *reader_declarations,
        extract_block(web_utils_header, "inline esp_err_t sendJsonResponse("),
        extract_block(web_utils_header, "inline esp_err_t sendJsonStatus("),
        extract_block(web_utils_header, "inline esp_err_t webSendSessionChanged("),
        extract_block(web_utils, "WebBodyStatus webRecvRequestBody("),
        extract_block(web_utils, "WebBodyBuffer webReadRequestBody("),
        extract_block(web_utils, "esp_err_t webSendBodyReadError("),
        *([] if args.baseline else [extract_block(utils, "CommandContext makeWebCommandContext(")]),
        extract_block(utils, "String redactOutputForLog("),
        extract_block(local, "static String redactWebCommandResult("),
        extract_block(local, "esp_err_t handleCLICommand("),
        extract_block(local, "static bool cliBatchCommandEquals("),
        extract_block(local, "static bool settingsSaveOutputConfirmed("),
        extract_block(local, "class SettingsBatchRequestCleanup") + ";",
        extract_block(local, "esp_err_t handleCliBatch("),
        extract_block(bond, "static esp_err_t handleBondCliBatch("),
    ]
    # Narrow storage contract: runtime output tests alone would also accept the
    # old vector. Guard the actual PSRAM macro and direct, owning String adds.
    for handler in definitions[-2:]:
        require("std::vector<String>" not in handler, "batch restored ordinary String vector")
        require(handler.count("PSRAM_JSON_DOC(respDoc)") == 1,
                "batch response must use exactly one PSRAM JSON document")
        require('JsonArray results = respDoc["results"].to<JsonArray>()' in handler,
                "batch does not accumulate directly in response JSON")
        require("resultsBuffered" in handler and "respDoc.overflowed()" in handler,
                "batch lost insertion/metadata allocation failure guards")
        require("batch_response_oom" in handler, "batch lost explicit response OOM error")
        require("String respStr;" in handler and "serializeJson(respDoc, respStr)" in handler,
                "narrow batch refactor changed the final String serializer")
    require("results.add(directOut)" in definitions[-2],
            "local batch must append owning direct-response String")
    require("results.add(out)" in definitions[-1],
            "bond batch must append owning output String")

    harness = (HERE / "web_batch_handler_harness.cpp").read_text()
    require(harness.count("// INSERT_COMMAND_TYPES") == 1, "missing command types marker")
    harness = harness.replace("// INSERT_COMMAND_TYPES", web_command_types())
    marker = "// INSERT_PRODUCTION_HANDLERS_HERE"
    require(harness.count(marker) == 1, "harness must have one production insertion marker")
    harness = harness.replace(marker, "\n\n".join(definitions))
    with tempfile.TemporaryDirectory(prefix="hw1-web-batch-host-") as temp:
        generated = Path(temp) / "web_batch_handlers.cpp"
        executable = Path(temp) / "web_batch_handlers"
        generated.write_text(harness)
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   "-pedantic", "-Wno-sign-compare", "-I", str(HERE / "web_batch_stubs"),
                   "-I", str(JSON_INCLUDE), str(generated), "-o", str(executable)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-g"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
