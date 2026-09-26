"""Production floor shading respects cave and surface strata."""

from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class CaveSurfaceShadingTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_real_floor_colors_and_unchanged_depth(self) -> None:
        result = run_js(Path(__file__).with_name("cave_surface_shading.js"), timeout=120)
        lines = result.stdout.splitlines()
        self.assertIn("CAVE_SURFACE_SHADING_RESULT PASS 22", lines, result.stdout + result.stderr)
        self.assertEqual(22, sum(line.startswith("PASS ") for line in lines))

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_height_blind_wall_shading(self) -> None:
        result = run_js(Path(__file__).with_name("cave_surface_shading.js"),
                        ["--height-blind-ao"], timeout=120)
        self.assertNotIn("CAVE_SURFACE_SHADING_RESULT PASS 22", result.stdout.splitlines())
        self.assertIn("Error: descending buried walls do not darken upper grass",
                      result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
