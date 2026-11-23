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
            reading = re.search(r"struct InputCache \{.*?\n\};.*?inline uint32_t inputNavButtons[^\n]*\n",
                                header, re.S)
            self.assertIsNotNone(reading)
            owner = source[source.index("// Shared input readings"):source.index("// Static State")]
            # Default argument normally comes from HAL_Input.h.
            declarations = ("bool inputConsumeSnapshot(InputCache&, uint32_t timeoutMs = 10);\n"
                            "bool inputPublishState(uint32_t, int, int, uint32_t, bool, unsigned long,"
                            " uint32_t navHidden = 0, uint32_t waitMs = 50);\n")
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
        reading = re.search(r"struct InputCache \{.*?\n\};.*?inline uint32_t inputNavButtons[^\n]*\n",
                            header, re.S).group()
        # The navHidden and waitMs defaults normally come from HAL_InputInternal.h.
        reading += ("bool inputPublishState(uint32_t, int, int, uint32_t, bool, unsigned long,"
                    " uint32_t navHidden = 0, uint32_t waitMs = 50);\n")
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

    def test_imu_input_reading_goes_stale_and_publishes_without_waiting(self):
        # The IMU input (INPUT_DEVICE_TYPE 5) has no task of its own: its hook
        # publishes with no wait for the lock, and when the IMU's samples stop
        # the last deflection is handed out centred instead of scrolling on.
        here = Path(__file__).parent
        source = (here.parents[1] / "HAL_Input.cpp").read_text()
        header = (here.parents[1] / "HAL_Input.h").read_text()
        reading = re.search(r"struct InputCache \{.*?\n\};.*?inline uint32_t inputNavButtons[^\n]*\n",
                            header, re.S).group()
        reading += ("bool inputConsumeSnapshot(InputCache&, uint32_t timeoutMs = 10);\n"
                    "bool inputCacheSnapshot(InputCache&, uint32_t timeoutMs = 10);\n"
                    "bool inputPublishState(uint32_t, int, int, uint32_t, bool, unsigned long,"
                    " uint32_t navHidden = 0, uint32_t waitMs = 50);\n")
        owner = source[source.index("// Shared input readings"):source.index("// Static State")]
        self.assertIn("imuInputExpire(out);", owner)
        with tempfile.TemporaryDirectory(prefix="hardwareone-imu-input-") as work:
            work = Path(work)
            program = work / "stale.cpp"
            program.write_text(r"""
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
using SemaphoreHandle_t = void*;
using StaticSemaphore_t = int;
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) { return storage; }
#define pdMS_TO_TICKS(ms) (ms)
static bool lockFree = true;
static unsigned lastWait = 99;
struct SensorCacheGuard {
  bool held;
  SensorCacheGuard(SemaphoreHandle_t, unsigned wait, const char*) : held(lockFree) { lastWait = wait; }
};
static bool gInputRunning = true;
static uint32_t now = 0;
static uint32_t millis() { return now; }
#define JOYSTICK_CENTER 512
#define ENABLE_OLED_INPUT 1
#define ENABLE_IMU_INPUT 1
""" + reading + "\n" + owner + r"""
int main() {
  InputCache out;
  // A sample at 1000 ms: deflected right, A held, an A press latched.
  now = 1000;
  assert(inputPublishState(~1u, 900, 512, 1u, true, 1000, 0, 0));
  assert(lastWait == 0);  // the hook's publish never waits for the lock
  now = 1100;
  assert(inputCacheSnapshot(out) && out.joyX == 900 && out.buttons == ~1u && out.buttonPressedAccum == 1u);
  now = 1250;  // 250 ms old: still the reading
  assert(inputCacheSnapshot(out) && out.joyX == 900);
  // The samples stopped: past a quarter of a second the reading is centred
  // and released; the latched press is still delivered, once.
  now = 1251;
  assert(inputCacheSnapshot(out) && out.joyX == 512 && out.joyY == 512 && out.buttons == UINT32_MAX);
  assert(out.buttonPressedAccum == 1u && out.dataValid);
  assert(inputConsumeSnapshot(out) && out.joyX == 512 && out.buttons == UINT32_MAX && out.buttonPressedAccum == 1u);
  assert(inputConsumeSnapshot(out) && out.buttonPressedAccum == 0);
  // The next sample is read as it is again.
  now = 5000;
  assert(inputPublishState(UINT32_MAX, 512, 100, 0, true, 5000, 0, 0));
  assert(inputConsumeSnapshot(out) && out.joyY == 100);
  // The millisecond counter wrapping is not staleness.
  now = 0xFFFFFFF0u;
  assert(inputPublishState(UINT32_MAX, 100, 512, 0, true, now, 0, 0));
  now = 0x00000010u;
  assert(inputCacheSnapshot(out) && out.joyX == 100);
  // A lock that is taken: the publish says so at once and the reading is unchanged.
  lockFree = false;
  assert(!inputPublishState(UINT32_MAX, 512, 512, 2u, true, now, 0, 0) && lastWait == 0);
  lockFree = true;
  assert(inputCacheSnapshot(out) && out.joyX == 100 && out.buttonPressedAccum == 0);
  // Every other producer keeps its 50 ms wait.
  assert(inputPublishState(UINT32_MAX, 512, 512, 0, false, now) && lastWait == 50);
}
""")
            binary = work / "stale"
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
                     "anoEncoderConsumeOneDetent"):
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
