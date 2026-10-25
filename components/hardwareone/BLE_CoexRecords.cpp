// BLE coexistence measurement records: see BLE_CoexRecords.h.
#include "System_BuildConfig.h"
#include "BLE_CoexRecords.h"
#include "System_CommandLimits.h"
#include <cstdio>
#include <cstring>
#if ENABLE_BLE_COEX_RECORDS
#include "System_WiFi.h"  // wifiCoexTag: cached state only (a stub without Wi-Fi)
#include <Arduino.h>
#include <BLEClient.h>
#include <BLEDevice.h>
#include <esp_attr.h>
#include <esp_gap_ble_api.h>
#include <esp_gattc_api.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

using namespace hw1coex;

static_assert(uint8_t(BLEClientConnectStage::OPEN_EVENT) == uint8_t(ConnectStage::Open) &&
              uint8_t(BLEClientConnectStage::OPEN_TIMEOUT) == uint8_t(ConnectStage::OpenTimeout) &&
              uint8_t(BLEClientConnectStage::COMPLETE) == uint8_t(ConnectStage::Complete),
              "hw1coex::ConnectStage must keep Arduino's BLEClientConnectStage numbering");

namespace {
const char* const kTag = "BLE_COEX";

// Records and totals (owner mutex), plus the log line built under it. Zero
// storage is an empty log.
EXT_RAM_BSS_ATTR Log gLog;
EXT_RAM_BSS_ATTR char gLine[320];
StaticSemaphore_t gLockStorage;
SemaphoreHandle_t gLock = nullptr;
portMUX_TYPE gInitMux = portMUX_INITIALIZER_UNLOCKED;

// The accessory scan in flight (G2 or ring). The central setup gate runs one
// at a time on the G2 connect worker; callbacks touch only the atomics.
Tally gScanTally;
std::atomic<uint8_t> gScanOwner{0};
struct ScanContext {
  Peer peer; Purpose purpose; uint16_t interval, window; bool active, duplicateFilter;
  uint8_t seconds; uint32_t beganAt; WifiSpan wifi;
};
ScanContext gScanContext{};

// The Arduino BLEClient connect in flight (G2 temples, ring), and the
// disconnect reason the passive observer caught for its address. The address
// is transient and never recorded.
portMUX_TYPE gConnectMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t gConnectTarget[6]{};
bool gConnectArmed = false;
ReasonLatch gConnectReason;
struct ConnectContext { Peer peer; Purpose purpose; WifiSpan wifi; bool active; };
ConnectContext gConnectContext{};
// Set and read on the connect worker only (its connects end there too).
uint16_t gAccessoryWaitMs = ConnectRecord::kNoWait;

bool lockLog(uint32_t waitMs = 50) {
  portENTER_CRITICAL(&gInitMux);
  if (!gLock) gLock = xSemaphoreCreateMutexStatic(&gLockStorage);
  SemaphoreHandle_t lock = gLock;
  portEXIT_CRITICAL(&gInitMux);
  return lock && xSemaphoreTake(lock, pdMS_TO_TICKS(waitMs)) == pdTRUE;
}
void unlockLog() { xSemaphoreGive(gLock); }

// The BLE_COEX lines are the record's own text, as two lines (the record,
// then its Wi-Fi span) so each stays within the 255 bytes `loglink on` passes
// on to the web log and file. Without loglink they reach only the console.
void appendScan(const ScanRecord& record) {
  if (!lockLog()) return;  // diagnostics only: drop rather than block a radio owner
  const ScanRecord& r = gLog.addScan(record);
  Log::scanBody(gLine, sizeof(gLine), r, false);
  ESP_LOGI(kTag, "scan #%lu %s", (unsigned long)r.seq, gLine);
  formatWifi(gLine, sizeof(gLine), r.wifi);
  ESP_LOGI(kTag, "scan #%lu%s", (unsigned long)r.seq, gLine);
  unlockLog();
}

void appendConnect(const ConnectRecord& record) {
  if (!lockLog()) return;
  const ConnectRecord& r = gLog.addConnect(record);
  Log::connectBody(gLine, sizeof(gLine), r, false);
  ESP_LOGI(kTag, "connect #%lu %s", (unsigned long)r.seq, gLine);
  formatWifi(gLine, sizeof(gLine), r.wifi);
  ESP_LOGI(kTag, "connect #%lu%s", (unsigned long)r.seq, gLine);
  unlockLog();
}

// Runs on the BTC task for every GATTC event, after Arduino's clients. A
// failed open is preceded by a DISCONNECT carrying the reason, which BLEClient
// drops (its conn_id is not set yet). One copy goes to each registered app,
// and other links' copies too: only the armed target's first reason counts.
void gattcObserver(esp_gattc_cb_event_t event, esp_gatt_if_t, esp_ble_gattc_cb_param_t* param) {
  if (event != ESP_GATTC_DISCONNECT_EVT || !param) return;
  portENTER_CRITICAL(&gConnectMux);
  const bool mine = gConnectArmed && !std::memcmp(gConnectTarget, param->disconnect.remote_bda, 6);
  portEXIT_CRITICAL(&gConnectMux);
  if (mine) gConnectReason.offer(uint16_t(param->disconnect.reason));
}

void wifiFlagNames(char* out, size_t cap, uint8_t flags) {
  static const char* const kNames[] = {"espnow", "ap", "sta", "ip", "forced-off", "companion-down",
                                       "reconnect", "boot-hunt"};
  size_t used = 0;
  out[0] = 0;
  for (unsigned i = 0; i < 8; ++i) if (flags & (1u << i))
    used += clampWrite(std::snprintf(out + used, cap - used, "%s%s", used ? "," : "", kNames[i]), cap - used);
  if (!used) std::snprintf(out, cap, "-");
}
}  // namespace

WifiCoexTag bleCoexWifiNow() { return wifiCoexTag(); }

void bleCoexScanBegin(Peer peer, Purpose purpose, uint16_t interval, uint16_t window, bool active,
                      bool duplicateFilter, uint8_t seconds) {
  gScanTally.reset();
  gScanContext = ScanContext{peer, purpose, interval, window, active, duplicateFilter, seconds, millis(), {}};
  gScanContext.wifi.begin(wifiCoexTag());
  gScanOwner.store(uint8_t(peer), std::memory_order_release);
}

void bleCoexScanEnd(ScanOutcome outcome) {
  gScanOwner.store(0, std::memory_order_release);
  if (gScanContext.peer == Peer::None) return;  // no matching Begin
  const uint32_t now = millis();
  const ScanTiming timing = concludeScan(gScanTally, outcome, gScanContext.beganAt, now, gScanContext.seconds);
  ScanRecord r{};
  r.endedAt = now;
  r.durationMs = timing.durationMs;
  r.controllerTimed = timing.controller;
  r.outcome = timing.outcome;
  r.reports = gScanTally.reports.load();
  r.matches = gScanTally.matches.load();
  r.firstMatchMs = gScanTally.firstMatchMs();
  r.rssi = int8_t(gScanTally.bestRssi.load());
  r.peer = gScanContext.peer;
  r.purpose = gScanContext.purpose;
  r.interval = gScanContext.interval; r.window = gScanContext.window;
  r.active = gScanContext.active; r.duplicateFilter = gScanContext.duplicateFilter; r.seconds = gScanContext.seconds;
  r.wifi = gScanContext.wifi;
  r.wifi.finish(wifiCoexTag());
  gScanContext.peer = Peer::None;
  appendScan(r);
}

void bleCoexOnGapEvent(int event, const void* param) {
  if (!gScanOwner.load(std::memory_order_acquire) || !param) return;
  const auto* p = static_cast<const esp_ble_gap_cb_param_t*>(param);
  switch (esp_gap_ble_cb_event_t(event)) {
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
      if (p->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS) gScanTally.start(millis());
      break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
      if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) gScanTally.report();
      else if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) gScanTally.end(millis());
      break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
      // A stop after the scan's own end fails ("scan not active"): keep the end.
      if (p->scan_stop_cmpl.status == ESP_BT_STATUS_SUCCESS) gScanTally.end(millis());
      break;
    default:
      break;
  }
}

void bleCoexOnScanMatch(int rssi) {
  if (gScanOwner.load(std::memory_order_acquire)) gScanTally.match(rssi, true, millis());
}

uint32_t bleCoexScanMatches() { return gScanTally.matches.load(); }

void bleCoexConnectBegin(Peer peer, Purpose purpose, const uint8_t address[6]) {
  gConnectReason.take();
  portENTER_CRITICAL(&gConnectMux);
  std::memcpy(gConnectTarget, address, 6);
  gConnectArmed = true;
  portEXIT_CRITICAL(&gConnectMux);
  gConnectContext = ConnectContext{peer, purpose, {}, true};
  gConnectContext.wifi.begin(wifiCoexTag());
}

void bleCoexConnectEnd(bool ok, uint8_t stage, int32_t gattStatus, uint32_t elapsedMs) {
  portENTER_CRITICAL(&gConnectMux);
  gConnectArmed = false;
  std::memset(gConnectTarget, 0, sizeof(gConnectTarget));
  portEXIT_CRITICAL(&gConnectMux);
  if (!gConnectContext.active) return;  // no matching Begin
  gConnectContext.active = false;
  ConnectRecord r{};
  r.endedAt = millis();
  r.elapsedMs = elapsedMs;
  r.peer = gConnectContext.peer;
  r.purpose = gConnectContext.purpose;
  r.outcome = ok ? ConnectOutcome::Connected : ConnectOutcome::Failed;
  r.stage = stage <= uint8_t(ConnectStage::Complete) ? ConnectStage(stage) : ConnectStage::None;
  r.gattStatus = (gattStatus < 0 || gattStatus > 0xff) ? int16_t(-1) : int16_t(gattStatus);
  r.reason = gConnectReason.take();
  r.waitMs = gAccessoryWaitMs;
  r.wifi = gConnectContext.wifi;
  r.wifi.finish(wifiCoexTag());
  appendConnect(r);
}

void bleCoexInstallGattcObserver() {
  // Arduino's custom GATTC slot is unused elsewhere; its pointer is a plain
  // static, so set it once (host quiescent) and keep it for the boot.
  static bool installed = false;
  if (installed) return;
  BLEDevice::setCustomGattcHandler(gattcObserver);
  installed = true;
}

void bleCoexSetAccessoryWait(uint32_t waitMs) { gAccessoryWaitMs = ConnectRecord::capWait(waitMs); }
void bleCoexClearAccessoryWait() { gAccessoryWaitMs = ConnectRecord::kNoWait; }
void bleCoexLogSetupWait(const char* job, uint32_t waitMs) {
  if (waitMs >= 50) ESP_LOGI(kTag, "setup wait %s ms=%lu", job ? job : "?", (unsigned long)waitMs);
}

void bleCoexRecordScan(const ScanRecord& record) { appendScan(record); }
void bleCoexRecordConnect(const ConnectRecord& record) { appendConnect(record); }

size_t bleCoexScanCounters(char* out, size_t cap, Peer peer) {
  if (!cap) return 0;
  out[0] = 0;
  if (!lockLog()) return 0;
  const size_t n = gLog.scanCounters(out, cap, peer);
  unlockLog();
  return n;
}

size_t bleCoexScanLines(char* out, size_t cap, uint32_t now, Peer only, size_t max) {
  if (!cap) return 0;
  out[0] = 0;
  if (!lockLog()) return 0;
  const size_t n = gLog.scanLines(out, cap, now, only, max);
  unlockLog();
  return n;
}

size_t bleCoexConnectCounters(char* out, size_t cap, Peer peer) {
  if (!cap) return 0;
  out[0] = 0;
  if (!lockLog()) return 0;
  const size_t n = gLog.connectCounters(out, cap, peer);
  unlockLog();
  return n;
}

size_t bleCoexConnectLines(char* out, size_t cap, uint32_t now, Peer only, size_t max) {
  if (!cap) return 0;
  out[0] = 0;
  if (!lockLog()) return 0;
  const size_t n = gLog.connectLines(out, cap, now, only, max);
  unlockLog();
  return n;
}

const char* bleCoexCommand(const char* args) {
  // Whole lines only: the reply never reaches the executor's limit.
  EXT_RAM_BSS_ATTR static char text[CMD_RESULT_MAX];
  while (args && *args == ' ') ++args;
  const bool scans = args && !std::strcmp(args, "scans");
  const bool connects = args && !std::strcmp(args, "connects");
  const bool clear = args && !std::strcmp(args, "clear");
  if (args && *args && !scans && !connects && !clear) return "Usage: blerecords [scans|connects|clear]";
  if (!lockLog(200)) return "Error: BLE records busy";
  if (clear) {
    std::memset(&gLog, 0, sizeof(gLog));
    unlockLog();
    return "BLE records cleared";
  }
  const WifiCoexTag wifi = wifiCoexTag();
  char flags[96];
  wifiFlagNames(flags, sizeof(flags), wifi.flags);
  char age[8] = "-";
  if (wifi.lastDiscAgeS < 255) std::snprintf(age, sizeof(age), "%s%u", wifi.lastDiscAgeS == 254 ? ">" : "", unsigned(wifi.lastDiscAgeS));
  const uint32_t now = millis();
  size_t used = clampWrite(std::snprintf(text, sizeof(text),
      "BLE scan/connect records (no addresses). Wi-Fi now: %s [%s] last_disconnect=%u age_s=%s"
      " scans=%u connects=%u disconnects=%u",
      wifiCoexName(wifi.state), flags, unsigned(wifi.lastReason), age, unsigned(wifi.scans),
      unsigned(wifi.connects), unsigned(wifi.disconnects)), sizeof(text));
  if (!connects) {
    for (Peer p : {Peer::Glasses, Peer::Ring, Peer::Phone}) {
      used += clampWrite(std::snprintf(text + used, sizeof(text) - used, "\n%s ", name(p)), sizeof(text) - used);
      used += gLog.scanCounters(text + used, sizeof(text) - used, p);
    }
    // With both views, the scans leave half the room for the connects.
    const size_t room = sizeof(text) - used;
    used += gLog.scanLines(text + used, scans ? room : room / 2, now, Peer::None, scans ? kKept : 6);
  }
  if (!scans) {
    for (Peer p : {Peer::GlassesLeft, Peer::GlassesRight, Peer::Ring, Peer::Phone}) {
      used += clampWrite(std::snprintf(text + used, sizeof(text) - used, "\n%s ", name(p)), sizeof(text) - used);
      used += gLog.connectCounters(text + used, sizeof(text) - used, p);
    }
    gLog.connectLines(text + used, sizeof(text) - used, now, Peer::None, connects ? kKept : 6);
  }
  unlockLog();
  return text;
}
#else
// Records compiled out (ENABLE_BLE_COEX_RECORDS=0). The hooks stay where they
// are and do nothing; the readers say the records are absent instead of
// showing an empty log.
using namespace hw1coex;

#define HW1_COEX_NOT_BUILT "BLE records are not in this build (ENABLE_BLE_COEX_RECORDS=0)"

void bleCoexScanBegin(Peer, Purpose, uint16_t, uint16_t, bool, bool, uint8_t) {}
void bleCoexScanEnd(ScanOutcome) {}
void bleCoexOnGapEvent(int, const void*) {}
void bleCoexOnScanMatch(int) {}
uint32_t bleCoexScanMatches() { return 0; }
void bleCoexConnectBegin(Peer, Purpose, const uint8_t[6]) {}
void bleCoexConnectEnd(bool, uint8_t, int32_t, uint32_t) {}
void bleCoexSetAccessoryWait(uint32_t) {}
void bleCoexClearAccessoryWait() {}
void bleCoexLogSetupWait(const char*, uint32_t) {}
void bleCoexInstallGattcObserver() {}
void bleCoexRecordScan(const ScanRecord&) {}
void bleCoexRecordConnect(const ConnectRecord&) {}
WifiCoexTag bleCoexWifiNow() { return WifiCoexTag{}; }

size_t bleCoexScanCounters(char* out, size_t cap, Peer) {
  if (!cap) return 0;
  const int n = std::snprintf(out, cap, "%s", HW1_COEX_NOT_BUILT);
  return n < 0 ? 0 : size_t(n) >= cap ? cap - 1 : size_t(n);
}

size_t bleCoexScanLines(char* out, size_t cap, uint32_t, Peer, size_t) {
  if (cap) out[0] = 0;
  return 0;
}

size_t bleCoexConnectCounters(char* out, size_t cap, Peer peer) { return bleCoexScanCounters(out, cap, peer); }

size_t bleCoexConnectLines(char* out, size_t cap, uint32_t, Peer, size_t) {
  if (cap) out[0] = 0;
  return 0;
}

const char* bleCoexCommand(const char*) { return "Error: " HW1_COEX_NOT_BUILT; }
#endif  // ENABLE_BLE_COEX_RECORDS
