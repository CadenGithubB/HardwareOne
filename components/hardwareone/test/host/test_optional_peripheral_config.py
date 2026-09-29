#!/usr/bin/env python3
"""Compile the real configuration for independent display/input/storage choices."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

SOURCE = pathlib.Path(__file__).resolve().parents[2]

class PeripheralConfig(unittest.TestCase):
    def check_profile(self, flags, checks, succeeds=True):
        compiler = shutil.which('c++')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as temp:
            override = pathlib.Path(temp) / 'features.h'
            override.write_text('\n'.join(f'#undef {key}\n#define {key} {value}'
                                           for key, value in flags.items()))
            source = '#include "System_BuildConfig.h"\n'
            source += '\n'.join(f'static_assert({expr}, "{expr}");' for expr in checks)
            result = subprocess.run([compiler, '-x', 'c++', '-std=c++17', '-fsyntax-only',
                                     '-I', str(SOURCE),
                                     '-DCONFIG_IDF_TARGET_ESP32P4=1',
                                     f'-DHW1_DEPLOYMENT_CONFIG_HEADER="{override}"', '-'],
                                    input=source, text=True, capture_output=True)
            if succeeds:
                self.assertEqual(result.returncode, 0, result.stderr)
            else:
                self.assertNotEqual(result.returncode, 0)

    def profile(self, display=0, wheel=0, sd=0):
        return dict(HW_BOARD_P4X_EYE=1, I2C_FEATURE_LEVEL=0,
                    DISPLAY_TYPE=display, INPUT_DEVICE_TYPE=wheel,
                    ENABLE_SDMMC_CARD=sd, ENABLE_CAMERA_SENSOR=0,
                    ENABLE_MICROPHONE_SENSOR=0, ENABLE_LLM_SOURCE_CM5=0,
                    ENABLE_RASPBERRY_PI_HOST_POWER=0, ENABLE_RASPBERRY_PI_HOST_FAN=0)

    def test_all_independent_combinations(self):
        for display in (0, 4):
            for wheel in (0, 3):
                for sd in (0, 1):
                    with self.subTest(display=display, wheel=wheel, sd=sd):
                        self.check_profile(self.profile(display, wheel, sd), [
                            f'ENABLE_OLED_DISPLAY == {int(display != 0)}',
                            f'ENABLE_OLED_INPUT == {int(wheel != 0)}',
                            f'ENABLE_GPIO_ENCODER_BUTTONS == {int(wheel == 3)}',
                            f'ENABLE_SD_CARD == {sd}',
                            'ENABLE_I2C_SYSTEM == 0', 'ENABLE_I2C_SENSOR_QUEUE == 0',
                            'ENABLE_GAMEPAD_SENSOR == 0', 'ENABLE_ANO_ENCODER == 0',
                            'ENABLE_CAMERA_SENSOR == 0', 'SD_MMC_SLOT == 0',
                            'SD_MMC_CLK_PIN == 43', 'SD_MMC_CMD_PIN == 44',
                            'EYE_LCD_SCLK == 17', 'GPIO_ENCODER_PIN_A == 48',
                            'EYE_BUTTON_1_PIN == 3', 'EYE_BUTTON_2_PIN == 4',
                            'EYE_BUTTON_3_PIN == 5'])

    def test_board_buttons_can_be_forced_off(self):
        profile = self.profile(wheel=3)
        profile['ENABLE_GPIO_ENCODER_BUTTONS'] = 0
        self.check_profile(profile, ['ENABLE_GPIO_ENCODER == 1',
                                     'ENABLE_GPIO_ENCODER_BUTTONS == 0'])

    def test_board_buttons_default_off_for_other_inputs(self):
        for wheel in (0, 1, 2):
            with self.subTest(wheel=wheel):
                profile = self.profile(wheel=wheel)
                profile['I2C_FEATURE_LEVEL'] = 1
                self.check_profile(profile, ['ENABLE_GPIO_ENCODER_BUTTONS == 0'])

    def test_board_buttons_require_gpio_encoder(self):
        for wheel in (0, 1, 2):
            with self.subTest(wheel=wheel):
                profile = self.profile(wheel=wheel)
                profile['I2C_FEATURE_LEVEL'] = 1
                profile['ENABLE_GPIO_ENCODER_BUTTONS'] = 1
                self.check_profile(profile, [], succeeds=False)

    def test_board_buttons_require_eye_board(self):
        profile = self.profile(wheel=3)
        profile['HW_BOARD_P4X_EYE'] = 0
        self.check_profile(profile, ['ENABLE_GPIO_ENCODER == 1',
                                     'ENABLE_GPIO_ENCODER_BUTTONS == 0'])
        profile['ENABLE_GPIO_ENCODER_BUTTONS'] = 1
        self.check_profile(profile, [], succeeds=False)

    def test_board_buttons_are_cpp_constant_without_eye_macro(self):
        profile = self.profile()
        del profile['HW_BOARD_P4X_EYE']
        del profile['INPUT_DEVICE_TYPE']
        # check_profile emits a C++ static_assert, not a preprocessor #if:
        # an undefined board macro must not leak into this derived flag.
        self.check_profile(profile, ['ENABLE_GPIO_ENCODER_BUTTONS == 0'])
        profile['INPUT_DEVICE_TYPE'] = 3
        self.check_profile(profile, ['ENABLE_GPIO_ENCODER == 1',
                                     'ENABLE_GPIO_ENCODER_BUTTONS == 0'])

    def test_ano_needs_i2c(self):
        self.check_profile(self.profile(wheel=2), ['ENABLE_OLED_INPUT == 0'])

    def test_generic_p4_is_not_eye(self):
        profile = self.profile(display=4)
        profile['HW_BOARD_P4X_EYE'] = 0
        self.check_profile(profile, [], succeeds=False)

    def test_sd_can_be_forced_off(self):
        profile = self.profile(sd=1)
        profile['ENABLE_SD_CARD'] = 0
        self.check_profile(profile, ['ENABLE_SD_CARD == 0'])

if __name__ == '__main__':
    unittest.main()
