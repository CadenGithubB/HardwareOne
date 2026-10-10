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
            header = (here.parents[1] / "HAL_Input.h").read_text()
            reading = re.search(r"struct InputCache \{.*?\n\};", header, re.S)
            self.assertIsNotNone(reading)
            owner = source[source.index("// Shared input readings"):source.index("// Static State")]
            # Default argument normally comes from HAL_Input.h.
            declarations = "bool inputConsumeSnapshot(InputCache&, uint32_t timeoutMs = 10);\n"
            (work / "input_snapshot_under_test.h").write_text(
                reading.group() + "\n" + declarations + owner + "\n" + function.group())
            binary = work / "snapshot"
            subprocess.run(["c++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I", str(work),
                            str(here / "input_button_snapshot_harness.cpp"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_no_input_owner_reports_unavailable(self):
        here = Path(__file__).parent
        source = (here.parents[1] / "HAL_Input.cpp").read_text()
        header = (here.parents[1] / "HAL_Input.h").read_text()
        reading = re.search(r"struct InputCache \{.*?\n\};", header, re.S).group()
        owner = source[source.index("// Shared input readings"):source.index("// Static State")]
        with tempfile.TemporaryDirectory(prefix="hardwareone-no-input-") as work:
            work = Path(work)
            program = work / "none.cpp"
            program.write_text("#include <stdint.h>\n#include <stddef.h>\n#include <assert.h>\n"
                               "#define ENABLE_OLED_INPUT 0\n" + reading + "\n" + owner + r"""
int main() {
  InputCache out; out.dataValid = true; out.buttonPressedAccum = 123;
  assert(!inputCacheSnapshot(out, 0) && !out.dataValid && !out.buttonPressedAccum);
  assert(!inputConsumeSnapshot(out, 0));
  uint32_t seq = 91;
  assert(!inputActivitySequence(seq, 0) && seq == 91);
  assert(!inputCacheInit() && inputCacheStorageBytes() == 0);
  assert(!inputCacheInvalidate() && !inputCacheReset(0, 0, 0, true));
  assert(!inputPublishState(0, 0, 0, 0, false, 0));
  assert(!inputPublishGamepadState(0, 0, 0, 0, 0));
}
""")
            binary = work / "none"
            subprocess.run(["c++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", str(program), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_native_ano_snapshot_detents_and_start_admission(self):
        here = Path(__file__).parent
        source = (here.parents[1] / "i2csensor_ano_encoder.cpp").read_text()
        header = (here.parents[1] / "i2csensor_ano_encoder.h").read_text()
        reading = re.search(r"struct AnoEncoderCache \{.*?\n\};", header, re.S).group()
        functions = []
        for name in ("anoEncoderCacheSnapshot", "invalidateAnoCache", "inputStartInternal",
                     "anoEncoderResetAxisForMode", "anoEncoderConsumeOneDetent"):
            match = re.search(r"^(?:static )?(?:bool|int|void) " + name + r"\([^\n]*\) \{\n.*?^\}",
                              source, re.S | re.M)
            self.assertIsNotNone(match, name)
            functions.append(match.group())
        with tempfile.TemporaryDirectory(prefix="hardwareone-ano-cache-") as work:
            work = Path(work)
            (work / "ano_owner_under_test.h").write_text(
                reading + "\nstatic AnoEncoderCache sAnoEncoderCache;\n"
                "static SemaphoreHandle_t sAnoCacheMutex = nullptr;\n" + "\n".join(functions))
            binary = work / "ano"
            subprocess.run(["c++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I", str(work),
                            str(here / "ano_cache_harness.cpp"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
