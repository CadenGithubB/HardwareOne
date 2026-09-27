"""Check the prepared R1 subscription block and actual checked descriptor writer.

The host harness extracts current production code; only the BLE/RTOS boundary
and characteristic registration are mocked. It verifies the requested security,
CCCD payload, bounded completion and fail-closed R1 setup gate. It cannot prove
SMP interoperability, peripheral policy, RF timing or real callback lifetimes.
Run with python3 -B experiments/p4_ble_roles/tests/test_ring_notify_security.py -v.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest


EXPERIMENT = Path(__file__).resolve().parents[1]
APP = Path(os.environ.get("HW1_BLE_ROLES_APP", EXPERIMENT / "private/app"))
COMPILER = shutil.which(os.environ.get("CXX", "c++"))


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


STUBS = r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
using esp_err_t = int;
using esp_gatt_auth_req_t = int;
constexpr int ESP_OK = 0, ESP_GATT_OK = 0, ESP_GATT_INSUF_AUTHENTICATION = 5;
constexpr int ESP_ERR_INVALID_STATE = 10, ESP_ERR_TIMEOUT = 11;
constexpr int ESP_GATT_AUTH_REQ_NO_MITM = 1;
constexpr int ESP_GATT_WRITE_TYPE_RSP = 1, ESP_GATT_WRITE_TYPE_NO_RSP = 2;
#define log_v(...) ((void)0)
#define log_e(...) ((void)0)
#define ERROR_RINGF(...) ((void)0)
void portENTER_CRITICAL(int*) {}
void portEXIT_CRITICAL(int*) {}
int scenario = 0, apiCalls = 0, authSeen = -1, failureNotes = 0, clears = 0;
int appSetupCalls = 0, gateGives = 0;
uint32_t millis() { return 100; }
struct BLEUUID { uint16_t value; explicit BLEUUID(uint16_t v): value(v) {} };
struct Client {
  bool connected = true;
  int disconnects = 0;
  bool isConnected() { return connected; }
  int getGattcIf() { return 7; }
  int getConnId() { return 9; }
  void disconnect() { connected = false; ++disconnects; }
} client;
struct Service { Client* getClient() { return &client; } } service;
class BLERemoteDescriptor;
struct Semaphore {
  BLERemoteDescriptor* owner;
  bool take(uint32_t timeout, const char* label);
  void give() { ++gateGives; }
};
struct BLERemoteNotifyResult {
  bool success = false, timedOut = false;
  int apiStatus = 0, eventStatus = 0, descriptorApiStatus = 0, descriptorEventStatus = 0;
};
struct Characteristic {
  BLERemoteDescriptor* descriptor = nullptr;
  int lookups = 0, registrations = 0;
  Service* getRemoteService() { return &service; }
  BLERemoteDescriptor* getDescriptor(BLEUUID uuid) {
    assert(uuid.value == 0x2902); ++lookups; return descriptor;
  }
  BLERemoteNotifyResult registerForNotify(void (*)());
} characteristic;
'''

DESCRIPTOR_STUB = r'''
class BLERemoteDescriptor {
public:
  uint8_t m_auth = 0;
  int m_writeResultMux = 0;
  uint32_t m_writeGeneration = 0, m_writePendingGeneration = 0, m_writeCompletedGeneration = 0;
  BLERemoteDescriptorWriteResult m_lastWriteResult;
  Semaphore m_semaphoreWriteDescrEvt{this};
  Characteristic* m_pRemoteCharacteristic = &characteristic;
  Characteristic* getRemoteCharacteristic() { return m_pRemoteCharacteristic; }
  uint16_t getHandle() { return 42; }
  std::string toString() { return "descriptor"; }
  void setAuth(uint8_t);
  BLERemoteDescriptorWriteResult writeValueChecked(uint8_t*, size_t, bool, uint32_t);
} descriptor;
bool Semaphore::take(uint32_t timeout, const char* label) {
  assert(timeout == 10000); // Existing bounded default reaches both waits.
  if (std::strstr(label, "-gate")) return true;
  assert(std::strstr(label, "-event"));
  if (scenario == 4) return false; // No completion: security exchange timed out.
  auto& result = owner->m_lastWriteResult;
  result.eventReceived = true;
  result.eventStatus = scenario == 1 ? ESP_GATT_INSUF_AUTHENTICATION : ESP_GATT_OK;
  result.disconnected = scenario == 5;
  owner->m_writeCompletedGeneration = result.generation;
  return true;
}
esp_err_t esp_ble_gattc_write_char_descr(int iface, int conn, uint16_t handle,
  size_t length, uint8_t* data, int writeType, esp_gatt_auth_req_t auth) {
  ++apiCalls;
  assert(iface == 7 && conn == 9 && handle == 42);
  assert(length == 2 && data[0] == 1 && data[1] == 0);
  assert(writeType == ESP_GATT_WRITE_TYPE_RSP);
  authSeen = auth;
  return scenario == 3 ? ESP_ERR_INVALID_STATE : ESP_OK;
}
'''

RING_STUBS = r'''
BLERemoteNotifyResult Characteristic::registerForNotify(void (*)()) {
  ++registrations;
  BLERemoteNotifyResult result;
  // Registration ACK alone cannot override a rejected/missing CCCD.
  auto* cached = getDescriptor(BLEUUID(0x2902));
  if (!cached) { result.descriptorApiStatus = 12; return result; }
  uint8_t value[] = {1, 0};
  const auto written = cached->writeValueChecked(value, sizeof(value), true, 10000);
  result.success = written.success && scenario != 6;
  result.timedOut = written.timedOut;
  result.descriptorApiStatus = written.apiStatus;
  result.descriptorEventStatus = written.eventStatus;
  return result;
}
struct Ring {
  Characteristic* notifyChar = &characteristic;
  Client* client = &::client;
} gRing;
void ringNotifyThunk() {}
void ringNoteConnectFailure(const char*, uint32_t) { ++failureNotes; }
void ringClearGattPointers(bool) { ++clears; }
bool trySubscription() {
  const uint32_t t0 = 0;
'''

CHECKS = r'''
  ++appSetupCalls; // Stand-in for the later real ringRunStandardSetup call.
  return true;
}
int main(int argc, char** argv) {
  assert(argc == 2); scenario = std::atoi(argv[1]);
  characteristic.descriptor = scenario == 2 ? nullptr : &descriptor;
  if (scenario == 7) client.connected = false;
  const bool ok = trySubscription();
  assert(characteristic.registrations == 1);
  assert(characteristic.lookups == 2); // Setter and registration share cached CCCD.
  if (scenario == 0) {
    assert(ok && appSetupCalls == 1 && client.disconnects == 0);
    assert(descriptor.m_lastWriteResult.success);
  } else {
    assert(!ok && appSetupCalls == 0);
    assert(failureNotes == 1 && clears == 1 && client.disconnects == 1);
  }
  if (scenario == 2 || scenario == 7) assert(apiCalls == 0);
  else { assert(apiCalls == 1); assert(authSeen == ESP_GATT_AUTH_REQ_NO_MITM); }
  if (scenario != 2) assert(descriptor.m_auth == ESP_GATT_AUTH_REQ_NO_MITM);
  if (scenario == 4) {
    assert(descriptor.m_lastWriteResult.timedOut && gateGives == 0);
    assert(descriptor.m_writePendingGeneration != 0); // No unsafe timeout reopen.
  }
  if (scenario == 1) assert(!descriptor.m_lastWriteResult.success);
}
'''


class RingNotifySecurityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not COMPILER or not APP.is_dir():
            raise RuntimeError("Prepare the roles app and provide a C++17 compiler")
        ring = (APP / "components/hardwareone/G2_Ring.cpp").read_text()
        ble = APP / "components/arduino/libraries/BLE/src"
        descriptor = (ble / "BLERemoteDescriptor.cpp").read_text()
        header = (ble / "BLERemoteDescriptor.h").read_text()
        result_start = header.index("struct BLERemoteDescriptorWriteResult {")
        result_end = header.index("\n};", result_start) + 3
        start = ring.index("  // R1 requires link encryption for its CCCD")
        end = ring.index("\n  if (ringAbortSupersededRequest", start)
        # Preserve the existing connection publication and application-auth order.
        cls.connect_tail = ring[end:ring.index("  bool completionCurrent", end)]
        cls.work = tempfile.TemporaryDirectory(prefix="hw1-ring-security-")
        cls.addClassCleanup(cls.work.cleanup)
        work = Path(cls.work.name)
        code = (STUBS + header[result_start:result_end] + DESCRIPTOR_STUB
                + function(descriptor, "void BLERemoteDescriptor::setAuth(")
                + function(descriptor, "BLERemoteDescriptorWriteResult BLERemoteDescriptor::writeValueChecked(")
                + RING_STUBS + ring[start:end] + CHECKS)
        source = work / "checks.cpp"
        source.write_text(code)
        cls.executable = work / "checks"
        result = subprocess.run([
            COMPILER, "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", str(source), "-o", str(cls.executable),
        ], capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def scenario(self, number):
        result = subprocess.run([str(self.executable), str(number)],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_encrypted_cccd_ack_permits_setup(self): self.scenario(0)
    def test_authentication_error_stops_setup(self): self.scenario(1)
    def test_missing_cccd_stops_setup(self): self.scenario(2)
    def test_rejected_api_stops_setup(self): self.scenario(3)
    def test_timeout_stops_setup_and_keeps_gate_closed(self): self.scenario(4)
    def test_disconnect_completion_stops_setup(self): self.scenario(5)
    def test_registration_error_stops_setup(self): self.scenario(6)
    def test_already_disconnected_never_writes(self): self.scenario(7)

    def test_application_auth_remains_after_subscription(self):
        self.assertLess(self.connect_tail.index("gRing.connected      = true;"),
                        self.connect_tail.index("if (!ringRunStandardSetup())"))


if __name__ == "__main__":
    unittest.main()
