#!/usr/bin/env python3
"""Compile real XIAO profiles for optional display/input and controller conflicts."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[2]
REPO = SOURCE.parents[1]
COMPILER = shutil.which("c++")


class XiaoPeripheralConfig(unittest.TestCase):
    def compile_config(self, features=None, checks=(), error=None, camera_port=1,
                       hook="FEATURE"):
        self.assertIsNotNone(COMPILER, "A host C++ compiler is required")
        for board_macro in ("ARDUINO_XIAO_ESP32S3_DEV",
                            "ARDUINO_XIAO_ESP32S3_SENSE_DEV"):
            with self.subTest(board=board_macro), tempfile.TemporaryDirectory() as temp:
                directory = Path(temp)
                (directory / "sdkconfig.h").write_text(
                    '#define CONFIG_IDF_TARGET "esp32s3"\n'
                    '#define CONFIG_IDF_TARGET_ESP32S3 1\n'
                    '#define CONFIG_BT_ENABLED 1\n'
                    '#define CONFIG_BT_BLUEDROID_ENABLED 1\n'
                    f'#define CONFIG_SCCB_HARDWARE_I2C_PORT{camera_port} 1\n')
                command = [COMPILER, "-x", "c++", "-std=c++17", "-fsyntax-only",
                           "-I", str(SOURCE), "-I", str(directory),
                           f"-D{board_macro}=1",
                           f'-DHW1_BOARD_CONFIG_HEADER="{REPO / "boards/xiao_s3.features.h"}"']
                if features is not None:
                    override = directory / "features.h"
                    override.write_text("\n".join(
                        f"#undef {key}\n#define {key} {value}"
                        for key, value in features.items()) + "\n")
                    command.append(f'-DHW1_{hook}_CONFIG_HEADER="{override}"')
                source = '#include "System_BuildConfig.h"\n'
                source += "\n".join(f'static_assert({expr}, "{expr}");' for expr in checks)
                result = subprocess.run(command + ["-"], input=source,
                                        text=True, capture_output=True)
                if error:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(error, result.stderr)
                else:
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_ordinary_profile_preserves_existing_defaults(self):
        self.compile_config(checks=[
            "ENABLE_MQTT == 0", "I2C_FEATURE_LEVEL == 0", "DISPLAY_TYPE == 0",
            "INPUT_DEVICE_TYPE == 0", "ENABLE_I2C_SYSTEM == 0",
            "ENABLE_OLED_DISPLAY == 0", "ENABLE_OLED_INPUT == 0",
            "ENABLE_CAMERA_SENSOR == 1", "ENABLE_MICROPHONE_SENSOR == 1",
            "ENABLE_SD_CARD == 1", "ENABLE_MAPS == 1",
            "CAMERA_SCCB_I2C_PORT == 1"])

    def test_display_and_i2c_inputs_are_independent(self):
        for display in (0, 1):
            for input_device in (0, 1, 2):
                with self.subTest(display=display, input=input_device):
                    self.compile_config({
                        "ENABLE_CAMERA_SENSOR": 0,
                        "I2C_FEATURE_LEVEL": 2 if input_device == 1 else 1,
                        "DISPLAY_TYPE": display,
                        "INPUT_DEVICE_TYPE": input_device,
                    }, checks=[
                        f"ENABLE_OLED_DISPLAY == {display}",
                        f"ENABLE_GAMEPAD_SENSOR == {int(input_device == 1)}",
                        f"ENABLE_ANO_ENCODER == {int(input_device == 2)}",
                        f"ENABLE_OLED_INPUT == {int(input_device != 0)}",
                        "ENABLE_I2C_SYSTEM == 1", "ENABLE_GPIO_ENCODER == 0",
                        "I2C_SDA_PIN_DEFAULT == 5", "I2C_SCL_PIN_DEFAULT == 6",
                        "OLED_BUS_DEFAULT == 0", "I2C2_BUS_ENABLED_DEFAULT == 0"])

    def test_saved_display_example_and_unrelated_pins(self):
        profile = json.loads((REPO / "configurations/xiao-display.json").read_text())
        self.assertEqual(profile["board"], "xiao_s3")
        self.compile_config(profile["features"], checks=[
            "ENABLE_OLED_DISPLAY == 1", "ENABLE_OLED_INPUT == 0",
            "ENABLE_CAMERA_SENSOR == 0", "ENABLE_MAPS == 0",
            "ENABLE_LLM_SOURCE_ONBOARD == 0", "ENABLE_LLM_SOURCE_CM5 == 1",
            "ENABLE_MICROPHONE_SENSOR == 1", "MIC_CLK_PIN == 42", "MIC_DATA_PIN == 41",
            "SD_CS_PIN == 21", "SD_SCK_PIN == 7", "SD_MISO_PIN == 8", "SD_MOSI_PIN == 9",
            "UART_LINK_TX_PIN == 43", "UART_LINK_RX_PIN == 44"])

    def test_camera_and_external_i2c_need_distinct_controllers(self):
        features = {"I2C_FEATURE_LEVEL": 1, "DISPLAY_TYPE": 1}
        self.compile_config(features, error="reserved by camera SCCB")
        self.compile_config(features, camera_port=0, checks=[
            "ENABLE_CAMERA_SENSOR == 1", "ENABLE_OLED_DISPLAY == 1",
            "CAMERA_SCCB_I2C_PORT == 0"])

    def test_saved_display_requires_i2c_and_oled_selection(self):
        for level, custom in ((0, 1), (4, 0)):
            with self.subTest(level=level):
                self.compile_config({"ENABLE_CAMERA_SENSOR": 0,
                                     "DISPLAY_TYPE": 1, "I2C_FEATURE_LEVEL": level,
                                     "CUSTOM_ENABLE_OLED": custom},
                                    error="SSD1306 requires I2C and the OLED selection")

    def test_saved_i2c_inputs_require_their_backend(self):
        for input_device, level, error in (
                (1, 0, "Seesaw gamepad input requires I2C"),
                (1, 1, "Seesaw gamepad input requires I2C"),
                (2, 0, "ANO encoder input requires I2C")):
            with self.subTest(input=input_device, level=level):
                self.compile_config({"ENABLE_CAMERA_SENSOR": 0,
                                     "INPUT_DEVICE_TYPE": input_device,
                                     "I2C_FEATURE_LEVEL": level}, error=error)

    def test_unintegrated_display_backends_are_not_advertised(self):
        for display in (2, 3):
            with self.subTest(display=display):
                self.compile_config({"DISPLAY_TYPE": display},
                                    error="Generic color TFT displays need an integrated UI backend")
        self.compile_config({"DISPLAY_TYPE": 4},
                            error="P4X-EYE display requires its explicit board profile")

    def test_native_encoder_needs_explicit_assembly_pins(self):
        self.compile_config({"INPUT_DEVICE_TYPE": 3},
                            error="GPIO encoder requires GPIO_ENCODER_PIN_A")

    def test_deployment_can_override_peripheral_defaults(self):
        self.compile_config({"ENABLE_CAMERA_SENSOR": 0, "I2C_FEATURE_LEVEL": 1,
                             "DISPLAY_TYPE": 1, "INPUT_DEVICE_TYPE": 2},
                            checks=["ENABLE_OLED_DISPLAY == 1", "ENABLE_ANO_ENCODER == 1"],
                            hook="DEPLOYMENT")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default=COMPILER)
    args, remaining = parser.parse_known_args()
    COMPILER = args.cxx
    unittest.main(argv=[__file__, *remaining])
