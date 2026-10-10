/**
 * System Setup Wizard Implementation
 * 
 * Core logic for setup wizard - display agnostic
 */

#include "System_SetupWizard.h"
#include "System_MemUtil.h"
#include <esp_attr.h>
#include <cerrno>
#include <cstdlib>
#include <strings.h>
#include "System_FeatureRegistry.h"
#include "System_Settings.h"
#include "System_BuildConfig.h"
#include "System_Debug.h"
#include "System_NTP.h"
#include "network/wifi/System_WiFiNetworks.h"

#if ENABLE_WIFI
#include <WiFi.h>
#include "System_WiFi.h"
#endif

#if ENABLE_OLED_DISPLAY
#include "OLED_SetupWizard.h"
#include "HAL_Display.h"
#include "HAL_Input.h"
extern bool oledConnected;
// OLED page handlers for ESP-NOW and MQTT (defined in OLED_SetupWizard.cpp)
void handleOLEDESPNowPage(SetupWizardResult& result, bool& running);
void handleOLEDMQTTPage(SetupWizardResult& result, bool& running);
#endif

extern String waitForSerialInputBlocking();

// ============================================================================
// Setup Archetypes (deployment presets) — see System_SetupWizard.h
// ============================================================================
static const char* const SEED_HANDHELD[] = { "wifi", "http", "i2c", "oled", "input", "automation", nullptr };
static const char* const SEED_HEADLESS[] = { "wifi", "http", "espnow", "bluetooth", nullptr };
static const char* const SEED_GLASSES[]  = { "wifi", "http", "i2c", "bluetooth", "automation", nullptr };
static const char* const SEED_MESH[]     = { "wifi", "espnow", "i2c", "automation", nullptr };

// requiredFeatures: ALL must be compiled, else the archetype is hidden.
static const char* const REQ_HANDHELD[] = { "oled", "input", nullptr };  // local display + non-serial input
static const char* const REQ_GLASSES[]  = { "bluetooth", nullptr };
static const char* const REQ_MESH[]     = { "espnow", nullptr };

const SetupArchetype setupArchetypes[] = {
  { "handheld", "Standard Handheld",
    "Board + OLED + gamepad + sensors. Local menu and web UI - the full handheld.",
    SEED_HANDHELD, REQ_HANDHELD },
  { "headless", "Headless Node",
    "No local screen or sensor bus. Managed over WiFi/web, Bluetooth, and ESP-NOW.",
    SEED_HEADLESS, nullptr },
  { "glasses", "G2 Companion",
    "Drives Even G2 smart glasses over Bluetooth, plus WiFi + web UI.",
    SEED_GLASSES, REQ_GLASSES },
  { "mesh", "Meshed Node",
    "Pairs with your other Hardware One devices over an ESP-NOW mesh.",
    SEED_MESH, REQ_MESH },
};
const size_t setupArchetypesCount = sizeof(setupArchetypes) / sizeof(setupArchetypes[0]);

bool setupArchetypeAvailable(const SetupArchetype* a) {
  if (!a) return false;
  if (!a->requiredFeatures) return true;  // general-purpose: always offered
  for (const char* const* p = a->requiredFeatures; *p; ++p) {
    const FeatureEntry* f = getFeatureById(*p);
    if (!f || !isFeatureCompiled(f)) return false;
  }
  return true;
}

// Enable an archetype's seed features (set each persisted flag). At first-boot
// setup these take effect on the post-setup boot (reboot-gated WiFi/OLED/I2C
// likewise). Detected sensors are NOT touched here — applyDetectedHardware()
// handles those. No-op for Advanced/Import. Returns the number of features enabled.
int applyArchetypeSeed(const SetupArchetype* a) {
  if (!a || !a->seedFeatures) return 0;
  int n = 0;
  for (const char* const* p = a->seedFeatures; *p; ++p) {
    const FeatureEntry* f = getFeatureById(*p);
    if (f && isFeatureCompiled(f) && f->enabledSetting) { *f->enabledSetting = true; n++; }
  }
  if (n) writeSettingsJson();
  return n;
}

// ============================================================================
// Time Zone Data
// ============================================================================

// Fixed-offset entries (no auto DST). The abbrev distinguishes the daylight
// vs standard variant of each zone; the name gives a representative city and
// the offset so it is findable without knowing the abbreviation.
static const TimezoneEntry timezones[] = {
  // UTC
  { "UTC",  "UTC / GMT (0:00)",                     0 },
  // North America — Daylight Saving
  { "EDT",  "US Eastern - New York (-4:00)",     -240 },
  { "CDT",  "US Central - Chicago (-5:00)",      -300 },
  { "MDT",  "US Mountain - Denver (-6:00)",      -360 },
  { "PDT",  "US Pacific - Los Angeles (-7:00)",  -420 },
  { "AKDT", "Alaska - Anchorage (-8:00)",        -480 },
  // North America — Standard Time
  { "EST",  "US Eastern - New York (-5:00)",     -300 },
  { "CST",  "US Central - Chicago (-6:00)",      -360 },
  { "MST",  "US Mountain - Denver/Phoenix (-7:00)", -420 },
  { "PST",  "US Pacific - Los Angeles (-8:00)",  -480 },
  { "AKST", "Alaska - Anchorage (-9:00)",        -540 },
  { "HST",  "Hawaii - Honolulu (-10:00)",        -600 },
  // Europe
  { "GMT",  "UK/Ireland - London (0:00)",           0 },
  { "BST",  "UK/Ireland - London (+1:00)",         60 },
  { "CET",  "Central Europe - Paris/Berlin (+1:00)", 60 },
  { "CEST", "Central Europe - Paris/Berlin (+2:00)", 120 },
  { "EET",  "Eastern Europe - Athens (+2:00)",    120 },
  { "EEST", "Eastern Europe - Athens (+3:00)",    180 },
  // Asia / Pacific
  { "IST",  "India - Mumbai/Delhi (+5:30)",       330 },
  { "SGT",  "Singapore (+8:00)",                  480 },
  { "JST",  "Japan - Tokyo (+9:00)",              540 },
  { "AEST", "Australia East - Sydney (+10:00)",   600 },
  { "AEDT", "Australia East - Sydney (+11:00)",   660 },
  { "NZST", "New Zealand - Auckland (+12:00)",    720 },
  { "NZDT", "New Zealand - Auckland (+13:00)",    780 },
};
static const size_t timezoneCount = sizeof(timezones) / sizeof(timezones[0]);

// ============================================================================
// Log Level Data
// ============================================================================

static const char* logLevelNames[] = {
  "ERROR",   // Only errors
  "WARN",    // Errors + warnings
  "INFO",    // Normal operation
  "DEBUG"    // Verbose
};
static const size_t logLevelCount = 4;

// ============================================================================
// NTP Presets
// ============================================================================

static const char* const ntpPresets[] = {
  "pool.ntp.org",
  "us.pool.ntp.org",
  "europe.pool.ntp.org",
  "time.google.com",
  "time.cloudflare.com"
};
static const size_t ntpPresetCount = sizeof(ntpPresets) / sizeof(ntpPresets[0]);

// ============================================================================
// LED Startup Effects
// ============================================================================

static const char* const ledEffects[] = {
  "none",
  "rainbow",
  "pulse",
  "fade",
  "blink",
  "strobe"
};
static const size_t ledEffectCount = sizeof(ledEffects) / sizeof(ledEffects[0]);

// ============================================================================
// Page Order (for dynamic navigation)
// ============================================================================

static const SetupWizardPage kPageOrder[] = {
  WIZARD_PAGE_CONNECTIONS,
  WIZARD_PAGE_WIFI,
  WIZARD_PAGE_WEBMODE,
  WIZARD_PAGE_BTMODE,
  WIZARD_PAGE_ESPNOW,
  WIZARD_PAGE_MQTT,
  WIZARD_PAGE_DISPLAY,
  WIZARD_PAGE_SENSORS,
  WIZARD_PAGE_SYSTEM,
};
static const size_t kPageOrderCount = sizeof(kPageOrder) / sizeof(kPageOrder[0]);

// ============================================================================
// Wizard State
// ============================================================================

static SetupWizardPage currentPage = WIZARD_PAGE_CONNECTIONS;
static int currentSelection = 0;
static int scrollOffset = 0;
static int timezoneSelection = 1;  // EDT default (index 1 — US Eastern Daylight, UTC-4)
static int logLevelSelection = 3;  // DEBUG default (all logging enabled)
static int ntpSelection = 0;       // pool.ntp.org default
static int ledEffectSelection = 1; // rainbow default
static char wizardDeviceName[20] = "";  // 19-byte name + NUL (matches V4 identity fields)

// Ordinary page rows share one model. This storage is dormant after setup.
// Capacity failures invalidate the draft and are reported; no feature is silently
// omitted if the registry grows beyond a page's capacity.
static constexpr size_t kWizardGroupCount = 4;
static constexpr size_t kWizardRowsPerGroup = 24;
EXT_RAM_BSS_ATTR static WizardMenuItem wizardRows[kWizardGroupCount][kWizardRowsPerGroup];
static size_t wizardRowCounts[kWizardGroupCount] = {};

static int wizardGroupIndex(SetupWizardPage page) {
  switch (page) {
    case WIZARD_PAGE_CONNECTIONS: return 0;
    case WIZARD_PAGE_DISPLAY: return 1;
    case WIZARD_PAGE_SENSORS: return 2;
    case WIZARD_PAGE_SYSTEM: return 3;
    default: return -1;
  }
}

// One draft for the active wizard, shared by serial and local display controls.
// Feature metadata remains in its registry; only values and their initial state
// live here. The bound pointers in page rows point to these copies.
struct WizardBoolDraft {
  bool* live = nullptr;
  bool initial = false;
  bool value = false;
};
struct WizardDraft {
  SetupWizardResult initial;
  SetupWizardResult values;
  WizardBoolDraft booleans[56]; // compiled registry flags plus separate startup choices
  size_t boolCount = 0;
  bool active = false;
  bool valid = true;
  char deviceNameBaseline[sizeof(wizardDeviceName)] = {}; // last accepted text-buffer value
};
static WizardDraft sWizardDraft;

struct WizardStringBinding {
  String* live;
  String SetupWizardResult::*field;
};
static const WizardStringBinding kWizardStringBindings[] = {
  { &gSettings.espnowDeviceName, &SetupWizardResult::deviceName },
  { &gSettings.bleDeviceName, &SetupWizardResult::bleDeviceName },
  { &gSettings.espnowFriendlyName, &SetupWizardResult::espnowFriendlyName },
  { &gSettings.espnowRoom, &SetupWizardResult::espnowRoom },
  { &gSettings.espnowZone, &SetupWizardResult::espnowZone },
  { &gSettings.mqttHost, &SetupWizardResult::mqttHost },
  { &gSettings.mqttUser, &SetupWizardResult::mqttUser },
  { &gSettings.mqttPassword, &SetupWizardResult::mqttPassword },
  { &gSettings.ntpServer, &SetupWizardResult::ntpServer },
#if ENABLE_NEOPIXEL
  { &gSettings.ledStartupEffect, &SetupWizardResult::ledStartupEffect },
#endif
};

static bool copyWizardString(String& target, const String& value) {
  if (!target.reserve(value.length())) {
    sWizardDraft.valid = false;
    return false;
  }
  target = value;
  if (target != value) sWizardDraft.valid = false;
  return sWizardDraft.valid;
}

static bool copyWizardString(String& target, const char* value) {
  const size_t length = strlen(value);
  if (!target.reserve(length)) {
    sWizardDraft.valid = false;
    return false;
  }
  target = value;
  if (target.length() != length || strcmp(target.c_str(), value) != 0) sWizardDraft.valid = false;
  return sWizardDraft.valid;
}

static bool* wizardBoolDraft(bool* live) {
  if (!live || !sWizardDraft.active) return nullptr;
  for (size_t i = 0; i < sWizardDraft.boolCount; ++i) {
    if (sWizardDraft.booleans[i].live == live) return &sWizardDraft.booleans[i].value;
  }
  if (sWizardDraft.boolCount == sizeof(sWizardDraft.booleans) / sizeof(sWizardDraft.booleans[0])) {
    broadcastOutput("[Setup] Too many setting choices; setup cannot be saved.");
    sWizardDraft.valid = false;
    return nullptr;
  }
  auto& choice = sWizardDraft.booleans[sWizardDraft.boolCount++];
  choice = {live, *live, *live};
  return &choice.value;
}

const SetupWizardResult& getWizardDraftValues() { return sWizardDraft.values; }

void discardSetupWizard() {
  sWizardDraft.active = false;
  // Release page credentials and choices, including after timeout/session loss.
  sWizardDraft.values = SetupWizardResult{};
  sWizardDraft.initial = SetupWizardResult{};
  sWizardDraft.boolCount = 0;
  sWizardDraft.valid = true;
  wizardDeviceName[0] = '\0';
}

static void initWizardDraft() {
  discardSetupWizard();
  sWizardDraft.active = true;
  auto& values = sWizardDraft.values;
  values.timezoneOffset = gSettings.tzOffsetMinutes;
  values.logLevel = gSettings.logLevel;
  values.httpsEnabled = gSettings.httpsEnabled;
  values.bleMode = gSettings.bleMode;
  values.espnowStationary = gSettings.espnowStationary;
  values.mqttPort = gSettings.mqttPort;
  sWizardDraft.initial = values;
  for (const auto& binding : kWizardStringBindings) {
    copyWizardString(values.*binding.field, *binding.live);
    copyWizardString(sWizardDraft.initial.*binding.field, *binding.live);
  }
  for (size_t i = 0; i < getFeatureCount(); ++i) {
    const FeatureEntry* feature = getFeatureByIndex(i);
    if (feature && isFeatureCompiled(feature)) wizardBoolDraft(feature->enabledSetting);
  }
#if ENABLE_WIFI
  wizardBoolDraft(&gSettings.wifiAutoStart);
#endif
#if ENABLE_ESPNOW
  wizardBoolDraft(&gSettings.espnowmesh);
#endif
#if ENABLE_MQTT
  wizardBoolDraft(&gSettings.mqttAutoStart);
#endif
}

bool wizardFeatureEnabled(const char* featureId) {
  const FeatureEntry* feature = getFeatureById(featureId);
  if (!feature || !isFeatureCompiled(feature)) return false;
  if (!feature->enabledSetting) return true;
  if (!sWizardDraft.active) return isFeatureEnabled(feature);
  const bool* value = wizardBoolDraft(feature->enabledSetting);
  return value && *value;
}

void wizardSetFeatureEnabled(const char* featureId, bool enabled) {
  const FeatureEntry* feature = getFeatureById(featureId);
  if (!feature || !isFeatureCompiled(feature)) return;
  bool* value = wizardBoolDraft(feature->enabledSetting);
  if (value) *value = enabled;
}

void wizardSetMQTTAutoStart(bool enabled) {
#if ENABLE_MQTT
  bool* value = wizardBoolDraft(&gSettings.mqttAutoStart);
  if (value) *value = enabled;
#endif
}

bool wizardParseMQTTPort(const String& input, int& port) {
  String value = input;
  if (value.length() != input.length()) return false;
  value.trim();
  if (value.isEmpty()) { port = 0; return true; }
  char* end = nullptr;
  errno = 0;
  const long parsed = strtol(value.c_str(), &end, 10);
  if (end != value.c_str() + value.length() || errno == ERANGE || parsed < 1 || parsed > 65535) return false;
  port = (int)parsed;
  return true;
}

// ============================================================================
// State Accessors
// ============================================================================

SetupWizardPage getWizardCurrentPage() { return currentPage; }
int getWizardCurrentSelection() { return currentSelection; }
int getWizardScrollOffset() { return scrollOffset; }

void setWizardCurrentPage(SetupWizardPage page) { currentPage = page; }
void setWizardCurrentSelection(int sel) { currentSelection = sel; }
void setWizardScrollOffset(int offset) { scrollOffset = offset; }

const char* getWizardPageTitle(SetupWizardPage page) {
  switch (page) {
    case WIZARD_PAGE_CONNECTIONS: return "Connections";
    case WIZARD_PAGE_WIFI: return "WiFi Setup";
    case WIZARD_PAGE_WEBMODE: return "Web Mode";
    case WIZARD_PAGE_BTMODE: return "Bluetooth Mode";
    case WIZARD_PAGE_ESPNOW: return "ESP-NOW Identity";
    case WIZARD_PAGE_MQTT: return "MQTT Broker";
    case WIZARD_PAGE_DISPLAY: return "Display & Controls";
    case WIZARD_PAGE_SENSORS: return "Sensors";
    case WIZARD_PAGE_SYSTEM: return "System";
    default: return "Setup";
  }
}

bool wizardIsGroupPage(SetupWizardPage page) { return wizardGroupIndex(page) >= 0; }

size_t getWizardMenuItemCount(SetupWizardPage page) {
  const int group = wizardGroupIndex(page);
  return group >= 0 ? wizardRowCounts[group] : 0;
}

const WizardMenuItem* getWizardMenuItem(SetupWizardPage page, int index) {
  const int group = wizardGroupIndex(page);
  if (group < 0 || index < 0 || index >= (int)wizardRowCounts[group]) return nullptr;
  return &wizardRows[group][index];
}

bool wizardFormatMenuItem(SetupWizardPage page, int index,
                          char* label, size_t labelCap,
                          char* value, size_t valueCap) {
  if (!label || !labelCap || !value || !valueCap) return false;
  label[0] = value[0] = '\0';
  const WizardMenuItem* item = getWizardMenuItem(page, index);
  if (!item) return false;
  snprintf(label, labelCap, "%s", item->label);
  const auto& draft = getWizardDraftValues();
  switch (item->type) {
    case WIZARD_ITEM_TOGGLE:
      snprintf(value, valueCap, "%s", item->setting && *item->setting ? "On" : "Off");
      break;
    case WIZARD_ITEM_STARTUP:
      snprintf(value, valueCap, "%s", item->setting && *item->setting ? "Boot on" : "Boot off");
      break;
    case WIZARD_ITEM_TIMEZONE: {
      // A custom offset must be displayed as-is until the user selects a preset.
      const long long offset = draft.timezoneOffset;
      const long long magnitude = offset < 0 ? -offset : offset;
      if (draft.timezoneOffset == timezones[timezoneSelection].offsetMinutes) {
        snprintf(value, valueCap, "%s %c%lld:%02lld", timezones[timezoneSelection].abbrev,
                 offset < 0 ? '-' : '+', magnitude / 60, magnitude % 60);
      } else {
        snprintf(value, valueCap, "UTC%c%lld:%02lld", offset < 0 ? '-' : '+', magnitude / 60, magnitude % 60);
      }
      break;
    }
    case WIZARD_ITEM_LOGLEVEL:
      if (draft.logLevel >= 0 && draft.logLevel < (int)logLevelCount)
        snprintf(value, valueCap, "%s", logLevelNames[draft.logLevel]);
      else snprintf(value, valueCap, "%d", draft.logLevel);
      break;
    case WIZARD_ITEM_NTP:
      snprintf(value, valueCap, "%s", draft.ntpServer.c_str());
      break;
    case WIZARD_ITEM_LED_EFFECT:
      snprintf(value, valueCap, "%s", draft.ledStartupEffect.c_str());
      break;
    case WIZARD_ITEM_DEVICE_NAME:
      snprintf(value, valueCap, "%s", wizardDeviceName);
      break;
  }
  return true;
}

size_t getTimezoneCount() { return timezoneCount; }
const TimezoneEntry* getTimezones() { return timezones; }
int getWizardTimezoneSelection() { return timezoneSelection; }
void setWizardTimezoneSelection(int sel) {
  if (sel < 0 || sel >= (int)timezoneCount || !sWizardDraft.active) return;
  timezoneSelection = sel;
  sWizardDraft.values.timezoneOffset = timezones[sel].offsetMinutes;
}
int getWizardLogLevelSelection() { return logLevelSelection; }
void setWizardLogLevelSelection(int sel) {
  if (sel < 0 || sel >= (int)logLevelCount || !sWizardDraft.active) return;
  logLevelSelection = sel;
  sWizardDraft.values.logLevel = sel;
}
const char** getLogLevelNames() { return (const char**)logLevelNames; }
size_t getLogLevelCount() { return logLevelCount; }

int getWizardNTPSelection() { return ntpSelection; }
void setWizardNTPSelection(int sel) {
  if (sel < 0 || sel >= (int)ntpPresetCount || !sWizardDraft.active) return;
  ntpSelection = sel;
  copyWizardString(sWizardDraft.values.ntpServer, ntpPresets[sel]);
}
int getWizardLEDEffectSelection() { return ledEffectSelection; }
void setWizardLEDEffectSelection(int sel) {
  if (sel < 0 || sel >= (int)ledEffectCount || !sWizardDraft.active) return;
  ledEffectSelection = sel;
  copyWizardString(sWizardDraft.values.ledStartupEffect, ledEffects[sel]);
}
const char* const* getNTPPresets() { return ntpPresets; }
size_t getNTPPresetCount() { return ntpPresetCount; }
const char* const* getLEDEffects() { return ledEffects; }
size_t getLEDEffectCount() { return ledEffectCount; }

const char* getWizardDeviceName() { return wizardDeviceName; }
char* getWizardDeviceNameBuf() { return wizardDeviceName; }

// ============================================================================
// Heap Bar Helper
// ============================================================================
 
 static uint32_t sWizardBaselineKB = 0;
 static bool sWizardBaselineCalibrated = false;
 
 static uint32_t getWizardInfrastructureCostKB() {
   uint32_t infraKB = 0;
 #if ENABLE_OLED_DISPLAY
   const FeatureEntry* i2cFeature = getFeatureById("i2c");
   if (i2cFeature && isFeatureCompiled(i2cFeature)) infraKB += i2cFeature->heapCostKB;
 
   const FeatureEntry* oledFeature = getFeatureById("oled");
   if (oledFeature && isFeatureCompiled(oledFeature)) infraKB += oledFeature->heapCostKB;
 
   const FeatureEntry* inputFeature = getFeatureById("input");
   if (inputFeature && isFeatureCompiled(inputFeature)) infraKB += inputFeature->heapCostKB;
 #endif
   return infraKB;
 }
 
 static void calibrateWizardBaseline() {
   // Internal 8-bit only (see hw1InternalFreeBytes, System_MemUtil.h) — the
   // old ESP.* calls overstated total by ~59 KB and free by ~26 KB, which
   // skewed every wizard heap bar.
   uint32_t totalHeapKB = (uint32_t)(hw1InternalTotalBytes() / 1024);
   if (totalHeapKB == 0) totalHeapKB = 1;
 
   const size_t wizTotal = hw1InternalTotalBytes();
   const size_t wizFree  = hw1InternalFreeBytes();
   uint32_t usedNowKB = (uint32_t)((wizTotal > wizFree ? wizTotal - wizFree : 0) / 1024);
   uint32_t infraKB = getWizardInfrastructureCostKB();
 
   sWizardBaselineKB = (usedNowKB > infraKB) ? (usedNowKB - infraKB) : 0;
   if (sWizardBaselineKB > totalHeapKB) sWizardBaselineKB = totalHeapKB;
   sWizardBaselineCalibrated = true;
 }

uint32_t wizardEnabledModeExtraHeapKB();  // defined with the mode-menu table below

void getHeapBarData(uint32_t* enabledKB, uint32_t* maxKB, int* percentage) {
  uint32_t totalHeapKB = (uint32_t)(hw1InternalTotalBytes() / 1024);
  if (totalHeapKB == 0) totalHeapKB = 1;

   if (!sWizardBaselineCalibrated) {
     calibrateWizardBaseline();
   }

  // Feature-granular base + the selected sub-mode's extra (HTTPS/G2) so the bar
  // reflects mode choices, not just which features are on.
  uint32_t enabledCostKB = wizardEnabledModeExtraHeapKB();
  for (size_t i = 0; i < getFeatureCount(); ++i) {
    const FeatureEntry* feature = getFeatureByIndex(i);
    if (feature && wizardFeatureEnabled(feature->id)) enabledCostKB += feature->heapCostKB;
  }
  uint32_t estimatedUsedKB = sWizardBaselineKB + enabledCostKB;
  if (estimatedUsedKB > totalHeapKB) estimatedUsedKB = totalHeapKB;

  *enabledKB = estimatedUsedKB;
  *maxKB = totalHeapKB;
  *percentage = (estimatedUsedKB * 100) / totalHeapKB;
}

// ============================================================================
// Initialize Wizard
// ============================================================================

// The registry owns availability and settings. These two small mappings own
// wizard presentation only; display/input do not inherit the registry's sensor
// category, and AutoStart-backed rows must say that they affect boot behavior.
static SetupWizardPage wizardFeaturePage(const FeatureEntry& feature) {
  if (feature.category == FEATURE_CAT_NETWORK) return WIZARD_PAGE_CONNECTIONS;
  if (feature.category == FEATURE_CAT_DISPLAY || strcmp(feature.id, "i2c") == 0 ||
      strcmp(feature.id, "input") == 0) return WIZARD_PAGE_DISPLAY;
  if (feature.category == FEATURE_CAT_SENSOR) return WIZARD_PAGE_SENSORS;
  return WIZARD_PAGE_SYSTEM;
}

static WizardMenuItemType wizardFeatureItemType(const FeatureEntry& feature) {
  if ((feature.category == FEATURE_CAT_SENSOR && strcmp(feature.id, "edgeimpulse") != 0) ||
      strcmp(feature.id, "http") == 0 || strcmp(feature.id, "bluetooth") == 0 ||
      strcmp(feature.id, "led") == 0 || strcmp(feature.id, "llm") == 0)
    return WIZARD_ITEM_STARTUP;
  return WIZARD_ITEM_TOGGLE;
}

static bool wizardInsertItem(SetupWizardPage page, size_t index, const WizardMenuItem& item) {
  const int group = wizardGroupIndex(page);
  if (group < 0) return false;
  size_t& count = wizardRowCounts[group];
  if (count == kWizardRowsPerGroup) {
    broadcastOutput("[Setup] Too many menu items; setup cannot be saved.");
    sWizardDraft.valid = false;
    return false;
  }
  if (index > count) index = count;
  for (size_t i = count; i > index; --i) wizardRows[group][i] = wizardRows[group][i - 1];
  wizardRows[group][index] = item;
  ++count;
  return true;
}

static void wizardInsertAfter(SetupWizardPage page, const char* parentId, const WizardMenuItem& item) {
  for (size_t i = 0; i < getWizardMenuItemCount(page); ++i) {
    if (strcmp(getWizardMenuItem(page, (int)i)->id, parentId) == 0) {
      wizardInsertItem(page, i + 1, item);
      return;
    }
  }
}

void initSetupWizard() {
  initWizardDraft();
  memset(wizardRowCounts, 0, sizeof(wizardRowCounts));

  const auto& draft = getWizardDraftValues();
  const char* curName = draft.deviceName.length() > 0 ? draft.deviceName.c_str()
                      : draft.bleDeviceName.length() > 0 ? draft.bleDeviceName.c_str()
                      : "HardwareOne";
  strncpy(wizardDeviceName, curName, sizeof(wizardDeviceName) - 1);
  wizardDeviceName[sizeof(wizardDeviceName) - 1] = '\0';
  memcpy(sWizardDraft.deviceNameBaseline, wizardDeviceName, sizeof(wizardDeviceName));

  sWizardBaselineKB = 0;
  sWizardBaselineCalibrated = false;
  calibrateWizardBaseline();

  // Registry rows are alphabetical within each group. Companion choices are
  // then inserted beside their parent so related controls remain together.
  for (size_t i = 0; i < getFeatureCount(); ++i) {
    const FeatureEntry* feature = getFeatureByIndex(i);
    if (!feature || !isFeatureCompiled(feature) || !feature->enabledSetting) continue;
    const SetupWizardPage page = wizardFeaturePage(*feature);
    const char* label = strcmp(feature->id, "http") == 0 ? "Web Interface" : feature->name;
    const WizardMenuItem item = {feature->id, label, wizardFeatureItemType(*feature),
                                 wizardBoolDraft(feature->enabledSetting), feature->heapCostKB};
    size_t index = 0;
    while (index < getWizardMenuItemCount(page) &&
           strcasecmp(getWizardMenuItem(page, (int)index)->label, label) <= 0) ++index;
    wizardInsertItem(page, index, item);
  }
#if ENABLE_WIFI
  wizardInsertAfter(WIZARD_PAGE_CONNECTIONS, "wifi",
                    {"wifi_autoconnect", "WiFi auto-connect", WIZARD_ITEM_STARTUP, wizardBoolDraft(&gSettings.wifiAutoStart), 0});
#endif
#if ENABLE_ESPNOW
  wizardInsertAfter(WIZARD_PAGE_CONNECTIONS, "espnow",
                    {"espnow_mesh", "ESP-NOW mesh", WIZARD_ITEM_TOGGLE, wizardBoolDraft(&gSettings.espnowmesh), 0});
#endif
#if ENABLE_MQTT
  wizardInsertAfter(WIZARD_PAGE_CONNECTIONS, "mqtt",
                    {"mqtt_autostart", "MQTT auto-start", WIZARD_ITEM_STARTUP, wizardBoolDraft(&gSettings.mqttAutoStart), 0});
#endif
#if ENABLE_NEOPIXEL
  wizardInsertAfter(WIZARD_PAGE_DISPLAY, "led",
                    {"led_effect", "LED effect", WIZARD_ITEM_LED_EFFECT, nullptr, 0});
#endif
  wizardInsertItem(WIZARD_PAGE_SYSTEM, getWizardMenuItemCount(WIZARD_PAGE_SYSTEM),
                   {"device_name", "Device name", WIZARD_ITEM_DEVICE_NAME, nullptr, 0});
  wizardInsertItem(WIZARD_PAGE_SYSTEM, getWizardMenuItemCount(WIZARD_PAGE_SYSTEM),
                   {"timezone", "Timezone", WIZARD_ITEM_TIMEZONE, nullptr, 0});
#if ENABLE_WIFI
  const FeatureEntry* wifiFeature = getFeatureById("wifi");
  if (wifiFeature && isFeatureCompiled(wifiFeature)) {
    wizardInsertItem(WIZARD_PAGE_SYSTEM, getWizardMenuItemCount(WIZARD_PAGE_SYSTEM),
                     {"ntp", "NTP server", WIZARD_ITEM_NTP, nullptr, 0});
  }
#endif
  wizardInsertItem(WIZARD_PAGE_SYSTEM, getWizardMenuItemCount(WIZARD_PAGE_SYSTEM),
                   {"loglevel", "Log level", WIZARD_ITEM_LOGLEVEL, nullptr, 0});

  currentPage = WIZARD_PAGE_SYSTEM;
  for (const SetupWizardPage page : kPageOrder) {
    if (wizardIsPageVisible(page)) { currentPage = page; break; }
  }
  currentSelection = 0;
  scrollOffset = 0;
  timezoneSelection = 1;
  logLevelSelection = (draft.logLevel >= 0 && draft.logLevel < (int)logLevelCount) ? draft.logLevel : 3;
  ntpSelection = 0;
  ledEffectSelection = 1;
  for (size_t i = 0; i < timezoneCount; ++i) {
    if (timezones[i].offsetMinutes == draft.timezoneOffset) { timezoneSelection = i; break; }
  }
  for (size_t i = 0; i < ntpPresetCount; ++i) {
    if (draft.ntpServer == ntpPresets[i]) { ntpSelection = i; break; }
  }
  for (size_t i = 0; i < ledEffectCount; ++i) {
    if (draft.ledStartupEffect == ledEffects[i]) { ledEffectSelection = i; break; }
  }
}

// ============================================================================
// Page Visibility & Dynamic Navigation
// ============================================================================

bool wizardShouldShowESPNow() {
#if ENABLE_ESPNOW
  const FeatureEntry* f = getFeatureById("espnow");
  return f && wizardFeatureEnabled(f->id);
#else
  return false;
#endif
}

bool wizardShouldShowMQTT() {
#if ENABLE_MQTT
  const FeatureEntry* f = getFeatureById("mqtt");
  const bool* autoStart = wizardBoolDraft(&gSettings.mqttAutoStart);
  return f && wizardFeatureEnabled(f->id) && autoStart && *autoStart;
#else
  return false;
#endif
}

bool wizardStageESPNowSettings(const SetupWizardResult& result) {
  if (!sWizardDraft.active) return false;
  auto& draft = sWizardDraft.values;
  const String& name = result.espnowFriendlyName.length() > 0
      ? result.espnowFriendlyName : draft.deviceName;
  copyWizardString(draft.bleDeviceName, name.length() ? name.c_str() : "HardwareOne");
  copyWizardString(draft.deviceName, draft.bleDeviceName);
  if (result.espnowFriendlyName.length()) copyWizardString(draft.espnowFriendlyName, result.espnowFriendlyName);
  if (result.espnowRoom.length()) copyWizardString(draft.espnowRoom, result.espnowRoom);
  if (result.espnowZone.length()) copyWizardString(draft.espnowZone, result.espnowZone);
  draft.espnowStationary = result.espnowStationary;
  strncpy(wizardDeviceName, draft.deviceName.c_str(), sizeof(wizardDeviceName) - 1);
  wizardDeviceName[sizeof(wizardDeviceName) - 1] = '\0';
  memcpy(sWizardDraft.deviceNameBaseline, wizardDeviceName, sizeof(wizardDeviceName));
  return sWizardDraft.valid;
}

bool wizardStageMQTTSettings(const SetupWizardResult& result) {
  if (!sWizardDraft.active || result.mqttPort < 0 || result.mqttPort > 65535) return false;
  auto& draft = sWizardDraft.values;
  if (result.mqttHost.length()) copyWizardString(draft.mqttHost, result.mqttHost);
  if (result.mqttPort > 0) draft.mqttPort = result.mqttPort;
  if (result.mqttUser.length()) copyWizardString(draft.mqttUser, result.mqttUser);
  if (result.mqttPassword.length()) copyWizardString(draft.mqttPassword, result.mqttPassword);
  return sWizardDraft.valid;
}

// ---- Shared "enable -> pick a mode" table (used by BOTH wizard engines) ----
// Web Interface -> HTTP/HTTPS (bound to httpsEnabled); Bluetooth -> Server/G2
// (bound to bleMode). The blocking FTS wizard and the CLI-mode `featuresetup`
// command both read this same table so their behaviour is identical.
// Per-mode extraHeapKB: estimated RAM the mode adds over the feature's base.
// HTTPS +20KB (mbedTLS session buffers, ~20KB each per HEAP_OPTIMIZATION_FINDINGS);
// G2 +10KB (glasses protocol state + persistent heartbeat worker over a plain
// BLE server). Baseline modes (HTTP, Server) add 0.
#if ENABLE_HTTP_SERVER && ENABLE_HTTPS
static const WizardModeOption kWebModes[] = { {"HTTP", 0, 0}, {"HTTPS", 1, 20} };
#endif
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
static const WizardModeOption kBtModes[]  = { {"Server (phone)", 0, 0}, {"G2 Glasses", 1, 10} };
#endif

static const WizardModeMenu kModeMenus[] = {
#if ENABLE_HTTP_SERVER && ENABLE_HTTPS
  { "http",      "Web Mode",       kWebModes, 2, &sWizardDraft.values.httpsEnabled, nullptr },
#endif
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
  { "bluetooth", "Bluetooth Mode", kBtModes,  2, nullptr, &sWizardDraft.values.bleMode },
#endif
};
static const size_t kModeMenuCount = sizeof(kModeMenus) / sizeof(kModeMenus[0]);

const WizardModeMenu* wizardModeMenuForFeature(const char* id) {
  if (!id) return nullptr;
  for (size_t i = 0; i < kModeMenuCount; i++) {
    if (strcmp(kModeMenus[i].featureId, id) == 0) return &kModeMenus[i];
  }
  return nullptr;
}

const WizardModeMenu* wizardModeMenuForPage(SetupWizardPage page) {
  if (page == WIZARD_PAGE_WEBMODE) return wizardModeMenuForFeature("http");
  if (page == WIZARD_PAGE_BTMODE)  return wizardModeMenuForFeature("bluetooth");
  return nullptr;
}

int wizardModeCurrentIndex(const WizardModeMenu* m) {
  if (!m) return 0;
  int cur = m->boolSetting ? (*m->boolSetting ? 1 : 0) : *m->intSetting;
  for (int i = 0; i < m->modeCount; i++) {
    if (m->modes[i].value == cur) return i;
  }
  return 0;
}

void wizardModeApply(const WizardModeMenu* m, int idx) {
  if (!m || idx < 0 || idx >= m->modeCount) return;
  int v = m->modes[idx].value;
  if (m->boolSetting)     *m->boolSetting = (v != 0);
  else if (m->intSetting) *m->intSetting  = v;
}

// Sum the extraHeapKB of the currently-selected mode for every enabled feature
// that has a mode menu (HTTPS over HTTP, G2 over BLE server). Feature-granular
// getEnabledFeaturesHeapEstimate() can't see this, so the wizard heap bar folds
// it in to reflect the mode choice. Returns 0 when no moded feature is enabled.
uint32_t wizardEnabledModeExtraHeapKB() {
  uint32_t extra = 0;
  for (size_t i = 0; i < kModeMenuCount; i++) {
    const WizardModeMenu* m = &kModeMenus[i];
    const FeatureEntry* f = getFeatureById(m->featureId);
    if (!f || !wizardFeatureEnabled(f->id)) continue;
    extra += m->modes[wizardModeCurrentIndex(m)].extraHeapKB;
  }
  return extra;
}

// The mode-picker pages are visible whenever their feature is enabled — on
// every build, OLED or headless. Both engines dispatch WEBMODE/BTMODE through
// the same OLED-if-connected-else-serial split as ESP-NOW/MQTT/WiFi (see
// runSetupWizard), so there's always a working handler regardless of whether
// a display is compiled in or physically present. Both engines navigate via
// wizardAdvanceFrom, which respects this — no engine-specific flag needed.
bool wizardShouldShowWebMode() {
#if ENABLE_HTTP_SERVER && ENABLE_HTTPS
  return wizardFeatureEnabled("http") && wizardModeMenuForFeature("http") != nullptr;
#else
  return false;
#endif
}

bool wizardShouldShowBtMode() {
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
  return wizardFeatureEnabled("bluetooth") && wizardModeMenuForFeature("bluetooth") != nullptr;
#else
  return false;
#endif
}

bool wizardIsPageVisible(SetupWizardPage page) {
  if (wizardIsGroupPage(page)) return getWizardMenuItemCount(page) > 0;
  switch (page) {
    case WIZARD_PAGE_WEBMODE:
      return wizardShouldShowWebMode();
    case WIZARD_PAGE_BTMODE:
      return wizardShouldShowBtMode();
    case WIZARD_PAGE_ESPNOW:
      return wizardShouldShowESPNow();
    case WIZARD_PAGE_MQTT:
      return wizardShouldShowMQTT();
    case WIZARD_PAGE_WIFI:
      return wizardShouldShowWiFi();
    default:
      return false;
  }
}

SetupWizardPage wizardAdvanceFrom(SetupWizardPage current) {
  bool found = false;
  for (size_t i = 0; i < kPageOrderCount; i++) {
    if (kPageOrder[i] == current) { found = true; continue; }
    if (found && wizardIsPageVisible(kPageOrder[i])) return kPageOrder[i];
  }
  return WIZARD_PAGE_COUNT; // no next page — signals completion
}

SetupWizardPage wizardRetreatFrom(SetupWizardPage current) {
  // Walk backward through kPageOrder
  bool found = false;
  for (int i = (int)kPageOrderCount - 1; i >= 0; i--) {
    if (kPageOrder[i] == current) { found = true; continue; }
    if (found && wizardIsPageVisible(kPageOrder[i])) return kPageOrder[i];
  }
  return current; // already at first page
}

int getWizardTotalPages() {
  int count = 0;
  for (size_t i = 0; i < kPageOrderCount; i++) {
    if (wizardIsPageVisible(kPageOrder[i])) count++;
  }
  return count;
}

int getWizardPageNumber(SetupWizardPage page) {
  int num = 0;
  for (size_t i = 0; i < kPageOrderCount; i++) {
    if (wizardIsPageVisible(kPageOrder[i])) {
      num++;
      if (kPageOrder[i] == page) return num;
    }
  }
  return 1; // fallback
}

// ============================================================================
// Wizard Actions
// ============================================================================

bool wizardActivateCurrentItem() {
  if (!sWizardDraft.active) return false;
  const WizardMenuItem* item = getWizardMenuItem(currentPage, currentSelection);
  if (!item) return false;
  switch (item->type) {
    case WIZARD_ITEM_TOGGLE:
    case WIZARD_ITEM_STARTUP: {
      const FeatureEntry* feature = getFeatureById(item->id);
      if (feature && (feature->flags & FEATURE_FLAG_ESSENTIAL)) return false;
      if (!item->setting) return false;
      *item->setting = !*item->setting;
      return true;
    }
    case WIZARD_ITEM_TIMEZONE:
      setWizardTimezoneSelection((timezoneSelection + 1) % timezoneCount);
      return true;
    case WIZARD_ITEM_LOGLEVEL:
      setWizardLogLevelSelection((logLevelSelection + 1) % logLevelCount);
      return true;
    case WIZARD_ITEM_NTP:
      setWizardNTPSelection((ntpSelection + 1) % ntpPresetCount);
      return true;
    case WIZARD_ITEM_LED_EFFECT:
      setWizardLEDEffectSelection((ledEffectSelection + 1) % ledEffectCount);
      return true;
    case WIZARD_ITEM_DEVICE_NAME:
      return false; // Adapter opens text input using getWizardDeviceNameBuf().
  }
  return false;
}

bool wizardMoveUp() {
  if (currentSelection > 0 && currentSelection < (int)getWizardMenuItemCount(currentPage)) {
    --currentSelection;
    if (currentSelection < scrollOffset) scrollOffset = currentSelection;
    return true;
  }
  return false;
}

bool wizardMoveDown() {
  if (currentSelection >= 0 && currentSelection < (int)getWizardMenuItemCount(currentPage) - 1) {
    ++currentSelection;
    if (currentSelection >= scrollOffset + 4) scrollOffset = currentSelection - 3;
    return true;
  }
  return false;
}

bool wizardIsFirstPage() { return wizardRetreatFrom(currentPage) == currentPage; }

bool wizardShouldShowWiFi() {
#if ENABLE_WIFI
  const FeatureEntry* wifiFeature = getFeatureById("wifi");
  return wifiFeature && wizardFeatureEnabled(wifiFeature->id);
#else
  return false;
#endif
}

static void wizardCollectDeviceName() {
  auto& draft = sWizardDraft.values;
  if (wizardDeviceName[0] &&
      strcmp(wizardDeviceName, sWizardDraft.deviceNameBaseline) != 0) {
    copyWizardString(draft.deviceName, wizardDeviceName);
    copyWizardString(draft.bleDeviceName, draft.deviceName);
    memcpy(sWizardDraft.deviceNameBaseline, wizardDeviceName, sizeof(wizardDeviceName));
  }
}

static void wizardCollectSystemChoices(SetupWizardResult& result) {
  wizardCollectDeviceName();
  const auto& draft = sWizardDraft.values;
  result.timezoneOffset = draft.timezoneOffset;
  result.timezoneAbbrev = "";
  for (const auto& timezone : timezones) {
    if (timezone.offsetMinutes == draft.timezoneOffset) {
      copyWizardString(result.timezoneAbbrev, timezone.abbrev);
      break;
    }
  }
}

bool wizardNextPage(SetupWizardResult& result) {
  if (!sWizardDraft.active) { result.completed = false; return false; }
  if (currentPage == WIZARD_PAGE_SYSTEM) wizardCollectSystemChoices(result);
  result.wifiEnabled = wizardShouldShowWiFi();

  const SetupWizardPage next = wizardAdvanceFrom(currentPage);
  if (next == WIZARD_PAGE_COUNT) {
    result.completed = sWizardDraft.valid;
    return false;
  }
  result.completed = false;
  currentPage = next;
  currentSelection = 0;
  scrollOffset = 0;
  return true;
}

bool wizardPrevPage() {
  if (currentPage == WIZARD_PAGE_SYSTEM) wizardCollectDeviceName();
  SetupWizardPage prev = wizardRetreatFrom(currentPage);
  if (prev == currentPage) return false; // already at first page

  currentPage = prev;
  currentSelection = 0;
  scrollOffset = 0;
  return true;
}

// ============================================================================
// Serial Console Wizard
// ============================================================================

// Both wizard engines print the same titles, rows, values and navigation.
void printSerialPageStatus() {
  const SetupWizardPage page = getWizardCurrentPage();
  const int selection = getWizardCurrentSelection();
  char line[160];
  broadcastOutput("");
  broadcastOutput("========================================");
  snprintf(line, sizeof(line), "  SETUP %d/%d: %s", getWizardPageNumber(page), getWizardTotalPages(), getWizardPageTitle(page));
  broadcastOutput(line);
  for (size_t i = 0; i < getWizardMenuItemCount(page); ++i) {
    char label[48], value[80];
    if (wizardFormatMenuItem(page, (int)i, label, sizeof(label), value, sizeof(value))) {
      snprintf(line, sizeof(line), " %s%zu. %s: %s", (int)i == selection ? ">" : " ", i + 1, label, value);
      broadcastOutput(line);
    }
  }
  broadcastOutput("----------------------------------------");
  snprintf(line, sizeof(line), "Serial: # to edit, 'n' %s, 'b' %s, 'cancel' discard",
           wizardAdvanceFrom(page) == WIZARD_PAGE_COUNT ? "finish" : "next",
           wizardIsFirstPage() ? "cancel" : "back");
  broadcastOutput(line);
#if ENABLE_OLED_DISPLAY
  snprintf(line, sizeof(line), "OLED:   Joystick + A=Edit, Right=%s",
           wizardAdvanceFrom(page) == WIZARD_PAGE_COUNT ? "Finish" : "Next");
  broadcastOutput(line);
#endif
  broadcastOutput("> ");
}

bool wizardHandleCancelInput(const String& input) {
  noteWizardInputActivity();
  String command = input;
  command.trim();
  if (!command.equalsIgnoreCase("cancel")) return false;
  requestWizardCancel();
  return true;
}

static String waitForWizardInput() {
  const String input = waitForSerialInputBlocking();
  if (isWizardCancelRequested() || wizardHandleCancelInput(input)) return String();
  return input;
}

// Serial ESP-NOW identity page
static void handleSerialESPNowPage(SetupWizardResult& result, bool& running) {
  result.espnowFriendlyName = "";
  result.espnowRoom = "";
  result.espnowZone = "";
  result.espnowStationary = getWizardDraftValues().espnowStationary;
  int pageNum = getWizardPageNumber(WIZARD_PAGE_ESPNOW);
  int totalPages = getWizardTotalPages();

  Serial.println();
  Serial.printf("=== ESP-NOW Identity (SETUP %d/%d) ===\n", pageNum, totalPages);
  Serial.println("Assign an optional identity for this device in the ESP-NOW mesh.");
  Serial.println("----------------------------------------");
  Serial.println(" c = Configure (enter fields)");
  Serial.println(" n = Next (skip)");
  Serial.println(" b = Back");
  Serial.print("Choice: ");
  String introChoice = waitForWizardInput();
  introChoice.trim();
  String deviceName;
  if (introChoice.equalsIgnoreCase("b") || introChoice.equalsIgnoreCase("back")) {
    wizardPrevPage();
    return;
  }
  if (introChoice.equalsIgnoreCase("n") || introChoice.equalsIgnoreCase("next") || introChoice.length() == 0) {
    // Explicit skip (or blank/Enter) — disable ESP-NOW since it's unconfigured;
    // it can't start without a name.
    wizardSetFeatureEnabled("espnow", false);
    if (!wizardNextPage(result)) running = false;
    return;
  }

  Serial.println("----------------------------------------");
  Serial.println("All fields optional. Enter to skip field, 'n' to finish, 'b' to go back.");
  Serial.println("----------------------------------------");

  String currentName = getWizardDraftValues().deviceName.length() > 0 ? getWizardDraftValues().deviceName : "HardwareOne";
  if (introChoice.equalsIgnoreCase("c") || introChoice.equalsIgnoreCase("configure")) {
    Serial.printf("Device Name (for Bluetooth + ESP-NOW) [%s]: ", currentName.c_str());
    deviceName = waitForWizardInput();
    deviceName.trim();
    if (deviceName.equalsIgnoreCase("b") || deviceName.equalsIgnoreCase("back")) {
      wizardPrevPage(); return;
    }
    if (deviceName.equalsIgnoreCase("n")) {
      result.espnowFriendlyName = "";  // Early finish keeps the current name, not an earlier draft.
      goto espnow_done;
    }
    if (deviceName.length() == 0) deviceName = currentName;
  } else {
    // Not b/n/c — the user almost certainly typed their device name directly
    // at the Choice: prompt instead of answering the c/n/b menu. Treat it as
    // the name and continue into the configure flow rather than silently
    // skipping (which used to also disable ESP-NOW) on what's very likely a
    // menu mix-up, not an intentional skip.
    deviceName = introChoice;
    Serial.printf("Device Name (for Bluetooth + ESP-NOW): %s\n", deviceName.c_str());
  }
  result.espnowFriendlyName = deviceName;

  { Serial.print("Room (e.g. 'Living Room'): ");
  String room = waitForWizardInput();
  room.trim();
  if (room.equalsIgnoreCase("b") || room.equalsIgnoreCase("back")) {
    wizardPrevPage(); return;
  }
  if (room.equalsIgnoreCase("n")) goto espnow_done;
  result.espnowRoom = room; }

  { Serial.print("Zone (e.g. 'North Wall'): ");
  String zone = waitForWizardInput();
  zone.trim();
  if (zone.equalsIgnoreCase("b") || zone.equalsIgnoreCase("back")) {
    wizardPrevPage(); return;
  }
  if (zone.equalsIgnoreCase("n")) goto espnow_done;
  result.espnowZone = zone; }

  { Serial.print("The device will be — (m)obile or (s)tationary [Enter keeps current]: ");
  String stat = waitForWizardInput();
  stat.trim();
  if (stat.equalsIgnoreCase("b") || stat.equalsIgnoreCase("back")) {
    wizardPrevPage(); return;
  }
  if (stat.length()) result.espnowStationary = (stat.equalsIgnoreCase("s") || stat.equalsIgnoreCase("stationary")); }

espnow_done:
  // If the wizard timed out partway through this page (waitForSerialInputBlocking
  // returns empty + sets the cancel flag), don't write any of the fields the
  // user didn't actually confirm. The outer loop will pick up the cancel flag
  // next iteration and abort the whole wizard.
  if (isWizardCancelRequested()) {
    running = false;
    return;
  }

  if (!wizardStageESPNowSettings(result)) {
    Serial.println("Error: could not stage ESP-NOW choices.");
    return;
  }

  Serial.println("ESP-NOW identity configured.");

  // Advance to next page
  if (!wizardNextPage(result)) {
    running = false;
  }
}

// Serial MQTT broker page
static void handleSerialMQTTPage(SetupWizardResult& result, bool& running) {
  result.mqttHost = "";
  result.mqttUser = "";
  result.mqttPassword = "";
  result.mqttPort = 0;
  int pageNum = getWizardPageNumber(WIZARD_PAGE_MQTT);
  int totalPages = getWizardTotalPages();

  Serial.println();
  Serial.printf("=== MQTT Broker (SETUP %d/%d) ===\n", pageNum, totalPages);
  Serial.println("Configure a MQTT broker for this device to publish data.");
  Serial.println("----------------------------------------");
  Serial.println(" c = Configure (enter broker details)");
  Serial.println(" n = Next (skip)");
  Serial.println(" b = Back");
  Serial.print("Choice: ");
  String introChoice = waitForWizardInput();
  introChoice.trim();
  if (introChoice.equalsIgnoreCase("b") || introChoice.equalsIgnoreCase("back")) {
    wizardPrevPage();
    return;
  }
  if (!introChoice.equalsIgnoreCase("c") && !introChoice.equalsIgnoreCase("configure")) {
    // Skip — disable MQTT auto-start since it's unconfigured
    wizardSetMQTTAutoStart(false);
    if (!wizardNextPage(result)) running = false;
    return;
  }

  Serial.println("----------------------------------------");
  Serial.println("Press Enter to use defaults, 'b' to go back.");
  Serial.println("----------------------------------------");

  Serial.printf("Host (default: %s): ", getWizardDraftValues().mqttHost.length() > 0 ? getWizardDraftValues().mqttHost.c_str() : "none");
  String host = waitForWizardInput();
  host.trim();
  if (host.equalsIgnoreCase("b") || host.equalsIgnoreCase("back")) {
    wizardPrevPage();
    return;
  }
  result.mqttHost = host;

  Serial.printf("Port (current: %d; Enter keeps it): ", getWizardDraftValues().mqttPort);
  String portStr = waitForWizardInput();
  portStr.trim();
  if (portStr.equalsIgnoreCase("b") || portStr.equalsIgnoreCase("back")) {
    wizardPrevPage();
    return;
  }
  if (!wizardParseMQTTPort(portStr, result.mqttPort)) {
    Serial.println("Error: MQTT port must be a decimal number from 1 to 65535.");
    return;
  }

  Serial.print("Username (blank = keep current): ");
  String user = waitForWizardInput();
  user.trim();
  if (user.equalsIgnoreCase("b") || user.equalsIgnoreCase("back")) {
    wizardPrevPage();
    return;
  }
  result.mqttUser = user;

  if (user.length() > 0) {
    Serial.print("Password: ");
    String pass = waitForWizardInput();
    pass.trim();
    if (pass.equalsIgnoreCase("b") || pass.equalsIgnoreCase("back")) {
      wizardPrevPage();
      return;
    }
    result.mqttPassword = pass;
  }

  // Mirror the ESP-NOW page: if we timed out partway, don't half-apply
  // settings — let the outer loop catch the cancel flag and abort.
  if (isWizardCancelRequested()) {
    running = false;
    return;
  }

  if (!wizardStageMQTTSettings(result)) {
    Serial.println("Error: could not stage MQTT choices.");
    return;
  }

  Serial.println("MQTT broker configured.");

  // Advance to next page
  if (!wizardNextPage(result)) {
    running = false;
  }
}

#if ENABLE_WIFI
// Shared WiFi scan-and-list printer — see System_SetupWizard.h for contract.
// Numbers NAMED networks only; hidden (empty-SSID) APs are collapsed into a
// single "(+H hidden ...)" line so a numbered pick can never yield a blank
// SSID. Only the copied SSID cache survives wifiScanForEach(); Arduino's
// global result block is released before the function returns.
static constexpr int kWizardNamedWifiCapacity = 24;
EXT_RAM_BSS_ATTR static char sWizardNamedWifi[kWizardNamedWifiCapacity][33];
static int sWizardNamedWifiCount = 0;

struct WizardWifiScanContext {
  int cap;
  int storedNamed;
  int totalNamed;
  int hiddenCount;
  int8_t rssi[kWizardNamedWifiCapacity];
  uint8_t authMode[kWizardNamedWifiCapacity];
};

static bool collectWizardWifiRecord(const WifiScanRecord& record,
                                    uint16_t /*index*/, uint16_t /*total*/,
                                    void* context) {
  WizardWifiScanContext* scan = static_cast<WizardWifiScanContext*>(context);
  if (record.ssid[0] == '\0') {
    scan->hiddenCount++;
    return true;
  }

  scan->totalNamed++;
  if (scan->storedNamed >= scan->cap) return true;

  int slot = scan->storedNamed++;
  strlcpy(sWizardNamedWifi[slot], record.ssid,
          sizeof(sWizardNamedWifi[slot]));
  scan->rssi[slot] = record.rssi;
  scan->authMode[slot] = record.authMode;
  return true;
}

void wifiScanClearNamed() {
  memset(sWizardNamedWifi, 0, sizeof(sWizardNamedWifi));
  sWizardNamedWifiCount = 0;
}

bool wifiScanGetNamedSSID(int zeroBasedIndex, String& outSSID) {
  outSSID = "";
  if (zeroBasedIndex < 0 || zeroBasedIndex >= sWizardNamedWifiCount) {
    return false;
  }
  outSSID = sWizardNamedWifi[zeroBasedIndex];
  return outSSID.length() > 0;
}

int wifiScanPrintNamed(int namedCap) {
  wifiScanClearNamed();
  if (namedCap < 0) namedCap = 0;
  if (namedCap > kWizardNamedWifiCapacity) {
    namedCap = kWizardNamedWifiCapacity;
  }

  WizardWifiScanContext scan = {};
  scan.cap = namedCap;
  WifiScanResult result = wifiScanForEach(
      /*includeHidden=*/true, collectWizardWifiRecord, &scan);
  if (!result.ok()) {
    broadcastOutput(String("WiFi scan unavailable: ") +
                    wifiScanStatusText(result.status));
    return 0;
  }

  sWizardNamedWifiCount = scan.storedNamed;
  if (scan.storedNamed > 0) {
    broadcastOutput(String("Found ") + scan.totalNamed + " network(s):");
    for (int j = 0; j < scan.storedNamed; j++) {
      char line[96];
      snprintf(line, sizeof(line), "  %d. %-24.32s  %lddBm  %s",
               j + 1, sWizardNamedWifi[j], (long)scan.rssi[j],
               scan.authMode[j] == 0 ? "Open" : "Secured");
      broadcastOutput(line);
    }
    if (scan.totalNamed > scan.storedNamed) {
      broadcastOutput(String("  (... and ") +
                      (scan.totalNamed - scan.storedNamed) +
                      " more named — type the exact SSID to join)");
    }
  } else {
    broadcastOutput("No named WiFi networks found.");
  }
  if (scan.hiddenCount > 0) {
    broadcastOutput(String("  (+") + scan.hiddenCount + " hidden network" +
                    (scan.hiddenCount == 1 ? "" : "s") +
                    " — type the exact SSID to join)");
  }
  return scan.storedNamed;
}
#endif

// Serial WiFi page helper - full scan + numbered list
static void handleSerialWiFiPage(SetupWizardResult& result, bool& running) {
  int pageNum = getWizardPageNumber(WIZARD_PAGE_WIFI);
  int totalPages = getWizardTotalPages();
  Serial.println();
  Serial.printf("=== WiFi Setup (SETUP %d/%d) ===\n", pageNum, totalPages);
  bool wifiPageDone = false;
  while (!wifiPageDone) {
#if ENABLE_WIFI
    int namedCount = wifiScanPrintNamed(24);
    Serial.println("----------------------------------------");
    Serial.println("Enter number, SSID directly, 'rescan', or 'skip':");
    Serial.println("('b' or 'back' to return to previous page)");
    Serial.print("> ");
    String ssidInput = waitForWizardInput();
    // Honor wizard-input timeout (CLI featuresetup installs one; FTS doesn't).
    if (isWizardCancelRequested()) {
      wifiScanClearNamed();
      result.completed = false;
      running = false;
      return;
    }
    if (ssidInput.equalsIgnoreCase("b") || ssidInput.equalsIgnoreCase("back")) {
      wifiScanClearNamed();
      wizardPrevPage();
      wifiPageDone = true;
      continue;
    }
    if (ssidInput.equalsIgnoreCase("rescan")) {
      wifiScanClearNamed();
      continue;
    }
    if (ssidInput.equalsIgnoreCase("skip") || ssidInput.length() == 0) {
      wifiScanClearNamed();
      result.wifiConfigured = false;
      result.wifiSSID = "";
      result.wifiPassword = "";
      if (!wizardNextPage(result)) running = false;
      wifiPageDone = true;
      continue;
    }
    String ssid = ssidInput;
    int idx = ssidInput.toInt();
    // Map a bare in-range integer to the Nth NAMED network; anything else
    // (incl. SSIDs that start with a digit, e.g. "2WIRE") is a typed SSID.
    if (idx >= 1 && idx <= namedCount && String(idx) == ssidInput) {
      if (!wifiScanGetNamedSSID(idx - 1, ssid)) {
        wifiScanClearNamed();
        Serial.println("WiFi list expired; scanning again.");
        continue;
      }
    }
    wifiScanClearNamed();
    if (ssid.length() > 0) {
      Serial.println("Enter WiFi password (or 'b' to go back):");
      Serial.print("> ");
      String pass = waitForWizardInput();
      if (isWizardCancelRequested()) {
        result.completed = false;
        running = false;
        return;
      }
      if (pass.equalsIgnoreCase("b") || pass.equalsIgnoreCase("back")) {
        continue;
      }
      result.wifiSSID = ssid;
      result.wifiPassword = pass;
      result.wifiConfigured = true;
    }
    if (!wizardNextPage(result)) running = false;
    wifiPageDone = true;
#else
    Serial.println("WiFi not compiled in this build.");
    if (!wizardNextPage(result)) running = false;
    wifiPageDone = true;
#endif
  }
}

// Serial Web/Bluetooth mode picker (HTTP vs HTTPS, Server vs G2 Bluetooth).
// Mirrors handleModePage()'s serial branch (OLED_SetupWizard.cpp) so headless
// (ENABLE_OLED_DISPLAY=0) builds and OLED builds with no display physically
// connected get the same choice — the OLED is an optional accessory, not a
// requirement for full first-time setup (matches the ESPNOW/MQTT/WiFi split
// above: one always-compiled serial handler, one optional richer OLED one).
static void handleSerialModePage(SetupWizardPage page, SetupWizardResult& result, bool& running) {
  const WizardModeMenu* sub = wizardModeMenuForPage(page);
  if (!sub) { if (!wizardNextPage(result)) running = false; return; }  // nothing to pick — skip

  int sel = wizardModeCurrentIndex(sub);
  int lastPrinted = -1;

  while (true) {
    if (isWizardCancelRequested()) { result.completed = false; running = false; return; }
    if (sel != lastPrinted) {
      Serial.println();
      Serial.printf("=== SETUP %d/%d: %s ===\n", getWizardPageNumber(page), getWizardTotalPages(), sub->title);
      for (int i = 0; i < sub->modeCount; i++) {
        Serial.printf(" %s%d. [%s] %s\n", sel == i ? ">" : " ", i + 1,
                      sel == i ? "X" : " ", sub->modes[i].label);
      }
      Serial.println("----------------------------------------");
      Serial.println("Serial: # to select, 'n' next, 'b' back, 'cancel' discard");
      Serial.print("> ");
      lastPrinted = sel;
    }

    String input = waitForWizardInput();
    input.trim();
    if (input.equalsIgnoreCase("b") || input.equalsIgnoreCase("back")) { wizardPrevPage(); return; }
    if (input.equalsIgnoreCase("n") || input.equalsIgnoreCase("next")) {
      wizardModeApply(sub, sel);
      if (!wizardNextPage(result)) running = false;
      return;
    }
    int n = input.toInt();
    if (n >= 1 && n <= sub->modeCount) {
      sel = n - 1;
      wizardModeApply(sub, sel);
      lastPrinted = -1;  // force re-render with the new selection
    }
  }
}

// ============================================================================
// THE unified wizard - single implementation for all builds
// Serial is always active. When ENABLE_OLED_DISPLAY=1 and display is connected,
// OLED rendering + joystick input are layered on top automatically.
// ============================================================================

SetupWizardResult runSetupWizard() {
  SetupWizardResult result{};
  result.completed = false;
  result.wifiEnabled = false;
  result.wifiConfigured = false;
  result.wifiSSID = "";
  result.wifiPassword = "";
  result.deviceName = "HardwareOne";
  result.timezoneOffset = -240;
  result.timezoneAbbrev = "EDT";
  result.espnowStationary = false;
  result.mqttPort = 0;

  initSetupWizard();

#if ENABLE_OLED_DISPLAY
  resetWizardJoystickState();
  uint32_t lastButtons = 0;
  bool lastButtonsInitialized = false;
#endif

  Serial.println();
  Serial.println("========================================");
  Serial.println("       FEATURE CONFIGURATION WIZARD    ");
  Serial.println("========================================");
  Serial.println("Configure which features to enable.");
  Serial.println();

  SetupWizardPage lastPrintedPage = WIZARD_PAGE_COUNT;
  int lastPrintedSel = -1;
  bool running = true;

  while (running) {
    // Idle-timeout escape hatch: when running under cmd_featuresetup the
    // caller installs a timeout (see runAndApplyFeatureWizard). If the
    // user didn't deliver any input within that window,
    // waitForSerialInputBlocking flips the cancel flag and we bail here
    // without saving anything. FTS at boot uses timeout=0 so this branch
    // never fires for fresh-device setup.
    if (isWizardCancelRequested()) {
      result.completed = false;
      running = false;
      break;
    }

    SetupWizardPage currentPage = getWizardCurrentPage();
    int currentSel = getWizardCurrentSelection();

    // ------------------------------------------------------------------
    // 1. Pages with their own input loop (ESP-NOW, MQTT, WiFi)
    // ------------------------------------------------------------------
    // Web Interface / Bluetooth mode picker — conditional page handled as a
    // self-contained blocking sub-flow, same split as ESP-NOW/MQTT/WiFi below:
    // the OLED-rich version only runs when a display is actually connected,
    // otherwise the always-compiled serial handler covers it.
    if (currentPage == WIZARD_PAGE_WEBMODE || currentPage == WIZARD_PAGE_BTMODE) {
#if ENABLE_OLED_DISPLAY
      if (oledDisplay && oledConnected) {
        handleModePage(currentPage, result, running);
        lastPrintedPage = WIZARD_PAGE_COUNT;
        continue;
      }
#endif
      handleSerialModePage(currentPage, result, running);
      lastPrintedPage = WIZARD_PAGE_COUNT;
      continue;
    }

    if (currentPage == WIZARD_PAGE_ESPNOW) {
#if ENABLE_OLED_DISPLAY
      if (oledDisplay && oledConnected) {
        handleOLEDESPNowPage(result, running);
        lastPrintedPage = WIZARD_PAGE_COUNT;
        continue;
      }
#endif
      handleSerialESPNowPage(result, running);
      lastPrintedPage = WIZARD_PAGE_COUNT;
      continue;
    }

    if (currentPage == WIZARD_PAGE_MQTT) {
#if ENABLE_OLED_DISPLAY
      if (oledDisplay && oledConnected) {
        handleOLEDMQTTPage(result, running);
        lastPrintedPage = WIZARD_PAGE_COUNT;
        continue;
      }
#endif
      handleSerialMQTTPage(result, running);
      lastPrintedPage = WIZARD_PAGE_COUNT;
      continue;
    }

    if (currentPage == WIZARD_PAGE_WIFI) {
#if ENABLE_OLED_DISPLAY
      if (oledDisplay && oledConnected) {
        renderWiFiPage(result); // Accept/Skip/Back all use shared page navigation.
        if (result.completed || isWizardCancelRequested()) running = false;
        lastPrintedPage = WIZARD_PAGE_COUNT;  // Force reprint on return
        continue;
      }
#endif
      handleSerialWiFiPage(result, running);
      lastPrintedPage = WIZARD_PAGE_COUNT;
      continue;
    }

    // ------------------------------------------------------------------
    // 2. OLED rendering for ordinary groups (when display connected)
    // ------------------------------------------------------------------
#if ENABLE_OLED_DISPLAY
    if (oledDisplay && oledConnected) {
      if (wizardIsGroupPage(currentPage)) renderWizardPage();
      else running = false;
    }
#endif

    if (!running) break;

    // ------------------------------------------------------------------
    // 3. Serial: print page state when it changes
    // ------------------------------------------------------------------
    if (currentPage != lastPrintedPage || currentSel != lastPrintedSel) {
      printSerialPageStatus();
      lastPrintedPage = currentPage;
      lastPrintedSel = currentSel;
    }

    delay(50);

    // ------------------------------------------------------------------
    // 4. Serial input (n/b/number) - works in all builds
    // ------------------------------------------------------------------
    bool serialHandled = false;
    if (Serial.available()) {
      String input = Serial.readStringUntil('\n');
      if (wizardHandleCancelInput(input)) { result.completed = false; break; }
      input.trim();
      input.toLowerCase();

      if (input == "n" || input == "next") {
        if (!wizardNextPage(result)) running = false;
        serialHandled = true;
      } else if (input == "b" || input == "back") {
        if (!wizardPrevPage()) requestWizardCancel();
        serialHandled = true;
      } else if (input.length() > 0) {
        const long number = input.toInt();
        if (number > 0 && number <= (long)getWizardMenuItemCount(currentPage) && String(number) == input) {
          const WizardMenuItem* item = getWizardMenuItem(currentPage, (int)number - 1);
          setWizardCurrentSelection((int)number - 1);
          if (item->type == WIZARD_ITEM_DEVICE_NAME) {
            Serial.printf("Device Name [%s]: ", wizardDeviceName);
            String newName = waitForWizardInput();
            newName.trim();
            if (!isWizardCancelRequested() && newName.length() > 0) {
              strncpy(wizardDeviceName, newName.c_str(), sizeof(wizardDeviceName) - 1);
              wizardDeviceName[sizeof(wizardDeviceName) - 1] = '\0';
            }
          } else {
            wizardActivateCurrentItem();
          }
          serialHandled = true;
        }
      }

      if (serialHandled) {
        lastPrintedSel = -1;  // Force reprint after change
        continue;
      }
    }

    // ------------------------------------------------------------------
    // 5. Joystick/button input (OLED builds only, when display connected)
    // ------------------------------------------------------------------
#if ENABLE_OLED_DISPLAY
    if (oledDisplay && oledConnected) {
      const uint32_t newButtons = inputConsumeButtonPresses(lastButtons, lastButtonsInitialized);
      if (newButtons) noteWizardInputActivity();

      JoystickNav nav = readWizardJoystickNav();
      bool hasInput = (newButtons != 0) || nav.up || nav.down || nav.left || nav.right;
      if (!hasInput) continue;

      const bool handled = handleWizardPageInput(newButtons, nav, result);
      if (result.completed || isWizardCancelRequested()) running = false;

      if (handled) {
        lastPrintedSel = -1;
        delay(150);
      }
    }
#endif
  }

  if (!result.completed || isWizardCancelRequested()) { result = {}; discardSetupWizard(); }
  return result;
}

bool wizardSaveAndApplySettings(SetupWizardResult& result) {
  if (!result.completed || isWizardCancelRequested()) {
    result = {};
    discardSetupWizard();
    return false;
  }
  if (!sWizardDraft.active) { result = {}; return false; }
  wizardCollectSystemChoices(result);
  const auto& draft = sWizardDraft.values;
  const auto& initial = sWizardDraft.initial;
  const bool invalid = !sWizardDraft.valid ||
      (draft.logLevel != initial.logLevel && (draft.logLevel < 0 || draft.logLevel >= (int)logLevelCount)) ||
      (draft.bleMode != initial.bleMode && (draft.bleMode < 0 || draft.bleMode > 1)) ||
      (draft.mqttPort != initial.mqttPort && (draft.mqttPort < 1 || draft.mqttPort > 65535));
  // Reserve every edited destination before publishing any value. A failed
  // String copy must not turn Apply into a partial configuration change.
  bool ready = !invalid;
  for (const auto& binding : kWizardStringBindings) {
    if (draft.*binding.field != initial.*binding.field &&
        !binding.live->reserve((draft.*binding.field).length())) ready = false;
  }
  if (!ready) {
    broadcastOutput("Error: setup choices could not be applied. No changes applied.");
    result = {};
    discardSetupWizard();
    return false;
  }

  // Short synchronous scope, never held while waiting for user input. It is
  // private to this task and leaves any caller-owned batch intact.
  SettingsWriteScope writes;
  if (!writes.active()) {
    broadcastOutput("Error: setup could not start its settings save. No changes applied.");
    result = {};
    discardSetupWizard();
    return false;
  }
  for (size_t i = 0; i < sWizardDraft.boolCount; ++i) {
    const auto& choice = sWizardDraft.booleans[i];
    if (choice.value != choice.initial) setSetting(*choice.live, choice.value);
  }
  for (const auto& binding : kWizardStringBindings) {
    if (draft.*binding.field != initial.*binding.field) setSetting(*binding.live, draft.*binding.field);
  }
  if (draft.timezoneOffset != initial.timezoneOffset) setSetting(gSettings.tzOffsetMinutes, draft.timezoneOffset);
  if (draft.logLevel != initial.logLevel) setDebugSetting(gSettings.logLevel, draft.logLevel);
  if (draft.httpsEnabled != initial.httpsEnabled) setSetting(gSettings.httpsEnabled, draft.httpsEnabled);
  if (draft.bleMode != initial.bleMode) setSetting(gSettings.bleMode, draft.bleMode);
  if (draft.espnowStationary != initial.espnowStationary) setSetting(gSettings.espnowStationary, draft.espnowStationary);
  if (draft.mqttPort != initial.mqttPort) setSetting(gSettings.mqttPort, draft.mqttPort);

#if ENABLE_WIFI
  if (result.wifiConfigured && result.wifiSSID.length() > 0) {
    const WifiNetworkSaveResult wifiSaved = saveSetupWiFiNetwork(result.wifiSSID, result.wifiPassword);
    broadcastOutput(wifiNetworkSaveResultMessage(wifiSaved));
    if (wifiSaved == WifiNetworkSaveResult::Accepted) broadcastOutput("WiFi network: " + result.wifiSSID);
  }
#endif
  if (!writes.finish()) {
    broadcastOutput("Warning: one or more setup settings files could not be saved; retry is pending.");
  }
  // These live actions happen only after successful draft acceptance. A failed
  // disk write retains the accepted live values and the Settings owner's retry.
  applySettings();
#if ENABLE_WIFI
  if (WiFi.isConnected()) setupNTP();
#endif
#if ENABLE_HTTPS
  if (gSettings.httpAutoStart && gSettings.httpsEnabled) {
    extern const char* cmd_certgen(const String&);
    broadcastOutput("[Wizard] HTTPS selected - generating self-signed certificate (ECDSA, ~1s)...");
    broadcastOutput(cmd_certgen(String("")));
  }
#endif
  discardSetupWizard();
  return true;
}

// ============================================================================
// Blocking first-time setup entry point. The CLI uses System_SetupWizardMode;
// both engines save completed results through wizardSaveAndApplySettings.
// ============================================================================

// "Wizard owns Serial" flag — see header for rationale. RAII-style
// management via runAndApplyFeatureWizard ensures it's always cleared
// even on early-return paths.
volatile bool gWizardOwnsSerial = false;

SetupWizardResult runAndApplyFeatureWizard(unsigned long idleTimeoutMs) {
  // Install the wizard-input timeout BEFORE entering runSetupWizard. The
  // setter also clears any stale cancel flag from a prior wizard run.
  // idleTimeoutMs=0 means waitForSerialInputBlocking() waits forever (FTS
  // boot behavior). Non-zero means we cancel the wizard after that many
  // milliseconds of idleness.
  setSerialWaitTimeout(idleTimeoutMs);

  // Claim Serial. While this is true, the main loop's CLI-input section
  // skips its drain, so every byte the user types reaches the wizard
  // (instead of being half-stolen by the CLI dispatcher and submitted
  // as a command that then times out because cmd_exec is in the wizard).
  gWizardOwnsSerial = true;

  SetupWizardResult result = runSetupWizard();

  gWizardOwnsSerial = false;

  // Always reset to "wait forever" after the wizard returns so a later
  // FTS-style use of waitForSerialInputBlocking() doesn't inherit a stale
  // timeout.
  const bool cancelled = isWizardCancelRequested();
  setSerialWaitTimeout(0);

  if (cancelled) {
    broadcastOutput("Feature setup cancelled or timed out. No changes saved.");
    result = {};
    discardSetupWizard();
    return result;
  }

  if (!result.completed) {
    result = {};
    broadcastOutput("Feature setup cancelled. No changes saved.");
    discardSetupWizard();
    return result;
  }

  uint32_t usedKB = 0, totalKB = 1;
  int pct = 0;
  getHeapBarData(&usedKB, &totalKB, &pct);
  if (!wizardSaveAndApplySettings(result)) return result;

  broadcastOutput("Feature configuration complete.");
  broadcastOutput("Timezone: " + result.timezoneAbbrev);
  {
    uint32_t estFreeKB = (usedKB >= totalKB) ? 0 : (totalKB - usedKB);
    char buf[64];
    snprintf(buf, sizeof(buf), "Heap estimate: ~%lu KB", (unsigned long)estFreeKB);
    broadcastOutput(buf);
  }

  return result;
}
