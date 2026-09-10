"""Behavioral coverage for interactive-help state in the shipping CLI page.

The browser cannot infer the CLI's mode from command spelling: ``help espnow``
enters help directly, module-level ``back`` remains in help, and an ordinary
command may pass through help and leave the mode.  The server therefore returns
``X-HW1-CLI-Help: active|inactive`` and the page must reconcile its runtime and
persisted state from that response.

This test executes the JavaScript extracted from ``WebPage_CLI.h`` against a
small fake DOM and scripted HTTP Responses.  It does not copy the page logic.

Run from the repository root:

    python3 -m unittest tools.webui.tests.test_cli_page
"""

from __future__ import annotations

import pathlib
import tempfile
import unittest

from tools.webui import extract_js
from tools.webui.js_engine import JS_ENGINE, run_js


REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
PAGE = REPO_ROOT / "components" / "hardwareone" / "WebPage_CLI.h"
HARNESS = REPO_ROOT / "tools" / "webui" / "harness" / "cli_page_harness.js"
EXPECTED_CHECKS = 50


class CliPageExtractionTests(unittest.TestCase):
    """Fail loudly if extraction or the response-aware transport regresses."""

    def test_shipping_page_exposes_response_headers_to_the_handler(self) -> None:
        page_js, _ = extract_js.page_js(PAGE)
        self.assertGreater(len(page_js), 8_000, "extracted CLI JS is implausibly small")
        self.assertNotIn("<script", page_js, "script tags must be removed before execution")
        self.assertIn("function executeCommand", page_js)
        self.assertIn("X-HW1-CLI-Help", page_js)
        self.assertIn("hw.postForm('/api/cli'", page_js)
        self.assertNotIn(
            "hw.postFormText('/api/cli'",
            page_js,
            "postFormText discards X-HW1-CLI-Help and cannot reconcile mode",
        )


@unittest.skipUnless(
    JS_ENGINE, "a JavaScript engine (node/deno/bun/qjs/jsc/osascript) is required"
)
class CliPageBehaviorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory()
        extracted = pathlib.Path(cls.temporary.name) / "cli_page.js"
        page_js, _line_map = extract_js.page_js(PAGE)
        extracted.write_text(page_js, encoding="utf-8")
        cls.result = run_js(HARNESS, [str(extracted)])
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
        self.assertEqual(
            1,
            len(verdicts),
            f"expected one harness verdict\n{self.result.stdout}{self.result.stderr}",
        )

    def test_all_shipping_behaviors_pass(self) -> None:
        verdicts = [line for line in self.lines if line.startswith("HARNESS_RESULT ")]
        if not verdicts:
            self.skipTest("verdict shape is covered separately")
        failures = [line for line in self.lines if line.startswith("FAIL ")]
        self.assertEqual(
            "HARNESS_RESULT PASS",
            verdicts[0],
            "CLI help-state reconciliation regressed:\n  " + "\n  ".join(failures),
        )

    def test_expected_number_of_checks_ran(self) -> None:
        checks = [
            line
            for line in self.lines
            if line.startswith("PASS ") or line.startswith("FAIL ")
        ]
        self.assertEqual(
            EXPECTED_CHECKS,
            len(checks),
            f"ran {len(checks)} checks, expected {EXPECTED_CHECKS}\n"
            f"{self.result.stdout}",
        )


if __name__ == "__main__":
    unittest.main()
