#ifndef SYSTEM_WIFI_H
#define SYSTEM_WIFI_H

#include <Arduino.h>
#include "System_BuildConfig.h"
#include "System_Debug.h"

// WiFi Constants
#define MAX_WIFI_NETWORKS 8

// WiFi Network Structure (always available for type-safe references)
struct WifiNetwork {
  String ssid;
  String password;
  int priority;            // 1 = highest priority
  bool hidden;             // informational only
  uint32_t lastConnected;  // millis when last connected
};

// Global WiFi network storage (defined in .ino)
extern WifiNetwork* gWifiNetworks;
extern int gWifiNetworkCount;

#if ENABLE_WIFI

// Compact, caller-independent view of one ESP-IDF scan record. Scan consumers
// copy only these fields while the driver owns its transient AP list; no
// Arduino WiFiScan result buffer is retained after wifiScanForEach() returns.
struct WifiScanRecord {
  char ssid[33];
  uint8_t bssid[6];
  int8_t rssi;
  uint8_t channel;
  uint8_t authMode;
};
static_assert(sizeof(WifiScanRecord) == 42,
              "WifiScanRecord layout changed; re-check scan snapshot memory");

enum class WifiScanStatus : uint8_t {
  OK = 0,
  BUSY,
  RADIO_UNAVAILABLE,
  DRIVER_ERROR,
  INVALID_ARGUMENT,
};

struct WifiScanResult {
  WifiScanStatus status;
  uint16_t found;
  uint16_t delivered;
  int32_t driverError;

  bool ok() const { return status == WifiScanStatus::OK; }
};

using WifiScanVisitor = bool (*)(const WifiScanRecord& record,
                                 uint16_t index, uint16_t total,
                                 void* context);

// Run one synchronous IDF scan and stream its records through a caller
// callback. An OK result guarantees that the driver AP list was released.
// Cleanup is attempted on every owned failure path; DRIVER_ERROR can mean the
// driver did not confirm that cleanup. A false visitor return stops delivery
// early without changing result.found.
//
// ORDER IS NOT GUARANTEED. This used to promise "RSSI-sorted records", and
// nothing enforces that: the walk uses the SINGULAR esp_wifi_scan_get_ap_record()
// cursor, whose doc block says nothing about ordering. The only "sorted in
// descending order based on RSSI" guarantee in esp_wifi.h belongs to the PLURAL
// esp_wifi_scan_get_ap_records(), which this code deliberately does not use
// because it requires buffering the whole list (wifi_ap_record_t is ~90 bytes,
// so 40 APs is ~3.6 KB).
//
// CONSEQUENCE FOR CAPPED CALLERS: a visitor that stops after N records keeps the
// first N the driver happens to hand back, NOT the strongest N. If you need
// strongest-N, sort or select on the caller's own copy — do not infer it from
// arrival order.
WifiScanResult wifiScanForEach(bool includeHidden, WifiScanVisitor visitor,
                               void* context,
                               uint32_t acquireTimeoutMs = 0);
const char* wifiScanStatusText(WifiScanStatus status);

// Format the radio's max TX power into caller storage as "21.00 dBm".
//
// The scale is the trap this exists to contain: wifi_power_t and
// esp_wifi_get_max_tx_power() are QUARTER-dBm (WIFI_POWER_21dBm == 84), so any
// site that prints the raw value with a "dBm" suffix reads "84dBm". Divide in
// exactly one place.
//
// Returns the driver status; on failure buf is untouched, so callers report an
// error rather than a number. Prefer this over Arduino's WiFi.getTxPower(),
// which returns a silent WIFI_POWER_19_5dBm fallback when neither STA nor AP
// has started. rawOut (optional) receives the underlying quarter-dBm value.
esp_err_t wifiFormatTxPower(char* buf, size_t cap, int8_t* rawOut = nullptr);

// Fence a driver/mode mutation against an explicit scan. The recursive mutex
// permits existing composed radio helpers, while a mutation attempted from a
// scan visitor is rejected to prevent re-entering the live driver AP list.
class WifiRadioMutationGuard {
 public:
  explicit WifiRadioMutationGuard(uint32_t timeoutMs = 250);
  ~WifiRadioMutationGuard();

  WifiRadioMutationGuard(const WifiRadioMutationGuard&) = delete;
  WifiRadioMutationGuard& operator=(const WifiRadioMutationGuard&) = delete;

  bool acquired() const { return acquired_; }
  void release();

 private:
  bool acquired_ = false;
};

// WiFi Command Handlers
const char* cmd_wifiinfo(const String& argsInput);
const char* cmd_wifilist(const String& argsInput);
const char* cmd_wifiadd(const String& originalCmd);
const char* cmd_wifirm(const String& originalCmd);
const char* cmd_wifipromote(const String& originalCmd);
const char* cmd_wificonnect(const String& originalCmd);
const char* cmd_wifidisconnect(const String& argsInput);
const char* cmd_wifidrop(const String& argsInput);   // `wifidisconnect` cmd: drop AP, keep radio + HTTP up
const char* cmd_radiopower(const String& originalCmd);  // `radiopower [on|off|toggle]`: power the whole radio (WiFi+ESP-NOW) up/down, runtime only
const char* cmd_wifiscan(const String& command);
const char* cmd_wifitxpower(const String& originalCmd);
const char* cmd_wifigettxpower(const String& argsInput);
const char* cmd_wifiautoreconnect(const String& originalCmd);

// WiFi Helper Functions
bool ensureWiFiInitialized();
void setupWiFi();

// Single source of truth for WiFi radio state, so every surface reports the same
// thing. CONNECTED = associated to an AP; UP_FOR_ESPNOW = radio on but STA
// unassociated because ESP-NOW holds it; UP_DISCONNECTED = radio on with no AP
// association and no ESP-NOW owner; OFF = radio powered down.
enum WifiRadioState {
  WIFI_RADIO_OFF = 0,
  WIFI_RADIO_UP_FOR_ESPNOW = 1,
  WIFI_RADIO_CONNECTED = 2,
  WIFI_RADIO_UP_DISCONNECTED = 3,
};
WifiRadioState wifiRadioState();
// RADIO power axis (separate from WiFi.isConnected() connection axis): true when
// the driver has any active mode — connected, disconnected, or held up by
// ESP-NOW; false only when fully off.
bool wifiRadioOn();
bool connectToBestWiFiNetwork();
// Main-loop drain for the disconnect snapshot captured on the arduino_events
// task (wifiEventLogger keeps its stack frame tiny; the logging happens here).
void wifiEventLogDrain();

// Command registry (for system_utils.cpp)
struct CommandEntry;
extern const CommandEntry wifiCommands[];
extern const size_t wifiCommandsCount;

#endif // ENABLE_WIFI

#endif // SYSTEM_WIFI_H
