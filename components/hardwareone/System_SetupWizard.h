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
  WIZARD_PAGE_SYSTEM,         // Time zone, log level
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

// Check if wizard should show WiFi page
bool wizardShouldShowWiFi();

// Finalize wizard results
void wizardFinalize(SetupWizardResult& result);

// ============================================================================
// Serial Console Wizard
// ============================================================================

// Run wizard via serial console (blocking)
SetupWizardResult runSerialSetupWizard();

// ============================================================================
// Heap Bar Helper
// ============================================================================

// Get heap bar data
void getHeapBarData(uint32_t* enabledKB, uint32_t* maxKB, int* percentage);

#endif // SYSTEM_SETUP_WIZARD_H
