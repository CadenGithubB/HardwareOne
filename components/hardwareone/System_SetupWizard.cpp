/**
 * System Setup Wizard Implementation
 * 
 * Core logic for setup wizard - display agnostic
 */

#include "System_SetupWizard.h"
#include "System_FeatureRegistry.h"
#include "System_Settings.h"
#include "System_BuildConfig.h"

#if ENABLE_WIFI
#include <WiFi.h>
#endif

#if ENABLE_OLED_DISPLAY
#include "OLED_SetupWizard.h"
#include "HAL_Display.h"
#include "HAL_Input.h"
extern bool oledConnected;
extern ControlCache gControlCache;
// OLED page handlers for ESP-NOW and MQTT (defined in OLED_SetupWizard.cpp)
void handleOLEDESPNowPage(SetupWizardResult& result, bool& running);
void handleOLEDMQTTPage(SetupWizardResult& result, bool& running);
#endif

extern String waitForSerialInputBlocking();

// ============================================================================
// Time Zone Data
// ============================================================================

static const TimezoneEntry timezones[] = {
  // UTC
  { "UTC",  "UTC / GMT (0:00)",                  0 },
  // North America — Daylight Saving (summer)
  { "EDT",  "US Eastern Daylight (-4:00)",     -240 },
  { "CDT",  "US Central Daylight (-5:00)",     -300 },
  { "MDT",  "US Mountain Daylight (-6:00)",    -360 },
  { "PDT",  "US Pacific Daylight (-7:00)",     -420 },
  { "AKDT", "Alaska Daylight (-8:00)",         -480 },
  // North America — Standard Time (winter)
  { "EST",  "US Eastern Standard (-5:00)",     -300 },
  { "CST",  "US Central Standard (-6:00)",     -360 },
  { "MST",  "US Mountain Standard (-7:00)",    -420 },
  { "PST",  "US Pacific Standard (-8:00)",     -480 },
  { "AKST", "Alaska Standard (-9:00)",         -540 },
  { "HST",  "Hawaii (-10:00)",                 -600 },
  // Europe
  { "GMT",  "UK / Ireland (0:00)",                0 },
  { "BST",  "UK Summer Time (+1:00)",            60 },
  { "CET",  "Central Europe (+1:00)",            60 },
  { "CEST", "Central Europe Summer (+2:00)",    120 },
  { "EET",  "Eastern Europe (+2:00)",           120 },
  { "EEST", "Eastern Europe Summer (+3:00)",    180 },
  // Asia / Pacific
  { "IST",  "India (+5:30)",                    330 },
  { "SGT",  "Singapore (+8:00)",                480 },
  { "JST",  "Japan (+9:00)",                    540 },
  { "AEST", "Australia East (+10:00)",          600 },
  { "AEDT", "Australia East Summer (+11:00)",   660 },
  { "NZST", "New Zealand (+12:00)",             720 },
  { "NZDT", "New Zealand Summer (+13:00)",      780 },
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

static const char* ntpPresets[] = {
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

static const char* ledEffects[] = {
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

// Feature items per page
static WizardFeatureItem featuresPage[16];
static size_t featuresPageCount = 0;

static WizardFeatureItem sensorsPage[16];
static size_t sensorsPageCount = 0;

static WizardNetworkItem networkPage[8];
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
  const FeatureEntry* wifiFeature = getFeatureById("wifi");
  return wifiFeature && isFeatureEnabled(wifiFeature);
#else
  return false;
#endif
}

static bool systemPageHasLED() {
  const FeatureEntry* ledFeature = getFeatureById("led");
  return ledFeature && isFeatureCompiled(ledFeature);
}

size_t getWizardSystemPageCount() {
  size_t count = 2; // timezone + log level always present
  if (systemPageHasNTP()) count++;
  if (systemPageHasLED()) count++;
  return count;
}

// Map system page selection index to logical item
// 0=log level, 1=timezone, 2=NTP (if visible), 3=LED (if visible)
enum SystemPageItem {
  SYS_ITEM_LOGLEVEL = 0,
  SYS_ITEM_TIMEZONE,
  SYS_ITEM_NTP,
  SYS_ITEM_LED
};

static SystemPageItem getSystemItemAt(int index) {
  if (index == 0) return SYS_ITEM_LOGLEVEL;
  if (index == 1) return SYS_ITEM_TIMEZONE;
  int nextIdx = 2;
  if (systemPageHasNTP()) {
    if (index == nextIdx) return SYS_ITEM_NTP;
    nextIdx++;
  }
  if (systemPageHasLED()) {
    if (index == nextIdx) return SYS_ITEM_LED;
  }
  return SYS_ITEM_TIMEZONE; // fallback
}

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
 
   const FeatureEntry* gamepadFeature = getFeatureById("gamepad");
   if (gamepadFeature && isFeatureCompiled(gamepadFeature)) infraKB += gamepadFeature->heapCostKB;
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

void getHeapBarData(uint32_t* enabledKB, uint32_t* maxKB, int* percentage) {
  uint32_t totalHeapKB = (uint32_t)(ESP.getHeapSize() / 1024);
  if (totalHeapKB == 0) totalHeapKB = 1;
 
   if (!sWizardBaselineCalibrated) {
     calibrateWizardBaseline();
   }

  uint32_t enabledCostKB = getEnabledFeaturesHeapEstimate();
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
      featuresPage[featuresPageCount].heapKB = f->heapCostKB;
      featuresPage[featuresPageCount].setting = f->enabledSetting;
      featuresPage[featuresPageCount].essential = (f->flags & FEATURE_FLAG_ESSENTIAL);
      featuresPage[featuresPageCount].compiled = true;
      featuresPageCount++;
    }
  }
  
  // Build sensors page (display + sensors)
  for (size_t i = 0; i < getFeatureCount(); i++) {
    const FeatureEntry* f = getFeatureByIndex(i);
    if (!f || !isFeatureCompiled(f)) continue;
    
    if ((f->category == FEATURE_CAT_DISPLAY || f->category == FEATURE_CAT_SENSOR) && sensorsPageCount < 16) {
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
  // Only show WiFi auto-connect if WiFi feature is enabled
  {
    const FeatureEntry* wifiFeature = getFeatureById("wifi");
    if (wifiFeature && isFeatureEnabled(wifiFeature)) {
      networkPage[networkPageCount].label = "WiFi auto-connect";
      networkPage[networkPageCount].boolSetting = &gSettings.wifiAutoReconnect;
      networkPage[networkPageCount].isBool = true;
      networkPageCount++;
    }
  }
#endif

#if ENABLE_HTTP_SERVER
  // Only show HTTP auto-start if HTTP feature is enabled
  {
    const FeatureEntry* httpFeature = getFeatureById("http");
    if (httpFeature && isFeatureEnabled(httpFeature)) {
      networkPage[networkPageCount].label = "HTTP auto-start";
      networkPage[networkPageCount].boolSetting = &gSettings.httpAutoStart;
      networkPage[networkPageCount].isBool = true;
      networkPageCount++;
    }
  }
#endif

#if ENABLE_BLUETOOTH
  // Only show BT auto-start if Bluetooth feature is enabled
  {
    const FeatureEntry* btFeature = getFeatureById("bluetooth");
    if (btFeature && isFeatureEnabled(btFeature)) {
      networkPage[networkPageCount].label = "BT auto-start";
      networkPage[networkPageCount].boolSetting = &gSettings.bluetoothAutoStart;
      networkPage[networkPageCount].isBool = true;
      networkPageCount++;
    }
  }
#endif

#if ENABLE_ESPNOW
  // Only show ESP-NOW mesh if ESP-NOW feature is enabled
  {
    const FeatureEntry* espnowFeature = getFeatureById("espnow");
    if (espnowFeature && isFeatureEnabled(espnowFeature)) {
      networkPage[networkPageCount].label = "ESP-NOW mesh";
      networkPage[networkPageCount].boolSetting = &gSettings.espnowmesh;
      networkPage[networkPageCount].isBool = true;
      networkPageCount++;
    }
  }
#endif

#if ENABLE_MQTT
  // Only show MQTT auto-start if MQTT feature is enabled
  {
    const FeatureEntry* mqttFeature = getFeatureById("mqtt");
    if (mqttFeature && isFeatureEnabled(mqttFeature)) {
      networkPage[networkPageCount].label = "MQTT auto-start";
      networkPage[networkPageCount].boolSetting = &gSettings.mqttAutoStart;
      networkPage[networkPageCount].isBool = true;
      networkPageCount++;
    }
  }
#endif
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

bool wizardIsPageVisible(SetupWizardPage page) {
  switch (page) {
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
    gSettings.logLevel = logLevelSelection;
    if (systemPageHasNTP()) {
      result.ntpServer = ntpPresets[ntpSelection];
    }
    if (systemPageHasLED()) {
      result.ledStartupEffect = ledEffects[ledEffectSelection];
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
  Serial.println("=== Network Settings ===");
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

  Serial.println("----------------------------------------");
  Serial.println("Enter number to cycle, 'n' for next, 'b' for back");
  Serial.print("> ");
}

// ============================================================================
// Shared serial page printer (used by runSetupWizard for both OLED and no-OLED)
// ============================================================================

static void printSerialPageStatus() {
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
      Serial.printf("  SETUP %d/%d: Network Settings\n", pageNum, totalPages);
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
  Serial.println(" s = Skip (leave as-is)");
  Serial.println(" b = Back");
  Serial.print("Choice [s]: ");
  String introChoice = waitForSerialInputBlocking();
  introChoice.trim();
  if (introChoice.equalsIgnoreCase("b") || introChoice.equalsIgnoreCase("back")) {
    wizardPrevPage();
    return;
  }
  if (!introChoice.equalsIgnoreCase("c") && !introChoice.equalsIgnoreCase("configure")) {
    // Default is skip
    if (!wizardNextPage(result)) running = false;
    return;
  }

  Serial.println("----------------------------------------");
  Serial.println("All fields optional. Press Enter to skip, 'b' to go back.");
  Serial.println("----------------------------------------");

  Serial.print("Room (e.g. 'Living Room'): ");
  String room = waitForSerialInputBlocking();
  room.trim();
  if (room.equalsIgnoreCase("b") || room.equalsIgnoreCase("back")) {
    wizardPrevPage();
    return;
  }
  result.espnowRoom = room;

  Serial.print("Zone (e.g. 'North Wall'): ");
  String zone = waitForSerialInputBlocking();
  zone.trim();
  if (zone.equalsIgnoreCase("b") || zone.equalsIgnoreCase("back")) {
    wizardPrevPage();
    return;
  }
  result.espnowZone = zone;

  // Friendly name mirrors the device name set at the start of setup
  result.espnowFriendlyName = gSettings.espnowDeviceName;

  Serial.print("The device will be — (m)obile or (s)tationary [m]: ");
  String stat = waitForSerialInputBlocking();
  stat.trim();
  if (stat.equalsIgnoreCase("b") || stat.equalsIgnoreCase("back")) {
    wizardPrevPage();
    return;
  }
  result.espnowStationary = (stat.equalsIgnoreCase("s") || stat.equalsIgnoreCase("stationary"));

  // Apply to settings
  if (result.espnowRoom.length() > 0) gSettings.espnowRoom = result.espnowRoom;
  if (result.espnowZone.length() > 0) gSettings.espnowZone = result.espnowZone;
  if (result.espnowFriendlyName.length() > 0) gSettings.espnowFriendlyName = result.espnowFriendlyName;
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
  Serial.println(" s = Skip (leave as-is)");
  Serial.println(" b = Back");
  Serial.print("Choice [s]: ");
  String introChoice = waitForSerialInputBlocking();
  introChoice.trim();
  if (introChoice.equalsIgnoreCase("b") || introChoice.equalsIgnoreCase("back")) {
    wizardPrevPage();
    return;
  }
  if (!introChoice.equalsIgnoreCase("c") && !introChoice.equalsIgnoreCase("configure")) {
    // Default is skip
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

// Serial WiFi page helper - full scan + numbered list
static void handleSerialWiFiPage(SetupWizardResult& result, bool& running) {
  int pageNum = getWizardPageNumber(WIZARD_PAGE_WIFI);
  int totalPages = getWizardTotalPages();
  Serial.println();
  Serial.printf("=== WiFi Setup (SETUP %d/%d) ===\n", pageNum, totalPages);
  bool wifiPageDone = false;
  while (!wifiPageDone) {
#if ENABLE_WIFI
    int n = WiFi.scanNetworks(false, true);
    if (n > 0) {
      Serial.printf("Found %d networks:\n", n);
      for (int i = 0; i < n && i < 10; i++) {
        Serial.printf("  %d. %-24s  %lddBm  %s\n",
          i + 1, WiFi.SSID(i).c_str(), (long)WiFi.RSSI(i),
          (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "Open" : "Secured");
      }
      if (n > 10) Serial.printf("  ... and %d more\n", n - 10);
    } else {
      Serial.println("No WiFi networks found.");
    }
    Serial.println("----------------------------------------");
    Serial.println("Enter number, SSID directly, 'rescan', or 'skip':");
    Serial.println("('b' or 'back' to return to previous page)");
    Serial.print("> ");
    while (!Serial.available()) { delay(10); }
    String ssidInput = Serial.readStringUntil('\n');
    ssidInput.trim();
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
    if (idx > 0 && idx <= n) {
      ssid = WiFi.SSID(idx - 1);
    }
    WiFi.scanDelete();
    if (ssid.length() > 0) {
      Serial.println("Enter WiFi password (or 'b' to go back):");
      Serial.print("> ");
      while (!Serial.available()) { delay(10); }
      String pass = Serial.readStringUntil('\n');
      pass.trim();
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
    SetupWizardPage currentPage = getWizardCurrentPage();
    int currentSel = getWizardCurrentSelection();

    // ------------------------------------------------------------------
    // 1. Pages with their own input loop (ESP-NOW, MQTT, WiFi)
    // ------------------------------------------------------------------
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
            wizardCycleOption();
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
      if (gControlCache.mutex && xSemaphoreTake(gControlCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (gControlCache.gamepadDataValid) {
          buttons = gControlCache.gamepadButtons;
          haveButtons = true;
        }
        xSemaphoreGive(gControlCache.mutex);
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
// runSerialSetupWizard - delegates to runSetupWizard() when OLED compiled;
// remains a real implementation only for ENABLE_OLED_DISPLAY=0 builds.
// ============================================================================

#if ENABLE_OLED_DISPLAY
SetupWizardResult runSerialSetupWizard() {
  return runSetupWizard();
}
#else
// NOTE: This is only the active path when ENABLE_OLED_DISPLAY=0.
// All wizard changes should be made to runSetupWizard() above.
SetupWizardResult runSerialSetupWizard() {
  SetupWizardResult result;
  result.completed = false;
  result.wifiEnabled = false;
  result.wifiConfigured = false;
  result.deviceName = "HardwareOne";
  result.timezoneOffset = -240;
  result.timezoneAbbrev = "EDT";
  
  initSetupWizard();
  
  Serial.println();
  Serial.println("========================================");
  Serial.println("       FEATURE CONFIGURATION WIZARD    ");
  Serial.println("========================================");
  Serial.println("Configure which features to enable.");
  Serial.println();
  
  bool running = true;
  
  while (running) {
    // Display current page
    switch (currentPage) {
      case WIZARD_PAGE_FEATURES:
        printSerialFeaturePage("Features", featuresPage, featuresPageCount);
        break;
        
      case WIZARD_PAGE_SENSORS:
        printSerialFeaturePage("Display & Sensors", sensorsPage, sensorsPageCount);
        break;
        
      case WIZARD_PAGE_NETWORK:
        printSerialNetworkPage();
        break;
        
      case WIZARD_PAGE_SYSTEM:
        printSerialSystemPage();
        break;
        
      case WIZARD_PAGE_WIFI:
        // Handle WiFi setup
        Serial.println();
        Serial.println("=== WiFi Setup ===");
        printSerialHeapBar();
        Serial.println("----------------------------------------");
        {
#if ENABLE_WIFI
          int n = WiFi.scanNetworks(false, true);
          if (n > 0) {
            Serial.printf("Found %d networks:\n", n);
            for (int i = 0; i < n && i < 10; i++) {
              Serial.printf("  %d. %-24s  %lddBm  %s\n",
                i + 1,
                WiFi.SSID(i).c_str(),
                (long)WiFi.RSSI(i),
                (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "Open" : "Secured");
            }
            if (n > 10) Serial.printf("  ... and %d more\n", n - 10);
          } else {
            Serial.println("No WiFi networks found");
          }
          Serial.println("----------------------------------------");
          Serial.println("Enter number to select, type an SSID directly, 'rescan' to refresh, or 'skip':");
          Serial.println("('b' or 'back' to return to previous page)");
          Serial.print("> ");
          String ssidInput = waitForSerialInputBlocking();
          ssidInput.trim();
          if (ssidInput.equalsIgnoreCase("b") || ssidInput.equalsIgnoreCase("back")) {
            WiFi.scanDelete();
            wizardPrevPage();
            continue;
          }
          if (ssidInput.equalsIgnoreCase("skip") || ssidInput.length() == 0) {
            WiFi.scanDelete();
            result.completed = true;
            running = false;
            continue;
          }
          String ssid = ssidInput;
          int idx = ssidInput.toInt();
          if (idx > 0 && idx <= n) {
            ssid = WiFi.SSID(idx - 1);
          }
          WiFi.scanDelete();
          if (ssid.length() > 0) {
            Serial.println("Enter WiFi password (or 'b' to go back):");
            Serial.print("> ");
            String pass = waitForSerialInputBlocking();
            pass.trim();
            if (pass.equalsIgnoreCase("b") || pass.equalsIgnoreCase("back")) {
              continue;  // Loop back to WiFi page (re-scan)
            }
            result.wifiSSID = ssid;
            result.wifiPassword = pass;
            result.wifiConfigured = true;
          }
#else
          Serial.println("WiFi not compiled in this build");
#endif
        }
        result.completed = true;
        running = false;
        continue;
        
      default:
        running = false;
        continue;
    }
    
    // Get input
    String input = waitForSerialInputBlocking();
    input.trim();
    input.toLowerCase();
    
    if (input == "n" || input == "next") {
      if (!wizardNextPage(result)) {
        running = false;
      }
    } else if (input == "b" || input == "back") {
      wizardPrevPage();
    } else if (input.length() > 0) {
      int num = input.toInt();
      if (num > 0) {
        currentSelection = num - 1;
        if (currentPage == WIZARD_PAGE_SYSTEM) {
          wizardCycleOption();
        } else {
          wizardToggleCurrentItem();
        }
      }
    }
  }
  
  wizardFinalize(result);
  
  Serial.println();
  Serial.println("========================================");
  Serial.println("    CONFIGURATION COMPLETE!");
  Serial.printf("    Timezone: %s\n", result.timezoneAbbrev.c_str());
  {
    uint32_t usedKB = 0;
    uint32_t totalKB = 1;
    int pct = 0;
    getHeapBarData(&usedKB, &totalKB, &pct);
    uint32_t estFreeKB = (usedKB >= totalKB) ? 0 : (totalKB - usedKB);
    Serial.printf("    Heap estimate: ~%luKB\n", (unsigned long)estFreeKB);
  }
  Serial.println("========================================");
  Serial.println();
  
  return result;
}
#endif // !ENABLE_OLED_DISPLAY
