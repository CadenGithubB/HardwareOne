"""Behavioral coverage for the shipping Dashboard notification-kind editor."""

from __future__ import annotations

import pathlib
import re
import tempfile
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
PAGE = REPO_ROOT / "components" / "hardwareone" / "WebPage_Dashboard.h"
LIMITS = REPO_ROOT / "components" / "hardwareone" / "System_CommandLimits.h"
HARNESS = REPO_ROOT / "tools" / "webui" / "harness" / "notification_kind_editor_harness.js"
EXPECTED_CHECKS = 23

_RAW_BLOCK_RE = re.compile(
    r"kDashboardNotificationKindEditorJs\[\]\s*=\s*"
    r'R"DASHNOTIFJS\((.*?)\)DASHNOTIFJS";',
    re.DOTALL,
)
_INPUT_LIMIT_RE = re.compile(r"\bCMD_INPUT_MAX\s*=\s*(\d+)\s*;")


def shipping_editor_js() -> str:
    source = PAGE.read_text(encoding="utf-8")
    matches = _RAW_BLOCK_RE.findall(source)
    if len(matches) != 1:
        raise AssertionError(
            "expected exactly one kDashboardNotificationKindEditorJs raw block, "
            f"found {len(matches)}"
        )
    return matches[0]


def production_input_limit() -> int:
    source = LIMITS.read_text(encoding="utf-8")
    match = _INPUT_LIMIT_RE.search(source)
    if not match:
        raise AssertionError("CMD_INPUT_MAX numeric contract was not found")
    return int(match.group(1))


class NotificationKindEditorExtractionTests(unittest.TestCase):
    def test_named_block_contains_shipping_planner_and_recovery(self) -> None:
        js = shipping_editor_js()
        self.assertGreater(len(js), 8_000, "notification editor JS is implausibly small")
        self.assertIn("buildMutationCommands", js)
        self.assertIn("/api/cli/batch", js)
        self.assertIn("reloadAfterFailure", js)
        self.assertNotIn("postFormText", js)

    def test_page_injects_the_shared_native_command_limit(self) -> None:
        page = PAGE.read_text(encoding="utf-8")
        self.assertIn('#include "System_CommandLimits.h"', page)
        self.assertIn("String((unsigned long)CMD_INPUT_MAX)", page)
        self.assertGreater(production_input_limit(), 0)


@unittest.skipUnless(
    JS_ENGINE, "a JavaScript engine (node/deno/bun/qjs/jsc/osascript) is required"
)
class NotificationKindEditorBehaviorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory()
        extracted = pathlib.Path(cls.temporary.name) / "notification_editor.js"
        extracted.write_text(shipping_editor_js(), encoding="utf-8")
        cls.result = run_js(
            HARNESS,
            [str(extracted), str(production_input_limit())],
        )
        cls.lines = cls.result.stdout.splitlines()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary.cleanup()

    def test_engine_did_not_crash(self) -> None:
        self.assertEqual(
            0,
            self.result.returncode,
            f"JS engine {JS_ENGINE} exited {self.result.returncode}\n"
            f"stderr:\n{self.result.stderr}\nstdout:\n{self.result.stdout}",
        )

    def test_harness_reported_one_verdict(self) -> None:
        verdicts = [line for line in self.lines if line.startswith("HARNESS_RESULT ")]
        self.assertEqual(1, len(verdicts), self.result.stdout + self.result.stderr)

    def test_all_shipping_behaviors_pass(self) -> None:
        verdicts = [line for line in self.lines if line.startswith("HARNESS_RESULT ")]
        if not verdicts:
            self.skipTest("verdict shape is covered separately")
        failures = [line for line in self.lines if line.startswith("FAIL ")]
        self.assertEqual(
            "HARNESS_RESULT PASS",
            verdicts[0],
            "notification editor regressed:\n  " + "\n  ".join(failures),
        )

    def test_expected_number_of_checks_ran(self) -> None:
        checks = [
            line for line in self.lines
            if line.startswith("PASS ") or line.startswith("FAIL ")
        ]
        self.assertEqual(
            EXPECTED_CHECKS,
            len(checks),
            f"ran {len(checks)} checks, expected {EXPECTED_CHECKS}\n{self.result.stdout}",
        )


if __name__ == "__main__":
    unittest.main()
