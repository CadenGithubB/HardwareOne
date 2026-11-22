#ifndef SYSTEM_WIFI_NETWORKS_H
#define SYSTEM_WIFI_NETWORKS_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "System_BuildConfig.h"

#define MAX_WIFI_NETWORKS 8

// Where the Wi-Fi settings sit in settings.json, one owner for every spelling:
// network.wifi holds the wifi settings module's switch rows (System_WiFi.cpp,
// whose jsonSection is kModuleSection) and the saved-network array that
// wifiNetworksWriteJson/ReadJson keep. The backup restore
// (WebServer_MigrationTool.cpp) keeps this device's networks and forces the
// two switches on by these names, so a renamed key cannot silently stop
// matching (it once forced "autoReconnect" after that key was gone). Char
// arrays, so ArduinoJson takes each key as the string literal it replaces.
// Every build, also with Wi-Fi compiled out: the restore and the settings
// file layout do not depend on the radio.
namespace hw1wifijson {
inline constexpr char kParent[] = "network";
inline constexpr char kSection[] = "wifi";
inline constexpr char kModuleSection[] = "network.wifi";  // SettingsModule::jsonSection
inline constexpr char kNetworks[] = "networks";
inline constexpr char kAutoStart[] = "wifiAutoStart";
inline constexpr char kAutoReconnect[] = "wifiAutoReconnect";

// True when whole is "<parent>.<child>".
constexpr bool isDottedPath(const char* whole, const char* parent, const char* child) {
  while (*parent) {
    if (*whole++ != *parent++) return false;
  }
  if (*whole++ != '.') return false;
  while (*child) {
    if (*whole++ != *child++) return false;
  }
  return *whole == '\0';
}
static_assert(isDottedPath(kModuleSection, kParent, kSection),
              "the wifi module section is network.wifi, the path the networks array and the restore use");
}  // namespace hw1wifijson

// Copies for presentation contain no credentials. IDs identify one saved entry
// for this boot, survive sorting, and are never reused after removal or reload.
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

enum class WifiNetworkSaveResult { Rejected, PersistFailed, Accepted };

// What an interactively added SSID or password cannot contain: a `"` (the
// quoted `wifiadd` field has no escape, so it would end the field early and
// shift the rest into the next argument) or a control character (0x00-0x1F,
// 0x7F). Reports the first such byte. upsertWifiNetwork refuses both.
enum class WifiFieldProblem : uint8_t { None, Quote, ControlCharacter };
WifiFieldProblem wifiNetworkFieldProblem(const char* text);

// Structured Add-WiFi for UI adapters (OLED, G2): writes the one `wifiadd`
// line that carries ssid, password, priority and hidden to cmd_wifiadd
// unchanged — `wifiadd "<ssid>" "<pass>"`, plus `<priority> <0|1>` when a
// priority (> 0) or hidden is given. The adapter submits it through its own
// session-fenced executor path, so the add runs on cmd_exec under the
// registry's admin rule and the audit masks the password token. A field the
// line cannot carry is refused before anything is written (line stays empty);
// length and blank-name rules stay with cmd_wifiadd, whose reply the adapter
// shows. Available in every build: with Wi-Fi off the command does not exist.
WifiFieldProblem wifiAddCommandLine(const String& ssid, const String& password,
                                    int priority, bool hidden, String& line);

// What an accepted upsertWifiNetwork() left in the list for that SSID.
struct WifiUpsertOutcome {
  bool existed = false;       // the SSID was already saved
  bool keptPassword = false;  // empty password given: the stored one stays
  int priority = 0;           // the entry's priority after the call
  bool hidden = false;        // the entry's hidden flag after the call
};

#if ENABLE_WIFI
// Boot only: construct internal-RAM storage before settings are loaded and
// before other tasks can use this service. False is a fatal boot allocation error.
bool initWifiNetworks();
int wifiNetworkCount();
bool wifiNetworkSnapshot(WifiNetworkSnapshot& out);

// These operations own synchronization. Callers never retain storage pointers.
// Mutation does not persist until saveWifiNetworks() is called.
// An existing entry keeps its priority when priority <= 0 and its hidden flag
// when hiddenGiven is false.
bool upsertWifiNetwork(const String& ssid, const String& password, int priority, bool hidden,
                       bool hiddenGiven = true, WifiUpsertOutcome* outcome = nullptr);
bool removeWifiNetwork(const String& ssid);
bool removeWifiNetworkById(uint32_t id, String* removedSsid = nullptr);
bool setWifiNetworkPriority(const String& ssid, int priority);
void sortWifiByPriority();
// False leaves RAM changes intact. True means the settings writer accepted the
// request; an owner-scoped batch's finalizer reports eventual durable completion.
bool saveWifiNetworks();

// Shared setup application: priority 1, visible network, auto-start on acceptance.
// Rejected input changes nothing. Failed persistence keeps the list edit in RAM
// and restores the prior auto-start value. Uses the existing settings writer.
WifiNetworkSaveResult saveSetupWifiNetwork(const String& ssid, const String& password);

// Settings owns crypto policy, key selection, load readiness and file writes.
// Restore preserves legacy records without applying interactive input validation.
// Write returns false when a complete consistent copy could not be obtained.
bool wifiNetworksWriteJson(JsonDocument& doc, bool excludePasswords);
unsigned wifiNetworksReadJson(const JsonDocument& doc);
#else
inline bool initWifiNetworks() { return false; }
inline int wifiNetworkCount() { return 0; }
inline bool wifiNetworkSnapshot(WifiNetworkSnapshot& out) { out = {}; return false; }
inline bool upsertWifiNetwork(const String&, const String&, int, bool, bool = true,
                              WifiUpsertOutcome* = nullptr) { return false; }
inline bool removeWifiNetwork(const String&) { return false; }
inline bool removeWifiNetworkById(uint32_t, String* = nullptr) { return false; }
inline bool setWifiNetworkPriority(const String&, int) { return false; }
inline void sortWifiByPriority() {}
inline bool saveWifiNetworks() { return false; }
inline WifiNetworkSaveResult saveSetupWifiNetwork(const String&, const String&) {
  return WifiNetworkSaveResult::Rejected;
}
// A disabled feature preserves its existing section in the merged settings file.
inline bool wifiNetworksWriteJson(JsonDocument&, bool) { return true; }
inline unsigned wifiNetworksReadJson(const JsonDocument&) { return 0; }
#endif

inline const char* wifiNetworkSaveResultMessage(WifiNetworkSaveResult result) {
  switch (result) {
    case WifiNetworkSaveResult::Accepted:
      return "WiFi credentials accepted; settings save requested.";
    case WifiNetworkSaveResult::PersistFailed:
      return "WiFi credentials could not be saved; changes may be lost after restarting.";
    default:
      return "WiFi credentials were not accepted.";
  }
}

#endif // SYSTEM_WIFI_NETWORKS_H
