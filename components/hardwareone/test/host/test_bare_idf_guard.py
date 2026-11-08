#!/usr/bin/env python3
"""A bare idf.py configure cannot pair one board's sdkconfig with another board.

Runs the real cmake/HW1BoardGuard.cmake helper with cmake -P (no ESP-IDF) and
checks that the root CMakeLists.txt calls it before anything writes the
sdkconfig. Without the guard, a bare `idf.py reconfigure` in a clean clone
defaulted the board to feathers3 (16 MB table) on top of the tracked XIAO
config/sdkconfig (8 MB) and rewrote that tracked file.
"""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[4]
GUARD = REPO / "cmake" / "HW1BoardGuard.cmake"


@unittest.skipUnless(shutil.which("cmake"), "cmake is required")
class BareIdfGuard(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def run_guard(self, sdkconfig, explicit, board="feathers3"):
        script = self.root / "check.cmake"
        board_file = REPO / "boards" / f"{board}.defaults"
        script.write_text(f'''cmake_minimum_required(VERSION 3.16)
include("{GUARD}")
hw1_check_board_selection("{REPO}" "{sdkconfig}" {explicit} "{board}" "{board_file}")
''')
        result = subprocess.run([shutil.which("cmake"), "-P", str(script)],
                                capture_output=True, text=True)
        return result.returncode, result.stdout + result.stderr

    def test_tracked_sdkconfig_with_defaulted_board_is_refused(self):
        tracked = REPO / "config" / "sdkconfig"
        before = tracked.read_bytes()
        code, out = self.run_guard(tracked, "FALSE")
        self.assertNotEqual(code, 0, out)
        self.assertIn("tools/build_board.sh", out)
        self.assertEqual(tracked.read_bytes(), before)

    def test_sdkconfig_saved_for_another_board_is_refused(self):
        sdk = self.root / "sdkconfig"
        sdk.write_text('CONFIG_ARDUINO_VARIANT="XIAO_ESP32S3"\n'
                       "CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y\n")
        code, out = self.run_guard(sdk, "FALSE", board="feathers3")
        self.assertNotEqual(code, 0, out)
        self.assertIn("tools/build_board.sh", out)

    def test_flash_size_mismatch_alone_is_refused(self):
        sdk = self.root / "sdkconfig"
        sdk.write_text('CONFIG_ARDUINO_VARIANT="um_feathers3"\n'
                       "CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y\n")
        code, out = self.run_guard(sdk, "FALSE", board="feathers3")
        self.assertNotEqual(code, 0, out)

    def test_matching_or_fresh_sdkconfig_is_accepted(self):
        sdk = self.root / "sdkconfig"
        code, out = self.run_guard(sdk, "FALSE")  # not created yet
        self.assertEqual(code, 0, out)
        sdk.write_text('CONFIG_ARDUINO_VARIANT="um_feathers3"\n'
                       "CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y\n")
        code, out = self.run_guard(sdk, "FALSE")
        self.assertEqual(code, 0, out)

    def test_explicit_board_is_not_second_guessed(self):
        # build_board.sh / build_deployment.sh always name the board; the
        # reconcile step then brings the saved sdkconfig in line.
        code, out = self.run_guard(REPO / "config" / "sdkconfig", "TRUE")
        self.assertEqual(code, 0, out)

    def test_root_cmakelists_calls_guard_before_writing_sdkconfig(self):
        text = (REPO / "CMakeLists.txt").read_text()
        call = text.find("hw1_check_board_selection(")
        self.assertNotEqual(call, -1, "root CMakeLists.txt does not call the guard")
        first_write = text.find('file(WRITE "${_sdkconfig}"')
        self.assertNotEqual(first_write, -1)
        self.assertLess(call, first_write)
        self.assertLess(call, text.find("hw1_reconcile_sdkconfig("))
        self.assertLess(call, text.find("include($ENV{IDF_PATH}/tools/cmake/project.cmake)"))


if __name__ == "__main__":
    unittest.main()
