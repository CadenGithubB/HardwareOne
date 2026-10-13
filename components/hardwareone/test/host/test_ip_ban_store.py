#!/usr/bin/env python3
"""Execute the production IP ban list on the shared fake VFS.

The harness compiles the IP ban section of WebServer_Server.cpp (load, save,
ban, unban, the lookup the httpd task runs on every request, the JSON listing)
with the real FsLockGuard, writeText/writeTextAtomic and vendored ArduinoJson
against fake_vfs.h. Encryption, sessions, events and output are doubles.

It checks that a failed save keeps the previous ip_bans.json and reports the
failure while the ban stays in force in memory, that a failed unban keeps the
ban, that an encryption failure never saves a list missing an entry, that 16
bans with 39-character IPs and 79-character reasons list as complete JSON, that
a list too large for the reply buffer is an explicit error rather than a
truncated document, and that the lookup takes the ban-table lock while no file
I/O ever runs under it.
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

SECTION_START = "EXT_RAM_BSS_ATTR static IpBanEntry sIpBans[MAX_IP_BANS];"
SECTION_END = "const char* banListJson()"


def string_double() -> str:
    """The shared host String plus the members the ban code calls."""
    stub = (HERE / "web_batch_stubs" / "Arduino.h").read_text()
    anchor = "  bool concat(const char* text) {"
    require(stub.count(anchor) == 1, "one concat(const char*) in the String double")
    extra = """  bool concat(const String& other) { return concat(other.c_str()); }
  bool reserve(size_t capacity) { text_.reserve(capacity); return true; }
"""
    return stub.replace(anchor, extra + anchor)


def production(server: str, header: str, mutex: str, mutex_header: str, utils: str) -> dict:
    guard = extract_block(mutex_header, "struct FsLockGuard") + ";\n"
    guard += "\n".join(extract_block(mutex, marker) for marker in (
        "FsLockGuard::FsLockGuard(const char* owner)",
        "FsLockGuard::FsLockGuard(const char* owner, TickType_t timeoutTicks)",
        "FsLockGuard::~FsLockGuard()"))
    text_io = "\n\n".join(extract_block(utils, marker) for marker in ("bool writeText(", "bool writeTextAtomic("))

    defines = re.findall(r"^#define (?:MAX_IP_BANS|IP_BANS_FILE)\s+.*$", header, re.M)
    require(len(defines) == 2, "MAX_IP_BANS and IP_BANS_FILE")
    require(header.count("struct IpBanEntry") == 1, "one IpBanEntry")
    types = "\n".join(defines) + "\n" + extract_block(header, "struct IpBanEntry") + ";"

    require(server.count(SECTION_START) == 1, "one ban table definition")
    require(server.count(SECTION_END) == 1, "one banListJson definition")
    start = server.index(SECTION_START)
    end_start = server.index(SECTION_END, start)
    section = server[start:end_start] + extract_block(server[end_start:], SECTION_END)
    return {"// INSERT_FS_GUARD": guard, "// INSERT_TEXT_IO": text_io,
            "// INSERT_TYPES": types, "// INSERT_PRODUCTION": section}


def source_contract(server: str) -> None:
    """Checked writer for the save, and the lock on the httpd-side lookup."""
    save = extract_block(server, "static bool saveIpBans()")
    require("writeTextAtomic(" in save, "saveIpBans must save through writeTextAtomic")
    require('"w"' not in save, "saveIpBans must not truncate ip_bans.json in place")
    lookup = extract_block(server, "bool isIpBanned(const char* ip)")
    require("IpBanLock" in lookup, "isIpBanned must read the table under the ban lock")
    listing = extract_block(server, SECTION_END)
    require("GLOBAL_DEBUG_BUFFER_SIZE" in listing and "measureJson(" in listing,
            "banListJson must size the reply against the debug buffer")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--case", help="run one harness case by name (for per-case evidence)")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    server = (COMPONENT / "WebServer_Server.cpp").read_text()
    harness = (HERE / "ip_ban_store_harness.cpp").read_text()
    replacements = production(server, (COMPONENT / "WebServer_Server.h").read_text(),
                              (COMPONENT / "System_Mutex.cpp").read_text(),
                              (COMPONENT / "System_Mutex.h").read_text(),
                              (COMPONENT / "System_Utils.cpp").read_text())
    for marker, replacement in replacements.items():
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, replacement)
    with tempfile.TemporaryDirectory(prefix="hw1-ip-ban-") as temporary:
        directory = Path(temporary)
        (directory / "Arduino.h").write_text(string_double())
        unit = directory / "ip_ban_store.cpp"
        unit.write_text(harness)
        binary = directory / "ip_ban_store"
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-parameter", "-Wno-unused-function",
                   "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
                   "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0", "-DARDUINOJSON_ENABLE_PROGMEM=0",
                   "-I", str(directory), "-I", str(HERE), "-I", str(COMPONENT), "-I", str(JSON_INCLUDE),
                   str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=180)
        subprocess.run([str(binary)] + ([args.case] if args.case else []), check=True, timeout=60)
    source_contract(server)


if __name__ == "__main__":
    main()
