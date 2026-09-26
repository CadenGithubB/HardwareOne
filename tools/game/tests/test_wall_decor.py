"""Wall decoration and flame attachment contracts."""

from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class WallDecorationTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_wall_attachment_and_light_position(self) -> None:
        result = run_js(Path(__file__).with_name("wall_decor.js"), timeout=20)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("WALL_DECOR_RESULT PASS 28", result.stdout.splitlines())
        self.assertEqual(28, sum(line.startswith("PASS ") for line in result.stdout.splitlines()))


if __name__ == "__main__":
    unittest.main()
