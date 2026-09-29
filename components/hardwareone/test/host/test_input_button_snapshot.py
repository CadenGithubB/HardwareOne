"""Compile the production HAL snapshot against a minimal cache/mutex fixture."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


class InputButtonSnapshotTest(unittest.TestCase):
    def test_latched_edges(self):
        here = Path(__file__).parent
        source = (here.parents[1] / "HAL_Input.cpp").read_text()
        function = re.search(r"uint32_t inputConsumeButtonPresses\([^\n]+\) \{\n.*?\n\}",
                             source, re.S)
        self.assertIsNotNone(function)
        with tempfile.TemporaryDirectory(prefix="hardwareone-input-") as work:
            work = Path(work)
            (work / "input_snapshot_under_test.h").write_text(function.group())
            binary = work / "snapshot"
            subprocess.run(["c++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I", str(work),
                            str(here / "input_button_snapshot_harness.cpp"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
