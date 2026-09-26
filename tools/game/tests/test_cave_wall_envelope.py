"""Actual generated cave-wall roof bounds and surface-wall preservation."""

from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class CaveWallEnvelopeTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_generated_walls_end_inside_real_roof_cover(self) -> None:
        result = run_js(Path(__file__).with_name("cave_wall_envelope.js"), timeout=120)
        lines = result.stdout.splitlines()
        self.assertIn("CAVE_WALL_ENVELOPE_RESULT PASS 21", lines, result.stdout + result.stderr)
        self.assertEqual(21, sum(line.startswith("PASS ") for line in lines))

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_old_highest_cap_wall_limit(self) -> None:
        result = run_js(Path(__file__).with_name("cave_wall_envelope.js"), ["--old-max-cap"], timeout=120)
        self.assertNotIn("CAVE_WALL_ENVELOPE_RESULT PASS 21", result.stdout.splitlines())
        self.assertIn("Error: 12345 descending cave walls stay strictly inside the terrain cover",
                      result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
