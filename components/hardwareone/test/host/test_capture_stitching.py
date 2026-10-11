#!/usr/bin/env python3
"""Exercise the real capture stitchers, parser and copy helper with file faults.

File/VFS, identity and filesystem locking are observed boundaries, not a model
of device storage durability or the authorization engine. Uses real Arduino
String and the production quoted-path, normalization and sealed-header readers.
An optional saved --baseline directory verifies normal saved bytes and replies.
The actual replacement helper, merge handlers and plaintext-export command run
against controlled filesystem failures; crypto transformation is a boundary.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_mqtt_boolean_commands import extract_macro
from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
CORE = COMPONENT.parent / "arduino/cores/esp32"


def generate(overlay: Path | None) -> str:
    def read(name: str) -> str:
        path = overlay / name if overlay and (overlay / name).exists() else COMPONENT / name
        return path.read_text()

    filesystem = read("System_Filesystem.cpp")
    helper = (extract_block(filesystem, "bool copyFileContents(")
              if "bool copyFileContents(" in filesystem else "")
    command = read("System_Command.cpp")
    parser = extract_block(read("System_Command.h"), "class CommandArgs") + ";\n"
    parser += "const String CommandArgs::empty_;\n"
    parser += "\n".join(extract_block(command, marker) for marker in (
        "CommandArgs::CommandArgs(", "void CommandArgs::parse(", "const String& CommandArgs::arg("))
    parser += "\n" + "\n".join(extract_block(filesystem, marker) for marker in (
        "bool normalizeFsPath(", "const char* requireQuotedPath(", "const char* requireQuotedToken("))
    scope = read("System_VFS.h")
    scope = "\n".join(extract_block(scope, marker) for marker in (
        "inline const char* captureScopeFor(", "inline bool pathInCaptureTree("))
    crypto = read("System_CaptureCrypto.cpp")
    magic = next(line for line in read("System_CaptureCrypto.h").splitlines()
                 if line.startswith("#define CAPCRYPT_MAGIC_PREFIX "))
    crypto = magic + "\nconstexpr size_t kMagicPfxLen = sizeof(CAPCRYPT_MAGIC_PREFIX) - 1;\n" + extract_block(crypto, "bool captureCryptoLooksSealed(")
    replacement_source = read("System_VFS.cpp")
    staged = "bool replaceFileGuarded(" in replacement_source
    replacement = extract_block(replacement_source, "bool replaceFileGuarded(") if staged else ""
    row_helper = extract_block(filesystem, "bool writeLineChecked(")
    magic_check = extract_block(read("System_CaptureCrypto.cpp"), "bool captureCryptoIsMagicLine(")
    handlers = "\n\n".join(extract_block(read(filename), marker) for filename, marker in (
        ("System_SensorLogging.cpp", "const char* cmd_healthlogmerge("),
        ("System_Maps.cpp", "const char* cmd_gpstrackmerge("),
        ("System_SensorLogging.cpp", "const char* cmd_capturecrypt(")))
    harness = (HERE / "capture_stitching_harness.cpp").read_text()
    for marker, text in {
        "// INSERT_PARSER": parser,
        "// INSERT_REPLACEMENT": replacement,
        "// INSERT_SCOPES": scope,
        "// INSERT_PRODUCTION": extract_macro(read("System_Utils.h"), "RETURN_VALID_IF_VALIDATE_CSTR") + "\n" + crypto + "\n" + magic_check + "\n" + helper + "\n" + row_helper + "\n" + handlers,
    }.items():
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, text)
    return f"#define HAS_COPY_HELPER {int(bool(helper))}\n#define HAS_STAGED {int(staged)}\n" + harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path, help="saved component sources; also runs current")
    parser.add_argument("--evidence", type=Path, help="retain generated sources, logs and transcripts")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-capture-stitching-") as temp:
        work = args.evidence.resolve() if args.evidence else Path(temp)
        work.mkdir(parents=True, exist_ok=True)
        # Only SDK include/log dependencies are replaced in these private copies.
        string = (CORE / "WString.cpp").read_text().replace('#include "Arduino.h"', '#include <cstdio>')
        string = string.replace('#include "esp32-hal-log.h"', '#define log_e(...) ((void)0)\n#define log_w(...) ((void)0)')
        (work / "WString.cpp").write_text(string)
        conversions = (CORE / "stdlib_noniso.c").read_text().replace('#include "esp_system.h"', '')
        conversions += "\nchar *itoa(int v,char*s,int b){return ltoa(v,s,b);}\nchar *utoa(unsigned int v,char*s,int b){return ultoa(v,s,b);}\n"
        (work / "stdlib_noniso.c").write_text(conversions)
        sanitize = (["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                     "-fno-omit-frame-pointer", "-g"] if args.sanitize else [])
        flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                 "-Wno-unused-parameter", "-I", str(CORE), *sanitize]
        subprocess.run([shutil.which("cc"), "-std=c11", *sanitize, "-I", str(CORE),
                        "-c", str(work / "stdlib_noniso.c"), "-o", str(work / "conversions.o")], check=True)
        subprocess.run([args.cxx, *flags, "-Wno-sign-compare", "-c", str(work / "WString.cpp"), "-o", str(work / "WString.o")], check=True)
        transcripts = {}
        versions = [("baseline", args.baseline)] if args.baseline else []
        for label, source in [*versions, ("current", None)]:
            unit, binary = work / f"{label}.cpp", work / label
            unit.write_text(generate(source))
            subprocess.run([args.cxx, *flags, str(unit), str(work / "WString.o"),
                            str(work / "conversions.o"), "-o", str(binary)], check=True, timeout=120)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            (work / f"{label}-transcript.txt").write_text(result.stdout)
            (work / f"{label}-assertions.log").write_text(result.stderr)
            require(result.returncode == 0, f"{label}: {result.stderr}")
            transcripts[label] = result.stdout
            print(f"{label}: {result.stderr.strip()}")
        if args.baseline:
            require(transcripts["baseline"] == transcripts["current"], "unchanged baseline/current transcripts differ")
            print("Normal capture bytes/replies match baseline; failure preservation cases pass separately.")


if __name__ == "__main__":
    main()
