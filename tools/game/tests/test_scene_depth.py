"""Shipping pixel-depth regression contracts, independent of the local lab."""

from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class SceneDepthTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_partial_occlusion_interpolation_and_projection(self) -> None:
        result = run_js(Path(__file__).with_name("scene_depth.js"), timeout=20)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("SCENE_DEPTH_RESULT PASS 32", result.stdout.splitlines())
        self.assertEqual(32, sum(line.startswith("PASS ") for line in result.stdout.splitlines()))


if __name__ == "__main__":
    unittest.main()
