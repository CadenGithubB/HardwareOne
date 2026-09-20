"""Shared entity support, absolute-height and transparent-depth contracts."""
from pathlib import Path
import unittest
from tools.webui.js_engine import JS_ENGINE, run_js


class EntityDepthTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_support_height_and_billboard_contracts(self):
        result = run_js(Path(__file__).with_name("entity_depth.js"), timeout=30)
        lines = result.stdout.splitlines()
        self.assertIn("ENTITY_DEPTH_RESULT PASS 21", lines, result.stdout + result.stderr)
        self.assertEqual(21, sum(line.startswith("PASS ") for line in lines))
