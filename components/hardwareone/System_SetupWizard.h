/**
 * System Setup Wizard
 * 
 * Core logic for multi-page first-time setup wizard.
 * Display-agnostic - can be rendered on OLED or Serial console.
 */

#ifndef SYSTEM_SETUP_WIZARD_H
#define SYSTEM_SETUP_WIZARD_H

#include <Arduino.h>

// ============================================================================
// Wizard Pages and State
// ============================================================================

enum SetupWizardPage {
  WIZARD_PAGE_FEATURES = 0,   // Network features (WiFi, HTTP, BT, ESP-NOW)
  WIZARD_PAGE_SENSORS,        // Display + I2C sensors
  WIZARD_PAGE_NETWORK,        // Auto-start options, device name
  WIZARD_PAGE_SYSTEM,         // Time zone, log level, NTP, LED effect
  WIZARD_PAGE_ESPNOW,         // ESP-NOW identity (conditional: ESP-NOW enabled)
  WIZARD_PAGE_MQTT,           // MQTT broker config (conditional: MQTT enabled+autostarted)
  WIZARD_PAGE_WIFI,           // WiFi SSID/password (conditional)
  WIZARD_PAGE_COUNT
};

// Wizard result structure
struct SetupWizardResult {
  bool completed;             // User completed wizard (didn't cancel)
  bool wifiEnabled;           // WiFi feature enabled
  bool wifiConfigured;        // WiFi credentials entered
  String wifiSSID;
  String wifiPassword;
  String deviceName;
  int timezoneOffset;         // Minutes from UTC
  String timezoneAbbrev;      // e.g., "EST", "PST"
  // ESP-NOW identity (all optional — empty = use defaults)
  String espnowRoom;
  String espnowZone;
  String espnowFriendlyName;
  bool espnowStationary;      // false = mobile (default)
  // MQTT broker
  String mqttHost;
  int mqttPort;               // 0 = use default (1883)
  String mqttUser;
  String mqttPassword;
  // NTP server
  String ntpServer;
  // LED startup effect
  String ledStartupEffect;
};

// Feature item for display
struct WizardFeatureItem {
  const char* id;
  const char* label;
  uint16_t heapKB;
  bool* setting;
  bool essential;
  bool compiled;
};

// Network settings item
struct WizardNetworkItem {
  const char* label;
  bool* boolSetting;
  String* stringSetting;
  bool isBool;
};

// Timezone entry
struct TimezoneEntry {
  const char* abbrev;
  const char* name;
  int offsetMinutes;
};

// ============================================================================
// Wizard State Access
// ============================================================================

// Get current wizard state
SetupWizardPage getWizardCurrentPage();
int getWizardCurrentSelection();
int getWizardScrollOffset();

// Set wizard state
void setWizardCurrentPage(SetupWizardPage page);
void setWizardCurrentSelection(int sel);
void setWizardScrollOffset(int offset);

// Get page data
size_t getWizardFeaturesPageCount();
size_t getWizardSensorsPageCount();
size_t getWizardNetworkPageCount();
WizardFeatureItem* getWizardFeaturesPage();
WizardFeatureItem* getWizardSensorsPage();
WizardNetworkItem* getWizardNetworkPage();

// Timezone/log level
size_t getTimezoneCount();
const TimezoneEntry* getTimezones();
int getWizardTimezoneSelection();
void setWizardTimezoneSelection(int sel);
int getWizardLogLevelSelection();
void setWizardLogLevelSelection(int sel);
const char** getLogLevelNames();
size_t getLogLevelCount();

// Network page builder
void rebuildNetworkSettingsPage();
bool hasNetworkSettings();

// System page items
size_t getWizardSystemPageCount();
int getWizardNTPSelection();
void setWizardNTPSelection(int sel);
int getWizardLEDEffectSelection();
void setWizardLEDEffectSelection(int sel);
const char* const* getNTPPresets();
size_t getNTPPresetCount();
const char* const* getLEDEffects();
size_t getLEDEffectCount();

// ============================================================================
// Wizard Actions
// ============================================================================

// Initialize wizard (build feature lists, set defaults)
void initSetupWizard();

// Handle input actions (returns true if state changed)
bool wizardToggleCurrentItem();
bool wizardMoveUp();
bool wizardMoveDown();
bool wizardNextPage(SetupWizardResult& result);
bool wizardPrevPage();
bool wizardCycleOption();  // For system page options

// Page visibility and navigation
bool wizardIsPageVisible(SetupWizardPage page);
SetupWizardPage wizardAdvanceFrom(SetupWizardPage current);
SetupWizardPage wizardRetreatFrom(SetupWizardPage current);
int getWizardTotalPages();
int getWizardPageNumber(SetupWizardPage page);
bool wizardShouldShowWiFi();
bool wizardShouldShowESPNow();
bool wizardShouldShowMQTT();

// Finalize wizard results
void wizardFinalize(SetupWizardResult& result);

// ============================================================================
// Unified Wizard (single implementation - serial always, OLED optional)
// ============================================================================

// The one wizard to rule them all. Serial is always active; when
// ENABLE_OLED_DISPLAY=1 and the display is connected at runtime, OLED
// rendering and joystick input are layered on top automatically.
SetupWizardResult runSetupWizard();

// Serial-only fallback for builds without OLED compiled in.
// When ENABLE_OLED_DISPLAY=1, this just calls runSetupWizard().
SetupWizardResult runSerialSetupWizard();

// ============================================================================
// Heap Bar Helper
// ============================================================================

// Get heap bar data
void getHeapBarData(uint32_t* enabledKB, uint32_t* maxKB, int* percentage);

#endif // SYSTEM_SETUP_WIZARD_H
