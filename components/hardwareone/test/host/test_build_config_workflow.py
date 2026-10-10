#!/usr/bin/env python3
"""Exercise configuration discovery and editing without ESP-IDF or build output."""

from contextlib import redirect_stderr, redirect_stdout
import io
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO / "tools"))
import build_config


class BuildConfigurationWorkflow(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        repo_patch = patch.object(build_config, "REPO", self.root)
        repo_patch.start()
        self.addCleanup(repo_patch.stop)
        for source in ("boards/xiao_s3.defaults", "boards/feathers3.defaults",
                       "presets/sdkconfig.defaults",
                       "components/hardwareone/System_BuildConfig.h"):
            destination = self.root / source
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(REPO / source, destination)
        (self.root / "configurations").mkdir()
        self.original = {
            "board": "xiao_s3",
            "features": {"ENABLE_MQTT": 0, "ENABLE_MAPS": 1},
            "sdkconfig": {"CONFIG_BT_ENABLED": False},
        }
        self.path = build_config.config_path("device")
        self.path.write_text(build_config.encode(self.original))

    def cli(self, *arguments):
        stdout, stderr = io.StringIO(), io.StringIO()
        with patch.object(sys, "argv", ["build_config.py", *arguments]), \
                redirect_stdout(stdout), redirect_stderr(stderr):
            status = build_config.main()
        return status, stdout.getvalue(), stderr.getvalue()

    def success(self, *arguments):
        status, stdout, stderr = self.cli(*arguments)
        self.assertEqual(status, 0, stderr)
        return stdout

    def state(self):
        """Include bytes and mtimes so dry runs cannot silently prepare a build."""
        return {str(path.relative_to(self.root)): (path.read_bytes(), path.stat().st_mtime_ns)
                for path in self.root.rglob("*") if path.is_file()}

    def test_show_needs_no_sdk_or_build_and_preserves_explicit_false(self):
        before = self.state()
        with patch.object(build_config.subprocess, "call", side_effect=AssertionError("build invoked")), \
                patch.object(build_config.gen_build_info, "resolve_macros",
                             side_effect=AssertionError("compiler invoked")), \
                patch.dict(os.environ, {"PATH": "", "IDF_PATH": ""}):
            shown = json.loads(self.success("show", "device", "--json"))
        self.assertEqual(shown, self.original)
        self.assertIs(shown["sdkconfig"]["CONFIG_BT_ENABLED"], False)
        self.assertEqual(self.state(), before)

    def test_catalogs_discover_valid_inputs_without_building_or_inventing_defaults(self):
        # OTA fragments lack a hardware target marker and are not board choices.
        (self.root / "boards/xiao_s3.ota.defaults").write_text("CONFIG_EXAMPLE=y\n")
        # An empty target must not consume a bare identifier on the next line.
        (self.root / "boards/malformed.defaults").write_text("# HW_TARGET:\nesp32s3\n")
        before = self.state()
        with patch.object(build_config.subprocess, "call", side_effect=AssertionError("build invoked")), \
                patch.object(build_config.gen_build_info, "resolve_macros",
                             side_effect=AssertionError("compiler invoked")), \
                patch.dict(os.environ, {"PATH": "", "IDF_PATH": ""}):
            catalogs = {kind: json.loads(self.success("list", kind, "--json"))
                        for kind in ("boards", "configurations", "features", "sdk")}
        self.assertEqual(catalogs["boards"], [
            {"name": "feathers3", "target": "esp32s3"},
            {"name": "xiao_s3", "target": "esp32s3"},
        ])
        self.assertEqual(catalogs["configurations"], [{"name": "device", "board": "xiao_s3"}])
        self.assertEqual({row["name"] for row in catalogs["features"]}, build_config.editable_choices())
        self.assertEqual({row["name"] for row in catalogs["sdk"]}, build_config.sdk_choices())
        for row in catalogs["features"]:
            self.assertEqual(set(row), {"name", "type", "minimum", "maximum"})
            self.assertEqual(row["type"], "integer")
            for value in (row["minimum"], row["maximum"]):
                build_config.validate({"board": "xiao_s3", "features": {row["name"]: value}})
            for value in (row["minimum"] - 1, row["maximum"] + 1):
                with self.assertRaises(ValueError, msg=f"catalog bound for {row['name']}"):
                    build_config.validate({"board": "xiao_s3", "features": {row["name"]: value}})
        for row in catalogs["sdk"]:
            self.assertEqual(set(row), {"name", "type"})
            self.assertEqual(row["type"], "boolean")
            for value in (False, True):
                build_config.validate({"board": "xiao_s3", "features": {},
                                       "sdkconfig": {row["name"]: value}})
        self.assertEqual(self.state(), before)

    def test_missing_or_duplicate_editable_marker_refuses_discovery_and_edit(self):
        header = self.root / "components/hardwareone/System_BuildConfig.h"
        original = header.read_text()
        marker = "// CHECKED-IN DEPLOYMENT PROFILE OVERRIDE"
        for text in (original.replace(marker, "// no editable boundary"),
                     original + "\n" + marker + "\n"):
            with self.subTest(marker_count=text.count(marker)):
                header.write_text(text)
                before = self.state()
                for arguments in (("list", "features", "--json"),
                                  ("edit", "device", "--set", "ENABLE_MQTT=1")):
                    status, _, error = self.cli(*arguments)
                    self.assertNotEqual(status, 0)
                    self.assertIn("editable feature section", error)
                    self.assertEqual(self.state(), before)

    def test_mutation_rejects_unknown_original_field_instead_of_dropping_it(self):
        config = dict(self.original, unexpected="preserve until corrected")
        before_config = json.dumps(config, sort_keys=True)
        with self.assertRaises(ValueError):
            build_config.apply_changes(config, feature_sets=("ENABLE_MQTT=1",))
        self.assertEqual(json.dumps(config, sort_keys=True), before_config)
        self.path.write_text(build_config.encode(config))
        before_files = self.state()
        status, _, error = self.cli("edit", "device", "--set", "ENABLE_MQTT=1")
        self.assertNotEqual(status, 0)
        self.assertTrue(error)
        self.assertEqual(self.state(), before_files)

    def test_edit_preserves_board_and_unmentioned_choices(self):
        self.success("edit", "device", "--set", "ENABLE_MQTT=1", "--sdk", "CONFIG_BT_ENABLED=y")
        self.assertEqual(build_config.load("device"), {
            "board": "xiao_s3",
            "features": {"ENABLE_MQTT": 1, "ENABLE_MAPS": 1},
            "sdkconfig": {"CONFIG_BT_ENABLED": True},
        })
        self.assertFalse(build_config.build_dir("device").exists())

    def test_create_from_and_edit_use_the_same_choice_mutations(self):
        changes = ("--set", "ENABLE_MAPS=0", "--set", "ENABLE_MQTT=1",
                   "--sdk", "CONFIG_BT_ENABLED=y")
        self.success("create", "copy", "--from", "device", *changes)
        self.assertEqual(build_config.load("device"), self.original)
        self.success("edit", "device", *changes)
        self.assertEqual(build_config.load("copy"), build_config.load("device"))

    def test_unset_removes_choice_instead_of_replacing_it_with_zero(self):
        self.success("edit", "device", "--unset", "ENABLE_MQTT", "--unset-sdk", "CONFIG_BT_ENABLED")
        edited = build_config.load("device")
        self.assertEqual(edited["features"], {"ENABLE_MAPS": 1})
        self.assertNotIn("CONFIG_BT_ENABLED", edited.get("sdkconfig", {}))
        self.assertEqual(edited["board"], "xiao_s3")
        # An explicit false and an inherited choice must remain distinguishable.
        self.success("edit", "device", "--sdk", "CONFIG_BT_ENABLED=n")
        self.assertIs(build_config.load("device")["sdkconfig"]["CONFIG_BT_ENABLED"], False)

    def test_noop_edit_preserves_original_bytes_and_mtime(self):
        # A semantic no-op also preserves a user's harmless JSON formatting.
        self.path.write_text(json.dumps(self.original, indent=4) + "\n")
        before = self.state()
        self.success("edit", "device", "--set", "ENABLE_MQTT=0", "--sdk", "CONFIG_BT_ENABLED=n")
        self.assertEqual(self.state(), before)

    def test_invalid_edits_leave_configuration_and_build_inputs_unchanged(self):
        build_config.prepare("device", "xiao_s3")
        before = self.state()
        for arguments in (("--set", "ENABLE_MQTT=2"), ("--set", "ENABLE_WIFI=1"),
                          ("--set", "NOT_A_FEATURE=1"), ("--set", "ENABLE_MQTT=yes"),
                          ("--unset", "NOT_A_FEATURE"),
                          ("--sdk", "CONFIG_SECURE_FLASH_ENC_ENABLED=y"),
                          ("--unset-sdk", "CONFIG_SECURE_FLASH_ENC_ENABLED"),
                          ("--sdk", "CONFIG_BT_ENABLED=false")):
            with self.subTest(arguments=arguments):
                status, _, error = self.cli("edit", "device", *arguments)
                self.assertNotEqual(status, 0)
                self.assertTrue(error)
                self.assertEqual(self.state(), before)

    def test_set_unset_conflicts_are_rejected_without_writes(self):
        before = self.state()
        for arguments in (("--set", "ENABLE_MQTT=1", "--unset", "ENABLE_MQTT"),
                          ("--sdk", "CONFIG_BT_ENABLED=y", "--unset-sdk", "CONFIG_BT_ENABLED")):
            with self.subTest(arguments=arguments):
                status, _, error = self.cli("edit", "device", *arguments)
                self.assertNotEqual(status, 0)
                self.assertTrue(error)
                self.assertEqual(self.state(), before)

    def test_dry_run_leaves_configuration_and_previous_build_untouched(self):
        build_config.prepare("device", "xiao_s3")
        (build_config.build_dir("device") / "image.bin").write_bytes(b"previous firmware")
        before = self.state()
        output = self.success("edit", "device", "--set", "ENABLE_MQTT=1",
                              "--unset-sdk", "CONFIG_BT_ENABLED", "--dry-run")
        self.assertIn("ENABLE_MQTT", output)
        self.assertEqual(self.state(), before)

    def test_edit_requires_reprepare_before_old_build_can_be_verified(self):
        build_config.prepare("device", "xiao_s3")
        directory = build_config.build_dir("device")
        snapshot = (directory / "configuration.json").read_bytes()
        self.success("edit", "device", "--set", "ENABLE_MQTT=1")
        self.assertEqual((directory / "configuration.json").read_bytes(), snapshot)
        with self.assertRaisesRegex(ValueError, "configuration changed"):
            build_config.check("device")

    def test_failed_atomic_replace_preserves_original_file(self):
        before = self.state()
        with patch("os.replace", side_effect=OSError("simulated replace failure")):
            status, _, error = self.cli("edit", "device", "--set", "ENABLE_MQTT=1")
        self.assertNotEqual(status, 0)
        self.assertIn("simulated replace failure", error)
        self.assertEqual(self.state(), before)

    def test_atomic_create_publishes_only_a_complete_configuration(self):
        destination = build_config.config_path("new")
        link = os.link

        def publish(source, target):
            self.assertEqual(Path(target), destination)
            self.assertEqual(Path(source).parent, destination.parent)
            self.assertFalse(destination.exists())
            self.assertEqual(Path(source).read_text(), build_config.encode(self.original))
            link(source, target)

        with patch.object(build_config.os, "link", side_effect=publish) as publisher:
            self.assertTrue(build_config.save_configuration("new", self.original))
        publisher.assert_called_once()
        self.assertEqual(build_config.load("new"), self.original)
        self.assertEqual(set(destination.parent.iterdir()), {self.path, destination})

    def test_create_race_preserves_the_other_writer(self):
        destination = build_config.config_path("racing")
        winner = build_config.encode({"board": "xiao_s3", "features": {"ENABLE_MQTT": 1}})
        original = self.path.read_bytes()
        link = os.link

        def publish_after_other_writer(source, target):
            Path(target).write_text(winner)
            link(source, target)

        with patch.object(build_config.os, "link", side_effect=publish_after_other_writer):
            with self.assertRaises(FileExistsError):
                build_config.save_configuration("racing", self.original)
        self.assertEqual(destination.read_text(), winner)
        self.assertEqual(self.path.read_bytes(), original)
        self.assertEqual(set(destination.parent.iterdir()), {self.path, destination})

    def test_failed_atomic_creation_leaves_no_partial_file(self):
        before = self.state()
        for operation in ("link", "fsync"):
            with self.subTest(operation=operation):
                with patch.object(build_config.os, operation, side_effect=OSError("publication failed")):
                    with self.assertRaisesRegex(OSError, "publication failed"):
                        build_config.save_configuration("new", self.original)
                self.assertEqual(self.state(), before)

    def test_save_rejects_stale_original_even_for_a_semantic_noop(self):
        original = self.path.read_text()
        # Another writer changed formatting only. The exact reviewed text no
        # longer matches, even though the candidate has identical choices.
        self.path.write_text(json.dumps(self.original, indent=4) + "\n")
        before = self.state()
        with self.assertRaisesRegex(ValueError, "changed while editing"):
            build_config.save_configuration("device", self.original, original=original)
        self.assertEqual(self.state(), before)
        self.assertFalse(build_config.save_configuration(
            "device", self.original, original=self.path.read_text()))
        self.assertEqual(self.state(), before)

    def test_edit_detects_changes_while_temporary_file_is_written(self):
        original = self.path.read_text()
        candidate = build_config.apply_changes(self.original, feature_sets=("ENABLE_MQTT=1",))
        winner = json.dumps(self.original, indent=4) + "\n"
        sync = os.fsync

        def sync_after_other_writer(descriptor):
            sync(descriptor)
            self.path.write_text(winner)

        with patch.object(build_config.os, "fsync", side_effect=sync_after_other_writer):
            with self.assertRaisesRegex(ValueError, "changed while editing"):
                build_config.save_configuration("device", candidate, original=original)
        self.assertEqual(self.path.read_text(), winner)
        self.assertEqual(list(self.path.parent.iterdir()), [self.path])

    def test_shared_save_checks_schema_and_existing_build_board(self):
        before = self.state()
        with self.assertRaises(ValueError):
            build_config.save_configuration("new", dict(self.original, unknown="unsupported"))
        self.assertEqual(self.state(), before)

        build_config.prepare("device", "xiao_s3")
        candidate = {"board": "feathers3", "features": {}}
        before = self.state()
        with self.assertRaisesRegex(ValueError, "changed board"):
            build_config.save_configuration("device", candidate, original=self.path.read_text())
        self.assertEqual(self.state(), before)
        # The same-name build directory remains protected if its old saved
        # configuration has been removed and a new one is being created.
        self.path.unlink()
        before = self.state()
        with self.assertRaisesRegex(ValueError, "changed board"):
            build_config.save_configuration("device", candidate)
        self.assertEqual(self.state(), before)

    def test_shared_save_preserves_board_before_the_first_build(self):
        original = self.path.read_text()
        before = self.state()
        self.assertFalse(build_config.build_dir("device").exists())
        with self.assertRaisesRegex(ValueError, "changed board; use a new configuration name"):
            build_config.save_configuration(
                "device", {"board": "feathers3", "features": {}}, original=original)
        self.assertEqual(self.state(), before)

    def test_shared_build_uses_existing_wrapper_without_extra_actions(self):
        before = self.state()
        expected = ["bash", str(self.root / "tools/build_board.sh"), "xiao_s3", "--config", "device"]
        with patch.object(build_config.subprocess, "call", return_value=23) as wrapper:
            self.assertEqual(build_config.build("device"), 23)
            wrapper.assert_called_once_with(expected)
            status, _, _ = self.cli("build", "device", "size")
            self.assertEqual(status, 23)
            self.assertEqual(wrapper.call_count, 2)
            wrapper.assert_called_with(expected + ["size"])
        self.assertEqual(self.state(), before)


if __name__ == "__main__":
    unittest.main()
