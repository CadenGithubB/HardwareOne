"""First-person visual timing, equipment colors and bounded cache behavior."""

from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class FirstPersonArtTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_gesture_and_cache_contracts(self) -> None:
        result = run_js(Path(__file__).with_name("firstperson_art.js"), timeout=40)
        lines = result.stdout.splitlines()
        self.assertIn("FIRSTPERSON_ART_RESULT PASS 29", lines, result.stdout + result.stderr)
        self.assertEqual(29, sum(line.startswith("PASS ") for line in lines))


if __name__ == "__main__":
    unittest.main()
