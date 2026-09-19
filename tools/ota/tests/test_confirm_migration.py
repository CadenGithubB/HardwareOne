from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import unittest

from tools.ota import confirm_migration


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = REPOSITORY / "tools" / "ota" / "confirm_migration.py"
HEADLESS_SELECTOR = "headless/feather_esp32_v2"
HEADLESS_PHRASE = f"MIGRATE {HEADLESS_SELECTOR} hw1-hl-fv2-ota-v1"
HEADLESS_S3_SELECTOR = "headless/feathers3"
HEADLESS_S3_PHRASE = f"MIGRATE {HEADLESS_S3_SELECTOR} hw1-hl-f3-ota-v1"


class MigrationConfirmationTests(unittest.TestCase):
    def run_guard(
        self, *arguments: str, confirmation: str | None = None
    ) -> subprocess.CompletedProcess[str]:
        environment = os.environ.copy()
        environment.pop("HW1_OTA_MIGRATION_CONFIRM", None)
        if confirmation is not None:
            environment["HW1_OTA_MIGRATION_CONFIRM"] = confirmation
        return subprocess.run(
            [sys.executable, str(SCRIPT), *arguments],
            cwd=REPOSITORY,
            env=environment,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            check=False,
        )

    def test_legacy_phrase_remains_board_only(self) -> None:
        self.assertEqual(
            confirm_migration.expected_confirmation("feathers3"),
            "MIGRATE feathers3",
        )
        result = self.run_guard(
            "--board", "feathers3", confirmation="MIGRATE feathers3"
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_deployment_phrase_binds_selector_and_layout(self) -> None:
        self.assertEqual(
            confirm_migration.expected_confirmation(
                "feather_esp32_v2", HEADLESS_SELECTOR
            ),
            HEADLESS_PHRASE,
        )
        result = self.run_guard(
            "--board",
            "feather_esp32_v2",
            "--deployment",
            HEADLESS_SELECTOR,
            confirmation=HEADLESS_PHRASE,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"Deployment: {HEADLESS_SELECTOR}", result.stdout)
        self.assertIn("OTA layout identity: hw1-hl-fv2-ota-v1", result.stdout)

    def test_board_only_phrase_cannot_approve_deployment_migration(self) -> None:
        result = self.run_guard(
            "--board",
            "feather_esp32_v2",
            "--deployment",
            HEADLESS_SELECTOR,
            confirmation="MIGRATE feather_esp32_v2",
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn(repr(HEADLESS_PHRASE), result.stderr)

    def test_feathers3_deployment_phrase_binds_its_layout(self) -> None:
        self.assertEqual(
            confirm_migration.expected_confirmation(
                "feathers3", HEADLESS_S3_SELECTOR
            ),
            HEADLESS_S3_PHRASE,
        )
        result = self.run_guard(
            "--board",
            "feathers3",
            "--deployment",
            HEADLESS_S3_SELECTOR,
            confirmation=HEADLESS_S3_PHRASE,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"Deployment: {HEADLESS_S3_SELECTOR}", result.stdout)
        self.assertIn("OTA layout identity: hw1-hl-f3-ota-v1", result.stdout)

    def test_cross_board_deployment_is_rejected(self) -> None:
        result = self.run_guard(
            "--board",
            "qtpy_esp32",
            "--deployment",
            HEADLESS_SELECTOR,
            confirmation=HEADLESS_PHRASE,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("is for feather_esp32_v2, not qtpy_esp32", result.stderr)


if __name__ == "__main__":
    unittest.main()
