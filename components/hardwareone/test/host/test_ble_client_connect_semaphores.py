#!/usr/bin/env python3
"""A connect() whose register or open wait timed out must not block the next one.

Splices the real BLEClient::connect() (components/arduino/libraries/BLE/src/
BLEClient.cpp) over a scripted Bluedroid and a binary semaphore that fails the
test the moment a take() would block forever. Before the 2026-11-06 patch a
timed-out timedWait() left the semaphore taken, and the next connect() on the
same object blocked in take() before any host call: the 2026-11-05 four-hour
BLE setup-worker stall, after a ring open-timeout with no OPEN event.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from test_web_batch_handlers import extract_block
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
p = argparse.ArgumentParser(description=__doc__); p.add_argument('--sanitize', action='store_true'); args = p.parse_args()
client = (ROOT / 'components/arduino/libraries/BLE/src/BLEClient.cpp').read_text()
connect = extract_block(client, 'bool BLEClient::connect(BLEAddress address, uint8_t type, uint32_t timeoutMs)')
connect = connect.replace('BLEClient::connect', 'Client::connect')
assert 'm_semaphoreOpenEvt.take("connect")' in connect and 'm_semaphoreRegEvt.take("connect")' in connect
unit = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
typedef int esp_err_t; typedef uint8_t esp_gatt_if_t; typedef int esp_ble_addr_type_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_GATT_OK 0
#define ESP_GATT_IF_NONE 0xff
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(ms) (ms)
#define log_e(...) ((void)0)
#define log_v(...) ((void)0)
struct portMUX_TYPE { int unused; };
#define portENTER_CRITICAL(m) ((void)0)
#define portEXIT_CRITICAL(m) ((void)0)
static constexpr int32_t BLE_CLIENT_STATUS_NOT_SET = INT32_MIN;
static constexpr uint32_t kGattcUnregisterWaitMs = 2000;
enum class BLEClientConnectStage : uint8_t { NONE = 0, PRECHECK, APP_REGISTER_API, APP_REGISTER_EVENT, APP_REGISTER_TIMEOUT, OPEN_API, OPEN_EVENT, OPEN_TIMEOUT, COMPLETE };
struct BLEAddress { uint8_t b[6] = {1, 2, 3, 4, 5, 6}; const uint8_t* getNative() const { return b; } std::string toString() const { return "01:02:03:04:05:06"; } };
// The library's binary semaphore contract: take() blocks until given, timedWait()
// takes within the timeout and gives back on success only, give() releases.
// Here a take() that finds the semaphore taken is the bug under test: abort.
struct Semaphore {
  const char* name; int count = 1; uint32_t m_value = 0;
  explicit Semaphore(const char* n) : name(n) {}
  bool take(const char* owner) { if (count == 0) { std::fprintf(stderr, "FAIL: %s.take(%s) would block forever\n", name, owner); std::abort(); } count = 0; return true; }
  bool take(uint32_t, const char*) { if (count == 0) return false; count = 0; return true; }
  bool timedWait(const char*, uint32_t) { if (count == 0) return false; count = 0; count = 1; return true; }
  void give() { count = 1; }
  void give(uint32_t v) { m_value = v; give(); }
  uint32_t value() const { return m_value; }
};
struct Client;
static Client* gClient = nullptr;
static bool gDeliverRegEvt = true, gDeliverOpenEvt = true;  // what the scripted host answers
static int gRegisterCalls = 0, gOpenCalls = 0, gUnregisters = 0;
struct BLEDevice {
  static bool allocateAppId(uint16_t& id) { id = 1; return true; }
  static void addPeerDevice(void*, bool, uint16_t) {}
  static void removePeerDevice(uint16_t, bool) {}
  static bool clientHasRouting(void*) { return false; }
};
struct Outcome { BLEClientConnectStage stage = BLEClientConnectStage::NONE; int32_t espStatus = BLE_CLIENT_STATUS_NOT_SET; int32_t gattStatus = BLE_CLIENT_STATUS_NOT_SET; bool success = false; };
struct Client {
  uint16_t m_appId = 0; esp_gatt_if_t m_gattc_if = ESP_GATT_IF_NONE; bool m_isConnected = false;
  BLEAddress m_peerAddress; portMUX_TYPE m_connectOutcomeMux{};
  Semaphore m_semaphoreRegEvt{"RegEvt"}, m_semaphoreOpenEvt{"OpenEvt"};
  Outcome outcome;
  esp_gatt_if_t getGattcIf() { return m_gattc_if; }
  BLEAddress getPeerAddress() { return m_peerAddress; }
  void resetConnectOutcome() { outcome = Outcome{}; }
  void setConnectOutcomeStage(BLEClientConnectStage s, int32_t esp = BLE_CLIENT_STATUS_NOT_SET, int32_t gatt = BLE_CLIENT_STATUS_NOT_SET, int32_t = BLE_CLIENT_STATUS_NOT_SET) { outcome.stage = s; outcome.espStatus = esp; outcome.gattStatus = gatt; }
  void finishConnectOutcome(bool ok) { outcome.success = ok; }
  esp_err_t requestGattcUnregister() { ++gUnregisters; m_gattc_if = ESP_GATT_IF_NONE; return ESP_OK; }  // "unreg=completed"
  bool waitForGattcUnregister(uint32_t) { return true; }
  bool connect(BLEAddress address, uint8_t type, uint32_t timeoutMs);
};
// The scripted host: registration and open are accepted; their events arrive
// (as the BTC task's give) only when the script says so.
esp_err_t esp_ble_gattc_app_register(uint16_t) { ++gRegisterCalls; if (gDeliverRegEvt) { gClient->m_gattc_if = 3; gClient->m_semaphoreRegEvt.give(ESP_GATT_OK); } return ESP_OK; }
esp_err_t esp_ble_gattc_open(esp_gatt_if_t, const uint8_t*, esp_ble_addr_type_t, int) { ++gOpenCalls; if (gDeliverOpenEvt) { gClient->m_isConnected = true; gClient->m_semaphoreOpenEvt.give(ESP_GATT_OK); } return ESP_OK; }
// CONNECT
int main() {
  BLEAddress a;
  {  // Registration never answered, then a healthy host: the second connect must run.
    Client c; gClient = &c; gDeliverRegEvt = false; gDeliverOpenEvt = false;
    assert(!c.connect(a, 0, 1000) && c.outcome.stage == BLEClientConnectStage::APP_REGISTER_TIMEOUT && gOpenCalls == 0);
    gDeliverRegEvt = true; gDeliverOpenEvt = true;
    assert(c.connect(a, 0, 1000) && c.outcome.success && c.outcome.stage == BLEClientConnectStage::OPEN_EVENT && gOpenCalls == 1);
  }
  {  // Open never answered (the 2026-11-05 ring case), unregister completes, then a healthy host.
    Client d; gClient = &d; gDeliverRegEvt = true; gDeliverOpenEvt = false; gOpenCalls = 0;
    assert(!d.connect(a, 0, 1000) && d.outcome.stage == BLEClientConnectStage::OPEN_TIMEOUT && gUnregisters >= 1 && gOpenCalls == 1);
    assert(d.getGattcIf() == ESP_GATT_IF_NONE && !d.m_isConnected);
    gDeliverOpenEvt = true;
    assert(d.connect(a, 0, 1000) && d.outcome.success && gOpenCalls == 2);
  }
  puts("BLE client connect: a timed-out register or open wait leaves the next connect() runnable (no blocking take) passed");
}
'''.replace('// CONNECT', connect)
with tempfile.TemporaryDirectory(prefix='hw1-ble-connect-sem-') as tmp:
    d = Path(tmp); (d / 'test.cpp').write_text(unit)
    cmd = [os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra', '-Wno-unused-parameter', str(d / 'test.cpp'), '-o', str(d / 'test')]
    if args.sanitize: cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
    subprocess.run(cmd, check=True); subprocess.run([str(d / 'test')], check=True, timeout=60)
