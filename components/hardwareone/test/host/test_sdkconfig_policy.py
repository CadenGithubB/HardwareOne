#!/usr/bin/env python3
"""Check SDK overlay ordering and incremental recovery using the real CMake helper."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[4]


@unittest.skipUnless(shutil.which("cmake"), "cmake is required")
class SdkconfigPolicy(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.sdk = self.root / "sdkconfig"
        self.sdk.write_text("CONFIG_UNRELATED_TUNING=73\n")
        self.named = self.root / "sdkconfig.features"
        self.feature = self.root / "features.h"
        self.feature.write_text("#pragma once\n")

    def run_helper(self, board="xiao_s3", named="", feature=None, deployment="", hardware=None):
        script = self.root / "check.cmake"
        if hardware is None:
            hardware = REPO / "boards" / (board + ".defaults")
        if feature is None:
            feature = self.feature
        layers = self.root / "layers.txt"
        script.write_text(f'''cmake_minimum_required(VERSION 3.16)
include("{REPO / 'cmake/HW1Sdkconfig.cmake'}")
hw1_sdkconfig_policy_files("{REPO}" "{board}" "{feature}" "{named}" "{deployment}" policy)
file(WRITE "{layers}" "${{policy}}")
hw1_reconcile_sdkconfig("{self.sdk}" "{hardware}" ${{policy}})
''')
        result = subprocess.run([shutil.which("cmake"), "-P", str(script)],
                                capture_output=True, text=True)
        self.output = result.stdout + result.stderr
        self.layers = layers.read_text().split(";") if layers.exists() else []
        return result

    def require_success(self, **kwargs):
        result = self.run_helper(**kwargs)
        self.assertEqual(result.returncode, 0, self.output)
        self.assertIn("CONFIG_UNRELATED_TUNING=73", self.sdk.read_text())

    def test_named_bluetooth_overrides_qtpy_preset_and_removal_restores_it(self):
        self.sdk.write_text("CONFIG_UNRELATED_TUNING=73\nCONFIG_BT_ENABLED=y\n")
        self.named.write_text("CONFIG_BT_ENABLED=y\n")
        self.require_success(board="qtpy_esp32", named=self.named)
        self.assertEqual(self.layers, [str(REPO / "presets/sdkconfig.defaults"),
                                      str(REPO / "presets/qtpy_esp32.sdkconfig.defaults"),
                                      str(self.named)])
        self.assertIn("CONFIG_BT_ENABLED=y", self.sdk.read_text())
        self.require_success(board="qtpy_esp32")
        self.assertNotIn("CONFIG_BT_ENABLED", self.sdk.read_text())
        # Kconfig now receives the inherited =n preset for this unset key.
        self.assertEqual(self.layers[-1], str(REPO / "presets/qtpy_esp32.sdkconfig.defaults"))

    def test_bluetooth_off_to_on_and_back_clears_previous_kconfig_value(self):
        for previous, selected in (("# CONFIG_BT_ENABLED is not set", "y"),
                                   ("CONFIG_BT_ENABLED=y", "n")):
            with self.subTest(selected=selected):
                self.sdk.write_text("CONFIG_UNRELATED_TUNING=73\n" + previous + "\n")
                self.named.write_text(f"CONFIG_BT_ENABLED={selected}\n")
                self.require_success(named=self.named)
                self.assertNotIn("CONFIG_BT_ENABLED", self.sdk.read_text())

    def test_disabled_equivalence_and_absent_conditional_keys_keep_mtime(self):
        self.sdk.write_text("CONFIG_UNRELATED_TUNING=73\n# CONFIG_BT_ENABLED is not set\n")
        before = self.sdk.stat().st_mtime_ns
        self.require_success(board="qtpy_esp32")
        self.assertEqual(self.sdk.stat().st_mtime_ns, before)

    def test_removing_xiao_override_restores_shared_enabled_preset(self):
        self.sdk.write_text("CONFIG_UNRELATED_TUNING=73\n# CONFIG_BT_ENABLED is not set\n")
        self.require_success()
        self.assertNotIn("CONFIG_BT_ENABLED", self.sdk.read_text())
        self.assertEqual(self.layers, [str(REPO / "presets/sdkconfig.defaults")])

    def test_deployment_policy_wins_and_cannot_mix_with_named_choices(self):
        deployment = self.root / "deployment.defaults"
        deployment.write_text("CONFIG_BT_ENABLED=y\n")
        self.sdk.write_text("CONFIG_UNRELATED_TUNING=73\nCONFIG_BT_ENABLED=y\n")
        self.require_success(board="qtpy_esp32", deployment=deployment)
        self.assertIn("CONFIG_BT_ENABLED=y", self.sdk.read_text())
        self.assertEqual(self.layers[-1], str(deployment))
        self.named.write_text("CONFIG_BT_ENABLED=n\n")
        self.assertNotEqual(self.run_helper(named=self.named, deployment=deployment).returncode, 0)
        self.assertIn("cannot be combined", self.output)

    def test_board_switch_clears_choice_siblings_and_stale_security(self):
        self.sdk.write_text('''CONFIG_UNRELATED_TUNING=73
CONFIG_SPIRAM_MODE_QUAD=y
# CONFIG_SPIRAM_MODE_OCT is not set
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_SECURE_FLASH_ENC_ENABLED=y
CONFIG_NVS_ENCRYPTION=y
CONFIG_PARTITION_TABLE_OFFSET=0x9000
''')
        self.require_success()
        result = self.sdk.read_text()
        for key in ("CONFIG_SPIRAM_MODE", "CONFIG_ESPTOOLPY_FLASHSIZE",
                    "CONFIG_SECURE_FLASH_ENC_ENABLED", "CONFIG_NVS_ENCRYPTION",
                    "CONFIG_PARTITION_TABLE_OFFSET"):
            self.assertNotIn(key, result)

    def test_matching_hardware_and_security_values_are_unchanged(self):
        contents = ("CONFIG_UNRELATED_TUNING=73\n" +
                    (REPO / "boards/xiao_s3.defaults").read_text() +
                    "CONFIG_BT_ENABLED=y\n")
        self.sdk.write_text(contents)
        before = self.sdk.stat().st_mtime_ns
        self.require_success()
        self.assertEqual(self.sdk.read_text(), contents)
        self.assertEqual(self.sdk.stat().st_mtime_ns, before)

    def test_setting_values_are_matched_literally(self):
        hardware = self.root / "hardware.defaults"
        hardware.write_text('CONFIG_ARDUINO_VARIANT="board.x"\n')
        self.sdk.write_text('CONFIG_UNRELATED_TUNING=73\nCONFIG_ARDUINO_VARIANT="boardXx"\n')
        self.require_success(hardware=hardware)
        self.assertNotIn("CONFIG_ARDUINO_VARIANT", self.sdk.read_text())

    def test_named_choices_cannot_change_security_geometry_or_backend(self):
        for line in ("CONFIG_SECURE_FLASH_ENC_ENABLED=y", "CONFIG_NVS_ENCRYPTION=y",
                     "CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y", "CONFIG_BT_BLUEDROID_ENABLED=n",
                     "CONFIG_BT_ENABLED=1", "# CONFIG_NVS_ENCRYPTION is not set"):
            with self.subTest(line=line):
                self.named.write_text(line + "\n")
                self.assertNotEqual(self.run_helper(named=self.named).returncode, 0, self.output)

    def test_named_sdk_requires_feature_companion_and_existing_file(self):
        self.named.write_text("CONFIG_BT_ENABLED=n\n")
        self.assertNotEqual(self.run_helper(named=self.named, feature="").returncode, 0)
        self.assertIn("requires HW1_FEATURE_CONFIG_FILE", self.output)
        self.named.unlink()
        self.assertNotEqual(self.run_helper(named=self.named).returncode, 0)
        self.assertIn("missing or not a file", self.output)


if __name__ == "__main__":
    unittest.main()
