#!/usr/bin/env python3
"""The guided setup's catalogue against this checkout: every switch and board has a plain name, the
saved configurations say what they are for, and the header accepts what the catalogue offers."""

from pathlib import Path
import shutil
import sys
import unittest

REPO = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO / "tools"))
import build_config
import build_config_resolve
from build_config_catalog import Refused, Setup

HAVE_COMPILER = unittest.skipUnless(shutil.which("c++"), "needs a host C++ preprocessor")


class Catalogue(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.setup = Setup(build_config)
        cls.catalog = cls.setup.catalog
        cls.schema = build_config.choice_schema()

    def test_every_switch_has_exactly_one_plain_name(self):
        editable = set(self.schema["features"])
        options = [row["switch"] for row in self.catalog["options"]]
        owned = {key for row in self.catalog["choices"] for value in row["values"] for key in value["set"]}
        managed = set(self.catalog["managed"])
        self.assertEqual(len(options), len(set(options)), "an option is listed twice")
        self.assertFalse(set(options) & owned, "a switch is both an option and part of a choice")
        self.assertFalse((set(options) | owned) & managed)
        self.assertEqual(set(options) | owned | managed, editable,
                         "add new switches to tools/build_config_catalog.json and drop removed ones")
        self.assertEqual(self.setup.uncatalogued(), [])

    def test_entries_are_complete_and_point_at_real_things(self):
        groups = [group["id"] for group in self.catalog["groups"]]
        self.assertEqual(len(groups), len(set(groups)))
        options = {row["switch"]: row for row in self.catalog["options"]}
        titles = [row["title"] for row in self.catalog["options"] + self.catalog["choices"]]
        self.assertEqual(len(titles), len(set(titles)), "two entries share a title")
        for row in self.catalog["options"] + self.catalog["choices"]:
            with self.subTest(entry=row["title"]):
                self.assertIn(row["group"], groups)
                self.assertTrue(row["title"].strip() and row["summary"].strip())
                self.assertEqual(self.schema["features"].get(row.get("switch"), {"maximum": 1})["maximum"], 1,
                                 "an option is an on/off switch")
                for key in row.get("needs", []) + row.get("off_with", []):
                    self.assertIn(key, options)
                for key in row.get("sdk", []) + row.get("sdk_on", []):
                    self.assertIn(key, self.schema["sdkconfig"])
                self.assertLessEqual(set(row.get("boards", [])), set(build_config.board_choices()))
                self.assertLessEqual(set(row.get("targets", [])), set(build_config.board_choices().values()))
        for row in self.catalog["choices"]:
            for value in row["values"]:
                for key, number in value["set"].items():
                    spec = self.schema["features"][key]
                    self.assertTrue(spec["minimum"] <= number <= spec["maximum"], f"{row['id']}: {key}={number}")

    def test_every_board_is_described(self):
        boards = self.catalog["boards"]
        self.assertEqual(set(boards), set(build_config.board_choices()))
        shorts = [row["short"] for row in boards.values()]
        self.assertEqual(len(shorts), len(set(shorts)))
        for name, row in boards.items():
            with self.subTest(board=name):
                self.assertTrue(row["title"].strip() and row["hardware"].strip())
                build_config.checked_name(row["short"] + "-custom")

    def test_custom_toggles_are_tied_to_their_level_and_derived_switch(self):
        custom = {name for name in self.schema["features"] if name.startswith("CUSTOM_ENABLE_")}
        self.assertEqual(set(build_config_resolve.level_groups(REPO)), custom)
        self.assertEqual(set(build_config_resolve.derived_names(REPO)), custom)
        self.assertEqual(set(build_config_resolve.level_groups(REPO).values()), set(self.catalog["managed"]))

    def test_saved_configurations_say_what_they_are_for(self):
        rows = build_config.list_choices("configurations")
        self.assertTrue(rows)
        seen = set()
        for row in rows:
            with self.subTest(configuration=row["name"]):
                self.assertTrue(row.get("title") and row.get("summary"),
                                "give it a title and a summary; the setup lists it as a starting point")
                self.assertNotIn((row["board"], row["title"]), seen)
                seen.add((row["board"], row["title"]))

    @HAVE_COMPILER
    def test_the_header_accepts_every_saved_configuration(self):
        for row in build_config.list_choices("configurations"):
            with self.subTest(configuration=row["name"]):
                values, messages = self.setup.evaluate(build_config.load(row["name"]))
                self.assertTrue(values)
                self.assertEqual(messages, ())

    @HAVE_COMPILER
    def test_every_item_is_on_or_can_be_switched_on_for_some_board(self):
        reachable, reasons = set(), {}
        for board in build_config.board_choices():
            config = {"board": board, "features": {}}
            values, _ = self.setup.evaluate(config)
            self.assertTrue(values, f"{board}: the header's values could not be read")
            offered = {switch for switch, row in self.setup.options.items() if self.setup.offered(config, row)}
            off = {switch: (lambda switch=switch: self.setup.change_option(config, switch, True))
                   for switch in offered if not self.setup.is_on(values, switch)}
            blocked = self.setup.blocked(config, off)
            reachable |= offered - set(blocked)
            reasons.update({switch: f"{board}: {reason}" for switch, reason in blocked.items()})
            # An item that cannot be switched on by itself still counts when an item that needs it
            # brings it on: the assistant (off in the shared defaults since 2026-11-20) comes on
            # with its first answer source, as its hint says.
            for switch in set(blocked) & offered:
                for other, row in self.setup.options.items():
                    if switch not in row.get("needs", ()) or other in blocked or other not in off:
                        continue
                    brought, _ = self.setup.change_option(config, other, True)
                    values_after, messages = self.setup.evaluate(brought)
                    if not messages and self.setup.is_on(values_after, switch):
                        self.assertTrue(self.setup.options[switch].get("hint"),
                                        f"{self.setup.title(switch)} only comes on with another item: "
                                        "give it a hint that says so")
                        reachable.add(switch)
                        break
        missing = {self.setup.title(switch): reasons.get(switch, "offered on no board")
                   for switch in set(self.setup.options) - reachable}
        self.assertEqual(missing, {}, "an item no board can have: its needs or board list are wrong")

    # G-15 (2026-11-20): HTTPS, the bonded pair and the assistant are off in the shared defaults; a
    # user gets them from the guided setup. From a bare board each is offered under a plain name,
    # starts at off, and can be switched on with what it needs.
    OPT_IN = ("ENABLE_HTTPS", "ENABLE_BONDED_MODE", "ENABLE_LLM_BACKEND", "ENABLE_LLM_SOURCE_ONBOARD",
              "ENABLE_LLM_SOURCE_CM5", "ENABLE_LLM_SOURCE_HTTP")

    @HAVE_COMPILER
    def test_https_bonded_pair_and_assistant_start_off_on_a_bare_board(self):
        header = (REPO / "components/hardwareone/System_BuildConfig.h").read_text().split(
            "// CHECKED-IN DEPLOYMENT PROFILE OVERRIDE", 1)[0]
        for switch in self.OPT_IN:
            with self.subTest(shared_default=switch):
                self.assertRegex(header, rf"(?m)^#define {switch}\s+0\b", "the shared default is off")
                row = self.setup.options[switch]
                self.assertTrue(row["title"].strip() and row["summary"].strip())
                self.assertNotIn("ENABLE_", row["title"] + row["summary"], "plain words, no switch names")
        for board in build_config.board_choices():
            config = {"board": board, "features": {}}
            values, _ = self.setup.evaluate(config)
            self.assertTrue(values, board)
            for switch in self.OPT_IN:
                with self.subTest(board=board, switch=switch):
                    self.assertTrue(self.setup.offered(config, self.setup.options[switch]))
                    self.assertFalse(self.setup.is_on(values, switch), "off until the user chooses it")

    @HAVE_COMPILER
    def test_the_setup_switches_them_on_with_what_they_need(self):
        config = {"board": "xiao_s3", "features": {}}
        self.assertEqual(self.setup.evaluate(config)[1], ())
        # HTTPS and the bonded pair: one step each.
        for switch in ("ENABLE_HTTPS", "ENABLE_BONDED_MODE"):
            with self.subTest(switch=switch):
                chosen, _ = self.setup.change_option(config, switch, True)
                values, messages = self.setup.evaluate(chosen)
                self.assertEqual(messages, ())
                self.assertTrue(self.setup.is_on(values, switch))
                self.assertEqual(chosen["features"][switch], 1)
        # The assistant: not by itself (the header wants a source), and the refusal says what to do.
        with self.assertRaises(Refused) as refused:
            self.setup.change_option(config, "ENABLE_LLM_BACKEND", True)
        self.assertIn("answer sources", str(refused.exception))
        # Each answer source brings the assistant on with it and resolves.
        for source in ("ENABLE_LLM_SOURCE_HTTP", "ENABLE_LLM_SOURCE_CM5", "ENABLE_LLM_SOURCE_ONBOARD"):
            with self.subTest(source=source):
                chosen, notes = self.setup.change_option(config, source, True)
                values, messages = self.setup.evaluate(chosen)
                self.assertEqual(messages, ())
                self.assertTrue(self.setup.is_on(values, source))
                self.assertTrue(self.setup.is_on(values, "ENABLE_LLM_BACKEND"))
                self.assertEqual(chosen["features"]["ENABLE_LLM_BACKEND"], 1)
                self.assertTrue(any("Assistant" in note for note in notes), notes)
                # The other sources stay off: nothing comes on that was not chosen.
                for other in set(self.OPT_IN) - {source, "ENABLE_LLM_BACKEND"}:
                    self.assertFalse(self.setup.is_on(values, other), other)
        # And off again: the last source takes the assistant with it or is refused with a reason.
        chosen, _ = self.setup.change_option(config, "ENABLE_LLM_SOURCE_HTTP", True)
        back, _ = self.setup.change_option(chosen, "ENABLE_LLM_BACKEND", False)
        values, messages = self.setup.evaluate(back)
        self.assertEqual(messages, ())
        self.assertFalse(self.setup.is_on(values, "ENABLE_LLM_BACKEND"))
        self.assertFalse(self.setup.is_on(values, "ENABLE_LLM_SOURCE_HTTP"))

    @HAVE_COMPILER
    def test_saved_configurations_do_not_lean_on_the_shared_defaults_for_them(self):
        # A saved configuration that has one of them on says so itself (the starting points of the
        # setup keep what they had before the defaults went off), and both daily P4 configurations
        # are untouched by the change: all five are off or written in the file or the board profile.
        for row in build_config.list_choices("configurations"):
            config = build_config.load(row["name"])
            values, messages = self.setup.evaluate(config)
            self.assertEqual(messages, (), row["name"])
            board_profile = (REPO / f"boards/{config['board']}.features.h")
            profile = board_profile.read_text() if board_profile.is_file() else ""
            for switch in self.OPT_IN:
                if self.setup.is_on(values, switch):
                    with self.subTest(configuration=row["name"], switch=switch):
                        self.assertEqual(config["features"].get(switch), 1,
                                         "on in the resolved build but not written in the configuration")
                elif row["name"] in ("p4-portable", "p4-home-node") and switch != "ENABLE_LLM_SOURCE_HTTP":
                    with self.subTest(configuration=row["name"], switch=switch):
                        self.assertTrue(config["features"].get(switch) == 0 or
                                        f"#define {switch} 0" in profile,
                                        "off, and written as off in the file or the board profile")

    # What each saved configuration resolves to for the six switches of OPT_IN, in that order. These
    # are the values from before the shared defaults went off (2026-11-20): the QT Py and XIAO files
    # write the switches themselves to keep them. A line dropped from one of those files resolves to
    # off without any other message, so the values are pinned here. Change a row only together with
    # the configuration it describes (the owner decides which of them really lose a feature).
    SAVED_OPT_IN = {
        "p4-glasses-test": (0, 0, 0, 0, 0, 0),
        "p4-home-node":    (0, 0, 0, 0, 0, 0),
        "p4-openclaw":     (0, 0, 1, 0, 0, 1),
        "p4-phone-pipe":   (0, 0, 1, 0, 0, 1),
        "p4-portable":     (0, 0, 1, 0, 0, 1),
        "qtpy-espnow":     (1, 1, 0, 0, 0, 0),
        "qtpy-oled":       (1, 0, 0, 0, 0, 0),
        "qtpy-sensors":    (1, 0, 0, 0, 0, 0),
        "xiao-base":       (1, 1, 1, 1, 1, 0),
        "xiao-default":    (1, 1, 1, 1, 1, 0),
        "xiao-display":    (1, 1, 1, 0, 1, 0),
        "xiao-mqtt":       (1, 1, 1, 1, 1, 0),
        "xiao-openclaw":   (1, 1, 1, 0, 1, 1),
        "xiao-status-sign": (1, 0, 0, 0, 0, 0),  # added 2026-11-22: HTTPS only
    }

    @HAVE_COMPILER
    def test_saved_configurations_keep_https_bonded_pair_and_assistant_as_they_were(self):
        names = [row["name"] for row in build_config.list_choices("configurations")]
        self.assertEqual(sorted(names), sorted(self.SAVED_OPT_IN),
                         "a saved configuration was added or removed: give it a row in SAVED_OPT_IN")
        for name in names:
            values, messages = self.setup.evaluate(build_config.load(name))
            self.assertEqual(messages, (), name)
            with self.subTest(configuration=name):
                self.assertEqual(tuple(int(self.setup.is_on(values, switch)) for switch in self.OPT_IN),
                                 self.SAVED_OPT_IN[name],
                                 "resolved " + ", ".join(self.OPT_IN) + " differ from the pinned values")

    @HAVE_COMPILER
    def test_a_rejected_board_default_can_be_repaired_from_the_checklist(self):
        # The Waveshare board has no UART link, and the shared defaults leave the Pi features on.
        config = {"board": "waveshare_p4_touch_lcd_5", "features": {}}
        values, messages = self.setup.evaluate(config)
        if not messages:
            self.skipTest("the bare Waveshare selection is accepted now")
        self.assertTrue(values)
        # A change that adds a message of its own is still refused while the selection is being repaired.
        model = next(row for row in self.setup.choices if row["id"] == "speech_model")
        with self.assertRaises(Refused):
            self.setup.change_choice(config, model, 1)
        for switch in ("ENABLE_LLM_SOURCE_CM5", "ENABLE_RASPBERRY_PI_HOST_POWER", "ENABLE_RASPBERRY_PI_HOST_FAN"):
            if self.setup.is_on(self.setup.evaluate(config)[0], switch):
                config, _ = self.setup.change_option(config, switch, False)
        self.assertEqual(self.setup.evaluate(config)[1], ())


if __name__ == "__main__":
    unittest.main()
