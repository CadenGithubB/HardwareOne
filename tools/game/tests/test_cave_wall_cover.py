"""Actual seeded cave wall rendering remains beneath its terrain cover."""

from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class CaveWallCoverTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_surface_cover_and_visible_interior(self) -> None:
        result = run_js(Path(__file__).with_name("cave_wall_cover.js"), timeout=120)
        lines = result.stdout.splitlines()
        self.assertIn("CAVE_WALL_COVER_RESULT PASS 21", lines, result.stdout + result.stderr)
        self.assertEqual(21, sum(line.startswith("PASS ") for line in lines))

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_highest_neighbor_cap(self) -> None:
        result = run_js(Path(__file__).with_name("cave_wall_cover.js"),
                        ["--restore-max-cap"], timeout=120)
        self.assertNotIn("CAVE_WALL_COVER_RESULT PASS 21", result.stdout.splitlines())
        self.assertIn("Error: descending above roof fully occludes buried cave walls",
                      result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
