#include "OLED_RemoteSettings.h"

#if ENABLE_OLED_DISPLAY && ENABLE_ESPNOW && ENABLE_BONDED_MODE

#include <ArduinoJson.h>
#include <LittleFS.h>
#include "System_VFS.h"
#include "System_Debug.h"

#include "System_ESPNow.h"
#include "System_Mutex.h"
#include "System_Utils.h"
#include "System_MemUtil.h"
#include "OLED_Utils.h"

// Storage for dynamically created remote settings modules
static SettingsModule* gRemoteModules = nullptr;
static SettingEntry* gRemoteEntries = nullptr;
static size_t gRemoteModuleCount = 0;
static size_t gRemoteEntryCount = 0;

// Helper: Convert JSON type to SettingType
static SettingType jsonTypeToSettingType(JsonVariantConst value) {
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

// ----------------------------------------------------------------------------
// Recursive JSON walk
// ----------------------------------------------------------------------------
// The remote settings.json mirrors the device's nested layout (post-v0.93:
// hardware.sensors.camera.image.cameraQuality, network.wifi.enabled, etc.).
// The OLED viewer renders modules → entries one level deep, so we flatten the
// tree by emitting a synthetic SettingsModule for every JSON object that has
// at least one leaf child. Each leaf becomes an entry on that module; nested
// object children recurse and produce additional modules with a dotted-path
// jsonSection (e.g. "hardware.sensors.camera.image"). The display name is
// the last path segment so the OLED list stays readable.

// Pass 1: count modules + entries so we can allocate flat arrays.
static void countNodesRecursive(JsonObjectConst node,
                                size_t& moduleCount,
                                size_t& entryCount) {
  bool hasLeaves = false;
  for (JsonPairConst kv : node) {
    if (kv.value().is<JsonObjectConst>()) {
      countNodesRecursive(kv.value().as<JsonObjectConst>(), moduleCount, entryCount);
    } else {
      hasLeaves = true;
      entryCount++;
    }
  }
  if (hasLeaves) moduleCount++;
}

// Pass 2: populate the pre-allocated arrays. dottedPath is the full path to
// `node` (empty for root); displayName is what shows in the OLED list.
static void buildNodesRecursive(JsonObjectConst node,
                                const String& dottedPath,
                                const String& displayName,
                                size_t& moduleIdx,
                                size_t& entryIdx) {
  size_t startIdx = entryIdx;
  bool hasLeaves = false;

  // Direct-leaf children become entries on a module rooted at this node.
  for (JsonPairConst kv : node) {
    if (kv.value().is<JsonObjectConst>()) continue;

    SettingEntry& entry = gRemoteEntries[entryIdx++];
    entry.jsonKey = strdup(kv.key().c_str());
    entry.label = strdup(kv.key().c_str());
    entry.type = jsonTypeToSettingType(kv.value());
    entry.valuePtr = nullptr;
    getValueRange(kv.key().c_str(), entry.minVal, entry.maxVal);
    entry.intDefault = 0;
    entry.floatDefault = 0.0f;
    entry.stringDefault = nullptr;
    entry.options = nullptr;
    entry.isSecret = false;
    hasLeaves = true;
  }

  if (hasLeaves) {
    SettingsModule& module = gRemoteModules[moduleIdx++];
    module.name = strdup(displayName.c_str());
    module.jsonSection = strdup(dottedPath.c_str());
    module.entries = &gRemoteEntries[startIdx];
    module.count = entryIdx - startIdx;
    module.isConnected = nullptr;
    module.description = "Remote settings";
  }

  // Recurse into nested object children — each becomes its own module(s).
  for (JsonPairConst kv : node) {
    if (!kv.value().is<JsonObjectConst>()) continue;
    String childKey(kv.key().c_str());
    String childPath = dottedPath.length() > 0 ? (dottedPath + "." + childKey) : childKey;
    buildNodesRecursive(kv.value().as<JsonObjectConst>(),
                        childPath, childKey,
                        moduleIdx, entryIdx);
  }
}

/**
 * Load remote settings from cache and create virtual SettingsModule entries
 */
bool loadRemoteSettingsModules() {
  // Free any existing remote modules
  freeRemoteSettingsModules();

  // Get bonded peer MAC
  if (!gSettings.bondModeEnabled || gSettings.bondPeerMac.length() < 12) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[RemoteSettings] Not bonded");
    return false;
  }

  // Build path to cached settings
  uint8_t peerMac[6];
  macParse(gSettings.bondPeerMac.c_str(), peerMac);  // canonical parser (System_Utils.h)

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

  // Pass 1: count
  size_t moduleCount = 0;
  size_t totalEntries = 0;
  countNodesRecursive(doc.as<JsonObjectConst>(), moduleCount, totalEntries);

  if (totalEntries == 0) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[RemoteSettings] No settings found");
    return false;
  }

  // Allocate the flat arrays in PSRAM. ps_calloc (zero-init) is equivalent to the
  // former new[] here: both structs are trivially-destructible and their only
  // in-class defaults are false/nullptr (= zero), which calloc supplies;
  // buildNodesRecursive populates the rest. Freed via free() (NOT delete[]) in
  // freeRemoteSettingsModules().
  gRemoteModules = (SettingsModule*)ps_calloc(moduleCount, sizeof(SettingsModule), AllocPref::PreferPSRAM);
  gRemoteEntries = (SettingEntry*)ps_calloc(totalEntries, sizeof(SettingEntry), AllocPref::PreferPSRAM);
  if (!gRemoteModules || !gRemoteEntries) {
    if (gRemoteModules) { free(gRemoteModules); gRemoteModules = nullptr; }
    if (gRemoteEntries) { free(gRemoteEntries); gRemoteEntries = nullptr; }
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[RemoteSettings] alloc failed (%zu mods, %zu entries)", moduleCount, totalEntries);
    return false;
  }

  // Pass 2: populate. The root object's display name is "Device" so its
  // direct-leaf children (firmwareVersion, etc.) get a sensible header.
  size_t moduleIdx = 0;
  size_t entryIdx = 0;
  buildNodesRecursive(doc.as<JsonObjectConst>(), "", "Device", moduleIdx, entryIdx);

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
    free(gRemoteEntries);   // ps_calloc'd — must free(), not delete[]
    gRemoteEntries = nullptr;
  }
  
  if (gRemoteModules) {
    // Both name and jsonSection are strdup'd for every module (the recursive
    // builder allocates uniformly), so free each independently if non-null.
    for (size_t i = 0; i < gRemoteModuleCount; i++) {
      if (gRemoteModules[i].name)        free((void*)gRemoteModules[i].name);
      if (gRemoteModules[i].jsonSection) free((void*)gRemoteModules[i].jsonSection);
    }
    free(gRemoteModules);   // ps_calloc'd — must free(), not delete[]
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
 * Apply remote setting change by sending command to bonded peer
 */
bool applyRemoteSettingChange(const char* moduleName, const char* settingKey, const String& value) {
  if (!moduleName || !settingKey) return false;
  
  // Build remote command: "set <key> <value>"
  char cmdBuf[128];
  snprintf(cmdBuf, sizeof(cmdBuf), "set %s %s", settingKey, value.c_str());
  String cmd = cmdBuf;
  
  // Execute via unified OLED command helper (SOURCE_LOCAL_DISPLAY context)
  char result[256];
  String remoteCmd = "remote:" + cmd;

  bool success = executeOLEDCommandWithResult(remoteCmd, result, sizeof(result));
  
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
    DEBUG_SYSTEMF("[HAS_REMOTE_SETTINGS] EXIT: bond mode disabled or MAC too short");
    return false;
  }
  
  // Check if settings cache exists
  uint8_t peerMac[6];
  macParse(gSettings.bondPeerMac.c_str(), peerMac);  // canonical parser (System_Utils.h)
  
  char macStr[13];
  macToPathToken(peerMac, macStr);  // canonical PATH TOKEN form (System_Utils.h)

  char filePathBuf[64];
  snprintf(filePathBuf, sizeof(filePathBuf), "/system/espnow/peers/%s/settings.json", macStr);
  String filePath = filePathBuf;
  DEBUG_SYSTEMF("[HAS_REMOTE_SETTINGS] Checking path: %s", filePath.c_str());
  
  extern bool filesystemReady;
  // trusted: cached remote-device settings manifest read for OLED rendering.
  bool exists = filesystemReady && VFS::existsGuarded(filePath, VFS::systemAuth("oled.remote_settings.read"));
  DEBUG_SYSTEMF("[HAS_REMOTE_SETTINGS] fsReady=%d exists=%d -> returning %d",
                filesystemReady ? 1 : 0, exists ? 1 : 0, exists ? 1 : 0);
  return exists;
}

#endif // ENABLE_OLED_DISPLAY && ENABLE_ESPNOW && ENABLE_BONDED_MODE
