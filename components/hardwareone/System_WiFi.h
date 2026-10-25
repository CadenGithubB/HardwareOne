#ifndef SYSTEM_WIFI_H
#define SYSTEM_WIFI_H

#include <Arduino.h>
#include "System_BuildConfig.h"
#include "System_Debug.h"

#include "network/wifi/System_WiFiNetworks.h"
#include "System_WiFiCoex.h"

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

// Who asked for a connect. It decides whether a keystroke on the serial console
// cancels the hunt: at boot the main task is blocked inside the connect, so the
// console has no other reader; for a command only one typed at the serial
// console may be cancelled that way (the main loop is waiting on it). Connects
// from the web, BLE, MQTT, UART or the radio companion never read the console.
enum class WifiConnectOrigin : uint8_t {
  COMMAND = 0,  // resolved from the executing command's context
  BOOT,         // the boot connect when setup itself waits for it
  // The boot connect on its own task while setup carries on. It never reads
  // the console (the main loop owns it), leaves NTP to the boot sequence that
  // follows it, and an explicit radio command supersedes it (below).
  BACKGROUND,
};

// A BACKGROUND connect can still be hunting when a radio command arrives on
// the command executor. openwifi, closewifi, wifidisconnect, radiopower off and
// the radio companion's procedures supersede it: this asks the hunt to stop and
// waits for it to let go of the radio. Returns true once no background connect
// holds the radio (immediately when none is running), false if it was still
// holding it after waitMs; the request stands, so the hunt still stops.
// Blocks up to waitMs; a hunt notices the request within ~2 s at worst.
constexpr uint32_t kWifiSupersedeWaitMs = 5000;
bool wifiSupersedeBackgroundConnect(uint32_t waitMs = kWifiSupersedeWaitMs);

// The boot task reserves its BACKGROUND connect before the task is created, so
// a command that arrives before the task reaches setupWiFi still supersedes it.
// setupWiFi(..., BACKGROUND) ends the reservation when it returns; pass false
// to drop one whose task never started.
void wifiBackgroundConnectReserve(bool reserve);
// True from that reservation until setupWiFi(..., BACKGROUND) returns.
bool wifiBackgroundConnectActive();
// True once the BACKGROUND connect has been superseded (sticky for the boot).
bool wifiBackgroundConnectSuperseded();

// True while `radiopower off` holds the radio down (runtime airplane mode).
bool wifiRadioForcedOff();

// Longest one connect request (every saved network, every attempt) may run.
// Each attempt still gets its full 35 s window for a link that is associating
// or handshaking; the budget stops the hunt as a whole.
constexpr uint32_t kWifiConnectHuntBudgetMs = 150000;

// Budget for the boot connect: the default, shortened on a trial OTA boot so
// the rest of setup still reaches RUNNING inside the probation setup window
// when setup has to wait for the connect (WifiConnectOrigin::BOOT).
uint32_t wifiBootConnectBudgetMs();

void setupWiFi();
void setupWiFi(uint32_t budgetMs, WifiConnectOrigin origin);

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
// The same answers for screens that redraw every frame. On the P4 the
// "started but unassociated" case is an RPC to the C6, so it may be up to a
// second old; a mode change made through WiFi.mode() shows at once. Decisions
// and commands use wifiRadioState()/wifiRadioOn().
WifiRadioState wifiRadioDisplayState();
bool wifiRadioDisplayOn();
bool connectToBestWiFiNetwork(uint32_t budgetMs = kWifiConnectHuntBudgetMs,
                              WifiConnectOrigin origin = WifiConnectOrigin::COMMAND);
// Main-loop drain for the disconnect snapshot captured on the arduino_events
// task (wifiEventLogger keeps its stack frame tiny; the logging happens here).
void wifiEventLogDrain();

// Command registry (for System_CommandRegistry.cpp)
struct CommandEntry;
extern const CommandEntry wifiCommands[];
extern const size_t wifiCommandsCount;

#endif // ENABLE_WIFI

// The device's own Wi-Fi state for the BLE coexistence records (lever 1 of
// docs/WIFI_BLE_COEXISTENCE_STUDY.md). wifiCoexTag() is callable from any task:
// it reads atomics kept by an esp_event mirror plus cached flags, never makes
// an RPC to the P4's companion, and stays safe while Wi-Fi turns off (unlike
// WiFi.isConnected(), whose event group Arduino frees). Install the mirror
// before the radio first starts; installing again does nothing. Without the
// records (ENABLE_BLE_COEX_RECORDS=0) there is no mirror and no reader.
#if ENABLE_WIFI && ENABLE_BLE_COEX_RECORDS
WifiCoexTag wifiCoexTag();
bool wifiCoexMirrorInstall();
#else
inline WifiCoexTag wifiCoexTag() {
  WifiCoexTag t;
  t.state = ENABLE_WIFI ? WifiCoex::Unknown : WifiCoex::NoWifi;
  return t;
}
inline bool wifiCoexMirrorInstall() { return true; }
#endif

#endif // SYSTEM_WIFI_H
