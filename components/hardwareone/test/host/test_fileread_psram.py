#!/usr/bin/env python3
"""Compile the shipping fileread handler/writers with hardware boundary mocks.

Uses the actual bounded PSRAM owner and real vendored ArduinoJson to parse the
wire output. VFS, argument tokenization, BLE and allocation backends are mocks;
MemUtil tests separately exercise the real PSRAM/internal fallback policy.
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
    handler = extract_block(source, "const char* cmd_fileread(")
    require("static PsramBuffer s_readJson(CMD_RESULT_MAX" in handler,
            "fileread must own a persistent response bounded by the command limit")
    require("static String" not in handler and "base64Encode(" not in handler,
            "fileread must not restore String output/base64 temporaries")
    for contract in (
        "RETURN_VALID_IF_VALIDATE_CSTR();", "CommandArgs a(argsInput);",
        "requireQuotedPath(a, 0, path)", "a.has(1) ? a.argInt(1, 0) : 0",
        "a.has(2) ? a.argInt(2, 0) : 0", "for (int i = 3; i < a.count(); i++)",
        'VFS::openGuarded(path, "r", ctx)', 'FsLockGuard _g("fileread")',
        "if (offset > 0) f.seek((uint32_t)offset);", "got = f.read(buf, want);",
        "ctx.transport == SOURCE_BLUETOOTH", "bleScEstablished(connId)",
        "/*blocking=*/true, /*binaryFrame=*/true", "(got == 0) ||",
    ):
        require(contract in handler, f"fileread compatibility boundary changed: {contract}")
    require(handler.index("s_readJson.reserve(OUT_CAP)") < handler.index("bleScSendEncrypted("),
            "reply allocation must finish before raw body delivery")
    require(handler.index('buf, got, eof, "raw", false)') < handler.index("bleScSendEncrypted("),
            "complete checked raw metadata must precede binary body delivery")
    definitions = [extract_block(source, f"static {signature}(") for signature in (
        "bool bytesNeedBase64", "size_t fileReadJsonStringLength",
        "bool fileReadAppendJsonString", "bool fileReadAppendBase64",
        "size_t fileReadUnsignedLength", "bool fileReadAppendUnsigned",
        "size_t fileReadEnvelopeLength", "bool fileReadBuildReply",
    )] + [handler]
    buffer = (COMPONENT / "System_PsramBuffer.h").read_text()
    buffer = buffer.replace('#include "System_MemUtil.h"', "").replace("#pragma once", "")
    limits = (COMPONENT / "System_CommandLimits.h").read_text()
    harness = (HERE / "fileread_psram_harness.cpp").read_text()
    for marker, production in {
        "// INSERT_PRODUCTION_BUFFER_HERE": buffer,
        "// INSERT_PRODUCTION_LIMITS_HERE": limits,
        "// INSERT_PRODUCTION_FILEREAD_HERE": "\n\n".join(definitions),
    }.items():
        require(harness.count(marker) == 1, f"expected exactly one marker: {marker}")
        harness = harness.replace(marker, production)
    with tempfile.TemporaryDirectory(prefix="hw1-fileread-host-") as temp:
        generated = Path(temp) / "fileread.cpp"
        generated.write_text(harness)
        for bluetooth in (0, 1):
            executable = Path(temp) / f"fileread_ble{bluetooth}"
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       f"-DENABLE_BLUETOOTH={bluetooth}", "-I", str(JSON_INCLUDE),
                       str(generated), "-o", str(executable)]
            if args.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-g"]
            subprocess.run(command, check=True)
            for memory in ("psram", "internal-fallback"):
                subprocess.run([str(executable), memory], check=True)


if __name__ == "__main__":
    main()
