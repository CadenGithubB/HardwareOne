#!/usr/bin/env python3
"""Run production listing walk/builders/HTTP/command wrapper with host boundaries.

The template walk, output adapter, both public overloads, HTTP handler, command
wrapper, and URL decoder are extracted unchanged. PsramBuffer, MemUtil and
ArduinoJson are real. VFS/File, permission-view queries, identity, and HTTP send
are mocks: the separate listing-permission contract tests the production ACL
implementation, and on-device concurrency/HTTP tests remain necessary.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
JSON_INCLUDE = COMPONENT.parent / "hardwareone_libs" / "ArduinoJson" / "src"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    source = (COMPONENT / "System_Filesystem.cpp").read_text()
    web = (COMPONENT / "WebServer_Server.cpp").read_text()
    utils = (COMPONENT / "System_Utils.cpp").read_text()
    header = (COMPONENT / "System_Filesystem.h").read_text()
    walk = source[source.index("class ListingPsramOutput"):
                  source.index("// Filesystem CLI Command Handlers")]
    buffers = extract_block(source, "bool buildFilesListJson(const String& path, const AuthContext& ctx, bool hideAdminPaths, PsramBuffer& out)")
    compatibility = extract_block(source, "bool buildFilesListJson(const String& path, const AuthContext& ctx, bool hideAdminPaths, String& out)")
    command = extract_block(source, "static const char* filesListingJsonForApp(")
    http = extract_block(web, "esp_err_t handleFilesList(")
    decode = extract_block(utils, "String urlDecode(")
    require("String& out" in header and "PsramBuffer& out" in header,
            "public String API remains alongside the owned PSRAM overload")
    require("static PsramBuffer s_listJson(CMD_RESULT_MAX" in command,
            "command reply retains its private owner and transport bound")
    require("PsramBuffer json(SIZE_MAX" in http and "static PsramBuffer" not in http,
            "HTTP reply storage must be request-owned and not CLI-limited")
    require("sendJsonResponse" not in buffers and "sendJsonResponse" not in walk,
            "shared builders cannot perform network IO while the FS lock is held")
    require(buffers.index("buildFilesListingImpl(") < buffers.index("listingPermissions.forChildOf(path)"),
            "toolbar permission query must follow the entry traversal")
    require("PsramBuffer buffer(SIZE_MAX, \"files.text\")" in extract_block(source, "const char* cmd_files("),
            "text command uses an owned output sink without changing the public String API")
    harness = (HERE / "filesystem_psram_listing_harness.cpp").read_text()
    for marker, replacement in {
        "// INSERT_PRODUCTION_LISTING_HERE": "\n\n".join((walk, buffers, compatibility, command)),
        "// INSERT_PRODUCTION_HTTP_HERE": decode + "\n\n" + http,
    }.items():
        require(harness.count(marker) == 1, f"expected one insertion marker: {marker}")
        harness = harness.replace(marker, replacement)
    with tempfile.TemporaryDirectory(prefix="hw1-listing-host-") as temp:
        generated = Path(temp) / "listing.cpp"
        generated.write_text(harness)
        for psram in (True, False):
            executable = Path(temp) / ("listing_psram" if psram else "listing_internal")
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-I", str(JSON_INCLUDE), "-I", str(HERE / "memutil_stubs"),
                       "-I", str(HERE), "-I", str(COMPONENT), str(generated),
                       str(COMPONENT / "System_MemUtil.cpp"), "-o", str(executable)]
            if psram:
                command.insert(1, "-DBOARD_HAS_PSRAM=1")
            if args.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-g"]
            subprocess.run(command, check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
