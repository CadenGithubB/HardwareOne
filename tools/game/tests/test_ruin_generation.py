"""Accessible and deterministic Ruined Hut generation contracts."""
from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class RuinGenerationTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_hut_footprint_and_pad_contracts(self) -> None:
        result = run_js(Path(__file__).with_name("ruin_generation.js"), timeout=20)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        lines = result.stdout.splitlines()
        self.assertIn("RUIN_GENERATION_RESULT PASS 10", lines, result.stdout + result.stderr)
        self.assertEqual(10, sum(line.startswith("PASS ") for line in lines))


if __name__ == "__main__":
    unittest.main()
