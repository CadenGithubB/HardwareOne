#!/usr/bin/env python3
"""Check SDK overlay ordering and incremental recovery using the real CMake helper."""

from pathlib import Path
import shutil
import subprocess
import sys
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

    def test_every_board_layers_only_the_shared_policy(self):
        # Board limits on SDK choices live in tools/build_config.py, not in per-board presets.
        for board in ("qtpy_esp32", "xiao_s3"):
            with self.subTest(board=board):
                self.require_success(board=board)
                self.assertEqual(self.layers, [str(REPO / "config/sdkconfig.features.defaults")])
        self.named.write_text("CONFIG_BT_ENABLED=n\n")
        self.require_success(board="qtpy_esp32", named=self.named)
        self.assertEqual(self.layers, [str(REPO / "config/sdkconfig.features.defaults"), str(self.named)])

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
        # A QT Py configuration build hands Kconfig its fixed CONFIG_BT_ENABLED=n.
        self.named.write_text("CONFIG_BT_ENABLED=n\n")
        before = self.sdk.stat().st_mtime_ns
        self.require_success(board="qtpy_esp32", named=self.named)
        self.assertEqual(self.sdk.stat().st_mtime_ns, before)

    def test_removing_xiao_override_restores_shared_enabled_preset(self):
        self.sdk.write_text("CONFIG_UNRELATED_TUNING=73\n# CONFIG_BT_ENABLED is not set\n")
        self.require_success()
        self.assertNotIn("CONFIG_BT_ENABLED", self.sdk.read_text())
        self.assertEqual(self.layers, [str(REPO / "config/sdkconfig.features.defaults")])

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


LITTLEFS_BLOCK = 4096
LOOKAHEAD_KEY = "CONFIG_LITTLEFS_LOOKAHEAD_SIZE"
# A board whose own table is larger than the shared value covers, and whose
# board file does not name the key yet (2026-11-20: that file was outside the
# file set of the change that raised the shared value; the line is reported in
# the change's notes). The value here is what its board file has to carry. Once
# the file names the key, the file's value is checked like any other and this
# entry no longer matters; a value in the file that is too small fails.
PENDING_BOARD_VALUES = {"waveshare_p4_touch_lcd_5": 768}


def littlefs_bytes(table: Path) -> int:
    """The littlefs partition's size from the table's size column (never its comments)."""
    for line in table.read_text().splitlines():
        fields = [field.strip() for field in line.split("#", 1)[0].split(",")]
        if len(fields) >= 5 and fields[0] == "littlefs":
            return int(fields[4], 0)
    raise AssertionError(f"{table.name} has no littlefs partition")


def config_value(path: Path, key: str):
    """The last `key=value` in a defaults file, or None."""
    value = None
    if path.exists():
        for line in path.read_text().splitlines():
            if line.startswith(key + "="):
                value = line.split("=", 1)[1].strip().strip('"')
    return value


class LittleFsLookahead(unittest.TestCase):
    """S-9: the lookahead window covers every block of the largest LittleFS partition a board can have.

    A fresh build takes the value from config/sdkconfig.defaults, then
    config/sdkconfig.defaults.<target>, then the board file (CMakeLists.txt,
    SDKCONFIG_DEFAULTS; a later file wins). A table named for a board
    (partitions_<sr>_<flash>_<board>.csv) belongs to that board; the others
    serve every board of that flash size.
    """

    def boards(self):
        found = {}
        for path in sorted((REPO / "boards").glob("*.defaults")):
            if path.name.count(".") != 1:
                continue  # <board>.ota.defaults is an overlay of <board>.defaults
            text = path.read_text()
            target = next((line.split(":", 1)[1].strip() for line in text.splitlines()
                           if line.startswith("# HW_TARGET:")), None)
            flash = config_value(path, "CONFIG_ESPTOOLPY_FLASHSIZE")
            self.assertTrue(target and flash, path.name)
            value = None
            for layer in (REPO / "config/sdkconfig.defaults",
                          REPO / f"config/sdkconfig.defaults.{target}", path):
                value = config_value(layer, LOOKAHEAD_KEY) or value
            self.assertIsNotNone(value, f"{path.stem}: no {LOOKAHEAD_KEY} in any defaults layer")
            pending = path.stem in PENDING_BOARD_VALUES and config_value(path, LOOKAHEAD_KEY) is None
            if pending:
                print(f"note: boards/{path.name} does not set {LOOKAHEAD_KEY} yet; "
                      f"checking the pending value {PENDING_BOARD_VALUES[path.stem]}", file=sys.stderr)
                value = PENDING_BOARD_VALUES[path.stem]
            found[path.stem] = (flash.lower(), int(value))
        return found

    def test_pending_values_are_needed_and_valid(self):
        for board, value in PENDING_BOARD_VALUES.items():
            self.assertTrue((REPO / "boards" / f"{board}.defaults").exists(), board)
            shared = int(config_value(REPO / "config/sdkconfig.defaults", LOOKAHEAD_KEY))
            self.assertTrue(value % 8 == 0 and value > shared, f"{board}: {value}")

    def test_lookahead_covers_every_partition_table(self):
        boards = self.boards()
        tables = sorted((REPO / "partitions").glob("partitions_*.csv"))
        self.assertGreaterEqual(len(tables), 9)
        checked = 0
        for table in tables:
            size = littlefs_bytes(table)
            blocks = -(-size // LITTLEFS_BLOCK)
            parts = table.stem.split("_")
            flash_at = next(i for i, part in enumerate(parts) if part.endswith("mb"))
            flash, board = parts[flash_at], "_".join(parts[flash_at + 1:])
            users = [board] if board else [name for name, (size_name, _) in boards.items() if size_name == flash]
            self.assertTrue(users, f"{table.name}: no board uses a {flash} table")
            for name in users:
                self.assertIn(name, boards, f"{table.name}: unknown board {name}")
                lookahead = boards[name][1]
                with self.subTest(table=table.name, board=name):
                    self.assertGreaterEqual(
                        8 * lookahead * LITTLEFS_BLOCK, size,
                        f"{LOOKAHEAD_KEY}={lookahead} covers {8 * lookahead} of the {blocks} blocks of "
                        f"{table.name} ({size // 1024} KiB); board {name} needs at least "
                        f"{-(-blocks // 64) * 8} in boards/{name}.defaults or config/sdkconfig.defaults")
                    checked += 1
        self.assertGreaterEqual(checked, 9)

    def test_value_is_a_multiple_of_eight_everywhere(self):
        # The allocator keeps one bit per block in whole bytes and the
        # component's Kconfig asks for a multiple of 8.
        files = [REPO / "config/sdkconfig.defaults",
                 *sorted((REPO / "config").glob("sdkconfig.defaults.*")),
                 *sorted((REPO / "boards").glob("*.defaults"))]
        seen = 0
        for path in files:
            value = config_value(path, LOOKAHEAD_KEY)
            if value is None:
                continue
            seen += 1
            self.assertTrue(value.isdigit() and int(value) > 0 and int(value) % 8 == 0, f"{path}: {value}")
        self.assertGreaterEqual(seen, 1)

    def test_shared_default_value(self):
        self.assertEqual(config_value(REPO / "config/sdkconfig.defaults", LOOKAHEAD_KEY), "320")

    def test_sizes_come_from_the_size_column(self):
        # The comments of four tables state sizes the columns no longer have.
        expected = {"partitions_ota_no_sr_8mb.csv": 1856, "partitions_no_sr_8mb.csv": 2284,
                    "partitions_ota_sr_16mb.csv": 5696, "partitions_sr_16mb.csv": 7808,
                    "partitions_ota_no_sr_16mb.csv": 8704, "partitions_no_sr_16mb_p4x_eye.csv": 9068,
                    "partitions_no_sr_16mb.csv": 10092, "partitions_sr_8mb.csv": 128,
                    "partitions_no_sr_32mb_waveshare_p4_touch_lcd_5.csv": 24512}
        for name, kib in expected.items():
            self.assertEqual(littlefs_bytes(REPO / "partitions" / name), kib * 1024, name)


MMAP_KEY = "CONFIG_LITTLEFS_MMAP_PARTITION"
MMU_PAGE = 64 * 1024
MIB = 1024 * 1024
# Address windows a mapping takes pages from (ESP-IDF soc/<chip>/ext_mem_defs.h).
# S3: one 32 MB window holds flash code, flash data, all of PSRAM and every
# esp_partition_mmap. P4: flash has its own 64 MB window; PSRAM has another.
S3_WINDOW = 32 * MIB
S3_PSRAM = 8 * MIB  # every S3 board: XIAO S3 Sense (octal), FeatherS3 / FeatherS3-FE (quad)
P4_FLASH_WINDOW = 64 * MIB
APP_SPLIT_PAGES = 2 * MMU_PAGE  # the running app maps code and rodata as two regions
# The option is set in these chip-family files and nowhere else. The classic
# ESP32 keeps esp_partition_read: its 4 MB data window also holds the app's
# rodata, and LittleFS takes 36-37 of its 64 pages (config/sdkconfig.defaults.esp32).
MMAP_FAMILIES = ("esp32s3", "esp32p4")


def partition_rows(table: Path):
    """(name, type, subtype, offset, size) rows from the table's columns (never its comments)."""
    rows = []
    for line in table.read_text().splitlines():
        fields = [field.strip() for field in line.split("#", 1)[0].split(",")]
        if len(fields) >= 5 and fields[0]:
            rows.append((fields[0], fields[1], fields[2], int(fields[3], 0), int(fields[4], 0)))
    return rows


def mapped_bytes(offset: int, size: int) -> int:
    """Window bytes one esp_partition_mmap of a whole partition takes (whole MMU pages)."""
    first, last = offset // MMU_PAGE, (offset + size - 1) // MMU_PAGE
    return (last - first + 1) * MMU_PAGE


def round_pages(size: int) -> int:
    return -(-size // MMU_PAGE) * MMU_PAGE


def board_targets():
    """{board: (target, flash size name)} from the board files' markers."""
    found = {}
    for path in sorted((REPO / "boards").glob("*.defaults")):
        if path.name.count(".") != 1:
            continue
        text = path.read_text()
        target = next((line.split(":", 1)[1].strip() for line in text.splitlines()
                       if line.startswith("# HW_TARGET:")), None)
        found[path.stem] = (target, config_value(path, "CONFIG_ESPTOOLPY_FLASHSIZE").lower())
    return found


def table_users():
    """(table, [boards]) for every partition table a build can select.

    partitions/partitions_<sr>_<flash>[_<board>].csv serve the named board, or
    every board of that flash size; deployments/<name>/boards/<board>/partitions.csv
    serve that board.
    """
    boards = board_targets()
    result = []
    for table in sorted((REPO / "partitions").glob("partitions_*.csv")):
        parts = table.stem.split("_")
        flash_at = next(i for i, part in enumerate(parts) if part.endswith("mb"))
        flash, board = parts[flash_at], "_".join(parts[flash_at + 1:])
        users = [board] if board else [name for name, (_, size) in boards.items() if size == flash]
        result.append((table, users))
    for table in sorted((REPO / "deployments").glob("*/boards/*/partitions.csv")):
        result.append((table, [table.parent.name]))
    return boards, result


class LittleFsMmap(unittest.TestCase):
    """S-1: LittleFS reads go through the flash memory map (CONFIG_LITTLEFS_MMAP_PARTITION).

    The component maps the whole LittleFS partition at mount. Running out of
    window fails the mount; on SR layouts ESP-SR maps its model partition
    afterwards under ESP_ERROR_CHECK, so running out there aborts at boot.
    """

    def test_option_is_set_by_the_s3_and_p4_family_files_only(self):
        for family in MMAP_FAMILIES:
            self.assertEqual(config_value(REPO / f"config/sdkconfig.defaults.{family}", MMAP_KEY), "y", family)
        # Every other layer leaves it alone: the shared file (it would reach the
        # classic ESP32), the classic family file, OTA/soak overlays, the feature
        # file and the deployment overlays. A board file may only turn it off
        # (a littlefs partition above the 16 MB mapping limit, below).
        for path in sorted((REPO / "boards").glob("*.defaults")):
            self.assertIn(config_value(path, MMAP_KEY), (None, "n"), path.relative_to(REPO).as_posix())
        others = [REPO / "config/sdkconfig.defaults",
                  *[path for path in sorted((REPO / "config").glob("sdkconfig.defaults.*"))
                    if path.suffix.lstrip(".") not in MMAP_FAMILIES],
                  *sorted((REPO / "config").glob("sdkconfig.*.defaults")),
                  *sorted((REPO / "deployments").glob("*/boards/*/sdkconfig.defaults"))]
        for path in others:
            self.assertNotIn(f"{MMAP_KEY}=", path.read_text(), path.relative_to(REPO).as_posix())
            self.assertNotIn(f"# {MMAP_KEY} is not set", path.read_text(), path.relative_to(REPO).as_posix())

    def check_tables(self, target):
        boards, tables = table_users()
        checked = 0
        for table, users in tables:
            users = [name for name in users if boards.get(name, (None,))[0] == target]
            if not users:
                continue
            rows = partition_rows(table)
            littlefs = [row for row in rows if row[0] == "littlefs"]
            self.assertEqual(len(littlefs), 1, table.name)
            models = [row for row in rows if row[1] == "data" and row[0] == "model"]
            apps = [row for row in rows if row[1] == "app"]
            self.assertTrue(apps, table.name)
            yield table, users, littlefs[0], models, max(row[4] for row in apps)
            checked += 1
        self.assertGreater(checked, 0, f"no partition table serves a {target} board")

    def test_s3_window_holds_littlefs_model_app_and_psram(self):
        # Upper bound: the largest app partition stands in for the mapped code
        # and rodata of whatever image it holds; code and rodata are mapped
        # separately, so each can end in a part page (APP_SPLIT_PAGES).
        names = []
        for table, users, littlefs, models, app in self.check_tables("esp32s3"):
            used = (mapped_bytes(littlefs[3], littlefs[4]) +
                    sum(mapped_bytes(row[3], row[4]) for row in models) +
                    round_pages(app) + APP_SPLIT_PAGES + S3_PSRAM)
            names.append(table.name)
            with self.subTest(table=table.name, boards=users):
                self.assertLessEqual(
                    used, S3_WINDOW,
                    f"{table.name}: LittleFS {littlefs[4] // 1024} KiB + model "
                    f"{sum(row[4] for row in models) // 1024} KiB + app {app // 1024} KiB + PSRAM "
                    f"{S3_PSRAM // MIB} MB = {used / MIB:.2f} MB of the {S3_WINDOW // MIB} MB S3 window")
        for name in ("partitions_ota_no_sr_16mb.csv", "partitions_ota_sr_16mb.csv",
                     "partitions_sr_8mb.csv", "partitions_sr_16mb.csv"):
            self.assertIn(name, names)

    def test_p4_flash_window_holds_littlefs_model_and_app(self):
        for table, users, littlefs, models, app in self.check_tables("esp32p4"):
            used = (mapped_bytes(littlefs[3], littlefs[4]) +
                    sum(mapped_bytes(row[3], row[4]) for row in models) + round_pages(app) +
                    APP_SPLIT_PAGES)
            with self.subTest(table=table.name, boards=users):
                self.assertLessEqual(used, P4_FLASH_WINDOW, f"{table.name}: {used / MIB:.2f} MB")

    def test_mapped_littlefs_ends_below_the_16mb_flash_mapping_limit(self):
        # The S3 and P4 caches map flash addresses below 16 MB only (24-bit;
        # the 32-bit mapping is an experimental ESP-IDF option). A mapped
        # littlefs partition that ends above it fails the mount at boot
        # ("out of range for 24bit flash mapping"), so such a board must turn
        # the mapping off in its board file.
        mapped_boards = {name for name in board_targets()
                         if config_value(REPO / "boards" / f"{name}.defaults", MMAP_KEY) != "n"}
        for target in MMAP_FAMILIES:
            for table, users, littlefs, models, app in self.check_tables(target):
                end = littlefs[3] + littlefs[4]
                for board in users:
                    with self.subTest(table=table.name, board=board):
                        if board in mapped_boards:
                            self.assertLessEqual(end, 16 * MIB, f"{table.name}: littlefs ends at {end / MIB:.2f} MB")
        self.assertNotIn("waveshare_p4_touch_lcd_5", mapped_boards)

    def test_page_arithmetic(self):
        self.assertEqual(mapped_bytes(0x10000, 0x10000), MMU_PAGE)
        self.assertEqual(mapped_bytes(0x7E0000, 0x20000), 2 * MMU_PAGE)
        self.assertEqual(mapped_bytes(0x5000, 0x10000), 2 * MMU_PAGE)  # starts 20 KiB into a page
        self.assertEqual(mapped_bytes(0x5C5000, 0x23B000), 36 * MMU_PAGE)
        self.assertEqual(round_pages(1), MMU_PAGE)


if __name__ == "__main__":
    unittest.main()
