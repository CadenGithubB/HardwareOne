"""Actor depth and stable-ground rendering without the optional audit lab."""

from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class ActorVisibilityTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_enemy_sprite_health_and_effects_use_scene_depth(self) -> None:
        result = run_js(Path(__file__).with_name("actor_visibility.js"), timeout=30)
        lines = result.stdout.splitlines()
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("ACTOR_VISIBILITY_RESULT PASS 23", lines, result.stdout + result.stderr)
        self.assertEqual(23, sum(line.startswith("PASS ") for line in lines))

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_omitted_actor_depth(self) -> None:
        result = run_js(Path(__file__).with_name("actor_visibility.js"),
                        ["--omit-actor-depth"], timeout=30)
        self.assertNotIn("ACTOR_VISIBILITY_RESULT PASS 23", result.stdout.splitlines())
        self.assertIn("Error: foreground terrain hides lower body but retains visible head",
                      result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
