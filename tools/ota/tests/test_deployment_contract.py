from __future__ import annotations

import os
import pathlib
import re
import shutil
import subprocess
import unittest

from tools.ota import deployment_contract, make_manifest


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
BUILD_CONFIG = REPOSITORY / "components/hardwareone/System_BuildConfig.h"


def literal_integer_defines(path: pathlib.Path) -> dict[str, list[int]]:
    """Return every literal integer definition without pretending to run C."""
    values: dict[str, list[int]] = {}
    pattern = re.compile(r"^#define\s+([A-Z0-9_]+)\s+([0-9]+)\b")
    for line in path.read_text(encoding="utf-8").splitlines():
        match = pattern.match(line)
        if match:
            values.setdefault(match.group(1), []).append(int(match.group(2)))
    return values


def preprocess_macro_matches(
    feature_header: pathlib.Path,
    board_define: str,
    expected: dict[str, int],
) -> dict[str, str]:
    """Resolve numeric macros with a real C++ preprocessor.

    The deployment profile is included through the same macro used by the
    firmware build. Each result is classified as matching, mismatching, or
    undefined, which avoids the false confidence of regex-reading conditional
    and expression-valued macros from System_BuildConfig.h.
    """
    compiler = next(
        (
            candidate
            for name in (os.environ.get("CXX"), "c++", "clang++", "g++")
            if name and (candidate := shutil.which(name))
        ),
        None,
    )
    if compiler is None:
        raise unittest.SkipTest("no C++ preprocessor is available")

    lines = [f'#include "{BUILD_CONFIG}"']
    for name, value in expected.items():
        lines.extend(
            (
                f"#if !defined({name})",
                f'HW1_MACRO_CHECK "{name}" undefined',
                f"#elif ({name}) == ({value})",
                f'HW1_MACRO_CHECK "{name}" matching',
                "#else",
                f'HW1_MACRO_CHECK "{name}" mismatching',
                "#endif",
            )
        )

    result = subprocess.run(
        [
            compiler,
            "-E",
            "-P",
            "-x",
            "c++",
            f"-D{board_define}=1",
            f'-DHW1_DEPLOYMENT_CONFIG_HEADER="{feature_header}"',
            "-",
        ],
        input="\n".join(lines) + "\n",
        cwd=REPOSITORY,
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(
            "System_BuildConfig.h failed to preprocess for "
            f"{board_define}:\n{result.stderr}"
        )

    resolved = dict(
        re.findall(
            r'^HW1_MACRO_CHECK\s+"([A-Z0-9_]+)"\s+'
            r"(matching|mismatching|undefined)$",
            result.stdout,
            re.MULTILINE,
        )
    )
    missing_markers = sorted(expected.keys() - resolved.keys())
    if missing_markers:
        raise AssertionError(
            "preprocessor emitted no result for: " + ", ".join(missing_markers)
        )
    return resolved


class DeploymentContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.contract = deployment_contract.load("headless/feather_esp32_v2")
        self.feathers3_contract = deployment_contract.load("headless/feathers3")

    def test_headless_feather_identity_and_geometry(self) -> None:
        self.assertEqual(self.contract.board_id, "feather_esp32_v2")
        self.assertEqual(self.contract.target, "esp32")
        self.assertEqual(self.contract.flash_size.upper(), "8MB")
        self.assertEqual(self.contract.layout_id, "hw1-hl-fv2-ota-v1")
        self.assertEqual(self.contract.version_suffix, "+fv2ho1")
        self.assertEqual(self.contract.ota_slot_size, 0x480000)
        self.assertEqual(self.contract.main_release_max, 0x400000)
        self.assertEqual(
            deployment_contract.partition_size(
                self.contract.partition_csv, "littlefs"
            ),
            0x250000,
        )

    def test_headless_feathers3_identity_and_geometry(self) -> None:
        contract = self.feathers3_contract
        self.assertEqual(contract.board_id, "feathers3")
        self.assertEqual(contract.target, "esp32s3")
        self.assertEqual(contract.flash_size.upper(), "16MB")
        self.assertEqual(contract.layout_id, "hw1-hl-f3-ota-v1")
        self.assertEqual(contract.version_suffix, "+f3ho1")
        self.assertEqual(contract.ota_slot_size, 0x480000)
        self.assertEqual(contract.main_release_max, 0x400000)
        self.assertEqual(contract.updater_release_max, 0x126666)
        self.assertFalse(contract.flash_encryption)
        self.assertEqual(
            deployment_contract.partition_size(contract.partition_csv, "littlefs"),
            0xA20000,
        )

    def test_headless_feature_policy_is_explicit(self) -> None:
        values = literal_integer_defines(self.contract.feature_header)
        expected = {
            "NETWORK_FEATURE_LEVEL": 3,
            "WEB_FEATURE_LEVEL": 4,
            "ENABLE_BLUETOOTH": 1,
            "ENABLE_BATTERY_MONITOR": 1,
            "ENABLE_NEOPIXEL": 0,
            "I2C_FEATURE_LEVEL": 0,
            "DISPLAY_TYPE": 0,
            "INPUT_DEVICE_TYPE": 0,
            "ENABLE_MQTT": 0,
            "ENABLE_AUTOMATION": 1,
            "ENABLE_G2_GLASSES": 0,
            "ENABLE_LLM_BACKEND": 0,
        }
        for name, value in expected.items():
            self.assertEqual(values.get(name), [value], name)

        build_config = BUILD_CONFIG.read_text(encoding="utf-8")
        self.assertRegex(
            build_config,
            re.compile(
                r"#if\s+ENABLE_BATTERY_MONITOR\s*&&\s*"
                r"BATTERY_BACKEND_FUEL_GAUGE\s*&&\s*!ENABLE_I2C_SYSTEM"
            ),
        )

    def test_headless_i2c_policy_is_board_specific_and_unambiguous(self) -> None:
        self.assertNotEqual(
            self.contract.feature_header, self.feathers3_contract.feature_header
        )

        v2 = literal_integer_defines(self.contract.feature_header)
        s3 = literal_integer_defines(self.feathers3_contract.feature_header)

        for name, value in {
            "I2C_FEATURE_LEVEL": 0,
            "DISPLAY_TYPE": 0,
            "INPUT_DEVICE_TYPE": 0,
            "CUSTOM_ENABLE_WEB_SENSORS": 0,
            "ENABLE_BATTERY_MONITOR": 1,
        }.items():
            self.assertEqual(v2.get(name), [value], f"Feather V2 {name}")

        for name, value in {
            "I2C_FEATURE_LEVEL": 4,
            "DISPLAY_TYPE": 0,
            "INPUT_DEVICE_TYPE": 0,
            "CUSTOM_ENABLE_WEB_SENSORS": 0,
            "ENABLE_BATTERY_MONITOR": 1,
            "I2C2_BUS_ENABLED_DEFAULT": 0,
            "CUSTOM_ENABLE_OLED": 0,
            "CUSTOM_ENABLE_GAMEPAD": 0,
            "CUSTOM_ENABLE_GPS": 0,
            "CUSTOM_ENABLE_IMU": 0,
            "CUSTOM_ENABLE_TOF": 0,
            "CUSTOM_ENABLE_THERMAL": 0,
            "CUSTOM_ENABLE_APDS": 0,
            "CUSTOM_ENABLE_FM_RADIO": 0,
            "CUSTOM_ENABLE_RTC": 0,
            "CUSTOM_ENABLE_PRESENCE": 0,
            "CUSTOM_ENABLE_SERVO": 0,
        }.items():
            self.assertEqual(s3.get(name), [value], f"FeatherS3 {name}")

    def test_headless_battery_i2c_policy_resolves_for_each_board(self) -> None:
        common_optional_off = {
            "ENABLE_THERMAL_SENSOR": 0,
            "ENABLE_TOF_SENSOR": 0,
            "ENABLE_IMU_SENSOR": 0,
            "ENABLE_GAMEPAD_SENSOR": 0,
            "ENABLE_APDS_SENSOR": 0,
            "ENABLE_GPS_SENSOR": 0,
            "ENABLE_FM_RADIO": 0,
            "ENABLE_RTC_SENSOR": 0,
            "ENABLE_PRESENCE_SENSOR": 0,
            "ENABLE_SERVO": 0,
            "ENABLE_OLED_DISPLAY": 0,
            "ENABLE_ANO_ENCODER": 0,
            "ENABLE_OLED_INPUT": 0,
            "ENABLE_WEB_SENSORS": 0,
            "ENABLE_I2C_SENSOR_QUEUE": 0,
            "DISPLAY_TYPE": 0,
            "INPUT_DEVICE_TYPE": 0,
        }
        v2_expected = {
            **common_optional_off,
            "I2C_FEATURE_LEVEL": 0,
            "ENABLE_I2C_SYSTEM": 0,
            "ENABLE_BATTERY_MONITOR": 1,
            "ENABLE_WEB_BATTERY": 1,
            "BATTERY_BACKEND_ADC": 1,
            "BATTERY_BACKEND_FUEL_GAUGE": 0,
            "I2C2_BUS_ENABLED_DEFAULT": 0,
        }
        s3_expected = {
            **common_optional_off,
            "I2C_FEATURE_LEVEL": 4,
            "ENABLE_I2C_SYSTEM": 1,
            "ENABLE_BATTERY_MONITOR": 1,
            "ENABLE_WEB_BATTERY": 1,
            "BATTERY_BACKEND_ADC": 0,
            "BATTERY_BACKEND_FUEL_GAUGE": 1,
            "I2C2_BUS_ENABLED_DEFAULT": 0,
        }

        for label, contract, board_define, expected in (
            (
                "Feather V2",
                self.contract,
                "ARDUINO_ADAFRUIT_FEATHER_ESP32_V2_DEV",
                v2_expected,
            ),
            (
                "FeatherS3",
                self.feathers3_contract,
                "ARDUINO_UM_FEATHERS3_DEV",
                s3_expected,
            ),
        ):
            with self.subTest(board=label):
                resolved = preprocess_macro_matches(
                    contract.feature_header, board_define, expected
                )
                failures = {
                    name: state
                    for name, state in resolved.items()
                    if state != "matching"
                }
                self.assertEqual(failures, {})

    def test_layouts_are_distinct_for_same_physical_board(self) -> None:
        legacy = make_manifest.resolve_contract("feather_esp32_v2")
        headless = make_manifest.resolve_contract(
            "feather_esp32_v2", "headless/feather_esp32_v2"
        )
        self.assertEqual(legacy["layout"], "hw1-fv2-ota-v1")
        self.assertEqual(legacy["suffix"], "+fv2o1")
        self.assertEqual(headless["layout"], self.contract.layout_id)
        self.assertEqual(headless["suffix"], self.contract.version_suffix)
        self.assertNotEqual(
            make_manifest.board_slot_size("feather_esp32_v2"),
            make_manifest.board_slot_size(
                "feather_esp32_v2", "headless/feather_esp32_v2"
            ),
        )
        self.assertEqual(
            make_manifest.contract_for_identity(
                "feather_esp32_v2", self.contract.layout_id
            )["main_release_max"],
            0x400000,
        )

        legacy_s3 = make_manifest.resolve_contract("feathers3")
        headless_s3 = make_manifest.resolve_contract(
            "feathers3", "headless/feathers3"
        )
        self.assertEqual(legacy_s3["layout"], "hw1-f3-ota-v1")
        self.assertEqual(legacy_s3["suffix"], "+f3o1")
        self.assertEqual(headless_s3["layout"], "hw1-hl-f3-ota-v1")
        self.assertEqual(headless_s3["suffix"], "+f3ho1")
        self.assertNotEqual(
            make_manifest.board_slot_size("feathers3"),
            make_manifest.board_slot_size("feathers3", "headless/feathers3"),
        )
        self.assertEqual(
            make_manifest.contract_for_identity(
                "feathers3", self.feathers3_contract.layout_id
            )["main_release_max"],
            0x400000,
        )

    def test_contract_fits_wire_identity_fields(self) -> None:
        for contract in (self.contract, self.feathers3_contract):
            self.assertLess(len(contract.board_id), make_manifest.PAYLOAD_SIZE)
            self.assertLess(len(contract.board_id.encode("ascii")), 24)
            self.assertLess(len(contract.layout_id.encode("ascii")), 24)
            self.assertLess(len(contract.version_suffix.encode("ascii")), 48)

    def test_partition_map_is_contiguous_and_fills_flash(self) -> None:
        for contract, flash_end in (
            (self.contract, 0x800000),
            (self.feathers3_contract, 0x1000000),
        ):
            rows = []
            for raw in contract.partition_csv.read_text(encoding="utf-8").splitlines():
                line = raw.split("#", 1)[0].strip()
                fields = [field.strip() for field in line.split(",")]
                if len(fields) >= 5 and fields[0]:
                    rows.append((fields[0], int(fields[3], 0), int(fields[4], 0)))
            self.assertEqual(rows[0][1], 0xA000)
            for previous, current in zip(rows, rows[1:]):
                self.assertEqual(previous[1] + previous[2], current[1])
            self.assertEqual(rows[-1][1] + rows[-1][2], flash_end)

    def test_protocol_header_names_both_headless_identities(self) -> None:
        header = (
            pathlib.Path(__file__).resolve().parents[3]
            / "components/hw1_ota_protocol/include/hw1_ota_protocol.h"
        ).read_text(encoding="utf-8")
        self.assertIn('"hw1-hl-fv2-ota-v1"', header)
        self.assertIn('"+fv2ho1"', header)
        self.assertIn('"hw1-hl-f3-ota-v1"', header)
        self.assertIn('"+f3ho1"', header)

    def test_unknown_or_cross_board_selector_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "contract not found"):
            deployment_contract.load("headless/not_a_board")
        with self.assertRaisesRegex(ValueError, "is for feather_esp32_v2"):
            make_manifest.resolve_contract(
                "qtpy_esp32", "headless/feather_esp32_v2"
            )
        with self.assertRaisesRegex(ValueError, "is for feathers3"):
            make_manifest.resolve_contract(
                "feather_esp32_v2", "headless/feathers3"
            )


if __name__ == "__main__":
    unittest.main()
