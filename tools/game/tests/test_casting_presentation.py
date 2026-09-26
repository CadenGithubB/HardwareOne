"""Hands/missile presentation must preserve combat and terrain occlusion."""

from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class CastingPresentationTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_pre_art_gameplay_and_new_world_art_contracts(self) -> None:
        result = run_js(Path(__file__).with_name("casting_presentation.js"), timeout=40)
        lines = result.stdout.splitlines()
        self.assertIn("CASTING_PRESENTATION_RESULT PASS 76", lines, result.stdout + result.stderr)
        self.assertEqual(76, sum(line.startswith("PASS ") for line in lines))

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_delayed_gameplay_spawn(self) -> None:
        result = run_js(Path(__file__).with_name("casting_presentation.js"), ["--delay-projectile-spawn"], timeout=40)
        self.assertNotIn("CASTING_PRESENTATION_RESULT PASS 76", result.stdout.splitlines())
        self.assertIn("Error: press cooldown and immediate spawn matches pre-art gameplay", result.stdout + result.stderr)

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_early_missile_release(self) -> None:
        result = run_js(Path(__file__).with_name("casting_presentation.js"), ["--early-missile-release"], timeout=40)
        self.assertNotIn("CASTING_PRESENTATION_RESULT PASS 76", result.stdout.splitlines())
        self.assertIn("Error: accepted missile reserves one cost and cooldown without an early projectile", result.stdout + result.stderr)

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_missile_depth_bypass(self) -> None:
        result = run_js(Path(__file__).with_name("casting_presentation.js"), ["--world-art-only", "--bypass-missile-depth"], timeout=40)
        self.assertNotIn("CASTING_PRESENTATION_RESULT PASS 22", result.stdout.splitlines())
        self.assertIn("Error: new missile cannot paint through opaque terrain", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
