#ifndef SYSTEM_WIFI_NETWORKS_H
#define SYSTEM_WIFI_NETWORKS_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "System_BuildConfig.h"

#define MAX_WIFI_NETWORKS 8

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

#if ENABLE_WIFI
// Boot only: construct internal-RAM storage before settings are loaded and
// before other tasks can use this service. False is a fatal boot allocation error.
bool initWiFiNetworks();
int wifiNetworkCount();
bool wifiNetworkSnapshot(WifiNetworkSnapshot& out);

// These operations own synchronization. Callers never retain storage pointers.
// Mutation does not persist until saveWiFiNetworks() is called.
bool upsertWiFiNetwork(const String& ssid, const String& password, int priority, bool hidden);
bool removeWiFiNetwork(const String& ssid);
bool removeWiFiNetworkById(uint32_t id, String* removedSsid = nullptr);
bool setWiFiNetworkPriority(const String& ssid, int priority);
void sortWiFiByPriority();
// False leaves RAM changes intact. True means the settings writer accepted the
// request; an owner-scoped batch's finalizer reports eventual durable completion.
bool saveWiFiNetworks();

// Shared setup application: priority 1, visible network, auto-start on acceptance.
// Rejected input changes nothing. Failed persistence keeps the list edit in RAM
// and restores the prior auto-start value. Uses the existing settings writer.
WifiNetworkSaveResult saveSetupWiFiNetwork(const String& ssid, const String& password);

// Settings owns crypto policy, key selection, load readiness and file writes.
// Restore preserves legacy records without applying interactive input validation.
// Write returns false when a complete consistent copy could not be obtained.
bool wifiNetworksWriteJson(JsonDocument& doc, bool excludePasswords);
unsigned wifiNetworksReadJson(const JsonDocument& doc);
#else
inline bool initWiFiNetworks() { return false; }
inline int wifiNetworkCount() { return 0; }
inline bool wifiNetworkSnapshot(WifiNetworkSnapshot& out) { out = {}; return false; }
inline bool upsertWiFiNetwork(const String&, const String&, int, bool) { return false; }
inline bool removeWiFiNetwork(const String&) { return false; }
inline bool removeWiFiNetworkById(uint32_t, String* = nullptr) { return false; }
inline bool setWiFiNetworkPriority(const String&, int) { return false; }
inline void sortWiFiByPriority() {}
inline bool saveWiFiNetworks() { return false; }
inline WifiNetworkSaveResult saveSetupWiFiNetwork(const String&, const String&) {
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
