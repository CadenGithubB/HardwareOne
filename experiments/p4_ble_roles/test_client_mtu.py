"""Exercise the actual shared G2/R1 MTU helper with controlled BLE callbacks.

These checks cover request ordering, negotiated values, failure and wait bounds.
They do not model Bluedroid timing, RF, service discovery internals or hardware.
Reconstruct private/app first; a missing copy/compiler produces a skip.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parent / "private/app/components/hardwareone/G2_Glasses.cpp"
COMPILER = shutil.which("c++")

STUBS = r"""
#include <cstdint>
#include <cstdlib>
#include <map>
#include <cassert>
#include <cstdio>
uint32_t tick = 0;
uint32_t millis() { return tick; }
#define pdMS_TO_TICKS(x) (x)
void vTaskDelay(uint32_t delta) { tick += delta; }
#define DEBUG_G2F(...) do { if (false) std::printf(__VA_ARGS__); } while (0)
struct BLEDevice {
  static uint16_t local;
  static int raises;
  static uint16_t getMTU() { return local; }
  static void setMTU(uint16_t value) { local = value; ++raises; }
};
uint16_t BLEDevice::local = 23;
int BLEDevice::raises = 0;
struct BLEClient {
  bool connected = true, empty = false, discoveryDisconnect = false;
  bool queue = true, disconnectWait = false;
  uint16_t mtu = 23, discoveryMtu = 23;
  int discoveries = 0, requests = 0;
  uint32_t requestAt = 0;
  std::map<int, int> services;
  bool isConnected() {
    if (disconnectWait && requests && uint32_t(tick - requestAt) >= 10) connected = false;
    return connected;
  }
  uint16_t getMTU() {
    // Only this simulated CFG_MTU callback changes the negotiated value.
    if (requests && queue && uint32_t(tick - requestAt) >= 20) mtu = 200;
    return mtu;
  }
  bool setMTU(uint16_t) {
    assert(discoveries == 1); // never race initial discovery
    ++requests;
    requestAt = tick;
    return queue;
  }
  std::map<int, int>* getServices() {
    ++discoveries;
    assert(requests == 0);
    if (discoveryDisconnect) connected = false;
    if (!empty) services[1] = 1;
    mtu = discoveryMtu;
    return &services;
  }
};
"""

CHECKS = r"""
int main(int argc, char** argv) {
  assert(argc == 2);
  BLEClient client;
  switch (std::atoi(argv[1])) {
    case 0:
      assert(bleNegotiateConnMtu(nullptr, 247, 100, "test") == 23);
      client.connected = false;
      assert(bleNegotiateConnMtu(&client, 247, 100, "test") == 23);
      assert(client.requests == 0 && client.discoveries == 0);
      break;
    case 1:
      client.mtu = 200; BLEDevice::local = 517;
      assert(bleNegotiateConnMtu(&client, 247, 100, "test") == 200);
      assert(client.requests == 0 && client.discoveries == 0);
      assert(BLEDevice::local == 517 && BLEDevice::raises == 0);
      break;
    case 2:
      client.discoveryMtu = 180;
      assert(bleNegotiateConnMtu(&client, 247, 100, "test") == 180);
      assert(client.requests == 0 && client.discoveries == 1);
      break;
    case 3:
      assert(bleNegotiateConnMtu(&client, 247, 100, nullptr) == 200);
      assert(client.requests == 1 && client.discoveries == 1);
      assert(BLEDevice::local == 247 && BLEDevice::raises == 1);
      break;
    case 4:
      client.empty = true;
      assert(bleNegotiateConnMtu(&client, 247, 100, "test") == 23);
      assert(client.requests == 0);
      break;
    case 5:
      client.discoveryDisconnect = true;
      assert(bleNegotiateConnMtu(&client, 247, 100, "test") == 23);
      assert(client.requests == 0);
      break;
    case 6:
      client.queue = false;
      assert(bleNegotiateConnMtu(&client, 247, 100, "test") == 23);
      assert(client.requests == 1 && tick == 0);
      break;
    case 7:
      assert(bleNegotiateConnMtu(&client, 247, 10, "test") == 23);
      assert(tick == 10); // requested ceiling must not masquerade as negotiation
      break;
    case 8:
      tick = 0xfffffff5;
      assert(bleNegotiateConnMtu(&client, 247, 100, "test") == 200);
      assert(tick == 9); // elapsed-time deadline survives millis() wrap
      break;
    case 9:
      client.disconnectWait = true;
      assert(bleNegotiateConnMtu(&client, 247, 100, "test") == 23);
      assert(tick == 10);
      break;
    default: assert(false);
  }
}
"""


@unittest.skipUnless(COMPILER and SOURCE.is_file(),
                     "Reconstructed BLE-role copy and C++17 compiler required")
class ClientMtuTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = SOURCE.read_text()
        start = source.index("uint16_t bleNegotiateConnMtu(")
        end = source.index("\nstatic bool connectTemple", start)
        cls.work = tempfile.TemporaryDirectory(prefix="hw1-client-mtu-")
        cls.addClassCleanup(cls.work.cleanup)
        work = Path(cls.work.name)
        (work / "checks.cpp").write_text(STUBS + source[start:end] + CHECKS)
        cls.executable = work / "checks"
        result = subprocess.run(
            [COMPILER, "-std=c++17", "-Wall", "-Wextra", "-Werror",
             str(work / "checks.cpp"), "-o", str(cls.executable)],
            capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def scenario(self, number):
        result = subprocess.run([str(self.executable), str(number)],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_absent_or_disconnected_client(self): self.scenario(0)
    def test_completed_exchange_not_repeated(self): self.scenario(1)
    def test_exchange_completes_during_discovery(self): self.scenario(2)
    def test_discovery_then_one_request_then_callback(self): self.scenario(3)
    def test_empty_discovery_does_not_request(self): self.scenario(4)
    def test_disconnect_during_discovery(self): self.scenario(5)
    def test_immediate_request_failure(self): self.scenario(6)
    def test_timeout_keeps_default_mtu(self): self.scenario(7)
    def test_clock_wrap(self): self.scenario(8)
    def test_disconnect_during_mtu_wait(self): self.scenario(9)


if __name__ == "__main__":
    unittest.main()
