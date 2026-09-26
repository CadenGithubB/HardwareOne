"""Structure Shell V1 and Floor Clutter V1 contracts."""

from pathlib import Path
import unittest

from tools.webui.js_engine import JS_ENGINE, run_js


class StructureFloorV1Tests(unittest.TestCase):
    @unittest.skipIf(JS_ENGINE is None, "a supported JavaScript engine is required")
    def test_structure_shell_and_floor_clutter_contracts(self) -> None:
        result = run_js(Path(__file__).with_name("structure_floor_v1.js"), timeout=20)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        lines = result.stdout.splitlines()
        self.assertIn("STRUCTURE_FLOOR_V1_RESULT PASS 19", lines, result.stdout + result.stderr)
        self.assertEqual(19, sum(line.startswith("PASS ") for line in lines))


if __name__ == "__main__":
    unittest.main()
