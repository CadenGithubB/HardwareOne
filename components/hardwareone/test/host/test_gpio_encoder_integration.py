"""Run the actual GPIO input task with fake pins, time, mutexes and task creation."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class GPIOEncoderIntegrationTest(unittest.TestCase):
    def test_driver_with_and_without_auxiliary_buttons(self):
        here = Path(__file__).parent
        production = here.parents[1]
        for auxiliary in (0, 1):
            with self.subTest(auxiliary=auxiliary):
                with tempfile.TemporaryDirectory(prefix="hardwareone-gpio-input-") as directory:
                    work = Path(directory)
                    # Quoted includes first search beside the production .cpp.
                    # Copy it intact so heavy platform headers can be substituted.
                    for name in ("Input_GPIOEncoder.cpp", "Input_GPIOEncoder.h",
                                 "Input_ButtonCore.h", "Input_RotaryCore.h",
                                 "System_Board_P4X_EYE.h"):
                        shutil.copyfile(production / name, work / name)
                    binary = work / "gpio-input"
                    subprocess.run([
                        "c++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined",
                        f"-DENABLE_GPIO_ENCODER_BUTTONS={auxiliary}",
                        "-I", str(work), "-I", str(here / "gpio_encoder_stubs"),
                        str(here / "gpio_encoder_integration_harness.cpp"),
                        "-o", str(binary),
                    ], check=True)
                    subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
