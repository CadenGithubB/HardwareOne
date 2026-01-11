#ifndef WIFI_SYSTEM_H
#define WIFI_SYSTEM_H

#include <Arduino.h>
#include "System_Debug.h"  // Centralized OUTPUT_* flags and gOutputFlags

// WiFi Constants
#define MAX_WIFI_NETWORKS 8

// WiFi Network Structure
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

// WiFi Command Handlers
const char* cmd_wifiinfo(const String& cmd);
const char* cmd_wifilist(const String& cmd);
const char* cmd_wifiadd(const String& originalCmd);
const char* cmd_wifirm(const String& originalCmd);
const char* cmd_wifipromote(const String& originalCmd);
const char* cmd_wificonnect(const String& originalCmd);
const char* cmd_wifidisconnect(const String& cmd);
const char* cmd_wifiscan(const String& command);
const char* cmd_wifitxpower(const String& originalCmd);
const char* cmd_wifigettxpower(const String& cmd);
const char* cmd_wifiautoreconnect(const String& originalCmd);

// WiFi Helper Functions
bool ensureWiFiInitialized();  // Lazy initialization (saves ~32KB at boot)
void setupWiFi();
bool connectToBestWiFiNetwork();

// Command registry (for system_utils.cpp)
struct CommandEntry;
extern const CommandEntry wifiCommands[];
extern const size_t wifiCommandsCount;

#endif // WIFI_SYSTEM_H
