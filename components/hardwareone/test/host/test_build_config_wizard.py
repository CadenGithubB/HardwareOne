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
        sources = ["boards/xiao_s3.defaults", "boards/feathers3.defaults", "presets/sdkconfig.defaults",
                   "components/hardwareone/System_BuildConfig.h"] + self.extra_sources()
        for source in sources:
            destination = self.root / source
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(REPO / source, destination)
        (self.root / "configurations").mkdir()
        self.original = {"board": "xiao_s3", "features": {"ENABLE_MQTT": 0, "ENABLE_MAPS": 1},
                         "sdkconfig": {"CONFIG_BT_ENABLED": False}}
        self.path = build_config.config_path("device")
        self.path.write_text(build_config.encode(self.original))

    def extra_sources(self):
        return []

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
                                  ["names", "ENABLE_MQTT", "on", "review", "s"],
                                  before_answer=lambda: self.assertEqual(self.state(), before))
        self.assertEqual(build_config.load("new-device")["features"], {"ENABLE_MQTT": 1})
        self.assertIn('"ENABLE_MQTT": 1', output)
        self.assertFalse(build_config.build_dir("new-device").exists())

    def test_copy_and_inherit_preserve_the_source_and_unmentioned_choices(self):
        self.success(["copy", "--from", "device"],
                     ["names", "ENABLE_MQTT", "inherit", "sdk", "CONFIG_BT_ENABLED", "inherit", "review", "s"])
        saved = build_config.load("copy")
        self.assertEqual(saved["board"], "xiao_s3")
        self.assertEqual(saved["features"], {"ENABLE_MAPS": 1})
        self.assertNotIn("CONFIG_BT_ENABLED", saved.get("sdkconfig", {}))
        self.assertEqual(build_config.load("device"), self.original)

    def test_board_then_saved_configuration_then_name(self):
        # Board first, then a saved configuration of that board to start from; the name comes at review.
        output = self.success([], ["xiao_s3", "device", "review", "copy", "", "s"])
        self.assertLess(output.index("1. Board"), output.index("2. What should it do?"))
        self.assertEqual(build_config.load("copy"), self.original)

    def test_first_screen_can_change_a_saved_configuration_instead(self):
        self.success([], ["edit", "device", "names", "ENABLE_MAPS", "off", "review", "s"])
        self.assertEqual(build_config.load("device")["features"], {"ENABLE_MQTT": 0, "ENABLE_MAPS": 0})

    def test_new_is_the_same_guided_setup(self):
        with patch.object(sys, "argv", ["build_config.py", "new", "from-new", "--board", "xiao_s3"]), \
                patch("builtins.input", side_effect=["review", "s"]), redirect_stdout(io.StringIO()):
            self.assertEqual(build_config.main(), 0)
        self.assertEqual(build_config.load("from-new"), {"board": "xiao_s3", "features": {}})

    def test_copy_does_not_take_the_title_or_summary(self):
        described = dict(self.original, title="Bench device", summary="What the bench device is for.")
        self.path.write_text(build_config.encode(described))
        output = self.success([], ["xiao_s3", "bench device", "review", "", "My own selection", "s"])
        self.assertIn("Bench device: What the bench device is for.", output)
        self.assertEqual(build_config.load("xiao-custom"), dict(self.original, summary="My own selection"))
        self.assertEqual(build_config.load("device"), described)

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
        self.success(["--board", "xiao_s3"], ["review", "=q", "", "s"])
        self.assertEqual(build_config.load("q"), {"board": "xiao_s3", "features": {}})

    def test_edit_without_name_can_select_an_existing_configuration(self):
        self.success(["--edit"], ["device", "names", "DISPLAY_TYPE", "1", "review", "s"])
        edited = build_config.load("device")
        self.assertEqual(edited["features"]["DISPLAY_TYPE"], 1)
        self.assertEqual(edited["sdkconfig"], self.original["sdkconfig"])

    def test_cancel_eof_and_interrupt_leave_existing_configuration_and_build_untouched(self):
        self.previous_build()
        before = self.state()
        for ending in ("q", EOFError(), KeyboardInterrupt()):
            with self.subTest(ending=type(ending).__name__):
                with patch.object(build_config, "build", side_effect=AssertionError("unrequested build")):
                    self.cli(["device", "--edit"], ["names", "ENABLE_MQTT", "on", ending])
                self.assertEqual(self.state(), before)

    def test_review_back_and_cancel_discard_candidate_changes(self):
        before = self.state()
        output = self.success(["device", "--edit"],
                              ["names", "ENABLE_MQTT", "on", "review", "e", "review", "q"])
        self.assertIn('"ENABLE_MQTT": 1', output)
        self.assertEqual(self.state(), before)

    def test_invalid_feature_value_retries_without_accepting_it(self):
        self.success(["new-device", "--board", "xiao_s3"],
                     ["names", "DISPLAY_TYPE", "99", "1", "review", "s"])
        self.assertEqual(build_config.load("new-device")["features"], {"DISPLAY_TYPE": 1})

    def test_blank_value_keeps_explicit_false_and_noop_preserves_formatting(self):
        self.path.write_text(json.dumps(self.original, indent=4) + "\n")
        before = self.state()
        self.success(["device", "--edit"], ["sdk", "CONFIG_BT_ENABLED", "", "review", "s"])
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
                                     ["names", "ENABLE_MQTT", "on", "review", "b"])
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
                                        ["names", "ENABLE_MQTT", "on", "review", save_after_external_edit])
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
                                    ["names", "ENABLE_MQTT", "on", "review", create_before_save])
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
                     ["names", "ENABLE_GUIDED_TEST", "on", "sdk", "CONFIG_GUIDED_TEST", "on", "review", "s"])
        saved = build_config.load("new-device")
        self.assertEqual(saved["features"], {"ENABLE_GUIDED_TEST": 1})
        self.assertEqual(saved["sdkconfig"], {"CONFIG_GUIDED_TEST": True})


@unittest.skipUnless(shutil.which("c++"), "the checklist asks a host C++ preprocessor for the header's values")
class BoardFirstChecklist(GuidedConfiguration):
    """The grouped checklist: plain names, the header's own states and rules, for the chosen board."""

    def extra_sources(self):
        # Everything the header evaluation reads for these boards, as the firmware build layers it.
        files = [path for pattern in ("boards/*.features.h", "boards/p4x_eye.defaults", "config/sdkconfig.defaults*",
                                      "presets/*.sdkconfig.defaults", "components/hardwareone/System_Board_*.h")
                 for path in REPO.glob(pattern)]
        return [str(path.relative_to(REPO)) for path in files]

    def new(self, board, answers):
        """Start from the board's defaults (no saved configuration of p4x_eye exists here)."""
        start = [board] + (["board defaults only"] if board == "xiao_s3" else [])
        return self.success([], start + answers)

    def test_plain_names_switch_an_item_and_save_under_the_suggested_name(self):
        output = self.new("xiao_s3", ["network", "mqtt (home assistant)", "", "review", "", "", "s"])
        self.assertIn("[ ] MQTT (Home Assistant)", output)
        self.assertIn("MQTT (Home Assistant): on", output)
        self.assertEqual(build_config.load("xiao-custom"), {"board": "xiao_s3", "features": {"ENABLE_MQTT": 1}})

    def test_board_without_saved_configuration_starts_from_its_defaults(self):
        output = self.success([], ["p4x_eye", "review", "", "", "s"])
        self.assertIn("No saved configuration for this board yet", output)
        self.assertEqual(build_config.load("p4-custom"), {"board": "p4x_eye", "features": {}})

    def test_prerequisites_come_on_with_the_item_and_are_named(self):
        output = self.new("xiao_s3", ["web pages", "mqtt settings page", "", "review", "", "", "s"])
        self.assertIn("turned on with it: MQTT (Home Assistant)", output)
        self.assertEqual(build_config.load("xiao-custom")["features"],
                         {"ENABLE_MQTT": 1, "CUSTOM_ENABLE_WEB_MQTT": 1})

    def test_a_change_the_header_rejects_is_refused_with_its_message(self):
        # The XIAO camera owns I2C controller 1: an OLED on external I2C needs the camera off first.
        output = self.new("xiao_s3", ["display and input", "display", "why", "", "", "q"])
        self.assertIn("SSD1306 OLED on I2C: External I2C uses controller 1, reserved by camera SCCB", output)
        self.assertIn('ENABLE_CAMERA_SENSOR ("Camera")', output)
        output = self.new("xiao_s3", ["sensors and add-ons", "gps (pa1010d)", "", "review", "", "", "s"])
        self.assertIn("[-] GPS (PA1010D)", output)
        self.assertIn("Not changed: External I2C uses controller 1", output)
        self.assertEqual(build_config.load("xiao-custom")["features"], {})

    def test_one_item_of_a_level_group_keeps_the_rest_of_the_group(self):
        # The P4X-EYE states its network as level 3; switching one item pins the level and its siblings.
        self.success([], ["p4x_eye", "network", "esp-now mesh", "", "review", "", "", "s"])
        self.assertEqual(build_config.load("p4-custom")["features"],
                         {"NETWORK_FEATURE_LEVEL": 4, "CUSTOM_ENABLE_NET_WIFI": 1, "CUSTOM_ENABLE_NET_HTTP": 1,
                          "CUSTOM_ENABLE_NET_ESPNOW": 0})

    def test_bluetooth_takes_its_sdk_stack_with_it(self):
        output = self.new("xiao_s3", ["bluetooth and wearables", "bluetooth", "", "review", "", "", "s"])
        self.assertIn("SDK choice CONFIG_BT_ENABLED turned off with it", output)
        self.assertIn("now off as well:", output)
        saved = build_config.load("xiao-custom")
        self.assertEqual(saved["features"], {"ENABLE_BLUETOOTH": 0})
        self.assertEqual(saved["sdkconfig"], {"CONFIG_BT_ENABLED": False})

    def test_only_what_the_board_can_have_is_listed(self):
        xiao = self.new("xiao_s3", ["voice", "", "q"])
        self.assertNotIn("Speech-to-text on the board", xiao)
        self.assertIn("Wake word and voice commands", xiao)
        eye = self.success([], ["p4x_eye", "voice", "speech-to-text on the board", "", "review", "", "", "s"])
        self.assertIn("turned on with it: microSD card (SDMMC slot)", eye)
        saved = build_config.load("p4-custom")
        self.assertEqual(saved["features"], {"ENABLE_LOCAL_STT": 1, "ENABLE_SDMMC_CARD": 1})
        self.assertEqual(saved["sdkconfig"], {"CONFIG_HEAP_USE_HOOKS": True})

    def test_a_multi_value_choice_sets_its_switches_together(self):
        self.new("xiao_s3", ["board hardware", "camera", "", "display and input", "display",
                             "ssd1306 oled on i2c", "", "review", "", "", "s"])
        features = build_config.load("xiao-custom")["features"]
        self.assertEqual({key: features[key] for key in ("ENABLE_CAMERA_SENSOR", "DISPLAY_TYPE", "I2C_FEATURE_LEVEL",
                                                         "CUSTOM_ENABLE_OLED", "CUSTOM_ENABLE_GAMEPAD")},
                         {"ENABLE_CAMERA_SENSOR": 0, "DISPLAY_TYPE": 1, "I2C_FEATURE_LEVEL": 4,
                          "CUSTOM_ENABLE_OLED": 1, "CUSTOM_ENABLE_GAMEPAD": 0})


if __name__ == "__main__":
    unittest.main()
