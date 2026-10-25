// Host harness for the Wi-Fi connect hunt. The production hunt and attempt
// functions come from System_WiFi.cpp unchanged (inserted by the runner); the
// driver, clock, console and command context are scripted doubles. Time is
// virtual: millis() reads a counter that delay() advances, so a 35 s window
// costs nothing to run and the assertions are about recorded virtual time.
#include <Arduino.h>
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

void webBatchTestStringAppend() {}

static int gChecks = 0;
static void require(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
  ++gChecks;
}

// ---------------------------------------------------------------------------
// Virtual clock
// ---------------------------------------------------------------------------
static unsigned long gNowMs = 1000;  // non-zero so "0" sentinels stay distinct
// Runs after every delay(); a case uses it to act "from another task" at a
// chosen point in virtual time (a superseding radio command).
static void (*gDelayHook)() = nullptr;
unsigned long millis() { return gNowMs; }
void delay(unsigned long ms) {
  gNowMs += ms;
  if (gDelayHook) gDelayHook();
}
template <class T>
T min(T a, T b) { return a < b ? a : b; }

// ---------------------------------------------------------------------------
// ESP-IDF / Arduino Wi-Fi surface the extracted functions touch
// ---------------------------------------------------------------------------
typedef int esp_err_t;
static constexpr esp_err_t ESP_OK = 0;
static constexpr esp_err_t ESP_FAIL = -1;
const char* esp_err_to_name(esp_err_t err) { return err == ESP_OK ? "ESP_OK" : "ESP_FAIL"; }

enum wifi_mode_t { WIFI_MODE_NULL = 0, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA };
static constexpr wifi_mode_t WIFI_STA = WIFI_MODE_STA;
static constexpr wifi_mode_t WIFI_AP_STA = WIFI_MODE_APSTA;
enum wifi_interface_t { WIFI_IF_STA = 0 };
enum wifi_auth_mode_t { WIFI_AUTH_WPA_WPA2_PSK = 4 };
enum wifi_scan_method_t { WIFI_FAST_SCAN = 0 };
enum wifi_sort_method_t { WIFI_CONNECT_AP_BY_SIGNAL = 0 };
enum wifi_ps_type_t { WIFI_PS_NONE = 0 };
struct wifi_sta_config_t {
  uint8_t ssid[32];
  uint8_t password[64];
  struct { wifi_auth_mode_t authmode; } threshold;
  struct { bool capable; bool required; } pmf_cfg;
  wifi_scan_method_t scan_method;
  wifi_sort_method_t sort_method;
};
struct wifi_config_t { wifi_sta_config_t sta; };
struct wifi_init_config_t { int unused; };

typedef enum {
  WL_NO_SHIELD = 255,
  WL_STOPPED = 254,
  WL_IDLE_STATUS = 0,
  WL_NO_SSID_AVAIL = 1,
  WL_SCAN_COMPLETED = 2,
  WL_CONNECTED = 3,
  WL_CONNECT_FAILED = 4,
  WL_CONNECTION_LOST = 5,
  WL_DISCONNECTED = 6
} wl_status_t;

// Scripted saved networks. Missing: the driver's scan finds no AP and reports
// WL_NO_SSID_AVAIL kScanMs after each connect. Handshaking: the link stays
// WL_DISCONNECTED (associating) for as long as it is given. Connects: joins
// connectAfterMs after the connect call.
enum class Behaviour { Missing, Handshaking, Connects };
struct ScriptedNetwork {
  std::string ssid;
  Behaviour behaviour;
  unsigned long connectAfterMs;
};
static constexpr unsigned long kScanMs = 1000;
static std::vector<ScriptedNetwork> gScript;

struct DriverState {
  bool inited = true;
  bool started = true;
  bool connecting = false;
  bool connected = false;
  unsigned long connectAt = 0;
  std::string ssid;
};
static DriverState gDriver;
// Virtual time of every esp_wifi_connect() call, per SSID.
static std::map<std::string, std::vector<unsigned long>> gConnectCalls;

static const ScriptedNetwork* scripted(const std::string& ssid) {
  for (const auto& n : gScript) if (n.ssid == ssid) return &n;
  return nullptr;
}

static wl_status_t driverStatus() {
  if (!gDriver.inited || !gDriver.started) return WL_STOPPED;
  if (gDriver.connected) return WL_CONNECTED;
  if (!gDriver.connecting) return WL_DISCONNECTED;
  const ScriptedNetwork* n = scripted(gDriver.ssid);
  const unsigned long since = gNowMs - gDriver.connectAt;
  if (!n || n->behaviour == Behaviour::Missing) return since >= kScanMs ? WL_NO_SSID_AVAIL : WL_DISCONNECTED;
  if (n->behaviour == Behaviour::Connects && since >= n->connectAfterMs) {
    gDriver.connected = true;
    return WL_CONNECTED;
  }
  return WL_DISCONNECTED;
}

// Driver transitions a connect must not make while ESP-NOW holds the radio.
static int gDriverStops = 0;
static std::vector<wifi_mode_t> gDriverModes;   // esp_wifi_set_mode requests
static std::vector<wifi_mode_t> gArduinoModes;  // WiFi.mode requests

esp_err_t esp_wifi_disconnect() { gDriver.connecting = false; gDriver.connected = false; return ESP_OK; }
esp_err_t esp_wifi_stop() { ++gDriverStops; gDriver.started = false; esp_wifi_disconnect(); return ESP_OK; }
esp_err_t esp_wifi_start() { gDriver.started = true; return ESP_OK; }
esp_err_t esp_wifi_set_mode(wifi_mode_t mode) { gDriverModes.push_back(mode); return ESP_OK; }
esp_err_t esp_wifi_get_mode(wifi_mode_t* mode) {
  if (!gDriver.inited) return ESP_FAIL;
  *mode = WIFI_MODE_STA;
  return ESP_OK;
}
esp_err_t esp_wifi_set_config(wifi_interface_t, wifi_config_t* config) {
  const char* raw = reinterpret_cast<const char*>(config->sta.ssid);
  gDriver.ssid = std::string(raw, strnlen(raw, sizeof(config->sta.ssid)));
  return ESP_OK;
}
esp_err_t esp_wifi_connect() {
  gDriver.connecting = true;
  gDriver.connected = false;
  gDriver.connectAt = gNowMs;
  gConnectCalls[gDriver.ssid].push_back(gNowMs);
  return ESP_OK;
}
esp_err_t esp_wifi_set_ps(wifi_ps_type_t) { return ESP_OK; }
esp_err_t esp_wifi_init(const wifi_init_config_t*) { gDriver.inited = true; return ESP_OK; }
esp_err_t esp_wifi_deinit() { gDriver.inited = false; return ESP_OK; }
static wifi_init_config_t makeArduinoCompatibleWifiInitConfig() { return wifi_init_config_t{0}; }

struct FakeIP { String toString() const { return String("10.0.0.2"); } };
struct FakeWiFi {
  bool mode(wifi_mode_t requested) { gArduinoModes.push_back(requested); return true; }
  wl_status_t status() { return driverStatus(); }
  bool disconnect(bool = true) { esp_wifi_disconnect(); return true; }
  FakeIP localIP() { return FakeIP{}; }
  long RSSI() { return -60; }
  long channel() { return 6; }
  wifi_mode_t getMode() { return WIFI_MODE_STA; }
  bool isConnected() { return gDriver.connected; }
  String SSID() { return String(gDriver.ssid.c_str()); }
  void setAutoReconnect(bool) {}
};
static FakeWiFi WiFi;

// The console double counts every byte the connect drains.
struct FakeSerial {
  int pending = 0;
  int reads = 0;
  int available() { return pending; }
  int read() {
    if (pending > 0) { --pending; ++reads; }
    return 'x';
  }
};
static FakeSerial Serial;

class WifiRadioMutationGuard {
 public:
  explicit WifiRadioMutationGuard(uint32_t = 250) {}
  bool acquired() const { return true; }
  void release() {}
};

// ---------------------------------------------------------------------------
// Saved-network, logging and command-context boundaries
// ---------------------------------------------------------------------------
#define MAX_WIFI_NETWORKS 8
struct SavedWifiNetwork {
  uint32_t id = 0;
  String ssid;
  int priority = 0;
  bool hidden = false;
  uint32_t lastConnected = 0;
};
struct WifiNetworkSnapshot {
  SavedWifiNetwork entries[MAX_WIFI_NETWORKS];
  int count = 0;
};
struct WifiConnectionTarget {
  uint32_t id = 0;
  String ssid;
  String password;
  int priority = 0;
  int index = -1;
};
bool wifiNetworkSnapshot(WifiNetworkSnapshot& out) {
  out = WifiNetworkSnapshot{};
  for (size_t i = 0; i < gScript.size() && i < MAX_WIFI_NETWORKS; ++i) {
    out.entries[i].id = static_cast<uint32_t>(i + 1);
    out.entries[i].ssid = String(gScript[i].ssid.c_str());
    out.entries[i].priority = static_cast<int>(i + 1);
    out.count = static_cast<int>(i + 1);
  }
  return true;
}
bool wifiNetworkConnectionById(uint32_t id, WifiConnectionTarget& out) {
  if (id == 0 || id > gScript.size()) return false;
  out.id = id;
  out.ssid = String(gScript[id - 1].ssid.c_str());
  out.password = String("secret-password");
  out.priority = static_cast<int>(id);
  out.index = static_cast<int>(id - 1);
  return true;
}
int wifiNetworkCount() { return static_cast<int>(std::min<size_t>(gScript.size(), MAX_WIFI_NETWORKS)); }
// setupWiFi's lazy driver init and the auto-reconnect setting it applies.
static bool gWifiInitOk = true;
static int gEnsureInitCalls = 0;
bool ensureWiFiInitialized() { ++gEnsureInitCalls; return gWifiInitOk; }
struct FakeSettings { bool wifiAutoReconnect = true; };
static FakeSettings gSettings;
void sortWiFiByPriority() {}
bool saveWiFiNetworks() { return true; }
void wifiNetworkMarkConnected(uint32_t, uint32_t) {}
// ESP-NOW radio ownership (System_ESPNow): initialized, or a timed-out
// closeespnow that still retains its runtime and the driver.
static bool gEspNowInitialized = false;
static bool gEspNowClosePending = false;
bool isEspNowInitialized() { return gEspNowInitialized; }
bool espnowOwnsRadio() { return gEspNowInitialized || gEspNowClosePending; }
static int gNtpSyncs = 0;
bool syncNTPAndResolve() { ++gNtpSyncs; return true; }
volatile bool gWifiUserCancelled = false;
bool gSkipNTPInWifiConnect = true;

static std::vector<std::string> gMessages;
void broadcastOutput(const String& message) { gMessages.emplace_back(message.c_str()); }
#define BROADCAST_PRINTF(...)                              \
  do {                                                     \
    char broadcastBuf[256];                                \
    std::snprintf(broadcastBuf, sizeof(broadcastBuf), __VA_ARGS__); \
    broadcastOutput(String(broadcastBuf));                 \
  } while (0)
// Swallows the arguments without printf checking (the debug lines format a
// size_t with %d on purpose, as the firmware's queue formatter accepts).
void debugSink(const char*, ...) {}
#define DEBUG_WIFIF(...) debugSink(__VA_ARGS__)
#define WARN_WIFIF(...) debugSink(__VA_ARGS__)
#define ERROR_WIFIF(...) debugSink(__VA_ARGS__)
void logSystemEvent(const char*, const char*, ...) {}
static constexpr uint8_t SYSEVT_WIFI_CONNECTED = 1;
static constexpr uint8_t SYSEVT_WIFI_CONNECT_FAILED = 2;
static int gConnectFailedEvents = 0;
void systemEventPost(uint8_t kind, const char* = nullptr, const char* = nullptr) {
  if (kind == SYSEVT_WIFI_CONNECT_FAILED) ++gConnectFailedEvents;
}

enum CommandOrigin { ORIGIN_SERIAL, ORIGIN_WEB, ORIGIN_BLUETOOTH, ORIGIN_MQTT };
struct CommandContext { CommandOrigin origin = ORIGIN_WEB; };
static CommandContext* gCommandContext = nullptr;
void* currentCommandContext() { return gCommandContext; }

static uint32_t gSetupRemainingMs = UINT32_MAX;
uint32_t otaSafetySetupRemainingMs() { return gSetupRemainingMs; }

// INSERT_PRODUCTION_SUPPORT

// INSERT_PRODUCTION_HUNT

// INSERT_PRODUCTION_ATTEMPT

// ---------------------------------------------------------------------------
// Checks
// ---------------------------------------------------------------------------
static CommandContext gWebContext{ORIGIN_WEB};
static CommandContext gSerialContext{ORIGIN_SERIAL};

static void reset(std::vector<ScriptedNetwork> script, CommandContext* context, int pendingBytes) {
  gScript = std::move(script);
  gDriver = DriverState{};
  gConnectCalls.clear();
  gMessages.clear();
  gCommandContext = context;
  Serial = FakeSerial{};
  Serial.pending = pendingBytes;
  gWifiUserCancelled = false;
  gSetupRemainingMs = UINT32_MAX;
  gEspNowInitialized = false;
  gEspNowClosePending = false;
  gDriverStops = 0;
  gDriverModes.clear();
  gArduinoModes.clear();
  gDelayHook = nullptr;
  gNtpSyncs = 0;
  gConnectFailedEvents = 0;
  gSkipNTPInWifiConnect = true;
  gWifiInitOk = true;
  gEnsureInitCalls = 0;
#if HW1_WIFI_SUPERSEDE_API
  sBackgroundConnectActive.store(false);
  sBackgroundConnectSuperseded.store(false);
#endif
}

static bool modeRequested(const std::vector<wifi_mode_t>& modes, wifi_mode_t mode) {
  for (wifi_mode_t m : modes) if (m == mode) return true;
  return false;
}

static std::vector<ScriptedNetwork> missing(int count) {
  std::vector<ScriptedNetwork> script;
  for (int i = 0; i < count; ++i) script.push_back({"missing-" + std::to_string(i + 1), Behaviour::Missing, 0});
  return script;
}

static size_t connectCalls(const std::string& ssid) {
  const auto it = gConnectCalls.find(ssid);
  return it == gConnectCalls.end() ? 0 : it->second.size();
}

static unsigned long longestAttemptGap(const std::string& ssid) {
  const auto& calls = gConnectCalls[ssid];
  unsigned long longest = 0;
  for (size_t i = 1; i < calls.size(); ++i) longest = std::max(longest, calls[i] - calls[i - 1]);
  return longest;
}

static bool sawMessage(const char* fragment) {
  for (const auto& m : gMessages) if (m.find(fragment) != std::string::npos) return true;
  return false;
}

int main() {
  const uint32_t budget = kWifiConnectHuntBudgetMs;

  // Eight saved networks, none in range: the whole hunt fits the budget and no
  // attempt waits out its 35 s window once the driver has reported no SSID.
  {
    reset(missing(8), &gWebContext, 0);
    const unsigned long t0 = millis();
    require(!huntBest(budget, WifiConnectOrigin::COMMAND), "eight missing networks: hunt reports no connection");
    const unsigned long elapsed = millis() - t0;
    std::fprintf(stderr, "eight missing networks: %lu ms virtual (budget %u ms)\n", elapsed, budget);
    require(elapsed <= budget + 500, "eight missing networks: hunt ends within the budget");
    require(connectCalls("missing-1") == 3, "eight missing networks: the first network got its three attempts");
    require(longestAttemptGap("missing-1") < 15000, "a NO_SSID attempt ends after the grace, not after 35 s");
    require(connectCalls("missing-6") >= 1, "the hunt reaches the sixth saved network within the budget");
  }

  // One saved network reached by index (openwifi --index) that is not in range.
  {
    reset(missing(1), &gWebContext, 0);
    WifiConnectionTarget target;
    require(wifiNetworkConnectionById(1, target), "single target: lookup");
    const unsigned long t0 = millis();
    require(!connectWiFiTarget(target, 35000, false), "single missing target: no connection");
    require(millis() - t0 < 20000, "single missing target: finishes long before three 35 s windows");
    require(connectCalls("missing-1") == 3, "single missing target: still three attempts");
  }

  // A link that keeps associating keeps the full 35 s window per attempt.
  {
    reset({{"handshaking", Behaviour::Handshaking, 0}}, &gWebContext, 0);
    const unsigned long t0 = millis();
    require(!huntBest(budget, WifiConnectOrigin::COMMAND), "handshaking link: no connection");
    require(connectCalls("handshaking") == 3, "handshaking link: three attempts");
    const auto& calls = gConnectCalls["handshaking"];
    for (size_t i = 1; i < calls.size(); ++i) {
      require(calls[i] - calls[i - 1] >= 35000, "handshaking link: every attempt kept its 35 s window");
    }
    require(millis() - t0 < budget, "handshaking link: one network fits the default budget");
  }

  // The deadline cuts the hunt: with 50 s for two handshaking networks the
  // second attempt on the first network stops at the deadline and the second
  // network is reported as not tried.
  {
    reset({{"hs-1", Behaviour::Handshaking, 0}, {"hs-2", Behaviour::Handshaking, 0}}, &gWebContext, 0);
    const unsigned long t0 = millis();
    require(!huntBest(50000, WifiConnectOrigin::COMMAND), "deadline: no connection");
    const unsigned long elapsed = millis() - t0;
    std::fprintf(stderr, "deadline cut: %lu ms virtual (budget 50000 ms)\n", elapsed);
    require(elapsed <= 50500, "deadline: the hunt stops at the budget");
    require(connectCalls("hs-1") == 2, "deadline: the first network's third attempt is not started");
    require(connectCalls("hs-2") == 0, "deadline: the second network is not tried");
    require(sawMessage("not tried"), "deadline: the untried network is reported");
  }

  // A reachable second network is still joined after a missing first one.
  {
    reset({{"missing-1", Behaviour::Missing, 0}, {"home", Behaviour::Connects, 2000}}, &gWebContext, 0);
    const unsigned long t0 = millis();
    require(huntBest(budget, WifiConnectOrigin::COMMAND), "missing then reachable: connected");
    require(millis() - t0 < 40000, "missing then reachable: the missing network did not cost three windows");
    require(connectCalls("home") == 1, "missing then reachable: one attempt on the reachable network");
    require(!gWifiUserCancelled, "missing then reachable: not cancelled");
  }

  // Console cancellation is honoured only for the console's own requests.
  {
    reset(missing(2), &gWebContext, 1);
    require(!huntBest(budget, WifiConnectOrigin::COMMAND), "web origin: hunt runs to the end");
    require(Serial.reads == 0 && Serial.pending == 1, "web origin: the console byte is left for the main loop");
    require(!gWifiUserCancelled, "web origin: a console keystroke does not cancel the hunt");
    require(connectCalls("missing-2") == 3, "web origin: every saved network was tried");
  }
  {
    reset(missing(2), nullptr, 1);
    require(!huntBest(budget, WifiConnectOrigin::COMMAND), "no command context (companion restore): hunt runs");
    require(Serial.reads == 0 && !gWifiUserCancelled, "no command context: the console is not read");
  }
  {
    reset(missing(2), &gSerialContext, 1);
    const unsigned long t0 = millis();
    require(!huntBest(budget, WifiConnectOrigin::COMMAND), "serial origin: cancelled hunt reports no connection");
    require(gWifiUserCancelled && Serial.reads == 1, "serial origin: a keystroke cancels and is consumed");
    require(millis() - t0 < 5000, "serial origin: cancellation is immediate");
    require(connectCalls("missing-2") == 0, "serial origin: the hunt stops at the first network");
  }
  {
    reset(missing(2), nullptr, 1);
    require(!huntBest(budget, WifiConnectOrigin::BOOT), "boot: cancelled hunt reports no connection");
    require(gWifiUserCancelled && Serial.reads == 1, "boot: a keystroke skips the connect");
  }

#if HW1_WIFI_CONNECT_BUDGET_API
  // Boot budget: the default on a verified image, the remaining probation
  // setup window minus the reserve on a trial boot, zero once that is gone.
  {
    reset(missing(1), nullptr, 0);
    gSetupRemainingMs = UINT32_MAX;
    require(wifiBootConnectBudgetMs() == kWifiConnectHuntBudgetMs, "verified image: default budget");
    gSetupRemainingMs = 300000;
    require(wifiBootConnectBudgetMs() == kWifiConnectHuntBudgetMs, "fresh trial boot: capped at the default");
    gSetupRemainingMs = 200000;
    require(wifiBootConnectBudgetMs() == 200000 - kWifiBootSetupReserveMs, "trial boot: remaining minus reserve");
    gSetupRemainingMs = kWifiBootSetupReserveMs;
    require(wifiBootConnectBudgetMs() == 0, "trial boot: nothing left once the reserve is reached");
    gSetupRemainingMs = 0;
    require(wifiBootConnectBudgetMs() == 0, "trial boot: nothing left after the deadline");
  }
  // A zero budget starts no attempt and says so.
  {
    reset(missing(3), nullptr, 0);
    const unsigned long t0 = millis();
    require(!huntBest(0, WifiConnectOrigin::BOOT), "zero budget: no connection");
    require(millis() - t0 < 500, "zero budget: returns at once");
    require(connectCalls("missing-1") == 0, "zero budget: no attempt started");
    require(sawMessage("3 saved network(s) not tried"), "zero budget: every network reported as not tried");
  }
#endif

  // ESP-NOW holds the radio: running, and also while a closeespnow that timed
  // out still retains its runtime. Neither the hunt nor a targeted connect may
  // drop AP_STA or stop and restart the driver beneath it.
  for (const bool closePending : {false, true}) {
    const char* what = closePending ? "pending ESP-NOW close" : "running ESP-NOW";
    char label[96];
    reset({{"home", Behaviour::Connects, 2000}}, &gWebContext, 0);
    gEspNowInitialized = !closePending;
    gEspNowClosePending = closePending;
    WifiConnectionTarget target;
    require(wifiNetworkConnectionById(1, target), "radio owner: lookup");
    std::snprintf(label, sizeof(label), "%s: targeted connect joins", what);
    require(connectWiFiTarget(target, 35000, false), label);
    std::snprintf(label, sizeof(label), "%s: the driver is not stopped or switched to STA", what);
    require(gDriverStops == 0 && !modeRequested(gDriverModes, WIFI_MODE_STA), label);
    reset({{"home", Behaviour::Connects, 2000}}, &gWebContext, 0);
    gEspNowInitialized = !closePending;
    gEspNowClosePending = closePending;
    std::snprintf(label, sizeof(label), "%s: hunt joins", what);
    require(huntBest(budget, WifiConnectOrigin::COMMAND), label);
    std::snprintf(label, sizeof(label), "%s: the hunt keeps AP_STA", what);
    require(modeRequested(gArduinoModes, WIFI_AP_STA) && !modeRequested(gArduinoModes, WIFI_STA) &&
            gDriverStops == 0, label);
  }
  // Nothing holds the radio: the connect restarts the driver in STA as before.
  {
    reset({{"home", Behaviour::Connects, 2000}}, &gWebContext, 0);
    WifiConnectionTarget target;
    require(wifiNetworkConnectionById(1, target), "no radio owner: lookup");
    require(connectWiFiTarget(target, 35000, false), "no radio owner: targeted connect joins");
    require(gDriverStops == 1 && modeRequested(gDriverModes, WIFI_MODE_STA),
            "no radio owner: the driver restarts in STA");
  }

#if HW1_WIFI_SUPERSEDE_API
  // The background boot connect runs beside the main loop: the console byte
  // stays with the loop and every saved network is still tried.
  {
    reset(missing(2), nullptr, 1);
    require(!huntBest(budget, WifiConnectOrigin::BACKGROUND), "background: hunt runs to the end");
    require(Serial.reads == 0 && Serial.pending == 1 && !gWifiUserCancelled,
            "background: the console is not read");
    require(connectCalls("missing-2") == 3, "background: every saved network was tried");
  }
  // It leaves the clock to the boot sequence; a command-origin hunt still syncs.
  {
    reset({{"home", Behaviour::Connects, 2000}}, nullptr, 0);
    gSkipNTPInWifiConnect = false;
    require(huntBest(budget, WifiConnectOrigin::BACKGROUND), "background: joins");
    require(gNtpSyncs == 0, "background: no NTP sync inside the hunt");
    reset({{"home", Behaviour::Connects, 2000}}, &gWebContext, 0);
    gSkipNTPInWifiConnect = false;
    require(huntBest(budget, WifiConnectOrigin::COMMAND), "command: joins");
    require(gNtpSyncs == 1, "command: the hunt syncs NTP after joining");
  }
  // A radio command supersedes it mid-attempt: the attempt ends within one
  // status poll, no later network is tried and no connect failure is reported.
  {
    static unsigned long sSupersedeAtMs = 0;
    reset({{"hs-1", Behaviour::Handshaking, 0}, {"hs-2", Behaviour::Handshaking, 0}}, nullptr, 0);
    const unsigned long t0 = millis();
    sSupersedeAtMs = t0 + 10000;
    gDelayHook = [] { if (gNowMs >= sSupersedeAtMs) sBackgroundConnectSuperseded.store(true); };
    require(!huntBest(budget, WifiConnectOrigin::BACKGROUND), "superseded mid-attempt: no connection");
    const unsigned long late = millis() - sSupersedeAtMs;
    std::fprintf(stderr, "superseded mid-attempt: stopped %lu ms after the request\n", late);
    require(late <= 1000, "superseded mid-attempt: stops within one status poll");
    require(connectCalls("hs-1") == 1 && connectCalls("hs-2") == 0,
            "superseded mid-attempt: no retry and no later network");
    require(gConnectFailedEvents == 0, "superseded mid-attempt: not reported as a connect failure");
    require(sawMessage("stopped by a WiFi command"), "superseded mid-attempt: says why it stopped");
  }
  // ...and during the backoff between attempts, which no longer runs to its end.
  {
    reset({{"missing-1", Behaviour::Missing, 0}}, nullptr, 0);
    gDelayHook = [] { if (sawMessage("Retrying '")) sBackgroundConnectSuperseded.store(true); };
    require(!huntBest(budget, WifiConnectOrigin::BACKGROUND), "superseded in backoff: no connection");
    require(connectCalls("missing-1") == 1, "superseded in backoff: the retry is not started");
    require(gConnectFailedEvents == 0, "superseded in backoff: not reported as a connect failure");
  }
  // Only a background connect can be superseded; a command's own hunt ignores it.
  {
    reset(missing(1), &gWebContext, 0);
    sBackgroundConnectSuperseded.store(true);
    require(!huntBest(budget, WifiConnectOrigin::COMMAND), "command origin: hunt runs");
    require(connectCalls("missing-1") == 3, "command origin: a stale supersede request is ignored");
  }
  // The waiting side: immediate with no background connect, bounded otherwise.
  {
    reset(missing(1), nullptr, 0);
    unsigned long t0 = millis();
    require(wifiSupersedeBackgroundConnect(5000), "supersede: nothing running returns at once");
    require(millis() == t0 && !sBackgroundConnectSuperseded.load(),
            "supersede: nothing running neither waits nor leaves a request behind");
    sBackgroundConnectActive.store(true);
    t0 = millis();
    require(!wifiSupersedeBackgroundConnect(5000), "supersede: a connect that never lets go times out");
    require(millis() - t0 >= 5000 && millis() - t0 < 5100, "supersede: waits the bound and no longer");
    require(sBackgroundConnectSuperseded.load(), "supersede: the request is left for the hunt to see");
    static unsigned long sReleaseAtMs = 0;
    sReleaseAtMs = millis() + 300;
    gDelayHook = [] { if (gNowMs >= sReleaseAtMs) sBackgroundConnectActive.store(false); };
    t0 = millis();
    require(wifiSupersedeBackgroundConnect(5000), "supersede: returns once the connect lets go");
    require(millis() - t0 < 400, "supersede: returns promptly after the release");
  }
  // ...and during the pause between saved networks, before the next is tried.
  {
    reset(missing(2), nullptr, 0);
    gDelayHook = [] {
      if (sawMessage("Failed connecting to 'missing-1'")) sBackgroundConnectSuperseded.store(true);
    };
    require(!huntBest(budget, WifiConnectOrigin::BACKGROUND), "superseded between networks: no connection");
    require(connectCalls("missing-1") == 3 && connectCalls("missing-2") == 0,
            "superseded between networks: the next network is not tried");
  }

  // setupWiFi owns the background connect's lifetime. Reserved before the boot
  // task starts, active for the whole hunt, released on every way out.
  {
    static bool sSawInactive = false;
    reset(missing(1), nullptr, 0);
    wifiBackgroundConnectReserve(true);
    require(wifiBackgroundConnectActive(), "reserve: active before the task runs");
    sSawInactive = false;
    gDelayHook = [] { if (!wifiBackgroundConnectActive()) sSawInactive = true; };
    setupWiFi(budget, WifiConnectOrigin::BACKGROUND);
    require(!sSawInactive, "setupWiFi: active throughout the hunt");
    require(!wifiBackgroundConnectActive(), "setupWiFi: released when the hunt returns");
    require(!wifiBackgroundConnectSuperseded(), "setupWiFi: a natural failure is not a supersede");
    require(sawMessage("WiFi connect timed out"), "setupWiFi: a natural failure is still reported");
    require(connectCalls("missing-1") == 3, "setupWiFi: the reserved hunt ran");
  }
  {
    reset(missing(1), nullptr, 0);
    gWifiInitOk = false;
    wifiBackgroundConnectReserve(true);
    setupWiFi(budget, WifiConnectOrigin::BACKGROUND);
    require(!wifiBackgroundConnectActive(), "setupWiFi: released on the driver-init early return");
  }
  {
    reset(missing(1), nullptr, 0);
    wifiBackgroundConnectReserve(true);
    wifiBackgroundConnectReserve(false);
    require(!wifiBackgroundConnectActive(), "reserve(false): drops a reservation whose task never ran");
  }
  // A command that arrives after the reservation but before the task reaches
  // setupWiFi still supersedes it: nothing is initialized or tried.
  {
    reset(missing(1), nullptr, 0);
    wifiBackgroundConnectReserve(true);
    require(!wifiSupersedeBackgroundConnect(0), "early supersede: the reserved connect has not let go yet");
    setupWiFi(budget, WifiConnectOrigin::BACKGROUND);
    require(gEnsureInitCalls == 0 && connectCalls("missing-1") == 0,
            "early supersede: the boot connect never starts");
    require(wifiBackgroundConnectSuperseded() && !wifiBackgroundConnectActive(),
            "early supersede: reported as superseded and released");
    require(!sawMessage("WiFi connect timed out"), "early supersede: not reported as a timeout");
  }
  // Superseded mid-hunt through setupWiFi: says why, not "timed out".
  {
    static unsigned long sSupersedeAtMs = 0;
    reset({{"hs-1", Behaviour::Handshaking, 0}}, nullptr, 0);
    wifiBackgroundConnectReserve(true);
    sSupersedeAtMs = millis() + 10000;
    gDelayHook = [] { if (gNowMs >= sSupersedeAtMs) sBackgroundConnectSuperseded.store(true); };
    setupWiFi(budget, WifiConnectOrigin::BACKGROUND);
    require(sawMessage("stopped by a WiFi command") && !sawMessage("WiFi connect timed out"),
            "setupWiFi superseded mid-hunt: reported as stopped, not timed out");
    require(wifiBackgroundConnectSuperseded() && !wifiBackgroundConnectActive(),
            "setupWiFi superseded mid-hunt: superseded and released");
  }
#endif

  // The coexistence mirror reads the attempt as "connecting" while it runs,
  // retries included, and not after it returns.
  {
    static uint8_t sDepthSeen = 0;
    reset(missing(1), &gWebContext, 0);
    gDelayHook = [] {
      const uint8_t depth = sCoexConnectDepth.load();
      if (depth > sDepthSeen) sDepthSeen = depth;
    };
    WifiConnectionTarget target;
    require(wifiNetworkConnectionById(1, target), "coex scope: lookup");
    const uint16_t attemptsBefore = sCoexConnects.load();
    require(!connectWiFiTarget(target, 35000, false), "coex scope: missing target fails");
    require(sDepthSeen == 1, "coex scope: the attempt counts as connecting while it runs");
    require(sCoexConnectDepth.load() == 0, "coex scope: released when the attempt returns");
    require(uint16_t(sCoexConnects.load() - attemptsBefore) == 1,
            "coex scope: one counted attempt per target, its retries included");
  }

  std::fprintf(stderr, "checks=%d\n", gChecks);
  std::printf("wifi connect budget: OK\n");
  return 0;
}
