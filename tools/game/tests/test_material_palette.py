"""Shared palette/material contracts without the optional audit lab."""

from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class MaterialPaletteTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_appearance_compatibility_and_shared_consumers(self) -> None:
        result = run_js(Path(__file__).with_name("material_palette.js"), timeout=20)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        lines = result.stdout.splitlines()
        self.assertIn("MATERIAL_PALETTE_RESULT PASS 34", lines)
        self.assertEqual(34, sum(line.startswith("PASS ") for line in lines))


if __name__ == "__main__":
    unittest.main()
