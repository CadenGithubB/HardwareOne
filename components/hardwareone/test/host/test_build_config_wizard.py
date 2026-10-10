#!/usr/bin/env python3
"""Exercise the guided setup through its CLI and shared configuration model."""

from contextlib import redirect_stderr, redirect_stdout
import io
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO / "tools"))
import build_config
import build_config_wizard


class GuidedConfiguration(unittest.TestCase):
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
        self.original = {"board": "xiao_s3", "features": {"ENABLE_MQTT": 0, "ENABLE_MAPS": 1},
                         "sdkconfig": {"CONFIG_BT_ENABLED": False}}
        self.path = build_config.config_path("device")
        self.path.write_text(build_config.encode(self.original))

    def cli(self, arguments, answers, before_answer=None):
        answers = iter(answers)
        stdout, stderr = io.StringIO(), io.StringIO()

        def answer(prompt):
            if before_answer:
                before_answer()
            item = next(answers)
            if isinstance(item, BaseException):
                raise item
            return item(prompt) if callable(item) else item

        with patch.object(sys, "argv", ["build_config.py", "configure", *arguments]), \
                patch("builtins.input", side_effect=answer), \
                redirect_stdout(stdout), redirect_stderr(stderr):
            status = build_config.main()
        return status, stdout.getvalue(), stderr.getvalue()

    def success(self, arguments, answers, **kwargs):
        status, output, error = self.cli(arguments, answers, **kwargs)
        self.assertEqual(status, 0, error)
        return output

    def state(self):
        return {str(path.relative_to(self.root)): (path.read_bytes(), path.stat().st_mtime_ns)
                for path in self.root.rglob("*") if path.is_file()}

    def previous_build(self):
        build_config.prepare("device", "xiao_s3")
        (build_config.build_dir("device") / "image.bin").write_bytes(b"previous firmware")

    def test_new_configuration_is_only_written_after_review_save(self):
        before = self.state()
        with patch.object(build_config, "build", side_effect=AssertionError("unrequested build")):
            output = self.success(["new-device", "--board", "xiao_s3"],
                                  ["1", "ENABLE_MQTT", "on", "3", "s"],
                                  before_answer=lambda: self.assertEqual(self.state(), before))
        self.assertEqual(build_config.load("new-device")["features"], {"ENABLE_MQTT": 1})
        self.assertIn('"ENABLE_MQTT": 1', output)
        self.assertFalse(build_config.build_dir("new-device").exists())

    def test_copy_and_inherit_preserve_the_source_and_unmentioned_choices(self):
        self.success(["copy", "--from", "device"],
                     ["1", "ENABLE_MQTT", "inherit", "2", "CONFIG_BT_ENABLED", "inherit", "3", "s"])
        saved = build_config.load("copy")
        self.assertEqual(saved["board"], "xiao_s3")
        self.assertEqual(saved["features"], {"ENABLE_MAPS": 1})
        self.assertNotIn("CONFIG_BT_ENABLED", saved.get("sdkconfig", {}))
        self.assertEqual(build_config.load("device"), self.original)

    def test_guided_origin_and_name_selection_uses_saved_configuration(self):
        self.success([], ["1", "device", "copy", "3", "s"])
        self.assertEqual(build_config.load("copy"), self.original)

    def test_literal_numeric_name_is_distinct_from_menu_ordinal(self):
        for answer, expected in (("2", "other"), ("=2", "2")):
            with self.subTest(answer=answer), patch("builtins.input", return_value=answer), \
                    redirect_stdout(io.StringIO()):
                self.assertEqual(build_config_wizard.choose("Saved configurations", ["2", "other"]),
                                 expected)

    def test_literal_cancel_name_can_be_saved_while_plain_q_cancels(self):
        before = self.state()
        self.success(["--board", "xiao_s3"], ["q"])
        self.assertEqual(self.state(), before)
        self.success(["--board", "xiao_s3"], ["=q", "3", "s"])
        self.assertEqual(build_config.load("q"), {"board": "xiao_s3", "features": {}})

    def test_edit_without_name_can_select_an_existing_configuration(self):
        self.success(["--edit"], ["device", "1", "DISPLAY_TYPE", "1", "3", "s"])
        edited = build_config.load("device")
        self.assertEqual(edited["features"]["DISPLAY_TYPE"], 1)
        self.assertEqual(edited["sdkconfig"], self.original["sdkconfig"])

    def test_cancel_eof_and_interrupt_leave_existing_configuration_and_build_untouched(self):
        self.previous_build()
        before = self.state()
        for ending in ("q", EOFError(), KeyboardInterrupt()):
            with self.subTest(ending=type(ending).__name__):
                with patch.object(build_config, "build", side_effect=AssertionError("unrequested build")):
                    self.cli(["device", "--edit"], ["1", "ENABLE_MQTT", "on", ending])
                self.assertEqual(self.state(), before)

    def test_review_back_and_cancel_discard_candidate_changes(self):
        before = self.state()
        output = self.success(["device", "--edit"],
                              ["1", "ENABLE_MQTT", "on", "3", "e", "3", "q"])
        self.assertIn('"ENABLE_MQTT": 1', output)
        self.assertEqual(self.state(), before)

    def test_invalid_feature_value_retries_without_accepting_it(self):
        self.success(["new-device", "--board", "xiao_s3"],
                     ["1", "DISPLAY_TYPE", "99", "1", "3", "s"])
        self.assertEqual(build_config.load("new-device")["features"], {"DISPLAY_TYPE": 1})

    def test_blank_value_keeps_explicit_false_and_noop_preserves_formatting(self):
        self.path.write_text(json.dumps(self.original, indent=4) + "\n")
        before = self.state()
        self.success(["device", "--edit"], ["2", "CONFIG_BT_ENABLED", "", "3", "s"])
        self.assertEqual(self.state(), before)

    def test_save_and_build_uses_shared_builder_after_save_and_propagates_failure(self):
        def build_saved(name, *args, **kwargs):
            self.assertEqual(name, "new-device")
            self.assertFalse(args)
            self.assertFalse(kwargs)
            self.assertEqual(build_config.load(name)["features"], {"ENABLE_MQTT": 1})
            return 7

        with patch.object(build_config, "build", side_effect=build_saved) as builder:
            status, _, _ = self.cli(["new-device", "--board", "xiao_s3"],
                                     ["1", "ENABLE_MQTT", "on", "3", "b"])
        self.assertEqual(status, 7)
        builder.assert_called_once_with("new-device")

    def test_existing_new_name_is_rejected_before_interaction(self):
        before = self.state()
        status, _, error = self.cli(["device", "--board", "xiao_s3"], [])
        self.assertNotEqual(status, 0)
        self.assertTrue(error)
        self.assertEqual(self.state(), before)

    def test_concurrent_edit_is_preserved_and_build_is_not_started(self):
        external = dict(self.original, features={"ENABLE_MAPS": 0})

        def save_after_external_edit(_):
            self.path.write_text(build_config.encode(external))
            return "b"

        with patch.object(build_config, "build", side_effect=AssertionError("build after failed save")):
            status, _, error = self.cli(["device", "--edit"],
                                        ["1", "ENABLE_MQTT", "on", "3", save_after_external_edit])
        self.assertNotEqual(status, 0)
        self.assertTrue(error)
        self.assertEqual(build_config.load("device"), external)
        self.assertEqual(list((self.root / "configurations").iterdir()), [self.path])

    def test_new_configuration_created_while_reviewing_is_not_overwritten(self):
        external = {"board": "feathers3", "features": {"ENABLE_MQTT": 0}}
        destination = build_config.config_path("new-device")

        def create_before_save(_):
            destination.write_text(build_config.encode(external))
            return "s"

        status, _, error = self.cli(["new-device", "--board", "xiao_s3"],
                                    ["1", "ENABLE_MQTT", "on", "3", create_before_save])
        self.assertNotEqual(status, 0)
        self.assertTrue(error)
        self.assertEqual(build_config.load("new-device"), external)
        self.assertEqual({p.name for p in destination.parent.iterdir()}, {"device.json", "new-device.json"})
        self.assertFalse(build_config.build_dir("new-device").exists())

    def test_choices_added_to_shared_schema_are_available_without_wizard_registry(self):
        header = self.root / "components/hardwareone/System_BuildConfig.h"
        header.write_text("#define ENABLE_GUIDED_TEST 0\n" + header.read_text())
        policy = self.root / "presets/sdkconfig.defaults"
        policy.write_text(policy.read_text() + "CONFIG_GUIDED_TEST=n\n")
        self.success(["new-device", "--board", "xiao_s3"],
                     ["1", "ENABLE_GUIDED_TEST", "on", "2", "CONFIG_GUIDED_TEST", "on", "3", "s"])
        saved = build_config.load("new-device")
        self.assertEqual(saved["features"], {"ENABLE_GUIDED_TEST": 1})
        self.assertEqual(saved["sdkconfig"], {"CONFIG_GUIDED_TEST": True})


if __name__ == "__main__":
    unittest.main()
