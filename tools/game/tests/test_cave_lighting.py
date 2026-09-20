"""Durable lighting contracts without browser or optional audit dependencies."""

from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class CaveLightingTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_portal_exposure_and_day_cycle_contracts(self) -> None:
        result = run_js(Path(__file__).with_name("cave_lighting.js"), timeout=20)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("CAVE_LIGHTING_RESULT PASS 17", result.stdout.splitlines())
        self.assertEqual(17, sum(line.startswith("PASS ") for line in result.stdout.splitlines()))


if __name__ == "__main__":
    unittest.main()
