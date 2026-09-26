"""Roof-aware upper-terrain lighting with unchanged interior lighting/depth."""

from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class CaveSurfaceLightingTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_real_floor_lighting_and_unchanged_depth(self) -> None:
        result = run_js(Path(__file__).with_name("cave_surface_lighting.js"), timeout=120)
        lines = result.stdout.splitlines()
        self.assertIn("CAVE_SURFACE_LIGHTING_RESULT PASS 49", lines, result.stdout + result.stderr)
        self.assertEqual(49, sum(line.startswith("PASS ") for line in lines))

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_lighting_through_roof(self) -> None:
        result = run_js(Path(__file__).with_name("cave_surface_lighting.js"),
                        ["--bypass-surface-light-roof"], timeout=120)
        self.assertNotIn("CAVE_SURFACE_LIGHTING_RESULT PASS 49", result.stdout.splitlines())
        self.assertIn("Error: descending buried lights do not brighten upper grass",
                      result.stdout + result.stderr)

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_upper_light_on_lower_floor(self) -> None:
        result = run_js(Path(__file__).with_name("cave_surface_lighting.js"),
                        ["--share-upper-light-with-lower-layer"], timeout=120)
        self.assertNotIn("CAVE_SURFACE_LIGHTING_RESULT PASS 49", result.stdout.splitlines())
        self.assertIn("Error: stacked lower floor cannot borrow upper receiver light",
                      result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
