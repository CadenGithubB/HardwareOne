"""Behavioral test for the device's /llm chat page JavaScript.

WHAT IS COVERED
    The page's client-side control logic, run as the REAL shipping source
    extracted from components/hardwareone/WebPage_LLM.h -- not a copy, not a
    reimplementation. tools/webui/harness/llm_page_harness.js supplies a stub
    DOM, a virtual clock and a scriptable fake device, then drives the page
    through the scenario that motivated the tests: a CM5 co-processor whose
    model catalog arrives asynchronously over UART and whose model "load" is a
    host-side llama-server restart that finishes long after the HTTP request
    that asked for it returned.

    That scenario is the one the firmware build cannot check at all. To the C++
    compiler the whole page is an opaque string literal, and the behaviors it
    exercises -- poll cadence, announcement de-duplication, catalog diffing,
    standing down while the device is generating -- are precisely the ones a
    later edit breaks silently.

WHAT IS NOT COVERED
    Rendering, layout, CSS, real event-loop ordering, and anything that needs a
    browser. The harness's promise stub resolves synchronously, so it asserts an
    ordering a browser never produces; see the comment on P() in the harness.
    Server-side behavior belongs in the firmware, not here.

REQUIREMENTS
    A JavaScript engine: node, deno, bun, qjs, jsc or osascript. The suite skips
    cleanly when none is present, the same way tools/ota/tests/test_bundle.py
    skips without openssl. On macOS osascript always exists, so the skip path
    realistically only fires on Linux.

Run from the repository root:

    python3 -m unittest discover -s tools/webui/tests -t .
"""

from __future__ import annotations

import json
import pathlib
import re
import tempfile
import unittest

from tools.webui import extract_js
from tools.webui.js_engine import JS_ENGINE, run_js

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
PAGE = REPO_ROOT / "components" / "hardwareone" / "WebPage_LLM.h"
HARNESS = REPO_ROOT / "tools" / "webui" / "harness" / "llm_page_harness.js"

# Bumped deliberately when checks are added. A harness that silently stops
# running checks -- an early return, a throw between sections -- would otherwise
# report PASS on whatever it happened to reach.
EXPECTED_CHECKS = 61

# Element ids are declared in the page's R"HTML( literal and consumed from its
# R"JS( literal. Nothing in the build makes the two agree, so the harness is
# handed the declared set and returns null for anything else, exactly as a
# browser does.
_ID_RE = re.compile(r"""\bid=['"]([A-Za-z0-9_-]+)['"]""")
_JS_DELIM_RE = re.compile(r"JS|SCRIPT", re.IGNORECASE)


def declared_element_ids(path: pathlib.Path) -> list[str]:
    """Ids declared in the page's markup literals (everything but the JS blocks)."""
    ids: set[str] = set()
    for block in extract_js.extract_blocks(path):
        if _JS_DELIM_RE.search(block.delim):
            continue
        ids.update(_ID_RE.findall(block.body))
    return sorted(ids)


@unittest.skipUnless(
    JS_ENGINE, "a JavaScript engine (node/deno/bun/qjs/jsc/osascript) is required"
)
class LlmPageBehaviorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory()
        root = pathlib.Path(cls.temporary.name)

        page_js, _line_map = extract_js.page_js(PAGE)
        cls.page_js_path = root / "page.js"
        cls.page_js_path.write_text(page_js, encoding="utf-8")

        ids = declared_element_ids(PAGE)
        cls.ids_path = root / "ids.json"
        cls.ids_path.write_text(json.dumps(ids), encoding="utf-8")

        cls.result = run_js(HARNESS, [str(cls.page_js_path), str(cls.ids_path)])
        cls.stdout_lines = cls.result.stdout.splitlines()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary.cleanup()

    def test_engine_did_not_crash(self) -> None:
        """A non-zero return code means the engine itself died, not a failed check.

        Exit codes are useless for pass/fail here -- they disagree across engines
        and jsc cannot report failure at all -- so this asserts only that the
        harness got to run. Everything else reads the stdout sentinel.
        """
        self.assertEqual(
            0,
            self.result.returncode,
            f"JS engine {JS_ENGINE} exited {self.result.returncode}\n"
            f"stderr:\n{self.result.stderr}\nstdout:\n{self.result.stdout}",
        )

    def test_harness_reported_a_verdict(self) -> None:
        """Exactly one terminal sentinel: truncated output must not score green."""
        verdicts = [l for l in self.stdout_lines if l.startswith("HARNESS_RESULT ")]
        self.assertEqual(
            1,
            len(verdicts),
            f"expected exactly one HARNESS_RESULT line, got {len(verdicts)}\n"
            f"stdout:\n{self.result.stdout}\nstderr:\n{self.result.stderr}",
        )

    def test_all_behaviors_pass(self) -> None:
        verdicts = [l for l in self.stdout_lines if l.startswith("HARNESS_RESULT ")]
        if not verdicts:
            self.skipTest("no verdict line; test_harness_reported_a_verdict covers this")
        failed = [l for l in self.stdout_lines if l.startswith("FAIL ")]
        self.assertEqual(
            "HARNESS_RESULT PASS",
            verdicts[0],
            "the /llm page regressed:\n  " + "\n  ".join(failed),
        )

    def test_expected_number_of_checks_ran(self) -> None:
        passed = [l for l in self.stdout_lines if l.startswith("PASS ")]
        failed = [l for l in self.stdout_lines if l.startswith("FAIL ")]
        self.assertEqual(
            EXPECTED_CHECKS,
            len(passed) + len(failed),
            f"harness ran {len(passed) + len(failed)} checks, expected {EXPECTED_CHECKS}. "
            "If you added or removed a check, update EXPECTED_CHECKS.",
        )


class LlmPageExtractionTests(unittest.TestCase):
    """Guards on the extraction itself. These need no JS engine.

    A silently-empty extraction would make every behavioral assertion above
    vacuous, so the shape of what we extracted is asserted directly.
    """

    def test_page_js_is_substantial(self) -> None:
        page_js, _ = extract_js.page_js(PAGE)
        self.assertGreater(len(page_js), 10_000, "extracted /llm page JS is implausibly small")
        self.assertNotIn("<script", page_js, "script tags must be stripped before parsing")
        self.assertIn("qaLoadModel", page_js, "extracted text does not look like the /llm page")

    def test_html_and_js_element_ids_agree(self) -> None:
        """Every id the markup declares is one the page JS actually looks up.

        The reverse direction (JS asking for an id nobody declared) is asserted
        inside the harness, where the real lookups happen.
        """
        declared = set(declared_element_ids(PAGE))
        page_js, _ = extract_js.page_js(PAGE)
        requested = set(re.findall(r"""(?:getElementById|hw\.\$)\(['"]([A-Za-z0-9_-]+)['"]\)""", page_js))
        self.assertTrue(declared, "no element ids found in the page markup")
        self.assertEqual(
            set(),
            declared - requested,
            "ids declared in the page HTML that no JS ever looks up (dead markup, "
            "or a rename that only landed on one side)",
        )


if __name__ == "__main__":
    unittest.main()
