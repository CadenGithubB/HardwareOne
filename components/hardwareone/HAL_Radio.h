/**
 * HAL_Radio - where this board's radio lives and how it is kept alive.
 *
 * On native boards (ESP32, ESP32-S3) the Wi-Fi/BLE radio is on the chip and
 * every function here is a no-op that reports "native". On the ESP32-P4X-EYE
 * the radio is an ESP32-C6 companion behind ESP-Hosted over SDIO; this HAL
 * owns the companion's transport lifecycle (boot, restart, hold-in-reset,
 * firmware update) and the raw facts about it (identity, bridge, heartbeat
 * and memory events). Policy (when to retry, when to recover, what to notify)
 * lives in System_RadioCompanion.cpp on top of Radio_CompanionCore.h.
 *
 * Threading: the event handler runs on the default esp_event task and must
 * only copy; everything else is called from the main loop or the command
 * executor and may block for the duration of a Hosted RPC (seconds).
 */
#ifndef HAL_RADIO_H
#define HAL_RADIO_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

#if defined(CONFIG_ESP_WIFI_REMOTE_ENABLED) && CONFIG_ESP_WIFI_REMOTE_ENABLED
#define HW1_RADIO_COMPANION 1
#else
#define HW1_RADIO_COMPANION 0
#endif

enum class RadioHalBackend : uint8_t { Native = 0, Hosted = 1 };

enum class RadioBridgeState : uint8_t {
  Unknown = 0,        // not probed yet
  Present = 1,        // bridge answered with info
  PresentNoInfo = 2,  // bridge answered but predates the info request
  Absent = 3,         // stock ESP-Hosted firmware: Wi-Fi/BLE work, ESP-NOW cannot start
  TransportError = 4, // Hosted refused the request
};

enum class RadioHalEvent : uint8_t {
  TransportUp = 0,
  TransportDown,     // orderly: the host closed the transport
  TransportFailure,  // the SDIO link stopped working
  Heartbeat,         // value = companion heartbeat number
  CompanionBoot,     // value = companion reset reason
  Memory,            // value = free heap, value2 = minimum free heap since boot
};

struct RadioHalEventData {
  RadioHalEvent type;
  uint32_t value;
  uint32_t value2;
};
typedef void (*RadioHalEventFn)(const RadioHalEventData& event);

// Everything known about the companion, refreshed by radioHalRefreshIdentity().
struct RadioCompanionIdentity {
  bool versionKnown;
  uint32_t major, minor, patch;   // ESP-Hosted version the companion reports
  bool versionMatches;            // equals the pinned esp_now_hosted_companion.h version
  bool descKnown;
  char project[32];
  char version[32];
  char idf[32];
  char date[16];
  char time[16];
  bool chipKnown;
  uint32_t chipId;
  char chip[16];
  RadioBridgeState bridge;
  bool infoKnown;                 // the fields below came from the bridge's info request
  uint8_t bridgeWire;
  uint8_t imageState;             // ESP_NOW_HOSTED_IMAGE_* (1 valid, 2 pending verify, 3 invalid)
  uint8_t runningSlot;            // 0xff unknown
  uint8_t resetReason;
  uint32_t uptimeMs;
  uint32_t freeHeap;
  uint32_t minFreeHeap;
  uint32_t cpuMhz;
  uint32_t nativeEspNowVersion;
  char build[17];
};

struct RadioBridgeStats {
  uint32_t malformed, stale, eventsDropped, timeouts, transportErrors;
};

// Last memory report from the companion (monitor event or explicit query).
struct RadioCompanionMemory {
  bool known;
  uint32_t totalHeap;
  uint32_t freeHeap;
  uint32_t minFreeHeap;
  uint32_t internalFree;
  uint32_t internalLargest;
  uint32_t atMs;                  // millis() when it was received
};

#if HW1_RADIO_COMPANION

inline RadioHalBackend radioHalBackend() { return RadioHalBackend::Hosted; }

// Boot. Prepares the board wiring, brings the transport up (a bounded number
// of attempts, each of which resets the companion), fetches its identity and
// probes the bridge. Never fatal: on failure the application boots with the
// radio offline and System_RadioCompanion retries in the background.
esp_err_t radioHalPrepare();
bool radioHalPrepared();

// True while the Hosted transport is up (TRANSPORT_UP seen, no failure since).
bool radioHalTransportUp();
// True when ESP-NOW can be started: transport up and the bridge present.
bool radioHalEspNowAvailable();

const RadioCompanionIdentity& radioHalIdentity();
esp_err_t radioHalRefreshIdentity();            // RPCs; transport must be up
void radioHalBridgeStats(RadioBridgeStats& out);
esp_err_t radioHalQueryMemory(RadioCompanionMemory& out); // RPC; transport must be up
const RadioCompanionMemory& radioHalLastMemory();

// Events are delivered on the esp_event task; the handler must only copy.
void radioHalSetEventHandler(RadioHalEventFn fn);
// Heartbeat every heartbeatSec (0 disables) and a low-memory report when the
// companion's free internal heap drops under lowMemoryBytes (0 disables).
// Applied now if the transport is up and again after every (re)start.
esp_err_t radioHalConfigureMonitoring(uint16_t heartbeatSec, uint32_t lowMemoryBytes);

// Transport lifecycle. Start (re)initializes Hosted, which resets the
// companion, waits for it (bounded, about twenty seconds at most) and probes
// it. Stop closes the transport; the caller has already stopped Wi-Fi, BLE
// and ESP-NOW. MarkTransportLost makes every remaining RPC fail at once so a
// teardown with a dead companion takes milliseconds, not timeouts.
esp_err_t radioHalStartTransport();
void radioHalStopTransport();
void radioHalMarkTransportLost();
bool radioHalTransportLost();

// Hold the companion in reset (radio fully off) and release it. Hold is
// refused while the transport is up. Starting the transport releases a hold.
esp_err_t radioHalHold(bool hold);
bool radioHalHeld();
// Hold through host deep sleep: the enable line stays low until the next boot.
void radioHalPrepareForDeepSleep();

// Companion firmware update, streamed over the transport (ESP-Hosted OTA).
esp_err_t radioHalUpdateBegin();
esp_err_t radioHalUpdateWrite(const uint8_t* data, size_t len);
esp_err_t radioHalUpdateEnd();
esp_err_t radioHalUpdateActivate();   // the companion reboots two seconds later
// Mark the companion's running image valid (cancels rollback).
esp_err_t radioHalBridgeConfirmImage();

const char* radioHalResetReasonLabel(uint8_t reason);
const char* radioHalBridgeStateName(RadioBridgeState state);

#else  // native radio

inline RadioHalBackend radioHalBackend() { return RadioHalBackend::Native; }
inline esp_err_t radioHalPrepare() { return ESP_OK; }
inline bool radioHalPrepared() { return true; }
inline bool radioHalTransportUp() { return true; }
inline bool radioHalEspNowAvailable() { return true; }
inline const RadioCompanionIdentity& radioHalIdentity() {
  static const RadioCompanionIdentity none = {};
  return none;
}
inline esp_err_t radioHalRefreshIdentity() { return ESP_ERR_NOT_SUPPORTED; }
inline void radioHalBridgeStats(RadioBridgeStats& out) { out = {}; }
inline esp_err_t radioHalQueryMemory(RadioCompanionMemory& out) { out = {}; return ESP_ERR_NOT_SUPPORTED; }
inline const RadioCompanionMemory& radioHalLastMemory() {
  static const RadioCompanionMemory none = {};
  return none;
}
inline void radioHalSetEventHandler(RadioHalEventFn) {}
inline esp_err_t radioHalConfigureMonitoring(uint16_t, uint32_t) { return ESP_ERR_NOT_SUPPORTED; }
inline esp_err_t radioHalStartTransport() { return ESP_OK; }
inline void radioHalStopTransport() {}
inline void radioHalMarkTransportLost() {}
inline bool radioHalTransportLost() { return false; }
inline esp_err_t radioHalHold(bool) { return ESP_ERR_NOT_SUPPORTED; }
inline bool radioHalHeld() { return false; }
inline void radioHalPrepareForDeepSleep() {}
inline esp_err_t radioHalUpdateBegin() { return ESP_ERR_NOT_SUPPORTED; }
inline esp_err_t radioHalUpdateWrite(const uint8_t*, size_t) { return ESP_ERR_NOT_SUPPORTED; }
inline esp_err_t radioHalUpdateEnd() { return ESP_ERR_NOT_SUPPORTED; }
inline esp_err_t radioHalUpdateActivate() { return ESP_ERR_NOT_SUPPORTED; }
inline esp_err_t radioHalBridgeConfirmImage() { return ESP_ERR_NOT_SUPPORTED; }
inline const char* radioHalResetReasonLabel(uint8_t) { return "n/a"; }
inline const char* radioHalBridgeStateName(RadioBridgeState) { return "native"; }

#endif  // HW1_RADIO_COMPANION
#endif  // HAL_RADIO_H
