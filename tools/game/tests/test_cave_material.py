"""Camera-independent cave material contracts without optional audit files."""

from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class CaveMaterialTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_authored_material_and_rebased_lookup_contracts(self) -> None:
        result = run_js(Path(__file__).with_name("cave_material.js"), timeout=20)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("CAVE_MATERIAL_RESULT PASS 24", result.stdout.splitlines())
        self.assertEqual(24, sum(line.startswith("PASS ") for line in result.stdout.splitlines()))


if __name__ == "__main__":
    unittest.main()
