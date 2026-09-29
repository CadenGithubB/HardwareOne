"""Exercise the production GPIO button debounce logic without board hardware."""
from pathlib import Path
import subprocess
import tempfile
import unittest


class GPIOButtonsTest(unittest.TestCase):
    def test_debounce_and_press_edges(self):
        source = Path(__file__).with_suffix(".cpp")
        with tempfile.TemporaryDirectory(prefix="hardwareone-gpio-buttons-") as work:
            binary = Path(work) / "gpio-buttons"
            subprocess.run(["c++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=undefined,address", str(source), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
