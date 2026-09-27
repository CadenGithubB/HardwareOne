#!/usr/bin/env python3
"""Offline checks of the actual copied application's STA-MAC cache.

Run directly or through unittest discovery. The test extracts the cache code
from private/app, supplies host substitutes for the driver and FreeRTOS mux,
and compiles it in a temporary directory. It never opens a serial port or
changes application sources. Missing private sources or a C++ compiler skip
the tests; compilation or behavior failures fail them.
"""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


HERE = Path(__file__).resolve().parent
SOURCE = HERE / "private/app/components/hardwareone/System_ESPNow.cpp"


HOST_STUBS = r"""
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#define CHECK(expr) do { if (!(expr)) { \
  std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #expr); \
  std::abort(); } } while (0)

using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define taskENTER_CRITICAL(m) (m)->lock()
#define taskEXIT_CRITICAL(m) (m)->unlock()
constexpr int ESP_OK = 0, WIFI_IF_STA = 0;
static const uint8_t kA[6] = {2, 1, 2, 3, 4, 5};
static const uint8_t kB[6] = {4, 6, 7, 8, 9, 10};
static const uint8_t kMixed[6] = {2, 1, 2, 8, 9, 10};
static std::atomic<int> queryCount{0};
static std::mutex driverLock;
static std::condition_variable driverCondition;
static uint8_t driverMac[6] = {2, 1, 2, 3, 4, 5};
static bool queryFails = false;
static bool blockQuery = false, queryEntered = false, queryMayReturn = false;

static void setDriver(const uint8_t* mac, bool fails = false) {
  std::lock_guard<std::mutex> guard(driverLock);
  std::memcpy(driverMac, mac, 6);
  queryFails = fails;
}

int esp_wifi_get_mac(int which, uint8_t* out) {
  CHECK(which == WIFI_IF_STA);
  ++queryCount;
  std::unique_lock<std::mutex> guard(driverLock);
  uint8_t snapshot[6];
  std::memcpy(snapshot, driverMac, 6);
  const bool fails = queryFails;
  if (blockQuery) {
    queryEntered = true;
    driverCondition.notify_all();
    CHECK(driverCondition.wait_for(guard, std::chrono::seconds(5),
                                   [] { return queryMayReturn; }));
  }
  // Return matching bytes even on error: ignoring the status must fail tests.
  std::memcpy(out, snapshot, 6);
  return fails ? -1 : ESP_OK;
}
"""


HOST_CASES = r"""
static void fallbackCase() {
  CHECK(!isSelfMac(nullptr));
  CHECK(queryCount == 0);
  setDriver(kA, true);
  CHECK(!isSelfMac(kA));
  CHECK(queryCount == 1);
  setDriver(kA);
  CHECK(isSelfMac(kA));
  CHECK(!isSelfMac(kB));
  setDriver(kB);
  CHECK(!isSelfMac(kA));
  CHECK(isSelfMac(kB));
  CHECK(queryCount == 5); // Stopped callers must not populate the runtime cache.
}

static void runtimeAndReinitCase() {
  setEspNowSelfMacCache(kA);
  setDriver(kB, true);
  for (int i = 0; i < 10000; ++i) {
    CHECK(isSelfMac(kA));
    CHECK(!isSelfMac(kB));
  }
  CHECK(queryCount == 0);
  setEspNowSelfMacCache(nullptr);
  CHECK(!isSelfMac(kB)); // A failed query cannot use the old or returned bytes.
  setDriver(kB);
  CHECK(isSelfMac(kB));
  CHECK(!isSelfMac(kA));
  setEspNowSelfMacCache(kB);
  const int before = queryCount;
  setDriver(kA, true);
  CHECK(isSelfMac(kB));
  CHECK(!isSelfMac(kA));
  CHECK(queryCount == before);
}

static void concurrentPublicationCase() {
  setEspNowSelfMacCache(kA);
  std::atomic<bool> start{false};
  std::thread writer([&] {
    while (!start.load()) std::this_thread::yield();
    for (int i = 0; i < 50000; ++i) setEspNowSelfMacCache(i % 2 ? kA : kB);
  });
  std::thread reader([&] {
    start.store(true);
    for (int i = 0; i < 50000; ++i) CHECK(!isSelfMac(kMixed));
  });
  writer.join();
  reader.join();
  CHECK(queryCount == 0);
}

static void lateFallbackCase() {
  setDriver(kA);
  {
    std::lock_guard<std::mutex> guard(driverLock);
    blockQuery = true;
  }
  std::thread reader([] { CHECK(isSelfMac(kA)); });
  {
    std::unique_lock<std::mutex> guard(driverLock);
    CHECK(driverCondition.wait_for(guard, std::chrono::seconds(5),
                                   [] { return queryEntered; }));
  }
  // The old driver query is still in flight. Startup must be able to publish
  // a newer identity without waiting for it or having it overwrite the cache.
  setEspNowSelfMacCache(kB);
  {
    std::lock_guard<std::mutex> guard(driverLock);
    queryMayReturn = true;
  }
  driverCondition.notify_all();
  reader.join();
  CHECK(isSelfMac(kB));
  CHECK(!isSelfMac(kA));
  CHECK(queryCount == 1);
}

int main(int argc, char** argv) {
  CHECK(argc == 2);
  if (std::strcmp(argv[1], "fallback") == 0) fallbackCase();
  else if (std::strcmp(argv[1], "runtime_reinit") == 0) runtimeAndReinitCase();
  else if (std::strcmp(argv[1], "concurrent") == 0) concurrentPublicationCase();
  else if (std::strcmp(argv[1], "late_fallback") == 0) lateFallbackCase();
  else CHECK(false);
}
"""


class MacCacheTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not SOURCE.is_file():
            raise unittest.SkipTest("Recreate private/app using BUILD.md to test its actual cache")
        compiler = next((path for name in ("clang++", "g++", "c++")
                         if (path := shutil.which(name))), None)
        if compiler is None:
            raise unittest.SkipTest("A host C++17 compiler is required")
        cls.source = SOURCE.read_text()
        start = cls.source.index("static portMUX_TYPE gEspNowSelfMacLock")
        end = cls.source.index("// Runtime boundary shared", start)
        # These are the real globals and functions, not a reimplementation.
        implementation = cls.source[start:end]
        cls.temporary = tempfile.TemporaryDirectory(prefix="hw1-mac-cache-")
        cls.addClassCleanup(cls.temporary.cleanup)
        source_path = Path(cls.temporary.name) / "cache.cpp"
        cls.executable = Path(cls.temporary.name) / "cache_test"
        source_path.write_text(HOST_STUBS + implementation + HOST_CASES)
        result = subprocess.run(
            [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
             str(source_path), "-o", str(cls.executable)],
            capture_output=True, text=True, timeout=30,
        )
        if result.returncode:
            raise AssertionError("Actual cache code failed to compile:\n" + result.stdout + result.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], capture_output=True,
                                text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_checked_fallback_and_null_address(self):
        self.run_case("fallback")

    def test_runtime_has_no_rpc_and_reinit_changes_identity(self):
        self.run_case("runtime_reinit")

    def test_concurrent_publication_does_not_expose_mixed_identity(self):
        self.run_case("concurrent")

    def test_late_fallback_cannot_overwrite_new_cache(self):
        self.run_case("late_fallback")

    def test_startup_and_shutdown_use_existing_admission_boundaries(self):
        source = self.source
        init = source[source.index("static bool initEspNow() {"):
                      source.index("// ESP-NOW init command")]
        self.assertLess(init.index("setEspNowSelfMacCache(nullptr)"),
                        init.index("WiFi.mode(WIFI_AP_STA)"))
        self.assertLess(init.index("WiFi.mode(WIFI_AP_STA)"),
                        init.index("selfMacErr = esp_wifi_get_mac"))
        checked = init.index("if (selfMacErr != ESP_OK)")
        publish = init.index("setEspNowSelfMacCache(radioSelfMac)")
        self.assertLess(checked, publish)
        self.assertIn("return false;", init[checked:publish])
        self.assertLess(publish, init.index("setEspNowCallbacksEnabled(true)"))
        failed_registration = init[init.index("const bool callbacksClean = waitForEspNowCallbacks(1000)"):]
        self.assertLess(failed_registration.index("if (callbacksClean)"),
                        failed_registration.index("setEspNowSelfMacCache(nullptr)"))
        close = source[source.index("static bool deinitEspNow(bool publishOffEvent) {"):
                       source.index("bool espnowShutdownPending()")]
        boundaries = ["waitForEspNowRuntimeOps(5000)", "stopEspNowTask(5000)",
                      "waitForEspNowCallbacks(2000)", "setEspNowSelfMacCache(nullptr)"]
        offsets = [close.index(boundary) for boundary in boundaries]
        self.assertEqual(offsets, sorted(offsets))


if __name__ == "__main__":
    unittest.main()
