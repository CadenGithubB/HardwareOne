"""Meta-test: prove the /llm page harness actually fails when the page breaks.

A test that has never been observed to fail is not evidence of anything. The
harness in tools/webui/harness/llm_page_harness.js is especially exposed to
this, because almost everything it asserts is a NEGATIVE -- no duplicate
announcement, no catalog rebuild, no heartbeat poll during generation. A
negative assertion passes just as happily when the code under test never ran at
all, or when the scenario never reached the state that would violate it.

That is not a theoretical worry here; it was measured. An earlier draft of the
harness advanced its virtual clock by 400ms to check that the page stops polling
while the device is generating. No heartbeat was due inside a 400ms window, so
the assertion held whether or not the page had the guard -- deleting the guard
entirely still produced a green run. Another draft reported "ALL CHECKS PASSED"
while the page threw a TypeError on every generation.

So each mutation below breaks the page in one specific way, and this asserts the
harness NOTICES, naming the check that is supposed to catch it. If a mutation
stops being caught, either the page changed shape or an assertion went vacuous;
both are worth a failing test.

Every mutation is applied to a COPY in a temp directory. No repository file is
ever written.

Run from the repository root:

    python3 -m unittest discover -s tools/webui/tests -t .
"""

from __future__ import annotations

import json
import pathlib
import tempfile
import unittest

from tools.webui import extract_js
from tools.webui.js_engine import JS_ENGINE, run_js
from tools.webui.tests.test_llm_page import (
    HARNESS,
    PAGE,
    declared_element_ids,
)

# (name, text to replace, replacement, checks that must report FAIL)
#
# Anchors are exact source text and are asserted to appear exactly once, so an
# edit that moves the code fails loudly here instead of silently mutating
# nothing and "proving" the harness is broken.
MUTATIONS: list[tuple[str, str, str, list[str]]] = [
    (
        "announcements stop being edge-triggered",
        "        lastAnnounced = sig;\n",
        "\n",
        ["steady: model-loaded said exactly once",
         "after generation: no stray re-announce"],
    ),
    (
        "loading cadence no longer tightens",
        "var BEAT_FAST = 1000, BEAT_SLOW = 5000;",
        "var BEAT_FAST = 5000, BEAT_SLOW = 5000;",
        ["load: cadence tightened"],
    ),
    (
        "model sizes revert to a flat KB figure",
        "            size = g >= 1 ? ' (' + g.toFixed(1) + 'GB)'\n"
        "                 : m >= 1 ? ' (' + m.toFixed(m < 10 ? 1 : 0) + 'MB)'\n"
        "                          : ' (' + Math.round(k) + 'KB)';",
        "            size = ' (' + Math.round(k) + 'KB)';",
        ["remote size scales to GB", "onboard size scales to MB"],
    ),
    (
        "an element id is renamed in the markup only",
        "id='qa-model'",
        "id='qa-model-picker'",
        ["every id the page JS looks up is declared in the page HTML"],
    ),
    (
        "the page throws while finishing a generation",
        "  function finishGen(ctx) {\n    busy = false;",
        "  function finishGen(ctx) {\n    (undefined).boom;\n    busy = false;",
        ["page threw no exceptions"],
    ),
    (
        "a refused load stops reporting its reason",
        "          addSys('Load failed: ' + why);\n",
        "",
        ["refused load reports the reason"],
    ),
    (
        "the catalog is rebuilt on every poll",
        "        if (sig === modelSig) return;",
        "        if (false) return;",
        ["idle: catalog not rebuilt while unchanged",
         "host up: catalog rebuilt exactly once"],
    ),
    (
        "the page re-frames the prompt client-side",
        "      prompt: isDoMode ? 'Do: ' + intent : q,",
        "      prompt: isDoMode ? 'Q: ' + intent + '\\nDo:' : 'Q: ' + q + '\\nA:',",
        ["generate: prompt is sent unframed"],
    ),
    (
        "Do: mode is no longer scoped to the on-device model",
        "    if (isDoMode && cmdModeOk === false) {",
        "    if (false) {",
        ["Do: on a remote model sends no request",
         "Do: on a remote model explains why"],
    ),
    (
        "the streamed answer never reaches the page",
        "          ctx.aText.textContent += j.text;",
        "          ;",
        ["generation: the answer was rendered exactly once"],
    ),
    (
        "the model picker stops restoring the selection",
        "        if (keep) modelSel.value = keep;                      "
        "// no-op if it went away",
        "",
        ["loading: selection survived a rebuild"],
    ),
    (
        # Invisible before the fake device honoured `offset`: every poll was
        # re-served from byte 0, so the answer duplicated without bound.
        "the page stops advancing its read offset",
        "          ctx.pollOffset = j.next;",
        "          ;",
        ["generation: the answer was rendered exactly once"],
    ),
    (
        # The original shipped defect, kept as a mutation. Counting UTF-16 code
        # units instead of taking the device's byte cursor leaves the offset 2
        # short per 3-byte character, so the device re-serves text already on
        # screen. Invisible against a pure-ASCII fixture, which is why
        # DEV.answer carries an em dash.
        "the page infers its own offset instead of using the device cursor",
        "          ctx.pollOffset = j.next;",
        "          ctx.pollOffset += j.text.length;",
        ["generation: the answer was rendered exactly once"],
    ),
    (
        # Invisible before the fake device tracked sessions: a superseded
        # generation polls a dead session forever and the turn never releases.
        "the superseded-session guard is removed",
        "        if (j.stale) { finishGen(ctx); return; }",
        "        ;",
        ["stale session ends the turn"],
    ),
    (
        "Stop posts to the device but never aborts the turn",
        "    if (abortCtrl) abortCtrl.abort();",
        "    ;",
        # NOT "the turn ended" — the device marks the session stale on stop, so
        # the turn still ends without the client-side abort, one poll later.
        ["stop: no further round trip"],
    ),
    (
        # THE reason this coverage exists. Dropping this finishGen leaves `busy`
        # true forever: Stop sticks on, the input stays locked, and beat() bails
        # on `busy` so the heartbeat stands down permanently. Nothing on screen
        # explains it and only a reload recovers.
        "a refused generation never ends the turn",
        "          ctx.pair.appendChild(err);\n          finishGen(ctx);\n          return;\n",
        "          ctx.pair.appendChild(err);\n          return;\n",
        ["refused generate: the turn ended",
         "refused generate: the heartbeat kept running"],
    ),
    (
        "a generate request that never lands never ends the turn",
        "        finishGen(ctx);\n      });\n",
        "      });\n",
        ["rejected generate: the turn ended",
         "rejected generate: the heartbeat kept running"],
    ),
    (
        "the refusal reason is read from the wrong field",
        "'[' + (j.error || 'error') + ']'",
        "'[' + (j.err || 'error') + ']'",
        ["refused generate: reports the reason"],
    ),
    (
        # The wrapper silently dropped qaAsk's argument, which turned the Do:
        # button into an ordinary Ask. Invisible until a mode existed to lose.
        "the guided-strip wrapper stops forwarding the mode",
        "    _qaAskOrig(mode);",
        "    _qaAskOrig();",
        ["Do: button sends the Do: marker"],
    ),
    (
        "the Do: button is offered regardless of the model",
        "    var allowed = (cmdModeOk === true);",
        "    var allowed = true;",
        ["Do: button is withdrawn for a model that cannot"],
    ),
    (
        # The keydown handler was registered against a no-op addEventListener, so
        # deleting it changed nothing anywhere in this suite until the stub grew
        # a real event dispatcher.
        "Enter no longer sends the question",
        "    if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); qaAsk(); }",
        "    ;",
        ["Enter in the input sends the question"],
    ),
    (
        "Shift+Enter starts sending too",
        "    if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); qaAsk(); }",
        "    if (e.key === 'Enter' && true) { e.preventDefault(); qaAsk(); }",
        ["Shift+Enter does not send"],
    ),
    (
        # The observed hardware symptom: stopping the CM5 daemon mid-generation
        # ended the turn correctly but left an empty answer and no explanation.
        "an abandoned turn stops explaining itself",
        "          if (j.error) {",
        "          if (false) {",
        ["an abandoned turn reports why"],
    ),
    (
        # Terminating on the first failure would kill generations during ordinary
        # radio blips -- at RSSI -78 the link drops beacons routinely.
        "a single dropped poll ends the turn",
        "    if (ctx.failMs >= LOST_GIVEUP_MS) {",
        "    if (true) {",
        ["a transient outage does not end the turn"],
    ),
    (
        "a lost connection retries forever instead of ending",
        "    if (ctx.failMs >= LOST_GIVEUP_MS) {",
        "    if (false) {",
        ["a lost connection ends the turn"],
    ),
    (
        "the heartbeat keeps polling during generation",
        "    if (busy || document.hidden) { scheduleBeat(BEAT_SLOW); return; }",
        "    if (document.hidden) { scheduleBeat(BEAT_SLOW); return; }",
        ["generating: no heartbeat status polls",
         "generating: no heartbeat catalog polls"],
    ),
]


@unittest.skipUnless(
    JS_ENGINE, "a JavaScript engine (node/deno/bun/qjs/jsc/osascript) is required"
)
class HarnessDetectsRegressionsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory()
        cls.work = pathlib.Path(cls.temporary.name)
        cls.source = PAGE.read_text(encoding="utf-8")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary.cleanup()

    def _run_source(self, text: str) -> tuple[str, set[str]]:
        """Run the harness against page source text; return (verdict, failures)."""
        page = self.work / PAGE.name
        page.write_text(text, encoding="utf-8")

        page_js, _ = extract_js.page_js(page)
        (self.work / "page.js").write_text(page_js, encoding="utf-8")
        # Read the ids back from the MUTATED copy: one mutation renames an id in
        # the markup, and the point is that the JS side no longer agrees with it.
        (self.work / "ids.json").write_text(
            json.dumps(declared_element_ids(page)), encoding="utf-8"
        )

        result = run_js(HARNESS, [str(self.work / "page.js"), str(self.work / "ids.json")])
        lines = result.stdout.splitlines()
        verdicts = [l for l in lines if l.startswith("HARNESS_RESULT")]
        failed = {
            l[len("FAIL "):].split("   [")[0].strip()
            for l in lines
            if l.startswith("FAIL ")
        }
        return (verdicts[0] if verdicts else "(no verdict line)"), failed

    def _run_mutated(self, old: str, new: str) -> tuple[str, set[str]]:
        """Apply one mutation to a copy and return (verdict, failing check names)."""
        self.assertEqual(
            1,
            self.source.count(old),
            f"mutation anchor appears {self.source.count(old)} times in "
            f"{PAGE.name}; it must appear exactly once. The page moved -- "
            "update the anchor rather than deleting the mutation.",
        )
        return self._run_source(self.source.replace(old, new, 1))

    def test_every_mutation_is_caught(self) -> None:
        for name, old, new, expected in MUTATIONS:
            with self.subTest(mutation=name):
                verdict, failed = self._run_mutated(old, new)
                self.assertTrue(
                    verdict.startswith("HARNESS_RESULT FAIL"),
                    f"breaking the page ({name}) did not fail the harness: "
                    f"verdict was {verdict!r}",
                )
                hit = failed & set(expected)
                self.assertTrue(
                    hit,
                    f"the harness noticed something, but not the right thing.\n"
                    f"  mutation: {name}\n"
                    f"  expected one of: {sorted(expected)}\n"
                    f"  actually failed: {sorted(failed) or 'nothing'}",
                )

    def test_unmutated_page_passes(self) -> None:
        """The control. Without it, a harness that fails everything scores 100%."""
        verdict, failed = self._run_source(self.source)
        self.assertEqual(
            "HARNESS_RESULT PASS",
            verdict,
            "the unmodified page does not pass, so the mutation results above "
            "mean nothing. Failing checks:\n  " + "\n  ".join(sorted(failed)),
        )


if __name__ == "__main__":
    unittest.main()
