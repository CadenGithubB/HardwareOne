"""Exercise production rotary decoding/gesture logic without GPIO hardware."""
from pathlib import Path
import subprocess
import tempfile
import unittest


class RotaryCoreTest(unittest.TestCase):
    def test_decode_and_gestures(self):
        source = Path(__file__).with_suffix(".cpp")
        with tempfile.TemporaryDirectory(prefix="hardwareone-rotary-") as work:
            binary = Path(work) / "rotary-core"
            subprocess.run(["c++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=undefined,address", str(source), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
