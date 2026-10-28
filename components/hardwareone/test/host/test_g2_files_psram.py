#!/usr/bin/env python3
"""Execute G2 Files page code from shipping source.

g2_files_psram_harness.cpp runs the reader/viewer and capture reveal
definitions with the real PsramBuffer, streaming wrapper, TextPager, shared
JSON re-indenter (System_JsonPretty.h; ArduinoJson is only the layout
reference), and System_MemUtil implementation. Filesystem/auth/render, crypto
open/key, and ESP-IDF heap backends are mocked. The crypto loop/String wrapper
are extracted unchanged; it does not claim hardware AEAD or UI transport.

g2_files_redraw_harness.cpp compiles the rest of the page (includes stripped,
the reader/viewer stubbed) and drives it from simulated tasks: the rename /
delete completion (cmd_exec_task), the info-overlay expiry (main loop) and the
image viewers' onDone (session worker) must only queue a menuGen-fenced lens
redraw, and every FileManager call and lens draw must run under the Files page
lock. Chooser actions must use FileManager's selected-entry path (a 70-byte
SD name is cut to 63 bytes in FileEntry.name) and refuse a path that cannot be
resolved or does not fit; folder rows keep their "[n]" badge whole.
Production sources come from --component (default: this component), so the
suite replays against a pre-fix tree.
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
from test_g2_page_publish import boundary_types, defines, list_publish_helper, paginator, \
    text_entry_types

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
JSON_INCLUDE = COMPONENT.parent / "hardwareone_libs" / "ArduinoJson" / "src"


PAGE_START = "#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES\n"
PAGE_END = "#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES"


def redraw_unit(component: Path) -> str:
    """The Files page minus its reader/viewer, beside g2_files_redraw_harness.cpp."""
    page = (component / "G2_Page_Files.cpp").read_text()
    glasses = (component / "G2_Glasses.h").read_text()
    common = (component / "G2_Page_Common.h").read_text()
    manager = (component / "System_FileManager.h").read_text()
    require(page.count(PAGE_START) == 1 and page.count(PAGE_END) == 1,
            "one ENABLE_BLUETOOTH && ENABLE_G2_GLASSES section in G2_Page_Files.cpp")
    body = page[page.index(PAGE_START) + len(PAGE_START):page.index(PAGE_END)]
    body = re.sub(r"^#include[^\n]*\n", "", body, flags=re.M)
    # The reader/viewer runs in g2_files_psram_harness.cpp; the harness stubs it.
    for marker in ("static bool filesReadTextBuffer(", "static bool showTextFileViaWidget("):
        require(body.count(marker) == 1, f"one definition {marker}")
        body = body.replace(extract_block(body, marker), "")
    read_cap = re.findall(r"^static constexpr size_t kFilesTextReadCapBytes = .*$", body, flags=re.M)
    require(len(read_cap) == 1, "one reader cap (used only by the viewer)")
    body = body.replace(read_cap[0], "")

    tap_fn = re.findall(r"^typedef void \(\*G2TapFn\)\(G2TapKind kind\);$", glasses, flags=re.M)
    require(len(tap_fn) == 1, "G2TapFn typedef")
    columns = re.findall(r"^static constexpr size_t G2_TEXT_DEFAULT_COLS = \d+;$", common, flags=re.M)
    require(len(columns) == 1, "G2_TEXT_DEFAULT_COLS")
    replacements = {
        "// INSERT_BOUNDARY_TYPES": boundary_types(component),
        "// INSERT_G2_TYPES": "\n".join((
            extract_block(glasses, "enum G2TapKind") + ";", tap_fn[0],
            extract_block(glasses, "struct G2TextPageChrome") + ";")),
        "// INSERT_TEXT_ENTRY_TYPES": text_entry_types(component),
        "// INSERT_FILE_MANAGER_TYPES": "\n".join((
            defines(manager, ("FILE_MANAGER_MAX_PATH", "FILE_MANAGER_MAX_NAME")),
            extract_block(manager, "struct FileEntry") + ";")),
        "// INSERT_PAGE_COMMON": "\n\n".join((
            paginator(component), list_publish_helper(component), columns[0])),
        "// INSERT_FILES_PAGE": body,
    }
    harness = (HERE / "g2_files_redraw_harness.cpp").read_text()
    for marker, replacement in replacements.items():
        require(harness.count(marker) == 1, f"expected one insertion marker: {marker}")
        harness = harness.replace(marker, replacement)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="component directory to read production sources from")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component.resolve()
    source = (component / "G2_Page_Files.cpp").read_text()
    crypto = (component / "System_CaptureCrypto.cpp").read_text()
    crypto_header = (component / "System_CaptureCrypto.h").read_text()
    reader = extract_block(source, "static bool filesReadTextBuffer(")
    viewer = extract_block(source, "static bool showTextFileViaWidget(")
    require("String raw" not in viewer and "String display" not in viewer,
            "viewer must not restore ordinary input/display String temporaries")
    require("JsonPrettyFormatter().write(raw.c_str(), raw.size()," in viewer and
            "JsonDocument" not in viewer and "PSRAM_JSON_DOC" not in viewer,
            "pretty JSON goes through the shared re-indenter, length-bounded, with no document")
    require(viewer.index("G2HijackCtxGuard ctxGuard") < viewer.index("canRead(") <
            viewer.index("filesReadTextBuffer(") < viewer.index("captureCryptoRevealText("),
            "paired identity and read authorization precede byte access and reveal")
    require(viewer.index("const bool hitReadCap") < viewer.index("captureCryptoRevealText("),
            "read-limit truncation is recorded before decryption shrinks input")
    require("readTextLimited(p, head, sizeof(CAPCRYPT_MAGIC_PREFIX) - 1, currentAuthContext())" in source,
            "the existing shared String reader remains available to Info sniff")
    require("FsLockGuard guard" in reader and
            "VFS::openGuarded(String(path), \"r\", currentAuthContext())" in reader,
            "G2 reader opens through the guarded VFS as the installed identity, under the FS lock")
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
                       "-I", str(component), str(generated),
                       str(component / "System_MemUtil.cpp"), "-o", str(executable)]
            if psram:
                command.insert(1, "-DBOARD_HAS_PSRAM=1")
            if args.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-g"]
            subprocess.run(command, check=True)
            subprocess.run([str(executable)], check=True)
        redraw = Path(temp) / "g2_files_redraw.cpp"
        redraw.write_text(redraw_unit(component))
        executable = Path(temp) / "g2_files_redraw"
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-I", str(component), str(redraw), "-o", str(executable)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-g"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
