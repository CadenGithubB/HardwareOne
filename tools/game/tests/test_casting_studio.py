"""Studio uses actual combat and restores the real world after each draw."""
from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class CastingStudioTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_actual_hit_and_state_restoration(self) -> None:
        result = run_js(Path(__file__).with_name("casting_studio.js"), timeout=120)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("CASTING_STUDIO_RESULT PASS 74", result.stdout.splitlines())


if __name__ == "__main__":
    unittest.main()
