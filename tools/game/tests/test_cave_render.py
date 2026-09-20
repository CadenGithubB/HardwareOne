"""Shipping regression for cave-mouth floor/cap stitching."""
from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class CaveRenderTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_semantic_floor_stitching(self) -> None:
        result = run_js(Path(__file__).with_name("cave_render.js"), timeout=20)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("CAVE_RENDER_RESULT PASS 12", result.stdout.splitlines())
        self.assertEqual(12, sum(line.startswith("PASS ") for line in result.stdout.splitlines()))


if __name__ == "__main__":
    unittest.main()
