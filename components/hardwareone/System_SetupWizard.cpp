/**
 * System Setup Wizard Implementation
 * 
 * Core logic for setup wizard - display agnostic
 */

#include "System_SetupWizard.h"
#include <esp_attr.h>
#include "System_FeatureRegistry.h"
#include "System_Settings.h"
#include "System_BuildConfig.h"
#include "System_Debug.h"
#include "System_Clock.h"  // Clock::applyTimezone — keep libc TZ in step with the picker
#include "System_Mutex.h"  // SensorCacheGuard

#if ENABLE_WIFI
#include <WiFi.h>
#endif

#if ENABLE_OLED_DISPLAY
#include "OLED_SetupWizard.h"
#include "HAL_Display.h"
#include "HAL_Input.h"
extern bool oledConnected;
extern InputCache gInputCache;
// OLED page handlers for ESP-NOW and MQTT (defined in OLED_SetupWizard.cpp)
void handleOLEDESPNowPage(SetupWizardResult& result, bool& running);
void handleOLEDMQTTPage(SetupWizardResult& result, bool& running);
#endif

extern String waitForSerialInputBlocking();

// ============================================================================
// Setup Archetypes (deployment presets) — see System_SetupWizard.h
// ============================================================================
static const char* const SEED_HANDHELD[] = { "wifi", "http", "i2c", "oled", "input", "automation", nullptr };
static const char* const SEED_HEADLESS[] = { "wifi", "http", "i2c", "espnow", "automation", nullptr };
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
  { "headless", "Headless / relay",
    "No local screen. Managed over WiFi/web + ESP-NOW. Good for relay & remote nodes.",
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
  WIZARD_PAGE_FEATURES,
  WIZARD_PAGE_WEBMODE,
  WIZARD_PAGE_BTMODE,
  WIZARD_PAGE_SENSORS,
  WIZARD_PAGE_NETWORK,
  WIZARD_PAGE_SYSTEM,
  WIZARD_PAGE_ESPNOW,
  WIZARD_PAGE_MQTT,
  WIZARD_PAGE_WIFI,
};
static const size_t kPageOrderCount = sizeof(kPageOrder) / sizeof(kPageOrder[0]);

// ============================================================================
// Wizard State
// ============================================================================

static SetupWizardPage currentPage = WIZARD_PAGE_FEATURES;
static int currentSelection = 0;
static int scrollOffset = 0;
static int timezoneSelection = 1;  // EDT default (index 1 — US Eastern Daylight, UTC-4)
static int logLevelSelection = 3;  // DEBUG default (all logging enabled)
static int ntpSelection = 0;       // pool.ntp.org default
static int ledEffectSelection = 1; // rainbow default
static char wizardDeviceName[21] = "";  // Device name entry (used when ESP-NOW not compiled)

// Feature items per page. Only touched during first-time setup; dormant
// after FTS completes — safe to park in PSRAM.
EXT_RAM_BSS_ATTR static WizardFeatureItem featuresPage[16];
static size_t featuresPageCount = 0;

EXT_RAM_BSS_ATTR static WizardFeatureItem sensorsPage[16];
static size_t sensorsPageCount = 0;

EXT_RAM_BSS_ATTR static WizardNetworkItem networkPage[20];
static size_t networkPageCount = 0;

// ============================================================================
// State Accessors
// ============================================================================

SetupWizardPage getWizardCurrentPage() { return currentPage; }
int getWizardCurrentSelection() { return currentSelection; }
int getWizardScrollOffset() { return scrollOffset; }

void setWizardCurrentPage(SetupWizardPage page) { currentPage = page; }
void setWizardCurrentSelection(int sel) { currentSelection = sel; }
void setWizardScrollOffset(int offset) { scrollOffset = offset; }

size_t getWizardFeaturesPageCount() { return featuresPageCount; }
size_t getWizardSensorsPageCount() { return sensorsPageCount; }
size_t getWizardNetworkPageCount() { return networkPageCount; }

WizardFeatureItem* getWizardFeaturesPage() { return featuresPage; }
WizardFeatureItem* getWizardSensorsPage() { return sensorsPage; }
WizardNetworkItem* getWizardNetworkPage() { return networkPage; }

size_t getTimezoneCount() { return timezoneCount; }
const TimezoneEntry* getTimezones() { return timezones; }
int getWizardTimezoneSelection() { return timezoneSelection; }
void setWizardTimezoneSelection(int sel) { timezoneSelection = sel; }
int getWizardLogLevelSelection() { return logLevelSelection; }
void setWizardLogLevelSelection(int sel) { logLevelSelection = sel; }
const char** getLogLevelNames() { return (const char**)logLevelNames; }
size_t getLogLevelCount() { return logLevelCount; }

int getWizardNTPSelection() { return ntpSelection; }
void setWizardNTPSelection(int sel) { ntpSelection = sel; }
int getWizardLEDEffectSelection() { return ledEffectSelection; }
void setWizardLEDEffectSelection(int sel) { ledEffectSelection = sel; }
const char* const* getNTPPresets() { return ntpPresets; }
size_t getNTPPresetCount() { return ntpPresetCount; }
const char* const* getLEDEffects() { return ledEffects; }
size_t getLEDEffectCount() { return ledEffectCount; }

// System page: items are timezone, log level, [NTP server], [LED effect]
// Item indices depend on which conditional items are visible
static bool systemPageHasNTP() {
#if ENABLE_WIFI
  // Row stays visible whenever WiFi is compiled in, regardless of whether
  // the user currently has WiFi enabled — wizard rows don't vanish based
  // on sibling toggles.
  const FeatureEntry* wifiFeature = getFeatureById("wifi");
  return wifiFeature && isFeatureCompiled(wifiFeature);
#else
  return false;
#endif
}

static bool systemPageHasLED() {
  const FeatureEntry* ledFeature = getFeatureById("led");
  return ledFeature && isFeatureCompiled(ledFeature);
}

static bool systemPageHasDeviceName() {
#if ENABLE_ESPNOW
  return false;  // Device name is set in the ESP-NOW configure panel
#else
  return true;   // No ESP-NOW page — collect device name here
#endif
}

size_t getWizardSystemPageCount() {
  size_t count = 2; // timezone + log level always present
  if (systemPageHasNTP()) count++;
  if (systemPageHasLED()) count++;
  if (systemPageHasDeviceName()) count++;
  return count;
}

// Map system page selection index to logical item
// 0=log level, 1=timezone, 2=NTP (if visible), 3=LED (if visible), 4=DeviceName (if visible)
SystemPageItem getSystemItemAt(int index) {
  if (index == 0) return SYS_ITEM_LOGLEVEL;
  if (index == 1) return SYS_ITEM_TIMEZONE;
  int nextIdx = 2;
  if (systemPageHasNTP()) {
    if (index == nextIdx) return SYS_ITEM_NTP;
    nextIdx++;
  }
  if (systemPageHasLED()) {
    if (index == nextIdx) return SYS_ITEM_LED;
    nextIdx++;
  }
  if (systemPageHasDeviceName()) {
    if (index == nextIdx) return SYS_ITEM_DEVICE_NAME;
  }
  return SYS_ITEM_TIMEZONE; // fallback
}

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
   uint32_t totalHeapKB = (uint32_t)(ESP.getHeapSize() / 1024);
   if (totalHeapKB == 0) totalHeapKB = 1;
 
   uint32_t usedNowKB = (uint32_t)((ESP.getHeapSize() - ESP.getFreeHeap()) / 1024);
   uint32_t infraKB = getWizardInfrastructureCostKB();
 
   sWizardBaselineKB = (usedNowKB > infraKB) ? (usedNowKB - infraKB) : 0;
   if (sWizardBaselineKB > totalHeapKB) sWizardBaselineKB = totalHeapKB;
   sWizardBaselineCalibrated = true;
 }

uint32_t wizardEnabledModeExtraHeapKB();  // defined with the mode-menu table below

void getHeapBarData(uint32_t* enabledKB, uint32_t* maxKB, int* percentage) {
  uint32_t totalHeapKB = (uint32_t)(ESP.getHeapSize() / 1024);
  if (totalHeapKB == 0) totalHeapKB = 1;

   if (!sWizardBaselineCalibrated) {
     calibrateWizardBaseline();
   }

  // Feature-granular base + the selected sub-mode's extra (HTTPS/G2) so the bar
  // reflects mode choices, not just which features are on.
  uint32_t enabledCostKB = getEnabledFeaturesHeapEstimate() + wizardEnabledModeExtraHeapKB();
  uint32_t estimatedUsedKB = sWizardBaselineKB + enabledCostKB;
  if (estimatedUsedKB > totalHeapKB) estimatedUsedKB = totalHeapKB;

  *enabledKB = estimatedUsedKB;
  *maxKB = totalHeapKB;
  *percentage = (estimatedUsedKB * 100) / totalHeapKB;
}

// ============================================================================
// Initialize Wizard
// ============================================================================

void initSetupWizard() {
  featuresPageCount = 0;
  sensorsPageCount = 0;
  networkPageCount = 0;

  // Pre-populate device name from current settings
  String curName = gSettings.espnowDeviceName.length() > 0 ? gSettings.espnowDeviceName
                 : gSettings.bleDeviceName.length() > 0    ? gSettings.bleDeviceName
                 : "HardwareOne";
  strncpy(wizardDeviceName, curName.c_str(), sizeof(wizardDeviceName) - 1);
  wizardDeviceName[sizeof(wizardDeviceName) - 1] = '\0';
  
   sWizardBaselineKB = 0;
   sWizardBaselineCalibrated = false;
   calibrateWizardBaseline();
  
  // Build features page (network features)
  for (size_t i = 0; i < getFeatureCount(); i++) {
    const FeatureEntry* f = getFeatureByIndex(i);
    if (!f || !isFeatureCompiled(f)) continue;
    
    if ((f->category == FEATURE_CAT_NETWORK || f->category == FEATURE_CAT_SYSTEM) && featuresPageCount < 16) {
      featuresPage[featuresPageCount].id = f->id;
      featuresPage[featuresPageCount].label = f->name;
#if ENABLE_HTTPS
      // The HTTP feature gains an HTTP/HTTPS sub-choice in the wizard, so label
      // it "Web Interface" here (wizard-local; the registry name is unchanged).
      if (strcmp(f->id, "http") == 0) featuresPage[featuresPageCount].label = "Web Interface";
#endif
      featuresPage[featuresPageCount].heapKB = f->heapCostKB;
      featuresPage[featuresPageCount].setting = f->enabledSetting;
      featuresPage[featuresPageCount].essential = (f->flags & FEATURE_FLAG_ESSENTIAL);
      featuresPage[featuresPageCount].compiled = true;
      featuresPageCount++;
    }
  }
  
  // Build sensors page — display features only.
  // Sensors are intentionally excluded: every sensor's persisted flag is an
  // *AutoStart flag (not a separate enable/disable). Listing them here would
  // make the checkbox silently toggle boot-time auto-start, which is
  // misleading. Sensor auto-start is configured on the Startup & Auto-start
  // page instead (see rebuildNetworkSettingsPage).
  for (size_t i = 0; i < getFeatureCount(); i++) {
    const FeatureEntry* f = getFeatureByIndex(i);
    if (!f || !isFeatureCompiled(f)) continue;

    if (f->category == FEATURE_CAT_DISPLAY && sensorsPageCount < 16) {
      sensorsPage[sensorsPageCount].id = f->id;
      sensorsPage[sensorsPageCount].label = f->name;
      sensorsPage[sensorsPageCount].heapKB = f->heapCostKB;
      sensorsPage[sensorsPageCount].setting = f->enabledSetting;
      sensorsPage[sensorsPageCount].essential = (f->flags & FEATURE_FLAG_ESSENTIAL);
      sensorsPage[sensorsPageCount].compiled = true;
      sensorsPageCount++;
    }
  }
  
  // Network settings page is built dynamically based on enabled features
  // Call rebuildNetworkSettingsPage() before showing that page
  networkPageCount = 0;

  // Reset state
  currentPage = WIZARD_PAGE_FEATURES;
  currentSelection = 0;
  scrollOffset = 0;
  timezoneSelection = 1;  // EDT default
  logLevelSelection = 3;  // DEBUG (all logging enabled)
  ntpSelection = 0;       // pool.ntp.org
  ledEffectSelection = 1; // rainbow

  // Find current timezone in list
  for (size_t i = 0; i < timezoneCount; i++) {
    if (timezones[i].offsetMinutes == gSettings.tzOffsetMinutes) {
      timezoneSelection = i;
      break;
    }
  }

  // Find current NTP server in presets
  if (gSettings.ntpServer.length() > 0) {
    for (size_t i = 0; i < ntpPresetCount; i++) {
      if (gSettings.ntpServer == ntpPresets[i]) {
        ntpSelection = i;
        break;
      }
    }
  }

  // Find current LED effect in list
  if (gSettings.ledStartupEffect.length() > 0) {
    for (size_t i = 0; i < ledEffectCount; i++) {
      if (gSettings.ledStartupEffect == ledEffects[i]) {
        ledEffectSelection = i;
        break;
      }
    }
  }
}

// ============================================================================
// Dynamic Network Settings Page
// ============================================================================
// Rebuild network settings based on which features are currently enabled
// This should be called before navigating to the network settings page

void rebuildNetworkSettingsPage() {
  networkPageCount = 0;
  
#if ENABLE_WIFI
  // Principle: wizard rows don't vanish based on other toggles. Gate on
  // compiled-ness only so the row stays visible even if WiFi itself is off.
  {
    const FeatureEntry* wifiFeature = getFeatureById("wifi");
    if (wifiFeature && isFeatureCompiled(wifiFeature)) {
      networkPage[networkPageCount].label = "WiFi auto-connect";
      networkPage[networkPageCount].boolSetting = &gSettings.wifiAutoStart;
      networkPage[networkPageCount].isBool = true;
      networkPageCount++;
    }
  }
#endif

  // NOTE: "HTTP auto-start" and "BT auto-start" rows used to live here, but
  // they bound to the very same flags (httpAutoStart / bleAutoStart) that
  // the "Web Interface" / "Bluetooth" toggles on the Features page already set
  // — a confusing duplicate. Enable now lives on the Features page; the
  // HTTP/HTTPS and Server/G2 *mode* is chosen on the dedicated WEBMODE/BTMODE
  // pages (see handleModePage). So these rows were removed.

#if ENABLE_ESPNOW
  // Gate on compiled-ness only (see WiFi comment above).
  {
    const FeatureEntry* espnowFeature = getFeatureById("espnow");
    if (espnowFeature && isFeatureCompiled(espnowFeature)) {
      networkPage[networkPageCount].label = "ESP-NOW mesh";
      networkPage[networkPageCount].boolSetting = &gSettings.espnowmesh;
      networkPage[networkPageCount].isBool = true;
      networkPageCount++;
    }
  }
#endif

#if ENABLE_MQTT
  // Gate on compiled-ness only (see WiFi comment above).
  {
    const FeatureEntry* mqttFeature = getFeatureById("mqtt");
    if (mqttFeature && isFeatureCompiled(mqttFeature)) {
      networkPage[networkPageCount].label = "MQTT auto-start";
      networkPage[networkPageCount].boolSetting = &gSettings.mqttAutoStart;
      networkPage[networkPageCount].isBool = true;
      networkPageCount++;
    }
  }
#endif

  // Sensor auto-start toggles (one per compiled sensor).
  // The registry already wires each sensor's enabledSetting pointer to its
  // *AutoStart flag, so we can reuse it here. Each entry's label comes from
  // the feature's display name.
  const size_t kNetworkPageCap = sizeof(networkPage)/sizeof(networkPage[0]);
  for (size_t i = 0; i < getFeatureCount() && networkPageCount < kNetworkPageCap; i++) {
    const FeatureEntry* f = getFeatureByIndex(i);
    if (!f || !isFeatureCompiled(f)) continue;
    if (f->category != FEATURE_CAT_SENSOR) continue;
    if (!f->enabledSetting) continue;
    networkPage[networkPageCount].label = f->name;       // e.g. "Camera", "IMU"
    networkPage[networkPageCount].boolSetting = f->enabledSetting;
    networkPage[networkPageCount].isBool = true;
    networkPageCount++;
  }
}

bool hasNetworkSettings() {
  rebuildNetworkSettingsPage();
  return networkPageCount > 0;
}

// ============================================================================
// Page Visibility & Dynamic Navigation
// ============================================================================

bool wizardShouldShowESPNow() {
#if ENABLE_ESPNOW
  const FeatureEntry* f = getFeatureById("espnow");
  return f && isFeatureEnabled(f);
#else
  return false;
#endif
}

bool wizardShouldShowMQTT() {
#if ENABLE_MQTT
  const FeatureEntry* f = getFeatureById("mqtt");
  return f && isFeatureEnabled(f) && gSettings.mqttAutoStart;
#else
  return false;
#endif
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
  { "http",      "Web Mode",       kWebModes, 2, &gSettings.httpsEnabled, nullptr },
#endif
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
  { "bluetooth", "Bluetooth Mode", kBtModes,  2, nullptr, &gSettings.bleMode },
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
    if (!f || !isFeatureEnabled(f)) continue;
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
  return gSettings.httpAutoStart && wizardModeMenuForFeature("http") != nullptr;
#else
  return false;
#endif
}

bool wizardShouldShowBtMode() {
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
  return gSettings.bleAutoStart && wizardModeMenuForFeature("bluetooth") != nullptr;
#else
  return false;
#endif
}

bool wizardIsPageVisible(SetupWizardPage page) {
  switch (page) {
    case WIZARD_PAGE_WEBMODE:
      return wizardShouldShowWebMode();
    case WIZARD_PAGE_BTMODE:
      return wizardShouldShowBtMode();
    case WIZARD_PAGE_NETWORK:
      return hasNetworkSettings();
    case WIZARD_PAGE_ESPNOW:
      return wizardShouldShowESPNow();
    case WIZARD_PAGE_MQTT:
      return wizardShouldShowMQTT();
    case WIZARD_PAGE_WIFI:
      return wizardShouldShowWiFi();
    default:
      return true;
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

bool wizardToggleCurrentItem() {
  if (currentPage == WIZARD_PAGE_FEATURES) {
    if (currentSelection < (int)featuresPageCount) {
      WizardFeatureItem* item = &featuresPage[currentSelection];
      if (item->setting && !item->essential) {
        *item->setting = !*item->setting;
        return true;
      }
    }
  } else if (currentPage == WIZARD_PAGE_SENSORS) {
    if (currentSelection < (int)sensorsPageCount) {
      WizardFeatureItem* item = &sensorsPage[currentSelection];
      if (item->setting && !item->essential) {
        *item->setting = !*item->setting;
        return true;
      }
    }
  } else if (currentPage == WIZARD_PAGE_NETWORK) {
    if (currentSelection < (int)networkPageCount && networkPage[currentSelection].isBool) {
      *networkPage[currentSelection].boolSetting = !*networkPage[currentSelection].boolSetting;
      return true;
    }
  }
  return false;
}

bool wizardMoveUp() {
  int maxItems = 0;
  if (currentPage == WIZARD_PAGE_FEATURES) maxItems = featuresPageCount;
  else if (currentPage == WIZARD_PAGE_SENSORS) maxItems = sensorsPageCount;
  else if (currentPage == WIZARD_PAGE_NETWORK) maxItems = networkPageCount;
  else if (currentPage == WIZARD_PAGE_SYSTEM) maxItems = getWizardSystemPageCount();

  if (currentSelection > 0) {
    currentSelection--;
    if (currentSelection < scrollOffset) {
      scrollOffset = currentSelection;
    }
    return true;
  }
  return false;
}

bool wizardMoveDown() {
  int maxItems = 0;
  if (currentPage == WIZARD_PAGE_FEATURES) maxItems = featuresPageCount;
  else if (currentPage == WIZARD_PAGE_SENSORS) maxItems = sensorsPageCount;
  else if (currentPage == WIZARD_PAGE_NETWORK) maxItems = networkPageCount;
  else if (currentPage == WIZARD_PAGE_SYSTEM) maxItems = getWizardSystemPageCount();

  if (currentSelection < maxItems - 1) {
    currentSelection++;
    if (currentSelection >= scrollOffset + 4) {
      scrollOffset = currentSelection - 3;
    }
    return true;
  }
  return false;
}

bool wizardCycleOption() {
  if (currentPage == WIZARD_PAGE_SYSTEM) {
    SystemPageItem item = getSystemItemAt(currentSelection);
    switch (item) {
      case SYS_ITEM_TIMEZONE:
        timezoneSelection = (timezoneSelection + 1) % timezoneCount;
        return true;
      case SYS_ITEM_LOGLEVEL:
        logLevelSelection = (logLevelSelection + 1) % logLevelCount;
        return true;
      case SYS_ITEM_NTP:
        ntpSelection = (ntpSelection + 1) % ntpPresetCount;
        return true;
      case SYS_ITEM_LED:
        ledEffectSelection = (ledEffectSelection + 1) % ledEffectCount;
        return true;
      case SYS_ITEM_DEVICE_NAME:
        return false;  // Text input — handled by caller, not cycled
    }
  }
  return false;
}

bool wizardShouldShowWiFi() {
#if ENABLE_WIFI
  const FeatureEntry* wifiFeature = getFeatureById("wifi");
  return wifiFeature && isFeatureEnabled(wifiFeature);
#else
  return false;
#endif
}

bool wizardNextPage(SetupWizardResult& result) {
  // Save system settings when leaving system page
  if (currentPage == WIZARD_PAGE_SYSTEM) {
    result.timezoneOffset = timezones[timezoneSelection].offsetMinutes;
    result.timezoneAbbrev = timezones[timezoneSelection].abbrev;
    gSettings.tzOffsetMinutes = result.timezoneOffset;
    // Push it into libc immediately. This page-leave write lands well before
    // wizardFinalize()'s applySettings(), and if the wizard is cancelled or
    // times out that call never happens — leaving gSettings on the new offset
    // while localtime_r() still used the old one, i.e. two different "local"
    // times in one firmware for the rest of the boot.
    Clock::applyTimezone();
    gSettings.logLevel = logLevelSelection;
    if (systemPageHasNTP()) {
      result.ntpServer = ntpPresets[ntpSelection];
    }
    if (systemPageHasLED()) {
      result.ledStartupEffect = ledEffects[ledEffectSelection];
    }
    if (systemPageHasDeviceName() && wizardDeviceName[0] != '\0') {
      gSettings.bleDeviceName = wizardDeviceName;
      gSettings.espnowDeviceName = wizardDeviceName;
    }
  }

  // Rebuild network page before navigating away from sensors (toggles may have changed)
  if (currentPage == WIZARD_PAGE_SENSORS) {
    rebuildNetworkSettingsPage();
  }

  SetupWizardPage next = wizardAdvanceFrom(currentPage);
  if (next == WIZARD_PAGE_COUNT) {
    // No more pages — wizard complete
    if (currentPage == WIZARD_PAGE_WIFI) result.wifiEnabled = true;
    result.completed = true;
    return false;
  }

  if (next == WIZARD_PAGE_WIFI) result.wifiEnabled = true;

  currentPage = next;
  currentSelection = 0;
  scrollOffset = 0;
  return true;
}

bool wizardPrevPage() {
  SetupWizardPage prev = wizardRetreatFrom(currentPage);
  if (prev == currentPage) return false; // already at first page

  currentPage = prev;
  currentSelection = 0;
  scrollOffset = 0;
  return true;
}

void wizardFinalize(SetupWizardResult& result) {
  result.timezoneOffset = timezones[timezoneSelection].offsetMinutes;
  result.timezoneAbbrev = timezones[timezoneSelection].abbrev;
  gSettings.tzOffsetMinutes = result.timezoneOffset;
  gSettings.logLevel = logLevelSelection;
  if (systemPageHasNTP()) {
    result.ntpServer = ntpPresets[ntpSelection];
    gSettings.ntpServer = result.ntpServer;
  }
  if (systemPageHasLED()) {
    result.ledStartupEffect = ledEffects[ledEffectSelection];
    gSettings.ledStartupEffect = result.ledStartupEffect;
  }
  if (systemPageHasDeviceName() && wizardDeviceName[0] != '\0') {
    gSettings.bleDeviceName = wizardDeviceName;
    gSettings.espnowDeviceName = wizardDeviceName;
  }

#if ENABLE_HTTPS
  // If the user picked HTTPS for the Web Interface, generate the self-signed
  // cert now (ECDSA P-256, ~1s) so the server starts in HTTPS on first launch.
  // The server-start path falls back to HTTP if certs are absent, so a failure
  // here degrades gracefully rather than leaving a dead server.
  if (gSettings.httpAutoStart && gSettings.httpsEnabled) {
    extern const char* cmd_certgen(const String&);
    broadcastOutput("[Wizard] HTTPS selected - generating self-signed certificate (ECDSA, ~1s)...");
    broadcastOutput(cmd_certgen(String("")));
  }
#endif
}

// ============================================================================
// Serial Console Wizard
// ============================================================================

static void printSerialHeapBar() {
  uint32_t enabledKB, maxKB;
  int pct;
  getHeapBarData(&enabledKB, &maxKB, &pct);
  
  // Draw ASCII bar
  Serial.print("[");
  int barWidth = 20;
  int filled = (barWidth * pct) / 100;
  for (int i = 0; i < barWidth; i++) {
    Serial.print(i < filled ? "#" : "-");
  }
  Serial.printf("] %lu/%luKB (%d%%)\n", (unsigned long)enabledKB, (unsigned long)maxKB, pct);
}

static void printSerialFeaturePage(const char* title, WizardFeatureItem* items, size_t count) {
  Serial.println();
  Serial.printf("=== %s ===\n", title);
  printSerialHeapBar();
  Serial.println("----------------------------------------");
  
  for (size_t i = 0; i < count; i++) {
    bool enabled = items[i].setting ? *items[i].setting : false;
    const char* essential = items[i].essential ? "*" : " ";
    const char* status = enabled ? "[ON] " : "[OFF]";
    Serial.printf(" %zu. %s %s%-14s ~%2dKB\n", 
      i + 1, status, essential, items[i].label, items[i].heapKB);
  }
  
  Serial.println("----------------------------------------");
  Serial.println("Enter number to toggle, 'n' for next, 'b' for back");
  Serial.print("> ");
}

static void printSerialNetworkPage() {
  Serial.println();
  Serial.println("=== Startup & Auto-start ===");
  printSerialHeapBar();
  Serial.println("----------------------------------------");
  
  for (size_t i = 0; i < networkPageCount; i++) {
    if (networkPage[i].isBool) {
      bool enabled = *networkPage[i].boolSetting;
      Serial.printf(" %zu. %-20s %s\n", i + 1, networkPage[i].label, enabled ? "[ON]" : "[OFF]");
    }
  }
  
  Serial.println("----------------------------------------");
  Serial.println("Enter number to toggle, 'n' for next, 'b' for back");
  Serial.print("> ");
}

static void printSerialSystemPage() {
  Serial.println();
  Serial.println("=== System Settings ===");
  printSerialHeapBar();
  Serial.println("----------------------------------------");

  int itemNum = 1;
  Serial.printf(" %d. Log level: %s\n", itemNum++, logLevelNames[logLevelSelection]);
  Serial.printf(" %d. Timezone:  %-5s %s\n", itemNum++,
    timezones[timezoneSelection].abbrev,
    timezones[timezoneSelection].name);
  if (systemPageHasNTP()) {
    Serial.printf(" %d. NTP Server: %s\n", itemNum++, ntpPresets[ntpSelection]);
  }
  if (systemPageHasLED()) {
    Serial.printf(" %d. LED Effect: %s\n", itemNum++, ledEffects[ledEffectSelection]);
  }
  if (systemPageHasDeviceName()) {
    Serial.printf(" %d. Device Name: %s\n", itemNum++, wizardDeviceName);
  }

  Serial.println("----------------------------------------");
  Serial.println("Enter number to cycle/edit, 'n' for next, 'b' for back");
  Serial.print("> ");
}

// ============================================================================
// Shared serial page printer (used by runSetupWizard for both OLED and no-OLED)
// ============================================================================

// External linkage so the new CLIMode-based wizard (System_SetupWizardMode.cpp)
// can reuse the page-status renderer without duplicating its logic.
// Previously `static`; promoting doesn't change behavior, just visibility.
void printSerialPageStatus() {
  SetupWizardPage page = getWizardCurrentPage();
  int sel = getWizardCurrentSelection();
  int pageNum = getWizardPageNumber(page);
  int totalPages = getWizardTotalPages();

  Serial.println();
  Serial.println("========================================");

  switch (page) {
    case WIZARD_PAGE_FEATURES: {
      Serial.printf("  SETUP %d/%d: Features\n", pageNum, totalPages);
      WizardFeatureItem* items = getWizardFeaturesPage();
      size_t count = getWizardFeaturesPageCount();
      for (size_t i = 0; i < count; i++) {
        bool enabled = items[i].setting ? *items[i].setting : false;
        Serial.printf(" %s%zu. [%s] %s%s ~%dKB\n",
          (int)i == sel ? ">" : " ", i + 1,
          enabled ? "X" : " ",
          items[i].label,
          items[i].essential ? "*" : "",
          items[i].heapKB);
      }
      break;
    }
    case WIZARD_PAGE_SENSORS: {
      Serial.printf("  SETUP %d/%d: Sensors & Display\n", pageNum, totalPages);
      WizardFeatureItem* items = getWizardSensorsPage();
      size_t count = getWizardSensorsPageCount();
      for (size_t i = 0; i < count; i++) {
        bool enabled = items[i].setting ? *items[i].setting : false;
        Serial.printf(" %s%zu. [%s] %s%s ~%dKB\n",
          (int)i == sel ? ">" : " ", i + 1,
          enabled ? "X" : " ",
          items[i].label,
          items[i].essential ? "*" : "",
          items[i].heapKB);
      }
      break;
    }
    case WIZARD_PAGE_NETWORK: {
      Serial.printf("  SETUP %d/%d: Startup & Auto-start\n", pageNum, totalPages);
      WizardNetworkItem* items = getWizardNetworkPage();
      size_t count = getWizardNetworkPageCount();
      for (size_t i = 0; i < count; i++) {
        if (items[i].isBool) {
          bool enabled = *items[i].boolSetting;
          Serial.printf(" %s%zu. [%s] %s\n",
            (int)i == sel ? ">" : " ", i + 1,
            enabled ? "X" : " ",
            items[i].label);
        }
      }
      break;
    }
    case WIZARD_PAGE_SYSTEM: {
      Serial.printf("  SETUP %d/%d: System Settings\n", pageNum, totalPages);
      const TimezoneEntry* tz = getTimezones();
      int tzSel = getWizardTimezoneSelection();
      int logSel = getWizardLogLevelSelection();
      const char** logNames = getLogLevelNames();
      int idx = 0;
      Serial.printf(" %s%d. Log Level: %s\n", sel == idx ? ">" : " ", idx + 1, logNames[logSel]);
      idx++;
      Serial.printf(" %s%d. Timezone: %-5s %s\n", sel == idx ? ">" : " ", idx + 1, tz[tzSel].abbrev, tz[tzSel].name);
      idx++;
      if (systemPageHasNTP()) {
        Serial.printf(" %s%d. NTP Server: %s\n", sel == idx ? ">" : " ", idx + 1, ntpPresets[ntpSelection]);
        idx++;
      }
      if (systemPageHasLED()) {
        Serial.printf(" %s%d. LED Effect: %s\n", sel == idx ? ">" : " ", idx + 1, ledEffects[ledEffectSelection]);
        idx++;
      }
      if (systemPageHasDeviceName()) {
        Serial.printf(" %s%d. Device Name: %s\n", sel == idx ? ">" : " ", idx + 1, wizardDeviceName);
        idx++;
      }
      break;
    }
    case WIZARD_PAGE_ESPNOW: {
      Serial.printf("  SETUP %d/%d: ESP-NOW Identity\n", pageNum, totalPages);
      Serial.println("  (All fields optional — press Enter to skip)");
      break;
    }
    case WIZARD_PAGE_MQTT: {
      Serial.printf("  SETUP %d/%d: MQTT Broker\n", pageNum, totalPages);
      Serial.println("  (Press Enter to use defaults)");
      break;
    }
    default:
      break;
  }

  Serial.println("----------------------------------------");
  Serial.println("Serial: # to toggle, 'n' next, 'b' back");
#if ENABLE_OLED_DISPLAY
  Serial.println("OLED:   Joystick + A=Toggle, Right=Next");
#endif
  Serial.print("> ");
}

// Serial ESP-NOW identity page
static void handleSerialESPNowPage(SetupWizardResult& result, bool& running) {
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
  String introChoice = waitForSerialInputBlocking();
  introChoice.trim();
  String deviceName;
  if (introChoice.equalsIgnoreCase("b") || introChoice.equalsIgnoreCase("back")) {
    wizardPrevPage();
    return;
  }
  if (introChoice.equalsIgnoreCase("n") || introChoice.equalsIgnoreCase("next") || introChoice.length() == 0) {
    // Explicit skip (or blank/Enter) — disable ESP-NOW since it's unconfigured;
    // it can't start without a name.
    gSettings.espnowEnabled = false;
    if (!wizardNextPage(result)) running = false;
    return;
  }

  Serial.println("----------------------------------------");
  Serial.println("All fields optional. Enter to skip field, 'n' to finish, 'b' to go back.");
  Serial.println("----------------------------------------");

  String currentName = gSettings.espnowDeviceName.length() > 0 ? gSettings.espnowDeviceName : "HardwareOne";
  if (introChoice.equalsIgnoreCase("c") || introChoice.equalsIgnoreCase("configure")) {
    Serial.printf("Device Name (for Bluetooth + ESP-NOW) [%s]: ", currentName.c_str());
    deviceName = waitForSerialInputBlocking();
    deviceName.trim();
    if (deviceName.equalsIgnoreCase("b") || deviceName.equalsIgnoreCase("back")) {
      wizardPrevPage(); return;
    }
    if (deviceName.equalsIgnoreCase("n")) { deviceName = currentName; goto espnow_done; }
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
  String room = waitForSerialInputBlocking();
  room.trim();
  if (room.equalsIgnoreCase("b") || room.equalsIgnoreCase("back")) {
    wizardPrevPage(); return;
  }
  if (room.equalsIgnoreCase("n")) goto espnow_done;
  result.espnowRoom = room; }

  { Serial.print("Zone (e.g. 'North Wall'): ");
  String zone = waitForSerialInputBlocking();
  zone.trim();
  if (zone.equalsIgnoreCase("b") || zone.equalsIgnoreCase("back")) {
    wizardPrevPage(); return;
  }
  if (zone.equalsIgnoreCase("n")) goto espnow_done;
  result.espnowZone = zone; }

  { Serial.print("The device will be — (m)obile or (s)tationary [m]: ");
  String stat = waitForSerialInputBlocking();
  stat.trim();
  if (stat.equalsIgnoreCase("b") || stat.equalsIgnoreCase("back")) {
    wizardPrevPage(); return;
  }
  result.espnowStationary = (stat.equalsIgnoreCase("s") || stat.equalsIgnoreCase("stationary")); }

espnow_done:
  // If the wizard timed out partway through this page (waitForSerialInputBlocking
  // returns empty + sets the cancel flag), don't write any of the fields the
  // user didn't actually confirm. The outer loop will pick up the cancel flag
  // next iteration and abort the whole wizard.
  if (isWizardCancelRequested()) {
    running = false;
    return;
  }

  // Apply to settings
  gSettings.bleDeviceName = deviceName;
  gSettings.espnowDeviceName = deviceName;
  if (result.espnowFriendlyName.length() > 0) gSettings.espnowFriendlyName = result.espnowFriendlyName;
  if (result.espnowRoom.length() > 0) gSettings.espnowRoom = result.espnowRoom;
  if (result.espnowZone.length() > 0) gSettings.espnowZone = result.espnowZone;
  gSettings.espnowStationary = result.espnowStationary;

  Serial.println("ESP-NOW identity configured.");

  // Advance to next page
  if (!wizardNextPage(result)) {
    running = false;
  }
}

// Serial MQTT broker page
static void handleSerialMQTTPage(SetupWizardResult& result, bool& running) {
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
  String introChoice = waitForSerialInputBlocking();
  introChoice.trim();
  if (introChoice.equalsIgnoreCase("b") || introChoice.equalsIgnoreCase("back")) {
    wizardPrevPage();
    return;
  }
  if (!introChoice.equalsIgnoreCase("c") && !introChoice.equalsIgnoreCase("configure")) {
    // Skip — disable MQTT auto-start since it's unconfigured
    gSettings.mqttAutoStart = false;
    if (!wizardNextPage(result)) running = false;
    return;
  }

  Serial.println("----------------------------------------");
  Serial.println("Press Enter to use defaults, 'b' to go back.");
  Serial.println("----------------------------------------");

  Serial.printf("Host (default: %s): ", gSettings.mqttHost.length() > 0 ? gSettings.mqttHost.c_str() : "none");
  String host = waitForSerialInputBlocking();
  host.trim();
  if (host.equalsIgnoreCase("b") || host.equalsIgnoreCase("back")) {
    wizardPrevPage();
    return;
  }
  result.mqttHost = host;

  Serial.print("Port (default: 1883): ");
  String portStr = waitForSerialInputBlocking();
  portStr.trim();
  if (portStr.equalsIgnoreCase("b") || portStr.equalsIgnoreCase("back")) {
    wizardPrevPage();
    return;
  }
  result.mqttPort = portStr.length() > 0 ? portStr.toInt() : 0;

  Serial.print("Username (blank = no auth): ");
  String user = waitForSerialInputBlocking();
  user.trim();
  if (user.equalsIgnoreCase("b") || user.equalsIgnoreCase("back")) {
    wizardPrevPage();
    return;
  }
  result.mqttUser = user;

  if (user.length() > 0) {
    Serial.print("Password: ");
    String pass = waitForSerialInputBlocking();
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

  // Apply to settings
  if (result.mqttHost.length() > 0) gSettings.mqttHost = result.mqttHost;
  if (result.mqttPort > 0) gSettings.mqttPort = result.mqttPort;
  if (result.mqttUser.length() > 0) gSettings.mqttUser = result.mqttUser;
  if (result.mqttPassword.length() > 0) gSettings.mqttPassword = result.mqttPassword;

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
// SSID. namedScanIdx[k] holds the raw scan index of the (k+1)-th named AP.
int wifiScanPrintNamed(int* namedScanIdx, int namedCap) {
  int n = WiFi.scanNetworks(false, true);
  int storedNamed = 0;   // named APs we numbered (capped at namedCap)
  int totalNamed  = 0;   // all named APs seen
  int hiddenCount = 0;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i).length() == 0) { hiddenCount++; continue; }
    totalNamed++;
    if (storedNamed < namedCap) namedScanIdx[storedNamed++] = i;
  }

  if (storedNamed > 0) {
    broadcastOutput(String("Found ") + totalNamed + " network(s):");
    for (int j = 0; j < storedNamed; j++) {
      char line[96];
      snprintf(line, sizeof(line), "  %d. %-24s  %lddBm  %s",
               j + 1, WiFi.SSID(namedScanIdx[j]).c_str(),
               (long)WiFi.RSSI(namedScanIdx[j]),
               (WiFi.encryptionType(namedScanIdx[j]) == WIFI_AUTH_OPEN) ? "Open" : "Secured");
      broadcastOutput(line);
    }
    if (totalNamed > storedNamed) {
      broadcastOutput(String("  (... and ") + (totalNamed - storedNamed) +
                      " more named — type the exact SSID to join)");
    }
  } else {
    broadcastOutput("No named WiFi networks found.");
  }
  if (hiddenCount > 0) {
    broadcastOutput(String("  (+") + hiddenCount + " hidden network" +
                    (hiddenCount == 1 ? "" : "s") + " — type the exact SSID to join)");
  }
  return storedNamed;
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
    int named[24];
    int namedCount = wifiScanPrintNamed(named, 24);
    Serial.println("----------------------------------------");
    Serial.println("Enter number, SSID directly, 'rescan', or 'skip':");
    Serial.println("('b' or 'back' to return to previous page)");
    Serial.print("> ");
    String ssidInput = waitForSerialInputBlocking();
    // Honor wizard-input timeout (CLI featuresetup installs one; FTS doesn't).
    if (isWizardCancelRequested()) {
      WiFi.scanDelete();
      result.completed = false;
      running = false;
      return;
    }
    if (ssidInput.equalsIgnoreCase("b") || ssidInput.equalsIgnoreCase("back")) {
      WiFi.scanDelete();
      wizardPrevPage();
      wifiPageDone = true;
      continue;
    }
    if (ssidInput.equalsIgnoreCase("rescan")) {
      WiFi.scanDelete();
      continue;
    }
    if (ssidInput.equalsIgnoreCase("skip") || ssidInput.length() == 0) {
      WiFi.scanDelete();
      result.completed = true;
      running = false;
      wifiPageDone = true;
      continue;
    }
    String ssid = ssidInput;
    int idx = ssidInput.toInt();
    // Map a bare in-range integer to the Nth NAMED network; anything else
    // (incl. SSIDs that start with a digit, e.g. "2WIRE") is a typed SSID.
    if (idx >= 1 && idx <= namedCount && String(idx) == ssidInput) {
      ssid = WiFi.SSID(named[idx - 1]);
    }
    WiFi.scanDelete();
    if (ssid.length() > 0) {
      Serial.println("Enter WiFi password (or 'b' to go back):");
      Serial.print("> ");
      String pass = waitForSerialInputBlocking();
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
    result.completed = true;
    running = false;
    wifiPageDone = true;
#else
    Serial.println("WiFi not compiled in this build.");
    result.completed = true;
    running = false;
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
    if (sel != lastPrinted) {
      Serial.println();
      Serial.printf("=== SETUP %d/%d: %s ===\n", getWizardPageNumber(page), getWizardTotalPages(), sub->title);
      for (int i = 0; i < sub->modeCount; i++) {
        Serial.printf(" %s%d. [%s] %s\n", sel == i ? ">" : " ", i + 1,
                      sel == i ? "X" : " ", sub->modes[i].label);
      }
      Serial.println("----------------------------------------");
      Serial.println("Serial: # to select, 'n' next, 'b' back");
      Serial.print("> ");
      lastPrinted = sel;
    }

    String input = waitForSerialInputBlocking();
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
  SetupWizardResult result;
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

  // Sync timezone selection from current settings
  const TimezoneEntry* tzList = getTimezones();
  size_t tzCount = getTimezoneCount();
  for (size_t i = 0; i < tzCount; i++) {
    if (tzList[i].offsetMinutes == gSettings.tzOffsetMinutes) {
      setWizardTimezoneSelection(i);
      break;
    }
  }

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
        if (renderWiFiPage(result)) {
          result.completed = true;
          running = false;
        }
        // renderWiFiPage() called wizardPrevPage() internally on back press
        lastPrintedPage = WIZARD_PAGE_COUNT;  // Force reprint on return
        continue;
      }
#endif
      handleSerialWiFiPage(result, running);
      continue;
    }

    // ------------------------------------------------------------------
    // 2. OLED rendering for pages 1-4 (when display connected)
    // ------------------------------------------------------------------
#if ENABLE_OLED_DISPLAY
    if (oledDisplay && oledConnected) {
      switch (currentPage) {
        case WIZARD_PAGE_FEATURES: renderFeaturesPage(); break;
        case WIZARD_PAGE_SENSORS:  renderSensorsPage();  break;
        case WIZARD_PAGE_NETWORK:  renderNetworkPage();  break;
        case WIZARD_PAGE_SYSTEM:   renderSystemPage();   break;
        default: running = false;  break;
      }
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
      input.trim();
      input.toLowerCase();

      if (input == "n" || input == "next") {
#if ENABLE_OLED_DISPLAY
        JoystickNav fakeRight = {false, false, false, true};
        switch (currentPage) {
          case WIZARD_PAGE_FEATURES: handleFeaturesInput(0, fakeRight); break;
          case WIZARD_PAGE_SENSORS:  handleSensorsInput(0, fakeRight);  break;
          case WIZARD_PAGE_NETWORK:  handleNetworkInput(0, fakeRight);  break;
          case WIZARD_PAGE_SYSTEM:
            if (!handleSystemInput(0, fakeRight, result)) running = false;
            break;
          default: break;
        }
#else
        if (!wizardNextPage(result)) running = false;
#endif
        serialHandled = true;
      } else if (input == "b" || input == "back") {
#if ENABLE_OLED_DISPLAY
        JoystickNav fakeLeft = {false, false, true, false};
        switch (currentPage) {
          case WIZARD_PAGE_FEATURES: handleFeaturesInput(0, fakeLeft); break;
          case WIZARD_PAGE_SENSORS:  handleSensorsInput(0, fakeLeft);  break;
          case WIZARD_PAGE_NETWORK:  handleNetworkInput(0, fakeLeft);  break;
          case WIZARD_PAGE_SYSTEM:   handleSystemInput(0, fakeLeft, result); break;
          default: break;
        }
#else
        wizardPrevPage();
#endif
        serialHandled = true;
      } else if (input.length() > 0) {
        int num = input.toInt();
        if (num > 0) {
          setWizardCurrentSelection(num - 1);
          if (currentPage == WIZARD_PAGE_SYSTEM) {
            if (getSystemItemAt(num - 1) == SYS_ITEM_DEVICE_NAME) {
              // Text input for device name — can't cycle
              Serial.printf("Device Name [%s]: ", wizardDeviceName);
              String newName = waitForSerialInputBlocking();
              newName.trim();
              if (newName.length() > 0) {
                strncpy(wizardDeviceName, newName.c_str(), sizeof(wizardDeviceName) - 1);
                wizardDeviceName[sizeof(wizardDeviceName) - 1] = '\0';
              }
            } else {
              wizardCycleOption();
            }
          } else {
            wizardToggleCurrentItem();
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
      uint32_t buttons = lastButtons;
      bool haveButtons = false;
      {
        SensorCacheGuard g(gInputCache.mutex, pdMS_TO_TICKS(10), "wizard.buttonRead");
        if (g.held && gInputCache.dataValid) {
          buttons = gInputCache.buttons;
          haveButtons = true;
        }
      }

      if (haveButtons && !lastButtonsInitialized) {
        lastButtons = buttons;
        lastButtonsInitialized = true;
        continue;
      }

      uint32_t pressedNow  = ~buttons;
      uint32_t pressedLast = ~lastButtons;
      uint32_t newButtons  = pressedNow & ~pressedLast;
      lastButtons = buttons;

      JoystickNav nav = readWizardJoystickNav();
      bool hasInput = (newButtons != 0) || nav.up || nav.down || nav.left || nav.right;
      if (!hasInput) continue;

      bool handled = false;
      switch (currentPage) {
        case WIZARD_PAGE_FEATURES: handled = handleFeaturesInput(newButtons, nav); break;
        case WIZARD_PAGE_SENSORS:  handled = handleSensorsInput(newButtons, nav);  break;
        case WIZARD_PAGE_NETWORK:  handled = handleNetworkInput(newButtons, nav);  break;
        case WIZARD_PAGE_SYSTEM:
          if (!handleSystemInput(newButtons, nav, result)) running = false;
          handled = true;
          break;
        default: break;
      }

      if (handled) {
        lastPrintedSel = -1;
        delay(150);
      }
    }
#endif
  }

  wizardFinalize(result);

  return result;
}

// ============================================================================
// runAndApplyFeatureWizard - shared entry point for feature configuration.
// Runs the wizard, saves any WiFi credentials entered, and persists settings.
// Used by both cmd_featuresetup (CLI) and firstTimeSetupIfNeeded() (FTS).
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
  // milliseconds of idleness (CLI featuresetup behavior).
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
  // CLI timeout.
  setSerialWaitTimeout(0);

  if (isWizardCancelRequested()) {
    broadcastOutput("Feature setup timed out. No changes saved.");
    result.completed = false;
    return result;
  }

  if (!result.completed) {
    broadcastOutput("Feature setup cancelled. No changes saved.");
    return result;
  }

  broadcastOutput("Feature configuration complete.");
  broadcastOutput("Timezone: " + result.timezoneAbbrev);
  {
    uint32_t usedKB = 0, totalKB = 1;
    int pct = 0;
    getHeapBarData(&usedKB, &totalKB, &pct);
    uint32_t estFreeKB = (usedKB >= totalKB) ? 0 : (totalKB - usedKB);
    char buf[64];
    snprintf(buf, sizeof(buf), "Heap estimate: ~%lu KB", (unsigned long)estFreeKB);
    broadcastOutput(buf);
  }

#if ENABLE_WIFI
  if (result.wifiConfigured && result.wifiSSID.length() > 0) {
    extern bool upsertWiFiNetwork(const String& ssid, const String& password, int priority, bool hidden);
    extern void sortWiFiByPriority();
    extern bool saveWiFiNetworks();
    upsertWiFiNetwork(result.wifiSSID, result.wifiPassword, 1, false);
    sortWiFiByPriority();
    saveWiFiNetworks();
    setSetting(gSettings.wifiAutoStart, true);
    broadcastOutput("WiFi credentials saved: " + result.wifiSSID);
  }
#endif

  writeSettingsJson();  // flush any other wizard changes made above
  applySettings();
#if ENABLE_WIFI
  // Re-register SNTP with the wizard's NTP choice — applySettings() does
  // not reach setupNTP(), so without this the new server only took effect
  // on the next reboot.
  extern void setupNTP();
  if (WiFi.isConnected()) setupNTP();
#endif

  return result;
}

