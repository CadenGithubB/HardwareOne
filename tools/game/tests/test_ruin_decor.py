"""Stable, world-authored ruin decoration contracts."""

from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class RuinDecorTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_ruin_decor_stays_fixed_during_camera_approach(self) -> None:
        result = run_js(Path(__file__).with_name("ruin_decor.js"), timeout=20)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        lines = result.stdout.splitlines()
        self.assertIn("RUIN_DECOR_RESULT PASS 11", lines, result.stdout + result.stderr)
        self.assertEqual(11, sum(line.startswith("PASS ") for line in lines))


if __name__ == "__main__":
    unittest.main()
