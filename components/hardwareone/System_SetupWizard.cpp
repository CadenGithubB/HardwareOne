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

extern Settings gSettings;
extern bool writeSettingsJson();
extern String waitForSerialInputBlocking();
extern void broadcastOutput(const String& s);

// ============================================================================
// Time Zone Data
// ============================================================================

static const TimezoneEntry timezones[] = {
  { "UTC",  "UTC (GMT)",           0 },
  { "EST",  "Eastern US",       -300 },  // UTC-5
  { "CST",  "Central US",       -360 },  // UTC-6
  { "MST",  "Mountain US",      -420 },  // UTC-7
  { "PST",  "Pacific US",       -480 },  // UTC-8
  { "AKST", "Alaska",           -540 },  // UTC-9
  { "HST",  "Hawaii",           -600 },  // UTC-10
  { "GMT",  "UK/London",           0 },  // UTC+0
  { "CET",  "Central Europe",    60 },   // UTC+1
  { "EET",  "Eastern Europe",   120 },   // UTC+2
  { "IST",  "India",            330 },   // UTC+5:30
  { "SGT",  "Singapore",        480 },   // UTC+8
  { "JST",  "Japan",            540 },   // UTC+9
  { "AEST", "Australia East",   600 },   // UTC+10
  { "NZST", "New Zealand",      720 },   // UTC+12
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
// Wizard State
// ============================================================================

static SetupWizardPage currentPage = WIZARD_PAGE_FEATURES;
static int currentSelection = 0;
static int scrollOffset = 0;
static int timezoneSelection = 1;  // EST default
static int logLevelSelection = 3;  // DEBUG default (all logging enabled)

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
    
    if (f->category == FEATURE_CAT_NETWORK && featuresPageCount < 16) {
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
  timezoneSelection = 1;  // EST
  logLevelSelection = 3;  // DEBUG (all logging enabled)
  
  // Find current timezone in list
  for (size_t i = 0; i < timezoneCount; i++) {
    if (timezones[i].offsetMinutes == gSettings.tzOffsetMinutes) {
      timezoneSelection = i;
      break;
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
  else if (currentPage == WIZARD_PAGE_SYSTEM) maxItems = 2;
  
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
  else if (currentPage == WIZARD_PAGE_SYSTEM) maxItems = 2;
  
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
    if (currentSelection == 0) {
      timezoneSelection = (timezoneSelection + 1) % timezoneCount;
      return true;
    } else if (currentSelection == 1) {
      logLevelSelection = (logLevelSelection + 1) % logLevelCount;
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
  switch (currentPage) {
    case WIZARD_PAGE_FEATURES:
      currentPage = WIZARD_PAGE_SENSORS;
      currentSelection = 0;
      scrollOffset = 0;
      return true;
      
    case WIZARD_PAGE_SENSORS:
      // Check if we have any network settings to show
      if (hasNetworkSettings()) {
        currentPage = WIZARD_PAGE_NETWORK;
        currentSelection = 0;
        scrollOffset = 0;
      } else {
        // Skip to system settings if no network settings
        currentPage = WIZARD_PAGE_SYSTEM;
        currentSelection = 0;
      }
      return true;
      
    case WIZARD_PAGE_NETWORK:
      currentPage = WIZARD_PAGE_SYSTEM;
      currentSelection = 0;
      return true;
      
    case WIZARD_PAGE_SYSTEM:
      // Save system settings
      result.timezoneOffset = timezones[timezoneSelection].offsetMinutes;
      result.timezoneAbbrev = timezones[timezoneSelection].abbrev;
      gSettings.tzOffsetMinutes = result.timezoneOffset;
      gSettings.logLevel = logLevelSelection;
      
      if (wizardShouldShowWiFi()) {
        currentPage = WIZARD_PAGE_WIFI;
        result.wifiEnabled = true;
        return true;
      } else {
        result.wifiEnabled = false;
        result.completed = true;
        return false;  // Signal completion
      }
      
    case WIZARD_PAGE_WIFI:
      result.completed = true;
      return false;  // Signal completion
      
    default:
      return false;
  }
}

bool wizardPrevPage() {
  switch (currentPage) {
    case WIZARD_PAGE_SENSORS:
      currentPage = WIZARD_PAGE_FEATURES;
      currentSelection = 0;
      scrollOffset = 0;
      return true;
      
    case WIZARD_PAGE_NETWORK:
      currentPage = WIZARD_PAGE_SENSORS;
      currentSelection = 0;
      scrollOffset = 0;
      return true;
      
    case WIZARD_PAGE_SYSTEM:
      // Check if we have any network settings - if not, skip back to sensors
      if (hasNetworkSettings()) {
        currentPage = WIZARD_PAGE_NETWORK;
        currentSelection = 0;
      } else {
        currentPage = WIZARD_PAGE_SENSORS;
        currentSelection = 0;
        scrollOffset = 0;
      }
      return true;
      
    case WIZARD_PAGE_WIFI:
      currentPage = WIZARD_PAGE_SYSTEM;
      currentSelection = 0;
      return true;
      
    default:
      return false;
  }
}

void wizardFinalize(SetupWizardResult& result) {
  result.timezoneOffset = timezones[timezoneSelection].offsetMinutes;
  result.timezoneAbbrev = timezones[timezoneSelection].abbrev;
  gSettings.tzOffsetMinutes = result.timezoneOffset;
  gSettings.logLevel = logLevelSelection;
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
  
  Serial.printf(" 1. Timezone:  %s (%s)\n", 
    timezones[timezoneSelection].abbrev, 
    timezones[timezoneSelection].name);
  Serial.printf(" 2. Log level: %s\n", logLevelNames[logLevelSelection]);
  
  Serial.println("----------------------------------------");
  Serial.println("Enter number to cycle, 'n' for next, 'b' for back");
  Serial.print("> ");
}

SetupWizardResult runSerialSetupWizard() {
  SetupWizardResult result;
  result.completed = false;
  result.wifiEnabled = false;
  result.wifiConfigured = false;
  result.deviceName = "HardwareOne";
  result.timezoneOffset = -300;
  result.timezoneAbbrev = "EST";
  
  initSetupWizard();
  
  Serial.println();
  Serial.println("========================================");
  Serial.println("       FEATURE CONFIGURATION WIZARD    ");
  Serial.println("========================================");
  Serial.println("Configure which features to enable.");
  Serial.println("Features marked with * are essential.");
  Serial.println();
  
  bool running = true;
  
  while (running) {
    // Display current page
    switch (currentPage) {
      case WIZARD_PAGE_FEATURES:
        printSerialFeaturePage("Network Features", featuresPage, featuresPageCount);
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
          Serial.println("Enter WiFi network number, or type SSID (or press Enter to skip):");
          Serial.print("> ");
          String ssidInput = waitForSerialInputBlocking();
          ssidInput.trim();
          String ssid = ssidInput;
          int idx = ssidInput.toInt();
          if (idx > 0 && idx <= n) {
            ssid = WiFi.SSID(idx - 1);
          }
          WiFi.scanDelete();

          if (ssid.length() > 0) {
            Serial.println("Enter WiFi password:");
            Serial.print("> ");
            String pass = waitForSerialInputBlocking();
            pass.trim();
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
