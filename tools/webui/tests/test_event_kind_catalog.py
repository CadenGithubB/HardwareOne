"""Behavioral coverage for the shared event-kind browser helper.

The helper ships as the named ``HW_EVENT_KINDS`` C++ raw string in
``WebServer_Utils.cpp``. Tests extract that exact body and execute it against a
deterministic Promise/DOM harness; no JavaScript implementation is copied into
the test suite.
"""

from __future__ import annotations

import pathlib
import tempfile
import unittest

from tools.webui import extract_js
from tools.webui.js_engine import JS_ENGINE, run_js


REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = REPO_ROOT / "components" / "hardwareone" / "WebServer_Utils.cpp"
HARNESS = REPO_ROOT / "tools" / "webui" / "harness" / "event_kind_catalog_harness.js"
RAW_DELIMITER = "HW_EVENT_KINDS"
EXPECTED_CHECKS = 32


def shipping_event_kind_helper() -> str:
    """Return only the JS inside the one production HW_EVENT_KINDS block."""

    blocks = [
        block
        for block in extract_js.extract_blocks(SOURCE)
        if block.delim == RAW_DELIMITER
    ]
    if len(blocks) != 1:
        raise AssertionError(
            f"expected exactly one {RAW_DELIMITER} raw block, found {len(blocks)}"
        )
    body = blocks[0].body
    opening = "<script>\n"
    closing = "\n</script>"
    if not body.startswith(opening) or not body.endswith(closing):
        raise AssertionError(
            f"{RAW_DELIMITER} must contain one tag-wrapped script body"
        )
    script = body[len(opening) : -len(closing)]
    if "<script" in script.lower() or "</script" in script.lower():
        raise AssertionError(f"{RAW_DELIMITER} unexpectedly contains nested script tags")
    return script


class EventKindCatalogExtractionTests(unittest.TestCase):
    def test_named_block_contains_the_shipping_shared_helper(self) -> None:
        script = shipping_event_kind_helper()
        self.assertGreater(len(script), 4_000, "event-kind helper is implausibly small")
        self.assertIn("hw.getEventKindFamilies=function()", script)
        self.assertIn("hw.fillEventKindSelect=function(target,options)", script)
        self.assertIn("/api/events/kinds", script)
        self.assertNotIn("wifi_connected", script, "helper must not embed a second catalog")


@unittest.skipUnless(
    JS_ENGINE, "a JavaScript engine (node/deno/bun/qjs/jsc/osascript) is required"
)
class EventKindCatalogBehaviorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory()
        cls.helper = pathlib.Path(cls.temporary.name) / "event_kind_helper.js"
        cls.helper.write_text(shipping_event_kind_helper(), encoding="utf-8")
        cls.result = run_js(HARNESS, [str(cls.helper)])
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
            "shared event-kind helper regressed:\n  " + "\n  ".join(failures),
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
            f"ran {len(checks)} checks, expected {EXPECTED_CHECKS}\n{self.result.stdout}",
        )


# Each mutation is applied to an extracted TEMPORARY copy. The named expected
# check proves the harness observes the particular regression, rather than only
# failing incidentally somewhere else.
MUTATIONS: list[tuple[str, str, str, set[str]]] = [
    (
        "pending callers stop coalescing",
        "    if(pendingFamilies) return pendingFamilies;",
        "    if(false&&pendingFamilies) return pendingFamilies;",
        {
            "concurrent catalog consumers share one pending promise",
            "concurrent catalog consumers issue one request",
        },
    ),
    (
        "successful responses stop being cached",
        "      cachedFamilies=families;\n"
        "      pendingFamilies=null;\n"
        "      return cachedFamilies;",
        "      pendingFamilies=null;\n"
        "      return families;",
        {"successful fetch is cached per page"},
    ),
    (
        "a failure poisons every later request",
        "      pendingFamilies=null;\n      throw error;",
        "      throw error;",
        {"failed catalog request can retry and then cache"},
    ),
    (
        "duplicate and malformed names are accepted",
        "if(typeof kind!=='string'||!/^[a-z0-9_]+$/.test(kind)||\n"
        "           /^(?:boot|none|set|patch|all|list)$/.test(kind)||seen[kind]){",
        "if(false){",
        {"duplicate and invalid event-kind names are rejected"},
    ),
    (
        "reserved catalog tokens are accepted",
        "/^(?:boot|none|set|patch|all|list)$/.test(kind)||seen[kind]",
        "/a^/.test(kind)||seen[kind]",
        {"reserved control and alias event-kind names are rejected"},
    ),
    (
        "an older async picker load can repaint a newer one",
        "      if(select.__hwEventKindLoadId!==loadId) return families;",
        "      ;",
        {"stale async completion cannot replace the newer selection"},
    ),
    (
        "stored unknown values silently disappear",
        "      if(desired&&!found){",
        "      if(false){",
        {"stored unknown event kind is preserved as unavailable"},
    ),
    (
        "catalog failure leaves the picker enabled",
        "        select.disabled=true;\n"
        "        select.title='Unable to load event kinds: '+message;",
        "        select.disabled=false;\n"
        "        select.title='Unable to load event kinds: '+message;",
        {
            "catalog failure leaves an empty picker fail-closed",
            "failed load shows but cannot authorize a stored event kind",
        },
    ),
]


@unittest.skipUnless(
    JS_ENGINE, "a JavaScript engine (node/deno/bun/qjs/jsc/osascript) is required"
)
class EventKindCatalogHarnessRegressionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = shipping_event_kind_helper()
        cls.temporary = tempfile.TemporaryDirectory()
        cls.work = pathlib.Path(cls.temporary.name)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary.cleanup()

    def _run(self, source: str) -> tuple[str, set[str]]:
        helper = self.work / "mutated_event_kind_helper.js"
        helper.write_text(source, encoding="utf-8")
        result = run_js(HARNESS, [str(helper)])
        lines = result.stdout.splitlines()
        verdicts = [line for line in lines if line.startswith("HARNESS_RESULT ")]
        failures = {
            line[len("FAIL ") :].split("   [", 1)[0].strip()
            for line in lines
            if line.startswith("FAIL ")
        }
        return (verdicts[0] if verdicts else "(no verdict line)"), failures

    def test_every_mutation_is_caught_by_its_behavior(self) -> None:
        for name, old, new, expected in MUTATIONS:
            with self.subTest(mutation=name):
                self.assertEqual(
                    1,
                    self.source.count(old),
                    f"mutation anchor appears {self.source.count(old)} times; "
                    "update the anchor instead of weakening the self-test",
                )
                verdict, failures = self._run(self.source.replace(old, new, 1))
                self.assertTrue(
                    verdict.startswith("HARNESS_RESULT FAIL"),
                    f"breaking the helper ({name}) did not fail the harness: {verdict}",
                )
                self.assertTrue(
                    failures & expected,
                    f"mutation {name!r} failed the wrong checks\n"
                    f"expected one of: {sorted(expected)}\n"
                    f"actually failed: {sorted(failures)}",
                )

    def test_unmodified_helper_passes_as_the_control(self) -> None:
        verdict, failures = self._run(self.source)
        self.assertEqual(
            "HARNESS_RESULT PASS",
            verdict,
            "the unmodified helper does not pass, so mutation evidence is invalid. "
            f"Failures: {sorted(failures)}",
        )


if __name__ == "__main__":
    unittest.main()
