"""Production spell drawing must obey terrain depth, including partial effects."""

from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class SpellVisibilityTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_all_spell_renderers_read_scene_depth(self) -> None:
        result = run_js(Path(__file__).with_name("spell_visibility.js"), timeout=60)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        lines = result.stdout.splitlines()
        self.assertIn("SPELL_VISIBILITY_RESULT PASS 88", lines, result.stdout + result.stderr)
        self.assertEqual(88, sum(line.startswith("PASS ") for line in lines))

    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_regression_rejects_unclipped_spells(self) -> None:
        result = run_js(Path(__file__).with_name("spell_visibility.js"), ["--bypass-spell-depth"], timeout=60)
        self.assertNotIn("SPELL_VISIBILITY_RESULT PASS 88", result.stdout.splitlines())
        self.assertIn("Error: fire projectile cannot paint through opaque terrain", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
