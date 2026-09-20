"""Shipping seeded cave contracts, without the optional local audit lab."""

from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class CaveGeometryTests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_seeded_portal_generation_and_spawn_contracts(self) -> None:
        result = run_js(Path(__file__).with_name("cave_geometry.js"), timeout=20)
        self.assertIn("CAVE_GEOMETRY_RESULT PASS 51", result.stdout.splitlines(), result.stdout + result.stderr)
        self.assertEqual(51, sum(line.startswith("PASS ") for line in result.stdout.splitlines()))


if __name__ == "__main__":
    unittest.main()
