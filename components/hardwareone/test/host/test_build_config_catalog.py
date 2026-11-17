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
        missing = {self.setup.title(switch): reasons.get(switch, "offered on no board")
                   for switch in set(self.setup.options) - reachable}
        self.assertEqual(missing, {}, "an item no board can have: its needs or board list are wrong")

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
