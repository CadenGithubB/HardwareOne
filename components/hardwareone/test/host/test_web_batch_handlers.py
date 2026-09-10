#!/usr/bin/env python3
"""Compile shipping CLI batch handlers with host boundary mocks.

The two handlers, their marker helpers, and the settings cleanup class are
extracted from production on every run. The JSON implementation is the real
vendored ArduinoJson with its Arduino String adapter enabled. Only hardware,
HTTP, command execution, identity, and allocation backends are mocked; this
does not execute ESP-IDF, Arduino's real heap policy, or a device task race.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
JSON_INCLUDE = COMPONENT.parent / "hardwareone_libs" / "ArduinoJson" / "src"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


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


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    if not args.cxx:
        raise SystemExit("a host C++17 compiler is required")
    local = (COMPONENT / "WebServer_Server.cpp").read_text()
    bond = (COMPONENT / "WebPage_Bond.cpp").read_text()
    definitions = [
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
    require("results.add(redacted)" in definitions[-2],
            "local batch must append owning redacted String")
    require("results.add(out)" in definitions[-1],
            "bond batch must append owning output String")

    harness = (HERE / "web_batch_handler_harness.cpp").read_text()
    marker = "// INSERT_PRODUCTION_HANDLERS_HERE"
    require(harness.count(marker) == 1, "harness must have one production insertion marker")
    harness = harness.replace(marker, "\n\n".join(definitions))
    with tempfile.TemporaryDirectory(prefix="hw1-web-batch-host-") as temp:
        generated = Path(temp) / "web_batch_handlers.cpp"
        executable = Path(temp) / "web_batch_handlers"
        generated.write_text(harness)
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   "-pedantic", "-I", str(HERE / "web_batch_stubs"),
                   "-I", str(JSON_INCLUDE), str(generated), "-o", str(executable)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-g"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
