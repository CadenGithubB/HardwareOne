#!/usr/bin/env python3
"""Execute G2 reader/viewer and capture reveal definitions from shipping source.

Uses the real PsramBuffer, streaming wrapper, TextPager, ArduinoJson, and
System_MemUtil implementation. Filesystem/auth/render, crypto open/key, and
ESP-IDF heap backends are mocked. The crypto loop/String wrapper are extracted
unchanged; this does not claim hardware AEAD, UI transport, or task-race tests.
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


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    source = (COMPONENT / "G2_Page_Files.cpp").read_text()
    crypto = (COMPONENT / "System_CaptureCrypto.cpp").read_text()
    crypto_header = (COMPONENT / "System_CaptureCrypto.h").read_text()
    reader = extract_block(source, "static bool filesReadTextBuffer(")
    viewer = extract_block(source, "static bool showTextFileViaWidget(")
    require("String raw" not in viewer and "String display" not in viewer,
            "viewer must not restore ordinary input/display String temporaries")
    require("PSRAM_JSON_DOC(doc)" in viewer and "JsonDocument doc;" not in viewer,
            "pretty JSON uses the shared fallback-capable allocator")
    require("deserializeJson(doc, raw.c_str(), raw.size())" in viewer,
            "JSON input remains length-bounded like the original Arduino String reader")
    require(viewer.index("G2HijackCtxGuard ctxGuard") < viewer.index("canRead(") <
            viewer.index("filesReadTextBuffer(") < viewer.index("captureCryptoRevealText("),
            "paired identity and read authorization precede byte access and reveal")
    require(viewer.index("const bool hitReadCap") < viewer.index("captureCryptoRevealText("),
            "read-limit truncation is recorded before decryption shrinks input")
    require("readTextLimited(p, head, sizeof(CAPCRYPT_MAGIC_PREFIX) - 1)" in source,
            "the existing shared String reader remains available to Info sniff")
    require("FsLockGuard guard" in reader and "VFS::open(String(path), \"r\")" in reader,
            "G2 reader preserves policy-free VFS dispatch under the FS lock")
    require("textWrapAppend" not in viewer and "TextWrapStream display" in viewer,
            "chunked serialization must retain wrap state")
    state = source[source.index("#define FILES_TEXT_MAX_PAGES"):
                   source.index("// -----------------------------------------------------------------------------",
                                source.index("#define FILES_TEXT_MAX_PAGES"))]
    definitions = "\n\n".join(extract_block(crypto, marker) for marker in (
        "size_t captureCryptoRevealCapacity(",
        "bool captureCryptoRevealText(char*",
        "size_t captureCryptoRevealText(String&",
    ))
    macros = "\n".join(re.findall(r"^#define CAPCRYPT_.*$", crypto_header, re.MULTILINE))
    harness = (HERE / "g2_files_psram_harness.cpp").read_text()
    for marker, replacement in {
        "// INSERT_PRODUCTION_CRYPTO_MACROS_HERE": macros,
        "// INSERT_PRODUCTION_CRYPTO_REVEAL_HERE": definitions,
        "// INSERT_PRODUCTION_G2_STATE_HERE": state,
        "// INSERT_PRODUCTION_G2_VIEWER_HERE": reader + "\n\n" + viewer,
    }.items():
        require(harness.count(marker) == 1, f"expected one insertion marker: {marker}")
        harness = harness.replace(marker, replacement)
    with tempfile.TemporaryDirectory(prefix="hw1-g2-files-host-") as temp:
        generated = Path(temp) / "g2_files.cpp"
        generated.write_text(harness)
        for psram in (True, False):
            executable = Path(temp) / ("g2_files_psram" if psram else "g2_files_internal")
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-I", str(JSON_INCLUDE), "-I", str(HERE / "memutil_stubs"),
                       "-I", str(COMPONENT), str(generated),
                       str(COMPONENT / "System_MemUtil.cpp"), "-o", str(executable)]
            if psram:
                command.insert(1, "-DBOARD_HAS_PSRAM=1")
            if args.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-g"]
            subprocess.run(command, check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
