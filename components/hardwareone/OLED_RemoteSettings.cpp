#include "OLED_RemoteSettings.h"

#if ENABLE_OLED_DISPLAY && ENABLE_ESPNOW

#include <ArduinoJson.h>
#include <LittleFS.h>
#include "System_Debug.h"
#include "System_ESPNow.h"
#include "System_Mutex.h"
#include "System_Utils.h"
#include "System_MemUtil.h"

// Storage for dynamically created remote settings modules
static SettingsModule* gRemoteModules = nullptr;
static SettingEntry* gRemoteEntries = nullptr;
static size_t gRemoteModuleCount = 0;
static size_t gRemoteEntryCount = 0;

// Helper: Convert JSON type to SettingType
static SettingType jsonTypeToSettingType(JsonVariant value) {
  if (value.is<bool>()) return SETTING_BOOL;
  if (value.is<int>()) return SETTING_INT;
  if (value.is<const char*>()) return SETTING_STRING;
  return SETTING_STRING; // Default
}

// Helper: Get value range for known settings
static void getValueRange(const char* key, int& minVal, int& maxVal) {
  // Default range
  minVal = 0;
  maxVal = 100;
  
  // Specific ranges for known settings
  if (strstr(key, "brightness") || strstr(key, "Brightness")) {
    minVal = 0; maxVal = 255;
  } else if (strstr(key, "contrast") || strstr(key, "Contrast")) {
    minVal = 0; maxVal = 255;
  } else if (strstr(key, "rotation") || strstr(key, "Rotation")) {
    minVal = 0; maxVal = 3;
  } else if (strstr(key, "channel") || strstr(key, "Channel")) {
    minVal = 1; maxVal = 14;
  } else if (strstr(key, "freq") || strstr(key, "Freq")) {
    minVal = 100000; maxVal = 1000000;
  } else if (strstr(key, "gain") || strstr(key, "Gain")) {
    minVal = 0; maxVal = 100;
  } else if (strstr(key, "count") || strstr(key, "Count")) {
    minVal = 1; maxVal = 64;
  } else if (strstr(key, "pin") || strstr(key, "Pin")) {
    minVal = 0; maxVal = 40;
  }
}

/**
 * Load remote settings from cache and create virtual SettingsModule entries
 */
bool loadRemoteSettingsModules() {
  // Free any existing remote modules
  freeRemoteSettingsModules();
  
  // Get paired peer MAC
  if (!gSettings.bondModeEnabled || gSettings.bondPeerMac.length() < 12) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[RemoteSettings] Not paired");
    return false;
  }
  
  // Build path to cached settings
  uint8_t peerMac[6];
  String macHex = gSettings.bondPeerMac;
  macHex.replace(":", "");
  for (int i = 0; i < 6; i++) {
    char byteStr[3] = {macHex[i*2], macHex[i*2+1], '\0'};
    peerMac[i] = (uint8_t)strtol(byteStr, nullptr, 16);
  }
  
  // Load settings from cache
  extern String loadSettingsFromCache(const uint8_t* peerMac);
  String settingsJson = loadSettingsFromCache(peerMac);
  
  if (settingsJson.length() == 0) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[RemoteSettings] No cached settings");
    return false;
  }
  
  // Parse JSON
  PSRAM_JSON_DOC(doc);
  DeserializationError err = deserializeJson(doc, settingsJson);
  if (err) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[RemoteSettings] JSON parse error: %s", err.c_str());
    return false;
  }
  
  // Count total entries across all sections
  size_t totalEntries = 0;
  size_t moduleCount = 0;
  
  for (JsonPair section : doc.as<JsonObject>()) {
    if (section.value().is<JsonObject>()) {
      moduleCount++;
      totalEntries += section.value().as<JsonObject>().size();
    } else {
      // Root-level setting
      totalEntries++;
    }
  }
  
  if (totalEntries == 0) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[RemoteSettings] No settings found");
    return false;
  }
  
  // Allocate storage
  gRemoteModules = new SettingsModule[moduleCount + 1]; // +1 for root settings
  gRemoteEntries = new SettingEntry[totalEntries];
  
  size_t moduleIdx = 0;
  size_t entryIdx = 0;
  
  // First pass: root-level settings (deviceName, bondRole)
  size_t rootEntryStart = entryIdx;
  for (JsonPair kv : doc.as<JsonObject>()) {
    if (!kv.value().is<JsonObject>()) {
      SettingEntry& entry = gRemoteEntries[entryIdx++];
      entry.jsonKey = strdup(kv.key().c_str());
      entry.label = strdup(kv.key().c_str());
      entry.type = jsonTypeToSettingType(kv.value());
      entry.valuePtr = nullptr; // Not used for remote
      entry.minVal = 0;
      entry.maxVal = 100;
      entry.intDefault = 0;
      entry.floatDefault = 0.0f;
      entry.stringDefault = nullptr;
      entry.options = nullptr;
      entry.isSecret = false;
    }
  }
  
  if (entryIdx > rootEntryStart) {
    SettingsModule& rootModule = gRemoteModules[moduleIdx++];
    rootModule.name = "Device";
    rootModule.jsonSection = nullptr;
    rootModule.entries = &gRemoteEntries[rootEntryStart];
    rootModule.count = entryIdx - rootEntryStart;
    rootModule.isConnected = nullptr;
    rootModule.description = "Device settings";
  }
  
  // Second pass: nested sections (network, display, sensors, etc.)
  for (JsonPair section : doc.as<JsonObject>()) {
    if (!section.value().is<JsonObject>()) continue;
    
    size_t sectionEntryStart = entryIdx;
    JsonObject sectionObj = section.value().as<JsonObject>();
    
    for (JsonPair kv : sectionObj) {
      SettingEntry& entry = gRemoteEntries[entryIdx++];
      entry.jsonKey = strdup(kv.key().c_str());
      entry.label = strdup(kv.key().c_str());
      entry.type = jsonTypeToSettingType(kv.value());
      entry.valuePtr = nullptr; // Not used for remote
      
      // Set appropriate ranges
      getValueRange(kv.key().c_str(), entry.minVal, entry.maxVal);
      
      entry.intDefault = 0;
      entry.floatDefault = 0.0f;
      entry.stringDefault = nullptr;
      entry.options = nullptr;
      entry.isSecret = false;
    }
    
    SettingsModule& module = gRemoteModules[moduleIdx++];
    module.name = strdup(section.key().c_str());
    module.jsonSection = strdup(section.key().c_str());
    module.entries = &gRemoteEntries[sectionEntryStart];
    module.count = entryIdx - sectionEntryStart;
    module.isConnected = nullptr;
    module.description = "Remote settings";
  }
  
  gRemoteModuleCount = moduleIdx;
  gRemoteEntryCount = entryIdx;
  
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[RemoteSettings] Loaded %zu modules, %zu entries", 
         gRemoteModuleCount, gRemoteEntryCount);
  
  return true;
}

/**
 * Free remote settings modules
 */
void freeRemoteSettingsModules() {
  if (gRemoteEntries) {
    // Free duplicated strings
    for (size_t i = 0; i < gRemoteEntryCount; i++) {
      if (gRemoteEntries[i].jsonKey) free((void*)gRemoteEntries[i].jsonKey);
      if (gRemoteEntries[i].label) free((void*)gRemoteEntries[i].label);
    }
    delete[] gRemoteEntries;
    gRemoteEntries = nullptr;
  }
  
  if (gRemoteModules) {
    // Free duplicated module names
    for (size_t i = 0; i < gRemoteModuleCount; i++) {
      if (gRemoteModules[i].name && gRemoteModules[i].jsonSection) {
        free((void*)gRemoteModules[i].name);
        free((void*)gRemoteModules[i].jsonSection);
      }
    }
    delete[] gRemoteModules;
    gRemoteModules = nullptr;
  }
  
  gRemoteModuleCount = 0;
  gRemoteEntryCount = 0;
}

/**
 * Get remote settings modules
 */
const SettingsModule** getRemoteSettingsModules(size_t& count) {
  static const SettingsModule* modulePointers[MAX_SETTINGS_MODULES];
  
  count = gRemoteModuleCount;
  for (size_t i = 0; i < gRemoteModuleCount && i < MAX_SETTINGS_MODULES; i++) {
    modulePointers[i] = &gRemoteModules[i];
  }
  
  return modulePointers;
}

/**
 * Apply remote setting change by sending command to paired peer
 */
bool applyRemoteSettingChange(const char* moduleName, const char* settingKey, const String& value) {
  if (!moduleName || !settingKey) return false;
  
  // Build remote command: "set <key> <value>"
  String cmd = "set " + String(settingKey) + " " + value;
  
  // Execute via unified remote command routing
  extern AuthContext gExecAuthContext;
  char result[256];
  String remoteCmd = "remote:" + cmd;
  
  bool success = executeCommand(gExecAuthContext, remoteCmd.c_str(), result, sizeof(result));
  
  if (success) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[RemoteSettings] Applied %s.%s = %s", 
           moduleName, settingKey, value.c_str());
  } else {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[RemoteSettings] Failed to apply %s.%s", 
           moduleName, settingKey);
  }
  
  return success;
}

/**
 * Check if remote settings are available
 */
bool hasRemoteSettings() {
  DEBUG_SYSTEMF("[HAS_REMOTE_SETTINGS] bondModeEnabled=%d peerMacLen=%d",
                gSettings.bondModeEnabled ? 1 : 0, gSettings.bondPeerMac.length());
  if (!gSettings.bondModeEnabled || gSettings.bondPeerMac.length() < 12) {
    DEBUG_SYSTEMF("[HAS_REMOTE_SETTINGS] EXIT: paired mode disabled or MAC too short");
    return false;
  }
  
  // Check if settings cache exists
  uint8_t peerMac[6];
  String macHex = gSettings.bondPeerMac;
  macHex.replace(":", "");
  for (int i = 0; i < 6; i++) {
    char byteStr[3] = {macHex[i*2], macHex[i*2+1], '\0'};
    peerMac[i] = (uint8_t)strtol(byteStr, nullptr, 16);
  }
  
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X",
           peerMac[0], peerMac[1], peerMac[2], peerMac[3], peerMac[4], peerMac[5]);
  
  String filePath = String("/cache/peers/") + macStr + "/settings.json";
  DEBUG_SYSTEMF("[HAS_REMOTE_SETTINGS] Checking path: %s", filePath.c_str());
  
  extern bool filesystemReady;
  bool exists = filesystemReady && LittleFS.exists(filePath.c_str());
  DEBUG_SYSTEMF("[HAS_REMOTE_SETTINGS] fsReady=%d exists=%d -> returning %d",
                filesystemReady ? 1 : 0, exists ? 1 : 0, exists ? 1 : 0);
  return exists;
}

#endif // ENABLE_OLED_DISPLAY && ENABLE_ESPNOW
