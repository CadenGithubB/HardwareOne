"""Real seeded cave-exit movement and slope-depth regressions."""

from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class CaveExitTransitionTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_keyboard_exit_keeps_uphill_terrain_opaque(self) -> None:
        result = run_js(Path(__file__).with_name("cave_exit_transition.js"), timeout=120)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        lines = result.stdout.splitlines()
        self.assertIn("CAVE_EXIT_TRANSITION_RESULT PASS 12", lines, result.stdout + result.stderr)
        self.assertEqual(12, sum(line.startswith("PASS ") for line in lines))

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_height_only_terrain_culling(self) -> None:
        result = run_js(Path(__file__).with_name("cave_exit_transition.js"),
                        ["--height-only-cull"], timeout=120)
        self.assertNotIn("CAVE_EXIT_TRANSITION_RESULT PASS 12", result.stdout.splitlines())
        self.assertIn("Error: descending exit slopes never expose deeper terrain or sky",
                      result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
