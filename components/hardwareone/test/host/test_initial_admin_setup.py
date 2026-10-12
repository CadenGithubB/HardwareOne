#!/usr/bin/env python3
"""Exercise production initial-admin publication and both onboarding callers.

Uses vendored ArduinoJson and the intact account helper/restore flow. The normal
caller's save-through-success tail runs unchanged inside a bounded restart loop;
earlier choice collection and unrelated restore/import UI are outside this suite.
Hashing, String storage, file owners, UI delivery and reboot are fault boundaries.
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


def string_boundary() -> str:
    source = (HERE / "web_batch_stubs/Arduino.h").read_text()
    source = source.replace("class String {", """inline bool setupTestReserveFails = false;
inline int setupTestAppendsBeforeFailure = -1;
class String {""")
    source = source.replace("  String() = default;", """  String() = default;
  bool reserve(size_t capacity) {
    if (setupTestReserveFails) return false;
    text_.reserve(capacity); return true;
  }
  bool isEmpty() const { return text_.empty(); }""")
    source = source.replace("    webBatchTestStringAppend();", """    webBatchTestStringAppend();
    if (setupTestAppendsBeforeFailure == 0) return false;
    if (setupTestAppendsBeforeFailure > 0) --setupTestAppendsBeforeFailure;""")
    return source


def generate() -> str:
    source = (COMPONENT / "System_FirstTimeSetup.cpp").read_text()
    header = (COMPONENT / "System_FirstTimeSetup.h").read_text()
    helper_start = source.index("static bool createInitialAdminUser(")
    helper_end = source.index("void firstTimeSetupIfNeeded()", helper_start)
    owner_and_restore = source[helper_start:helper_end]
    require(owner_and_restore.count("static bool createInitialAdminUser(") == 1,
            "expected one initial-admin owner")
    normal = extract_block(source, "void firstTimeSetupIfNeeded()")
    require(normal.count("createInitialAdminUser(") == 1, "normal setup must reuse the owner")
    require("hashUserPassword(" not in normal, "normal setup duplicates credential hashing")
    wizard = extract_block(normal, "if (advancedSetup)")
    require("continue;" in extract_block(wizard, "if (!wizardResult.applied)"),
            "advanced setup must return to its menu unless the wizard applied its draft")
    tail_start = normal.index("  // Saving configuration stage")
    tail_end = normal.index("  } // end while(true) restart loop", tail_start)
    tail = normal[tail_start:tail_end]
    require(tail.count("createInitialAdminUser(") == 1, "normal commit tail must call shared owner")
    require("PSRAM_JSON_DOC(doc)" not in tail, "duplicate initial-admin JSON remains")
    types = "\n".join(extract_block(header, marker) + ";" for marker in
                       ("enum FirstTimeSetupState", "enum SetupProgressStage", "enum SetupMode "))
    harness = (HERE / "initial_admin_setup_harness.cpp").read_text()
    for marker, replacement in (("// INSERT_TYPES", types),
                                ("// INSERT_OWNER_AND_RESTORE", owner_and_restore),
                                ("// INSERT_NORMAL_COMMIT_TAIL", tail)):
        require(harness.count(marker) == 1, f"expected exactly one marker {marker}")
        harness = harness.replace(marker, replacement)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-initial-admin-") as temporary:
        directory = Path(temporary)
        (directory / "Arduino.h").write_text(string_boundary())
        unit = directory / "test.cpp"
        unit.write_text(generate())
        for migration in (0, 1):
            for oled in (0, 1):
                binary = directory / f"test-{migration}-{oled}"
                command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                           "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-unused-variable",
                           "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1",
                           "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0", "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0",
                           "-DARDUINOJSON_ENABLE_PROGMEM=0", f"-DENABLE_MIGRATION_TOOL={migration}",
                           f"-DENABLE_OLED_DISPLAY={oled}", f"-DENABLE_OLED_INPUT={oled}",
                           "-DENABLE_AUTOMATION=1", "-I", str(directory), "-I", str(JSON_INCLUDE),
                           str(unit), "-o", str(binary)]
                if args.sanitize:
                    command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                                    "-fno-omit-frame-pointer", "-g"]
                subprocess.run(command, check=True, timeout=120)
                subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
