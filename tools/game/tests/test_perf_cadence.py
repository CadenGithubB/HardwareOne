"""Keep actual frame delivery distinct from reciprocal CPU time."""
from pathlib import Path
import unittest
from tools.webui.js_engine import JS_ENGINE, run_js


class PerfCadenceTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_frame_delivery(self):
        result = run_js(Path(__file__).with_name("perf_cadence.js"), timeout=30)
        self.assertIn("PERF_CADENCE_RESULT PASS 16", result.stdout.splitlines(), result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
