#!/usr/bin/env python3
"""Compile the shared HTTP request-body reader with host boundary mocks.

webRecvRequestBody, webReadRequestBody and webSendBodyReadError are extracted
from WebServer_Utils.cpp on every run, their types and sendJsonResponse from
WebServer_Utils.h, and the readPostBody wrapper from WebPage_LLM.cpp. Only the
httpd socket, the allocator and the response sink are mocked: this does not run
ESP-IDF's httpd, a real socket or the device heap.
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


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    if not args.cxx:
        raise SystemExit("a host C++17 compiler is required")
    utils = (COMPONENT / "WebServer_Utils.cpp").read_text()
    header = (COMPONENT / "WebServer_Utils.h").read_text()
    llm = (COMPONENT / "WebPage_LLM.cpp").read_text()
    for source, marker in ((utils, "WebBodyStatus webRecvRequestBody("),
                           (utils, "WebBodyBuffer webReadRequestBody("),
                           (utils, "esp_err_t webSendBodyReadError("),
                           (header, "enum class WebBodyStatus"),
                           (header, "struct WebBodyBuffer"),
                           (llm, "static bool readPostBody(")):
        require(source.count(marker) == 1, f"production marker must be unique: {marker}")
    reader = extract_block(llm, "static bool readPostBody(")
    require("httpd_req_recv" not in reader and "webRecvRequestBody" in reader,
            "WebPage_LLM readPostBody must go through the shared reader")
    # The default arguments live on the header declarations; carry them over.
    declarations = re.findall(r"^(?:WebBodyStatus webRecvRequestBody|esp_err_t webSendBodyReadError)\(.*;$",
                              header, re.MULTILINE)
    require(len(declarations) == 2, "shared reader declarations missing from WebServer_Utils.h")
    definitions = [
        extract_block(header, "enum class WebBodyStatus") + ";",
        extract_block(header, "struct WebBodyBuffer") + ";",
        *declarations,
        extract_block(header, "inline esp_err_t sendJsonResponse("),
        extract_block(utils, "WebBodyStatus webRecvRequestBody("),
        extract_block(utils, "WebBodyBuffer webReadRequestBody("),
        extract_block(utils, "esp_err_t webSendBodyReadError("),
        reader,
    ]
    harness = (HERE / "web_body_reader_harness.cpp").read_text()
    marker = "// INSERT_PRODUCTION_READERS_HERE"
    require(harness.count(marker) == 1, "harness must have one production insertion marker")
    harness = harness.replace(marker, "\n\n".join(definitions))
    with tempfile.TemporaryDirectory(prefix="hw1-web-body-reader-") as temp:
        generated = Path(temp) / "web_body_reader.cpp"
        executable = Path(temp) / "web_body_reader"
        generated.write_text(harness)
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   str(generated), "-o", str(executable)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-g"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
