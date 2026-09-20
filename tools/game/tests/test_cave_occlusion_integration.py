"""Actual seeded terrain/entrance depth integration without the optional lab."""

from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class CaveOcclusionIntegrationTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_oblique_terrain_occludes_only_buried_arch(self) -> None:
        result = run_js(Path(__file__).with_name("cave_occlusion_integration.js"), timeout=120)
        lines = result.stdout.splitlines()
        self.assertIn("CAVE_OCCLUSION_INTEGRATION_RESULT PASS 24", lines, result.stdout + result.stderr)
        self.assertEqual(24, sum(line.startswith("PASS ") for line in lines))

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_omitted_terrain_depth(self) -> None:
        result = run_js(Path(__file__).with_name("cave_occlusion_integration.js"),
                        ["--omit-terrain-depth"], timeout=120)
        self.assertNotIn("CAVE_OCCLUSION_INTEGRATION_RESULT PASS 24", result.stdout.splitlines())
        self.assertIn("Error: side view terrain hides buried lower jamb", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
