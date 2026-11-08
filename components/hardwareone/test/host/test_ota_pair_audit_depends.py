#!/usr/bin/env python3
"""The recovery-OTA pair audit builds every file it reads.

tools/ota/check_ota_builds.py reads partition_table/partition-table.bin and
ota_data_initial.bin from the main build. The hw1-ota-pair-audit target in the
root CMakeLists.txt used to depend only on gen_project_binary and the updater
image, so in a fresh build directory the first `idf.py ota0-flash` failed with
FileNotFoundError until a full build had produced those two files. This test
parses the root CMakeLists.txt and checks that the audit's DEPENDS names the
ESP-IDF targets that produce them: partition_table_bin and blank_ota_data.
"""

from pathlib import Path
import re
import unittest

REPO = Path(__file__).resolve().parents[4]
ROOT_CMAKE = REPO / "CMakeLists.txt"
AUDIT_SCRIPT = REPO / "tools" / "ota" / "check_ota_builds.py"

# add_custom_target keywords that end a DEPENDS list.
CUSTOM_TARGET_KEYWORDS = {
    "ALL", "COMMAND", "DEPENDS", "BYPRODUCTS", "WORKING_DIRECTORY", "COMMENT",
    "JOB_POOL", "JOB_SERVER_AWARE", "VERBATIM", "USES_TERMINAL",
    "COMMAND_EXPAND_LISTS", "SOURCES",
}


def strip_comments(text):
    """Drop CMake line comments that are not inside a quoted argument."""
    out = []
    for line in text.splitlines():
        in_quote = False
        cut = len(line)
        for i, ch in enumerate(line):
            if ch == '"' and (i == 0 or line[i - 1] != "\\"):
                in_quote = not in_quote
            elif ch == "#" and not in_quote:
                cut = i
                break
        out.append(line[:cut])
    return "\n".join(out)


def call_arguments(text, command, first_arg):
    """Return the argument tokens of `command(first_arg ...)`."""
    match = re.search(r"\b%s\s*\(\s*%s\b" % (re.escape(command),
                                             re.escape(first_arg)), text)
    if match is None:
        raise AssertionError(f"{command}({first_arg} ...) not found")
    start = text.index("(", match.start()) + 1
    depth = 1
    in_quote = False
    i = start
    while depth:
        ch = text[i]
        if ch == '"' and text[i - 1] != "\\":
            in_quote = not in_quote
        elif not in_quote and ch == "(":
            depth += 1
        elif not in_quote and ch == ")":
            depth -= 1
        i += 1
    body = text[start:i - 1]
    return re.findall(r'"(?:[^"\\]|\\.)*"|[^\s"]+', body)


def depends_of(tokens):
    deps = []
    collecting = False
    for token in tokens:
        if token in CUSTOM_TARGET_KEYWORDS:
            collecting = token == "DEPENDS"
            continue
        if collecting:
            deps.append(token.strip('"'))
    return deps


class OtaPairAuditDepends(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cmake = strip_comments(ROOT_CMAKE.read_text(encoding="utf-8"))

    def test_audit_still_reads_the_generated_files(self):
        # Keeps the premise honest: if the audit stops reading these files the
        # DEPENDS below can be revisited.
        audit = AUDIT_SCRIPT.read_text(encoding="utf-8")
        self.assertIn("partition_table/partition-table.bin", audit)
        self.assertIn("ota_data_initial.bin", audit)

    def test_pair_audit_depends_on_partition_table_and_otadata(self):
        deps = depends_of(call_arguments(self.cmake, "add_custom_target",
                                         "hw1-ota-pair-audit"))
        for target in ("gen_project_binary", "partition_table_bin",
                       "blank_ota_data"):
            self.assertIn(target, deps,
                          f"hw1-ota-pair-audit DEPENDS lacks {target}: {deps}")
        self.assertIn("${_ota_updater_bin}", deps)

    def test_missing_producer_targets_fail_configure(self):
        # Both producers are checked with if(NOT TARGET ...) so a layout that
        # lost one fails at configure instead of at the first cable write.
        guard = re.search(
            r"foreach\(\s*_ota_pair_input\s+IN\s+ITEMS\s+"
            r"partition_table_bin\s+blank_ota_data\s*\)(.*?)endforeach\(\)",
            self.cmake, re.S)
        self.assertIsNotNone(guard, "producer-target guard not found")
        self.assertIn("if(NOT TARGET ${_ota_pair_input})", guard.group(1))
        self.assertIn("FATAL_ERROR", guard.group(1))
        self.assertLess(guard.start(),
                        self.cmake.index("add_custom_target(hw1-ota-pair-audit"))

    def test_migration_preflight_keeps_its_dependencies(self):
        deps = depends_of(call_arguments(self.cmake, "add_custom_target",
                                         "hw1-ota-migration-preflight"))
        for target in ("hw1-ota-pair-audit", "bootloader",
                       "partition_table_bin", "blank_ota_data"):
            self.assertIn(target, deps)


if __name__ == "__main__":
    unittest.main()
