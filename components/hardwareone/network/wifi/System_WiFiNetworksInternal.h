#ifndef SYSTEM_WIFI_NETWORKS_INTERNAL_H
#define SYSTEM_WIFI_NETWORKS_INTERNAL_H

#include "System_WiFiNetworks.h"

// Radio connection adapter only. Public/UI snapshots never include passwords.
// The caller owns this copy; no list mutex is held during a connection attempt.
struct WifiConnectionTarget {
  uint32_t id = 0;
  String ssid;
  String password;
  int priority = 0;
  int index = -1;

  WifiConnectionTarget() = default;
  WifiConnectionTarget(const WifiConnectionTarget&) = default;
  WifiConnectionTarget(WifiConnectionTarget&&) = default;
  WifiConnectionTarget& operator=(const WifiConnectionTarget&) = default;
  WifiConnectionTarget& operator=(WifiConnectionTarget&&) = default;
  // Zeroes this copy's password before its buffer returns to the heap
  // (defined beside the saved list in System_WiFiNetworks.cpp).
  ~WifiConnectionTarget();
};

#if ENABLE_WIFI
bool wifiNetworkConnectionByIndex(int index, WifiConnectionTarget& out);
bool wifiNetworkConnectionById(uint32_t id, WifiConnectionTarget& out);
bool wifiNetworkConnectionBySSID(const String& ssid, WifiConnectionTarget& out);
void wifiNetworkMarkConnected(uint32_t id, uint32_t when);
#endif

#endif // SYSTEM_WIFI_NETWORKS_INTERNAL_H
