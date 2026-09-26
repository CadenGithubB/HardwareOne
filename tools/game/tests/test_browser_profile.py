"""Statistics and temporary-global/cancellation safety for browser profiling."""

from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class BrowserProfileTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_profile_statistics_and_cleanup(self) -> None:
        result = run_js(Path(__file__).with_name("browser_profile.js"), timeout=20)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        lines = result.stdout.splitlines()
        self.assertIn("BROWSER_PROFILE_RESULT PASS 32", lines, result.stdout + result.stderr)
        self.assertEqual(32, sum(line.startswith("PASS ") for line in lines))


if __name__ == "__main__":
    unittest.main()
