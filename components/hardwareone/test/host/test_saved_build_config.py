#!/usr/bin/env python3
"""Exercise saved configurations against the real configuration compiler path."""

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO / "tools"))
import build_config
import gen_build_info


class SavedConfiguration(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.patch = patch.object(build_config, "REPO", self.root)
        self.patch.start()
        self.addCleanup(self.patch.stop)
        for source in ("boards/xiao_s3.defaults", "boards/feathers3.defaults",
                       "presets/sdkconfig.defaults",
                       "components/hardwareone/System_BuildConfig.h"):
            destination = self.root / source
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(REPO / source, destination)
        (self.root / "configurations").mkdir()

    def save(self, name, features, board="xiao_s3"):
        config = {"board": board, "features": features}
        build_config.config_path(name).write_text(build_config.encode(config))
        return config

    def configure(self, name, bluetooth=True, header_root=None):
        """Supply host compile metadata with the actual build's overlay order.

        header_root names the checkout path the compiler was given (a symlink
        to the same checkout, as a logical `pwd` would produce)."""
        build_config.prepare(name, "xiao_s3")
        directory = build_config.build_dir(name)
        header_dir = (header_root / directory.relative_to(self.root)) if header_root else directory
        sdk = directory / "sdkconfig.h"
        sdk.write_text('#define CONFIG_IDF_TARGET "esp32s3"\n#define CONFIG_IDF_TARGET_ESP32S3 1\n' +
                       ("#define CONFIG_BT_ENABLED 1\n"
                        "#define CONFIG_BT_BLUEDROID_ENABLED 1\n" if bluetooth else ""))
        (directory / "sdkconfig").write_text("CONFIG_BT_ENABLED=y\n" if bluetooth
                                             else "# CONFIG_BT_ENABLED is not set\n")
        source = REPO / "components/hardwareone/main.cpp"
        argv = [shutil.which("c++"), "-DARDUINO_XIAO_ESP32S3_DEV=1", "-I", str(directory),
                '-DHW1_BOARD_CONFIG_HEADER="' + str(REPO / "boards/xiao_s3.features.h") + '"',
                '-DHW1_FEATURE_CONFIG_HEADER="' + str(header_dir / "features.h") + '"',
                "-c", str(source), "-o", "unused.o"]
        (directory / "compile_commands.json").write_text(json.dumps([
            {"file": str(source), "directory": str(directory), "command": shlex.join(argv)}]))
        return directory

    def test_two_builds_keep_choices_and_inputs_independent(self):
        self.save("default", {"ENABLE_MQTT": 0})
        self.save("mqtt", {"ENABLE_MQTT": 1, "CUSTOM_ENABLE_NET_WIFI": 1,
                           "NETWORK_FEATURE_LEVEL": 4, "CUSTOM_ENABLE_WEB_MQTT": 1})
        baseline = self.configure("default")
        before = (baseline / "features.h").read_bytes()
        other = self.configure("mqtt")
        self.assertNotEqual(baseline, other)
        self.assertEqual(build_config.check("default")["value"]["ENABLE_MQTT"], "0")
        result = build_config.check("mqtt")["value"]
        self.assertEqual(result["ENABLE_MQTT"], "1")
        self.assertEqual(result["ENABLE_WEB_MQTT"], "1")
        self.assertEqual((baseline / "features.h").read_bytes(), before)

    def test_missing_wifi_explains_dependency(self):
        self.save("bad", {"ENABLE_MQTT": 1, "NETWORK_FEATURE_LEVEL": 0})
        self.configure("bad")
        with self.assertRaisesRegex(ValueError, "ENABLE_MQTT requires WiFi"):
            build_config.check("bad")

    def test_missing_sdk_dependency_is_not_silently_accepted(self):
        self.save("bluetooth", {"ENABLE_BLUETOOTH": 1})
        self.configure("bluetooth", bluetooth=False)
        with self.assertRaisesRegex(ValueError, "ENABLE_BLUETOOTH: selected 1, compiler resolved 0"):
            build_config.check("bluetooth")

    def test_saved_display_can_override_board_default(self):
        self.save("display", {"DISPLAY_TYPE": 1, "I2C_FEATURE_LEVEL": 1,
                              "ENABLE_CAMERA_SENSOR": 0})
        self.configure("display")
        self.assertEqual(build_config.check("display")["value"]["ENABLE_OLED_DISPLAY"], "1")

    def test_reject_unknown_or_derived_flags_and_bad_values(self):
        for features in ({"ENABLE_MQT": 1}, {"ENABLE_WIFI": 1},
                         {"ENABLE_MQTT": True}, {"ENABLE_MQTT": 2},
                         {"NETWORK_FEATURE_LEVEL": 5}, {"DISPLAY_TYPE": -1}):
            with self.subTest(features=features), self.assertRaises(ValueError):
                build_config.validate({"board": "xiao_s3", "features": features})

    def test_name_and_board_guard_build_directory(self):
        with self.assertRaises(ValueError):
            build_config.build_dir("../elsewhere")
        self.save("same", {})
        build_config.prepare("same", "xiao_s3")
        with self.assertRaisesRegex(ValueError, "selects xiao_s3"):
            build_config.prepare("same", "feathers3")
        self.save("same", {}, "feathers3")
        with self.assertRaisesRegex(ValueError, "changed board"):
            build_config.prepare("same", "feathers3")

    def test_unchanged_prepare_preserves_manifest_freshness(self):
        self.save("same", {"ENABLE_MQTT": 0})
        directory = self.configure("same")
        files = [directory / "features.h", directory / "configuration.json",
                 directory / "sdkconfig.features"]
        times = [path.stat().st_mtime_ns for path in files]
        build_config.prepare("same", "xiao_s3")
        self.assertEqual(times, [path.stat().st_mtime_ns for path in files])

    def test_changed_saved_file_requires_prepare(self):
        self.save("same", {"ENABLE_MQTT": 0})
        self.configure("same")
        self.save("same", {"ENABLE_MQTT": 1})
        with self.assertRaisesRegex(ValueError, "configuration changed"):
            build_config.check("same")

    def test_manifest_refuses_overlay_newer_than_image(self):
        self.save("same", {"ENABLE_MQTT": 0})
        directory = self.configure("same")
        artifact = directory / "image.bin"
        artifact.write_bytes(b"test")
        future = artifact.stat().st_mtime + 10
        os.utime(directory / "features.h", (future, future))
        result, error = gen_build_info.resolve_macros(str(directory), str(artifact))
        self.assertIsNone(result)
        self.assertIn("STALE", error)
        self.assertIn("features.h", error)

    def test_manifest_refuses_sdk_overlay_newer_than_image(self):
        self.save("same", {})
        directory = self.configure("same")
        artifact = directory / "image.bin"
        artifact.write_bytes(b"test")
        future = artifact.stat().st_mtime + 10
        os.utime(directory / "sdkconfig.features", (future, future))
        result, error = gen_build_info.resolve_macros(str(directory), str(artifact))
        self.assertIsNone(result)
        self.assertIn("STALE", error)
        self.assertIn("sdkconfig.features", error)

    def test_symlinked_checkout_path_is_the_same_configuration(self):
        # build_board.sh passes the logical `pwd`; REPO is resolved. Both name
        # the same file, which must verify instead of failing the build.
        alias = self.root / "via-symlink"
        alias.symlink_to(self.root, target_is_directory=True)
        self.save("same", {"ENABLE_MQTT": 0})
        self.configure("same", header_root=alias)
        self.assertEqual(build_config.check("same")["value"]["ENABLE_MQTT"], "0")

    def test_wrong_configuration_header_names_both_paths(self):
        self.save("same", {"ENABLE_MQTT": 0})
        self.save("other", {"ENABLE_MQTT": 0})
        self.configure("other")
        self.configure("same", header_root=None)
        directory = build_config.build_dir("same")
        commands = json.loads((directory / "compile_commands.json").read_text())
        foreign = str(build_config.build_dir("other") / "features.h")
        commands[0]["command"] = commands[0]["command"].replace(str(directory / "features.h"), foreign)
        (directory / "compile_commands.json").write_text(json.dumps(commands))
        with self.assertRaises(ValueError) as raised:
            build_config.check("same")
        message = str(raised.exception)
        self.assertIn("compiler is not using this saved configuration", message)
        self.assertIn(foreign, message)
        self.assertIn(str(directory / "features.h"), message)

    def test_requires_build_local_sdkconfig(self):
        self.save("same", {})
        directory = self.configure("same")
        (directory / "sdkconfig").unlink()
        with self.assertRaisesRegex(ValueError, "build-local sdkconfig is missing"):
            build_config.check("same")

    def test_create_from_copies_choices_without_overwriting_original(self):
        original = self.save("original", {"ENABLE_MQTT": 0, "ENABLE_MAPS": 0})
        with patch.object(sys, "argv", ["build_config.py", "create", "new", "--from", "original",
                                        "--set", "ENABLE_MQTT=1"]):
            self.assertEqual(build_config.main(), 0)
            self.assertEqual(build_config.main(), 1)
        self.assertEqual(build_config.load("original"), original)
        self.assertEqual(build_config.load("new")["features"], {"ENABLE_MQTT": 1, "ENABLE_MAPS": 0})

    def test_sdk_choices_generate_verify_and_remove_independently(self):
        config = self.save("radio", {"ENABLE_BLUETOOTH": 0})
        config["sdkconfig"] = {"CONFIG_BT_ENABLED": False}
        build_config.config_path("radio").write_text(build_config.encode(config))
        directory = self.configure("radio", bluetooth=False)
        self.assertIn("CONFIG_BT_ENABLED=n", (directory / "sdkconfig.features").read_text())
        self.assertEqual(build_config.check("radio")["value"]["ENABLE_BLUETOOTH"], "0")
        (directory / "sdkconfig").write_text("CONFIG_BT_ENABLED=y\n")
        with self.assertRaisesRegex(ValueError, "CONFIG_BT_ENABLED: selected off, Kconfig resolved on"):
            build_config.check("radio")
        self.save("radio", {})
        build_config.prepare("radio", "xiao_s3")
        self.assertNotIn("CONFIG_BT_ENABLED", (directory / "sdkconfig.features").read_text())

    def test_sdk_schema_protects_hardware_and_security(self):
        for sdk in ({"CONFIG_SECURE_FLASH_ENC_ENABLED": True},
                    {"CONFIG_ESPTOOLPY_FLASHSIZE_16MB": True},
                    {"CONFIG_ARDUINO_VARIANT": "another"},
                    {"CONFIG_BT_ENABLED": "n"}, {"CONFIG_BT_ENABLED": 0}, []):
            with self.subTest(sdk=sdk), self.assertRaises(ValueError):
                build_config.validate({"board": "xiao_s3", "features": {}, "sdkconfig": sdk})

    def test_cli_sdk_choice_copies_with_configuration(self):
        with patch.object(sys, "argv", ["build_config.py", "create", "radio", "--board", "xiao_s3",
                                        "--sdk", "CONFIG_BT_ENABLED=n"]):
            self.assertEqual(build_config.main(), 0)
        with patch.object(sys, "argv", ["build_config.py", "create", "copy", "--from", "radio"]):
            self.assertEqual(build_config.main(), 0)
        self.assertEqual(build_config.load("copy")["sdkconfig"], {"CONFIG_BT_ENABLED": False})

    def test_wrapper_rejects_actions_that_remove_generated_inputs(self):
        for action in ("fullclean", "set-target"):
            with self.subTest(action=action):
                result = subprocess.run(["bash", str(REPO / "tools/build_board.sh"),
                                         "xiao_s3", "--config", "xiao-mqtt", action],
                                        capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("use clean or reconfigure", result.stderr)

    def test_wrapper_rejects_overrides_that_bypass_verified_build(self):
        for option in ("-DHW1_FEATURE_CONFIG_FILE=", "-DSDKCONFIG=elsewhere", "-Belsewhere",
                       "-C", "--build-dir=elsewhere", "--project-dir", "--define-cache-entry"):
            with self.subTest(option=option):
                result = subprocess.run(["bash", str(REPO / "tools/build_board.sh"),
                                         "xiao_s3", "--config", "xiao-mqtt", option, "build"],
                                        capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("manage CMake inputs and build paths", result.stderr)


if __name__ == "__main__":
    unittest.main()
