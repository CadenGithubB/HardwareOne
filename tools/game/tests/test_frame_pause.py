"""A paused/settings frame must not advance or accumulate simulation work."""
from pathlib import Path
import unittest
from tools.webui.js_engine import JS_ENGINE, run_js


class FramePauseTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_frame_pause(self):
        result = run_js(Path(__file__).with_name("frame_pause.js"), timeout=30)
        self.assertIn("FRAME_PAUSE_RESULT PASS 9", result.stdout.splitlines(), result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
