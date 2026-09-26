"""Cave cover shares the exterior terrain's world-authored material."""

from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class CaveSurfaceMaterialTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_seeded_material_identity_and_streaming_stability(self) -> None:
        result = run_js(Path(__file__).with_name("cave_surface_material.js"), timeout=120)
        lines = result.stdout.splitlines()
        self.assertIn("CAVE_SURFACE_MATERIAL_RESULT PASS 18", lines, result.stdout + result.stderr)
        self.assertEqual(18, sum(line.startswith("PASS ") for line in lines))

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_cave_only_color_jitter(self) -> None:
        result = run_js(Path(__file__).with_name("cave_surface_material.js"),
                        ["--restore-cap-jitter"], timeout=120)
        self.assertNotIn("CAVE_SURFACE_MATERIAL_RESULT PASS 18", result.stdout.splitlines())
        self.assertIn("Error: descending cap material equals pristine exterior at the same world XY",
                      result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
