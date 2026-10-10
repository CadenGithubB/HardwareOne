#!/usr/bin/env python3
"""Compile the real XIAO configuration to check MQTT defaults and overrides."""
import argparse
import pathlib
import shutil
import subprocess
import tempfile
import unittest

SOURCE = pathlib.Path(__file__).resolve().parents[2]
BOARD_PROFILE = SOURCE.parents[1] / "boards" / "xiao_s3.features.h"
COMPILER = shutil.which("c++")


class MqttFeatureConfig(unittest.TestCase):
    def check_config(self, *, feature=None, deployment=None, mqtt=0, wifi=1,
                     error=None):
        self.assertIsNotNone(COMPILER, "A host C++ compiler is required")
        for board_macro in ("ARDUINO_XIAO_ESP32S3_DEV",
                            "ARDUINO_XIAO_ESP32S3_SENSE_DEV"):
            with self.subTest(board=board_macro), tempfile.TemporaryDirectory() as temp:
                temp_path = pathlib.Path(temp)
                (temp_path / "sdkconfig.h").write_text(
                    '#define CONFIG_IDF_TARGET "esp32s3"\n'
                    '#define CONFIG_IDF_TARGET_ESP32S3 1\n'
                    '#define CONFIG_BT_ENABLED 1\n'
                    '#define CONFIG_BT_BLUEDROID_ENABLED 1\n')
                command = [COMPILER, "-x", "c++", "-std=c++17", "-fsyntax-only",
                           "-I", str(SOURCE), "-I", str(temp_path),
                           f"-D{board_macro}=1",
                           f'-DHW1_BOARD_CONFIG_HEADER="{BOARD_PROFILE}"']
                for kind, values in (("FEATURE", feature), ("DEPLOYMENT", deployment)):
                    if values is None:
                        continue
                    override = temp_path / f"{kind.lower()}.h"
                    override.write_text("\n".join(
                        f"#undef {key}\n#define {key} {value}"
                        for key, value in values.items()) + "\n")
                    command.append(f'-DHW1_{kind}_CONFIG_HEADER="{override}"')
                source = ('#include "System_BuildConfig.h"\n'
                          f'static_assert(ENABLE_MQTT == {mqtt}, "MQTT selection");\n'
                          f'static_assert(ENABLE_WIFI == {wifi}, "WiFi derivation");\n')
                result = subprocess.run(command + ["-"], input=source,
                                        text=True, capture_output=True)
                if error:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(error, result.stderr)
                else:
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_ordinary_xiao_defaults_to_mqtt_off(self):
        self.check_config()

    def test_saved_configuration_can_enable_mqtt(self):
        self.check_config(feature={"ENABLE_MQTT": 1}, mqtt=1)

    def test_saved_mqtt_configuration_requires_wifi(self):
        self.check_config(feature={"ENABLE_MQTT": 1, "NETWORK_FEATURE_LEVEL": 0},
                          wifi=0, error="ENABLE_MQTT requires WiFi")

    def test_deployment_can_override_xiao_mqtt_default(self):
        self.check_config(deployment={"ENABLE_MQTT": 1}, mqtt=1)

    def test_legacy_wifi_rule_still_disables_mqtt(self):
        self.check_config(deployment={"ENABLE_MQTT": 1, "NETWORK_FEATURE_LEVEL": 0},
                          wifi=0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default=COMPILER)
    args, remaining = parser.parse_known_args()
    COMPILER = args.cxx
    unittest.main(argv=[__file__, *remaining])
