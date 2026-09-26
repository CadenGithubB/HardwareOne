"""Browser-local casting preference without quality/gameplay coupling."""

from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class CastingStyleTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_registry_preferences_and_settings_independence(self) -> None:
        result = run_js(Path(__file__).with_name("casting_styles.js"), timeout=30)
        lines = result.stdout.splitlines()
        self.assertIn("CASTING_STYLES_RESULT PASS 33", lines, result.stdout + result.stderr)
        self.assertEqual(33, sum(line.startswith("PASS ") for line in lines))


if __name__ == "__main__":
    unittest.main()
