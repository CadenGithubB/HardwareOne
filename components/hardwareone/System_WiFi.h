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

// WiFi Command Handlers
const char* cmd_wifiinfo(const String& argsInput);
const char* cmd_wifilist(const String& argsInput);
const char* cmd_wifiadd(const String& originalCmd);
const char* cmd_wifirm(const String& originalCmd);
const char* cmd_wifipromote(const String& originalCmd);
const char* cmd_wificonnect(const String& originalCmd);
const char* cmd_wifidisconnect(const String& argsInput);
const char* cmd_wifidrop(const String& argsInput);   // `wifidisconnect` cmd: drop AP, keep radio + HTTP up
const char* cmd_wifiscan(const String& command);
const char* cmd_wifitxpower(const String& originalCmd);
const char* cmd_wifigettxpower(const String& argsInput);
const char* cmd_wifiautoreconnect(const String& originalCmd);

// WiFi Helper Functions
bool ensureWiFiInitialized();
void setupWiFi();
bool connectToBestWiFiNetwork();

// Command registry (for system_utils.cpp)
struct CommandEntry;
extern const CommandEntry wifiCommands[];
extern const size_t wifiCommandsCount;

#endif // ENABLE_WIFI

#endif // SYSTEM_WIFI_H
