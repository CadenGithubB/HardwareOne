"""Exact full-resolution mask coalescing and operation-count regressions."""

from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class SceneDepthCoalescingTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_exact_mask_depth_parity_and_fewer_canvas_rectangles(self) -> None:
        result = run_js(Path(__file__).with_name("scene_depth_coalescing.js"), timeout=30)
        lines = result.stdout.splitlines()
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("SCENE_DEPTH_COALESCING_RESULT PASS 10", lines, result.stdout + result.stderr)
        self.assertEqual(10, sum(line.startswith("PASS ") for line in lines))


if __name__ == "__main__":
    unittest.main()
