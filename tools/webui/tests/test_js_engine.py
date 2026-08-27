"""Guards on the JavaScript-engine layer that every other harness rides on.

js_engine.py exists because the engines genuinely disagree: on how arguments
arrive, on whether a file is an ES module or a sloppy classic script, on where
a stack trace is printed, and on whether a script can set an exit code at all.
Those differences are absorbed by harness/_bootstrap.js, and if that absorption
regresses, the failure surfaces as an unrelated harness printing nothing --
which looks exactly like a test that ran and found no problems.

So: run the same probe on EVERY engine present on this machine and require the
output to be byte-identical. On a box with one engine this degenerates to a
contract check, which is still worth having.

Run from the repository root:

    python3 -m unittest discover -s tools/webui/tests -t .
"""

from __future__ import annotations

import os
import pathlib
import shutil
import tempfile
import unittest

from tools.webui.js_engine import JSC_HELPER, JS_ENGINE, run_js

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
PROBE = REPO_ROOT / "tools" / "webui" / "harness" / "_selftest.js"

EXPECTED = [
    "ENGINE_CONTRACT begin",
    "argv 1",
    "arg0 nonempty",
    "slurp ok",
    "sloppy ok",
    "ENGINE_CONTRACT end",
]


def available_engines() -> list[tuple[str, str]]:
    """Every engine on this machine, not just the first one discovery picks."""
    found: list[tuple[str, str]] = []
    for name in ("node", "deno", "bun", "qjs", "quickjs", "osascript"):
        path = shutil.which(name)
        if path:
            found.append(("jxa" if name == "osascript" else name, path))
    if os.path.exists(JSC_HELPER):
        found.append(("jsc", JSC_HELPER))
    return found


@unittest.skipUnless(JS_ENGINE, "no JavaScript engine available")
class JsEngineContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory()
        # Deliberately OUTSIDE the repo: harnesses are handed their inputs through
        # tempfile so nothing is written into the source tree, and a sandboxing
        # engine must still be allowed to read them.
        cls.fixture = pathlib.Path(cls.temporary.name) / "fixture.txt"
        cls.fixture.write_text("marker-line\n", encoding="utf-8")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary.cleanup()

    def test_contract_holds_on_every_available_engine(self) -> None:
        engines = available_engines()
        self.assertTrue(engines, "no engines found, yet JS_ENGINE was truthy")
        transcripts: dict[str, str] = {}
        for name, path in engines:
            with self.subTest(engine=name):
                result = run_js(PROBE, [str(self.fixture)], engine=(name, path))
                self.assertEqual(
                    0, result.returncode,
                    f"{name} exited {result.returncode}; stderr:\n{result.stderr}",
                )
                self.assertEqual(
                    EXPECTED, result.stdout.splitlines(),
                    f"{name} broke the bootstrap contract.\n"
                    f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
                )
                transcripts[name] = result.stdout

        if len(transcripts) > 1:
            first = sorted(transcripts)[0]
            for name, text in sorted(transcripts.items()):
                self.assertEqual(
                    transcripts[first], text,
                    f"{name} and {first} disagree; the whole point of "
                    "_bootstrap.js is that they must not",
                )


if __name__ == "__main__":
    unittest.main()
