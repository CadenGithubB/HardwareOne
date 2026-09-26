"""Shipping regression for triangle-facing cave-exit terrain and ceilings."""
from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class TerrainFacingTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_slopes_use_actual_plane_not_absolute_eye_height(self) -> None:
        result = run_js(Path(__file__).with_name("terrain_facing.js"), timeout=30)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("TERRAIN_FACING_RESULT PASS 22", result.stdout.splitlines())

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_rejects_historical_whole_quad_height_cull(self) -> None:
        result = run_js(Path(__file__).with_name("terrain_facing.js"),
                        ["--legacy-floor-height-cull"], timeout=30)
        self.assertNotIn("TERRAIN_FACING_RESULT PASS 22", result.stdout.splitlines())
        self.assertIn("real renderer retains both uphill triangles below all corners", result.stdout + result.stderr)

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_wall_gradient_cache_separates_material_ramps(self) -> None:
        result = run_js(Path(__file__).with_name("terrain_facing.js"),
                        ["--omit-wall-ramp-key"], timeout=30)
        self.assertNotIn("TERRAIN_FACING_RESULT PASS 22", result.stdout.splitlines())
        self.assertIn("same-color same-bucket exterior wall retains its three-stop sunlight ramp",
                      result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
