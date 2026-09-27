"""Exercise the prepared G2 enqueue code and its real teardown admission gate.

Only RTOS, allocation and lifecycle-stamping boundaries are substituted. A
negative control restores the old guard placement and must reproduce the leak.
No firmware build, device or network is used. Run this file with python3 -B -v.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest


APP = Path(os.environ.get("HW1_BLE_ROLES_APP", Path(__file__).resolve().parents[1] / "private/app"))


def extract(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


PLATFORM = r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
using QueueHandle_t = void*;
using TaskHandle_t = void*;
using TickType_t = uint32_t;
#define DEBUG_G2F(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
constexpr int pdTRUE = 1;
int gControlLifecycleMux;
void portENTER_CRITICAL(int*) {}
void portEXIT_CRITICAL(int*) {}
uint8_t gClientInitters = 0, gUiWorkerInitters = 0, gUiSubmitters = 0;
uint8_t gControlStartBlocks = 0;
enum class G2WorkerInitState { Uninitialized, Ready };
G2WorkerInitState gPageSwapInitState = G2WorkerInitState::Ready;
QueueHandle_t gPageSwapQueue = reinterpret_cast<void*>(1);
TaskHandle_t gPageSwapTaskH = reinterpret_cast<void*>(2);
struct Runtime { bool initialized = true; } runtime;
Runtime* gG2State = &runtime;
bool stampAllowed = true, allocationAllowed = true, queueAllowed = true;
int jobCount = 0, sends = 0;
uint32_t gLensDropped = 0, gLensAccepted = 0, gLensQueueDepthPeak = 0;
constexpr uint32_t kPageSwapQueueDepth = 4;
struct PageSwapArgs {
  uint32_t lifecycleEpoch = 0, sourcePresentationEpoch = 0, sourceTextSlotSerial = 0;
};
enum class LensJobKind { PageSwap };
struct LensUiJob {
  LensJobKind kind;
  uint32_t submitMenuGen, enqueuedAtMs, cmdSeq, targetPage, targetNetSub;
  struct { PageSwapArgs* swap; } payload;
  static void* operator new(size_t n, const std::nothrow_t&) noexcept {
    if (!allocationAllowed) return nullptr;
    void* p = std::malloc(n);
    if (p) ++jobCount;
    return p;
  }
  static void operator delete(void* p) noexcept { --jobCount; std::free(p); }
};
LensUiJob* queued = nullptr;
bool pageSwapInit() { return true; }
TaskHandle_t xTaskGetCurrentTaskHandle() { return reinterpret_cast<void*>(3); }
uint32_t millis() { return 7; }
uint32_t g2CurrentMenuGen() { return 8; }
uint32_t g2GetHijackPage() { return 9; }
uint32_t uxQueueMessagesWaiting(QueueHandle_t) { return queued ? 1 : 0; }
void observeHwm(uint32_t* p, uint32_t n) { if (n > *p) *p = n; }
bool g2TextDispatchStampPageSwap(uint32_t* life, uint32_t* view, uint32_t* slot) {
  if (!stampAllowed) return false;
  *life = 101; *view = 102; *slot = 103;
  return true;
}
static bool g2ClientInitIdle();
int xQueueSend(QueueHandle_t, LensUiJob** job, TickType_t) {
  ++sends;
  // Teardown must remain blocked until the enqueue returns, including failure.
  assert(gUiSubmitters > 0 && !g2ClientInitIdle());
  if (!queueAllowed) return 0;
  queued = *job;
  return pdTRUE;
}
'''

CHECKS = r'''
int main(int argc, char** argv) {
  assert(argc == 2);
  PageSwapArgs args;
  const char* scenario = argv[1];
  if (!std::strcmp(scenario, "rejected")) {
    stampAllowed = false;
    for (int i = 0; i < 300; ++i) {
      assert(!pageSwapEnqueue(&args));
      assert(gUiSubmitters == 0 && g2ClientInitIdle());
    }
    assert(sends == 0 && jobCount == 0 && args.lifecycleEpoch == 0);
  } else if (!std::strcmp(scenario, "unavailable")) {
    gG2State = nullptr;
    assert(!pageSwapEnqueue(&args));
    assert(gUiSubmitters == 0 && sends == 0);
  } else if (!std::strcmp(scenario, "allocation")) {
    allocationAllowed = false;
    assert(!pageSwapEnqueue(&args));
    assert(g2ClientInitIdle() && sends == 0 && jobCount == 0);
  } else if (!std::strcmp(scenario, "queue-full")) {
    queueAllowed = false;
    assert(!pageSwapEnqueue(&args));
    assert(g2ClientInitIdle() && sends == 1 && jobCount == 0 && !queued);
  } else if (!std::strcmp(scenario, "other-claim")) {
    gUiSubmitters = 1;
    stampAllowed = false;
    assert(!pageSwapEnqueue(&args));
    assert(gUiSubmitters == 1 && !g2ClientInitIdle());
    gUiSubmitters = 0;
    gControlStartBlocks = 1;
    assert(!pageSwapEnqueue(&args));
    assert(gUiSubmitters == 0 && sends == 0);
  } else {
    assert(!std::strcmp(scenario, "success"));
    assert(pageSwapEnqueue(&args));
    assert(g2ClientInitIdle() && sends == 1 && jobCount == 1);
    assert(queued && queued->payload.swap == &args);
    assert(args.lifecycleEpoch == 101 && args.sourcePresentationEpoch == 102);
    assert(args.sourceTextSlotSerial == 103 && gLensAccepted == 1);
    delete queued;
    assert(jobCount == 0);
  }
}
'''


class G2SubmitLifetimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which(os.environ.get("CXX", "c++"))
        if not compiler:
            raise RuntimeError("C++ compiler required")
        source = (APP / "components/hardwareone/G2_Glasses.cpp").read_text()
        parts = [extract(source, signature) for signature in (
            "static bool g2ClientInitIdle()",
            "static void g2UiSubmitDone()",
            "struct G2UiSubmitClaim",
            "static bool pageSwapRuntimeSnapshot(",
            "static bool pageSwapEnqueue(",
        )]
        parts[2] += ";"
        cls.temp = tempfile.TemporaryDirectory(prefix="hw1-g2-submit-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binaries = {}
        for name in ("current", "old-placement"):
            selected = list(parts)
            if name == "old-placement":
                selected[-1] = selected[-1].replace("  G2UiSubmitClaim submitClaim;\n", "", 1)
                selected[-1] = selected[-1].replace(
                    "  LensUiJob* job = new", "  G2UiSubmitClaim submitClaim;\n  LensUiJob* job = new", 1)
            path = Path(cls.temp.name) / (name + ".cpp")
            path.write_text(PLATFORM + "\n".join(selected) + CHECKS)
            binary = path.with_suffix("")
            subprocess.run([compiler, "-std=c++17", "-fsanitize=address,undefined",
                            "-fno-omit-frame-pointer", str(path), "-o", str(binary)],
                           check=True, capture_output=True, text=True)
            cls.binaries[name] = binary

    def test_current_enqueue_lifetimes(self):
        for scenario in ("rejected", "unavailable", "allocation", "queue-full", "other-claim", "success"):
            with self.subTest(scenario=scenario):
                subprocess.run([str(self.binaries["current"]), scenario],
                               check=True, capture_output=True, text=True)

    def test_old_guard_placement_reproduces_stuck_gate(self):
        result = subprocess.run([str(self.binaries["old-placement"]), "rejected"],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("gUiSubmitters == 0 && g2ClientInitIdle()", result.stderr)


if __name__ == "__main__":
    unittest.main()
