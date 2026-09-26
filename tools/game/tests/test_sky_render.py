"""Deterministic, bounded Sky V1 rendering contracts."""

from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class SkyRenderTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_sky_render_contracts(self) -> None:
        result = run_js(Path(__file__).with_name("sky_render.js"), timeout=20)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        lines = result.stdout.splitlines()
        self.assertIn("SKY_RENDER_RESULT PASS 13", lines, result.stdout + result.stderr)
        self.assertEqual(13, sum(line.startswith("PASS ") for line in lines))


if __name__ == "__main__":
    unittest.main()
