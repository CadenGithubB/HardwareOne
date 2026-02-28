/**
 * @file first_time_setup.cpp
 * @brief First-time device setup and initialization
 * 
 * Handles initial device configuration when no user data exists.
 * Prompts for admin credentials and WiFi settings via serial console.
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

#include "OLED_Display.h"
#include "OLED_FirstTimeSetup.h"
#include "System_BuildConfig.h"
#include "System_Debug.h"
#include "System_FirstTimeSetup.h"
#include "System_MemUtil.h"
#include "System_I2C.h"
#include "System_SensorStubs.h"
#include "System_Settings.h"
#include "System_UserSettings.h"
#include "OLED_SetupWizard.h"
#include "System_SetupWizard.h"
#include "System_FeatureRegistry.h"

// ============================================================================
// Global Variables
// ============================================================================

bool gFirstTimeSetupPerformed = false;
volatile FirstTimeSetupState gFirstTimeSetupState = SETUP_NOT_NEEDED;
volatile SetupProgressStage gSetupProgressStage = SETUP_PROMPT_USERNAME;

// File paths
#define SETTINGS_JSON_FILE "/system/settings.json"
#define USERS_JSON_FILE "/system/users/users.json"
#define AUTOMATIONS_JSON_FILE "/system/automations.json"

// Global variables
extern uint32_t gBootSeq;
extern uint32_t gBootCounter;
extern int gWifiNetworkCount;

// Utility functions
extern String waitForSerialInputBlocking();
extern String hashUserPassword(const String& plaintext);
#if ENABLE_AUTOMATION
extern bool writeAutomationsJsonAtomic(const String& json);
#endif
extern void runUnifiedSystemCommand(const String& cmd);
extern void resolvePendingUserCreationTimes();

// DEBUG_SYSTEMF now defined in debug_system.h with performance optimizations

// BROADCAST_PRINTF now defined in debug_system.h with performance optimizations

// ============================================================================
// First-Time Setup Implementation
// ============================================================================

// ============================================================================
// State Management Implementation
// ============================================================================

void detectFirstTimeSetupState() {
  // Use USERS_JSON_FILE as the determinant - this is the actual indicator
  // that first-time setup has been completed. Settings can exist without users.
  bool usersExist = LittleFS.exists(USERS_JSON_FILE);
  gFirstTimeSetupState = usersExist ? SETUP_NOT_NEEDED : SETUP_REQUIRED;
  
  DEBUG_SYSTEMF("[SETUP_STATE] Early detection: %s (users file exists: %s)", 
                gFirstTimeSetupState == SETUP_NOT_NEEDED ? "NOT_NEEDED" : "REQUIRED",
                usersExist ? "YES" : "NO");
  
  // Also broadcast to serial for immediate feedback
  if (gFirstTimeSetupState == SETUP_REQUIRED) {
    broadcastOutput("");
    broadcastOutput("=== FIRST-TIME SETUP DETECTED ===");
    broadcastOutput("Users file not found - setup required");
    broadcastOutput("OLED should show setup message");
    broadcastOutput("===================================");
    broadcastOutput("");
  }
}

bool isFirstTimeSetup() {
  return gFirstTimeSetupState != SETUP_NOT_NEEDED;
}

void setFirstTimeSetupState(FirstTimeSetupState state) {
  gFirstTimeSetupState = state;
  DEBUG_SYSTEMF("[SETUP_STATE] State changed to: %d", (int)state);
}

void setSetupProgressStage(SetupProgressStage stage) {
  gSetupProgressStage = stage;
  DEBUG_SYSTEMF("[SETUP_PROGRESS] Stage changed to: %d", (int)stage);
}

const char* getSetupProgressMessage(SetupProgressStage stage) {
  static const char* messages[] = {
    "Enter username...",      // SETUP_PROMPT_USERNAME
    "Enter password...",      // SETUP_PROMPT_PASSWORD  
    "Configure WiFi...",      // SETUP_PROMPT_WIFI
    "Configure hardware...",  // SETUP_PROMPT_HARDWARE
    "Saving settings...",     // SETUP_SAVING_CONFIG
    "Setup complete!"         // SETUP_FINISHED
  };
  
  if (stage < sizeof(messages) / sizeof(messages[0])) {
    return messages[stage];
  }
  return "Unknown stage...";
}

// ============================================================================
// First-Time Setup Implementation
// ============================================================================

// Forward declaration for OLED setup mode selection
#if ENABLE_OLED_DISPLAY
extern bool getOLEDSetupModeSelection(bool& advancedMode);
#endif

void firstTimeSetupIfNeeded() {
  // Check current state instead of filesystem
  if (gFirstTimeSetupState == SETUP_NOT_NEEDED) {
    return;  // Already configured
  }

  // Update state for OLED animation
  setFirstTimeSetupState(SETUP_IN_PROGRESS);
  
  broadcastOutput("");
  broadcastOutput("FIRST-TIME SETUP");
  broadcastOutput("----------------");
  
  // ============================================================================
  // Setup Mode Selection: Basic vs Advanced
  // ============================================================================
  bool advancedSetup = false;
  
#if ENABLE_OLED_DISPLAY
  // Use OLED selection if available
  if (oledEnabled && oledConnected) {
    getOLEDSetupModeSelection(advancedSetup);
  } else {
#endif
    // Serial-only mode selection
    broadcastOutput("");
    broadcastOutput("Select setup mode:");
    broadcastOutput("  1. Basic Setup   - Quick start (username + password only)");
    broadcastOutput("  2. Advanced Setup - Full configuration wizard");
    broadcastOutput("");
    broadcastOutput("Enter 1 or 2 (default: 1): ");
    
    String modeInput = waitForSerialInputBlocking();
    modeInput.trim();
    advancedSetup = (modeInput == "2" || modeInput.equalsIgnoreCase("advanced"));
#if ENABLE_OLED_DISPLAY
  }
#endif

  broadcastOutput(advancedSetup ? "Advanced setup selected." : "Basic setup selected.");
  broadcastOutput("");
  
  // Username stage
  setSetupProgressStage(SETUP_PROMPT_USERNAME);
  broadcastOutput("Enter admin username (cannot be blank): ");
  String u = "";
  while (u.length() == 0) {
#if ENABLE_OLED_DISPLAY
    u = getOLEDTextInput("Admin Username:", false, "", 32);
#else
    u = waitForSerialInputBlocking();
#endif
    u.trim();
    if (u.length() == 0) {
      broadcastOutput("Username cannot be blank. Please enter admin username: ");
#if ENABLE_OLED_DISPLAY
      showOLEDMessage("Username cannot\nbe blank!", true);
#endif
    }
  }

  // Password stage  
  setSetupProgressStage(SETUP_PROMPT_PASSWORD);
  String p = "";
  while (p.length() == 0) {
    broadcastOutput("Enter admin password (cannot be blank): ");
#if ENABLE_OLED_DISPLAY
    p = getOLEDTextInput("Admin Password:", true, "", 32);
#else
    p = waitForSerialInputBlocking();
#endif
    p.trim();
    if (p.length() == 0) {
      broadcastOutput("Password cannot be blank. Please enter admin password: ");
#if ENABLE_OLED_DISPLAY
      showOLEDMessage("Password cannot\nbe blank!", true);
#endif
    }
  }

  // Create users.json with admin (ID 1), nextId field, and empty bootAnchors array - hash the password
  String hashedPassword = hashUserPassword(p);
  // At first-time setup, users.json does not exist yet; seed bootCounter starting at 1 and set admin bootCount to 1
  
  // ============================================================================
  // Feature Configuration Wizard (Advanced mode only)
  // ============================================================================
  String wifiSSID = "";
  String wifiPass = "";
  bool wifiConfigured = false;
  bool useDarkTheme = false;  // Theme preference (used when creating user settings)
  
  if (advancedSetup) {
    setSetupProgressStage(SETUP_PROMPT_HARDWARE);
    broadcastOutput("");
    broadcastOutput("Feature Configuration...");
    
    // Run unified setup wizard (works on both Serial AND OLED simultaneously)
    SetupWizardResult wizardResult;
    
#if ENABLE_OLED_DISPLAY
    // Use OLED wizard only if display is actually connected (runtime check)
    // This ensures uniform serial-only experience when no display present
    if (oledEnabled && oledConnected) {
      wizardResult = runOLEDSetupWizard();
    } else {
      wizardResult = runSerialSetupWizard();
    }
#else
    // No OLED compiled - use serial-only wizard
    wizardResult = runSerialSetupWizard();
#endif
    
    if (wizardResult.completed) {
      broadcastOutput("Feature configuration complete.");
      
      // Apply WiFi settings if configured
      if (wizardResult.wifiConfigured && wizardResult.wifiSSID.length() > 0) {
        wifiSSID = wizardResult.wifiSSID;
        wifiPass = wizardResult.wifiPassword;
        wifiConfigured = true;
      }
      
      // Log the selections
      broadcastOutput("Timezone: " + wizardResult.timezoneAbbrev);
      {
        uint32_t usedKB = 0;
        uint32_t totalKB = 1;
        int pct = 0;
        getHeapBarData(&usedKB, &totalKB, &pct);
        uint32_t estFreeKB = (usedKB >= totalKB) ? 0 : (totalKB - usedKB);
        BROADCAST_PRINTF("Heap estimate: ~%lu KB", (unsigned long)estFreeKB);
      }
    }
    
    // Device name customization (advanced mode only)
    // Shows on both Serial AND OLED simultaneously
    broadcastOutput("");
    broadcastOutput("========================================");
    broadcastOutput("       DEVICE NAME");
    broadcastOutput("========================================");
    broadcastOutput("Used for Bluetooth and ESP-NOW identity.");
    broadcastOutput("Press Enter to keep default [HardwareOne]");
    broadcastOutput("----------------------------------------");
    String deviceName = "";
#if ENABLE_OLED_DISPLAY
    // OLED text input already shows prompt on screen, serial sees the broadcastOutput above
    deviceName = getOLEDTextInput("Device Name:", false, "HardwareOne", 20);
#else
    deviceName = waitForSerialInputBlocking();
#endif
    deviceName.trim();
    if (deviceName.length() == 0) {
      deviceName = "HardwareOne";
    }
    // Apply device name to BLE and ESP-NOW
    gSettings.bleDeviceName = deviceName;
    gSettings.espnowDeviceName = deviceName;
    broadcastOutput("Device name set to: " + deviceName);
    
    // Theme preference (for web UI)
    // Shows on both Serial AND OLED simultaneously
    broadcastOutput("");
    broadcastOutput("========================================");
    broadcastOutput("       WEB UI THEME");
    broadcastOutput("========================================");
    broadcastOutput(" 1. Light (default)");
    broadcastOutput(" 2. Dark");
    broadcastOutput("----------------------------------------");
    broadcastOutput("Enter 1 or 2: ");
    String themeInput = "";
#if ENABLE_OLED_DISPLAY
    // For theme, show a simple selection on OLED too
    extern bool getOLEDThemeSelection(bool& darkMode);
    bool darkSelected = false;
    if (oledEnabled && oledConnected && getOLEDThemeSelection(darkSelected)) {
      themeInput = darkSelected ? "2" : "1";
    } else {
      themeInput = waitForSerialInputBlocking();
    }
#else
    themeInput = waitForSerialInputBlocking();
#endif
    themeInput.trim();
    // Store theme choice - will be applied when creating user settings
    useDarkTheme = (themeInput == "2" || themeInput.equalsIgnoreCase("dark"));
    broadcastOutput(useDarkTheme ? "Theme set to: Dark" : "Theme set to: Light");
    
  } else {
    // Basic mode uses defaults (light theme already set above)
    // Basic setup - use sensible defaults
    broadcastOutput("");
    broadcastOutput("Using default settings (Basic mode)");
    gSettings.wifiAutoReconnect = true;
    gSettings.httpAutoStart = true;
  }
  
  // Save WiFi credentials if configured
  if (wifiConfigured && wifiSSID.length() > 0) {
#if ENABLE_WIFI
    extern bool upsertWiFiNetwork(const String& ssid, const String& password, int priority, bool hidden);
    extern void sortWiFiByPriority();
    extern bool saveWiFiNetworks();
    
    upsertWiFiNetwork(wifiSSID, wifiPass, 1, false);
    sortWiFiByPriority();
    saveWiFiNetworks();
    broadcastOutput("WiFi credentials saved: " + wifiSSID);
    gSettings.wifiAutoReconnect = true;
#else
    broadcastOutput("WiFi disabled at compile time");
#endif
  } else {
    gSettings.wifiAutoReconnect = false;
    broadcastOutput("WiFi setup skipped");
  }
  
  // Check if I2C was disabled via wizard
  bool i2cDisabledByUser = !gSettings.i2cBusEnabled;
  
  // Saving configuration stage
  setSetupProgressStage(SETUP_SAVING_CONFIG);
  broadcastOutput("Saving configuration...");
  
  // Build JSON with ArduinoJson
  PSRAM_JSON_DOC(doc);
  doc["bootCounter"] = 1;
  doc["nextId"] = 2;
  
  JsonArray users = doc["users"].to<JsonArray>();
  JsonObject admin = users.add<JsonObject>();
  admin["id"] = 1;
  admin["username"] = u;
  // Password now stored in per-user settings file, not here
  admin["role"] = "admin";
  admin["createdAt"] = (const char*)nullptr;  // null
  admin["createdBy"] = "provisional";
  admin["createdMs"] = millis();
  admin["bootSeq"] = gBootSeq;
  admin["bootCount"] = 1;
  
  doc["bootAnchors"].to<JsonArray>();

  DEBUG_SYSTEMF("FTS: Writing initial users.json: bootCounter=%u (forced 1), admin.bootCount=%u, gBootSeq=%lu",
                1, 1, (unsigned long)gBootSeq);
  
  // Write to file
  File file = LittleFS.open(USERS_JSON_FILE, "w");
  if (!file) {
    broadcastOutput("ERROR: Failed to create users.json");
  } else {
    size_t written = serializeJson(doc, file);
    file.close();
    
    if (written == 0) {
      broadcastOutput("ERROR: Failed to write users.json");
    } else {
      broadcastOutput("Saved /system/users/users.json");

      {
        String settingsPath = getUserSettingsPath(1);
        // Create user settings with password and theme
        JsonDocument defaults;
        defaults["theme"] = useDarkTheme ? "dark" : "light";
        defaults["password"] = hashedPassword;  // Store password in user settings
        if (!saveUserSettings(1, defaults)) {
          broadcastOutput("ERROR: Failed to create user settings");
        }
      }

      // Update gBootCounter in memory to match what we wrote to the file
      // This ensures subsequent users created in the same boot get the correct value
      gBootCounter = 1;
      DEBUG_SYSTEMF("FTS: Updated gBootCounter to 1 in memory");
      // If NTP already synced, resolve the creation timestamp immediately
      if (time(nullptr) > 0) {
        resolvePendingUserCreationTimes();
      }
    }
  }

  // Create automations.json (empty) on first-time setup
 #if ENABLE_AUTOMATION
  if (!LittleFS.exists(AUTOMATIONS_JSON_FILE)) {
    String a = "{\n  \"version\": 1,\n  \"automations\": []\n}\n";
    if (!writeAutomationsJsonAtomic(a)) {
      broadcastOutput("ERROR: Failed to write automations.json");
    } else {
      broadcastOutput("Created /system/automations.json");
    }
  }
 #endif

  // Setup complete!
  setSetupProgressStage(SETUP_FINISHED);
  setFirstTimeSetupState(SETUP_NOT_NEEDED);  // Back to normal state
  gFirstTimeSetupPerformed = true;  // Suppress stale-cookie "device restarted" message on fresh setup
  
  broadcastOutput("");
  broadcastOutput("FIRST-TIME SETUP COMPLETE!");
  
  // Always save settings after wizard completes
  // Ensure i2cSensorsEnabled is set when i2cBusEnabled is enabled
  // The wizard only toggles i2cBusEnabled, but processAutoStartSensors checks both
  if (gSettings.i2cBusEnabled) {
    gSettings.i2cSensorsEnabled = true;
  }
  
  // Debug: Print sensor auto-start values before saving
  Serial.printf("[FTS] Before save: i2cBus=%d i2cSensors=%d\n",
                gSettings.i2cBusEnabled ? 1 : 0,
                gSettings.i2cSensorsEnabled ? 1 : 0);
  Serial.printf("[FTS] Sensors: thermal=%d tof=%d imu=%d gps=%d fmradio=%d apds=%d gamepad=%d rtc=%d presence=%d\n",
                gSettings.thermalAutoStart ? 1 : 0,
                gSettings.tofAutoStart ? 1 : 0,
                gSettings.imuAutoStart ? 1 : 0,
                gSettings.gpsAutoStart ? 1 : 0,
                gSettings.fmRadioAutoStart ? 1 : 0,
                gSettings.apdsAutoStart ? 1 : 0,
                gSettings.gamepadAutoStart ? 1 : 0,
                gSettings.rtcAutoStart ? 1 : 0,
                gSettings.presenceAutoStart ? 1 : 0);
  
  writeSettingsJson();
  applySettings();  // Apply log level and other debug settings immediately
  
  // If user disabled I2C, reboot so it takes effect from boot
  if (i2cDisabledByUser) {
    broadcastOutput("");
    broadcastOutput("Rebooting to apply I2C disabled setting...");

    // Clear the OLED before reboot so the previous setup text doesn't remain
    // visible on the next boot when OLED init is skipped.
#if ENABLE_OLED_DISPLAY
    if (gDisplay && oledConnected && oledEnabled) {
      displayClear();
      displayUpdate();
    }
#endif

    delay(1000);     // Give time for output to flush
    ESP.restart();
    // Will not return - device reboots
  }
  
  broadcastOutput("Starting WiFi connection...");
  broadcastOutput("");
}
