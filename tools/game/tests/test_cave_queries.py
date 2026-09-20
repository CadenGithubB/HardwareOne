"""Small shipping spatial regressions, independent of the optional audit lab."""

from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class CaveQueryTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_support_clearance_and_floor_cache_contracts(self) -> None:
        result = run_js(Path(__file__).with_name("cave_queries.js"), timeout=20)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("CAVE_QUERY_RESULT PASS 22", result.stdout.splitlines())
        self.assertEqual(22, sum(line.startswith("PASS ") for line in result.stdout.splitlines()))


if __name__ == "__main__":
    unittest.main()
