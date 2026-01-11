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
#include "System_I2C.h"
#include "System_SensorStubs.h"
#include "System_Settings.h"
#include "System_UserSettings.h"

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
extern void broadcastOutput(const String& s);
extern void broadcastOutput(const char* s);
extern String waitForSerialInputBlocking();
extern String hashUserPassword(const String& plaintext);
extern bool writeAutomationsJsonAtomic(const String& json);
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

inline bool isFirstTimeSetup() {
  return gFirstTimeSetupState != SETUP_NOT_NEEDED;
}

inline void setFirstTimeSetupState(FirstTimeSetupState state) {
  gFirstTimeSetupState = state;
  DEBUG_SYSTEMF("[SETUP_STATE] State changed to: %d", (int)state);
}

inline void setSetupProgressStage(SetupProgressStage stage) {
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

void firstTimeSetupIfNeeded() {
  // Check current state instead of filesystem
  if (gFirstTimeSetupState == SETUP_NOT_NEEDED) {
    return;  // Already configured
  }

  // Update state for OLED animation
  setFirstTimeSetupState(SETUP_IN_PROGRESS);
  
  // Force OLED to show first-time setup screen immediately
  if (oledEnabled && oledConnected) {
    updateOLEDDisplay();
  }

  broadcastOutput("");
  broadcastOutput("FIRST-TIME SETUP");
  broadcastOutput("----------------");
  
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
  
  // WiFi setup stage
  setSetupProgressStage(SETUP_PROMPT_WIFI);
  broadcastOutput("");
  broadcastOutput("WiFi Setup (optional - press Enter to skip)");
  
  String wifiSSID = "";
  bool wifiSelected = false;
  
#if ENABLE_OLED_DISPLAY
  wifiSelected = getOLEDWiFiSelection(wifiSSID);
#else
  broadcastOutput("Enter WiFi SSID (or press Enter to skip): ");
  wifiSSID = waitForSerialInputBlocking();
  wifiSSID.trim();
  wifiSelected = (wifiSSID.length() > 0);
#endif
  
  if (wifiSelected && wifiSSID.length() > 0) {
    broadcastOutput("Enter WiFi password: ");
    String wifiPass = "";
#if ENABLE_OLED_DISPLAY
    wifiPass = getOLEDTextInput("WiFi Password:", true, "", 64);
#else
    wifiPass = waitForSerialInputBlocking();
#endif
    wifiPass.trim();
    
#if ENABLE_WIFI
    // Save WiFi credentials using existing WiFi system functions
    extern bool upsertWiFiNetwork(const String& ssid, const String& password, int priority, bool hidden);
    extern void sortWiFiByPriority();
    extern bool saveWiFiNetworks();
    
    upsertWiFiNetwork(wifiSSID, wifiPass, 1, false);  // Priority 1, not hidden
    sortWiFiByPriority();
    saveWiFiNetworks();
    broadcastOutput("WiFi credentials saved successfully");
    
    // Ask if WiFi should auto-connect on boot
    broadcastOutput("Auto-connect WiFi on boot? (y/n) [default: y]: ");
    bool wifiAuto = true;
#if ENABLE_OLED_DISPLAY
    wifiAuto = getOLEDYesNoPrompt("Auto-connect WiFi\non boot?", true);
#else
    String wifiAutoChoice = waitForSerialInputBlocking();
    wifiAutoChoice.trim();
    wifiAutoChoice.toLowerCase();
    wifiAuto = !(wifiAutoChoice == "n" || wifiAutoChoice == "no");
#endif
    
    if (wifiAuto) {
      gSettings.wifiAutoReconnect = true;
      broadcastOutput("WiFi will auto-connect on boot.");
    } else {
      gSettings.wifiAutoReconnect = false;
      broadcastOutput("WiFi will NOT auto-connect on boot. Use 'wificonnect' command to connect manually.");
    }
#else
    broadcastOutput("WiFi disabled at compile time - credentials not saved");
#endif
  } else {
    gSettings.wifiAutoReconnect = false;  // No credentials = no auto-connect
    broadcastOutput("WiFi setup skipped - you can configure later via settings");
  }
  
  // Hardware configuration stage
  setSetupProgressStage(SETUP_PROMPT_HARDWARE);
  broadcastOutput("");
  broadcastOutput("Hardware Setup (optional)");
  
  bool i2cDisabledByUser = false;
  
#if ENABLE_I2C_SYSTEM
  // I2C is compiled in - ask user if they want to disable it at runtime
  broadcastOutput("Enable I2C bus? This controls OLED display and I2C sensors.");
  broadcastOutput("Disabling saves memory but turns off OLED and all I2C devices.");
  broadcastOutput("Enable I2C bus? (y/n) [default: y]: ");
  
  bool i2cEnable = true;
#if ENABLE_OLED_DISPLAY
  i2cEnable = getOLEDYesNoPrompt("Enable I2C bus?\n(OLED + sensors)", true);
#else
  String i2cChoice = waitForSerialInputBlocking();
  i2cChoice.trim();
  i2cChoice.toLowerCase();
  i2cEnable = !(i2cChoice == "n" || i2cChoice == "no");
#endif
  
  if (i2cEnable) {
    gSettings.i2cBusEnabled = true;
    gI2CBusEnabled = true;  // Update global flag immediately
    broadcastOutput("I2C bus enabled (default)");
  } else {
    gSettings.i2cBusEnabled = false;
    gSettings.i2cSensorsEnabled = false;  // Force sensors off if bus is off
    gI2CBusEnabled = false;  // Update global flag immediately
    i2cDisabledByUser = true;
    broadcastOutput("I2C bus DISABLED - device will reboot to apply this setting");
  }
#else
  // I2C compiled out - force settings to match and inform user
  gSettings.i2cBusEnabled = false;
  gSettings.i2cSensorsEnabled = false;
  gI2CBusEnabled = false;
  broadcastOutput("I2C bus: DISABLED (compiled out for memory savings)");
  broadcastOutput("To enable I2C, change I2C_FEATURE_LEVEL in sensor_config.h and recompile.");
#endif
  
  // Saving configuration stage
  setSetupProgressStage(SETUP_SAVING_CONFIG);
  broadcastOutput("Saving configuration...");
  
  // Build JSON with ArduinoJson
  JsonDocument doc;
  doc["version"] = 1;
  doc["bootCounter"] = 1;
  doc["nextId"] = 2;
  
  JsonArray users = doc["users"].to<JsonArray>();
  JsonObject admin = users.add<JsonObject>();
  admin["id"] = 1;
  admin["username"] = u;
  admin["password"] = hashedPassword;
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
        if (!LittleFS.exists(settingsPath.c_str())) {
          JsonDocument defaults;
          defaults["theme"] = "light";
          if (!saveUserSettings(1, defaults)) {
            broadcastOutput("ERROR: Failed to create default user settings");
          }
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
  if (!LittleFS.exists(AUTOMATIONS_JSON_FILE)) {
    String a = "{\n  \"version\": 1,\n  \"automations\": []\n}\n";
    if (!writeAutomationsJsonAtomic(a)) {
      broadcastOutput("ERROR: Failed to write automations.json");
    } else {
      broadcastOutput("Created /system/automations.json");
    }
  }

  // Setup complete!
  setSetupProgressStage(SETUP_FINISHED);
  setFirstTimeSetupState(SETUP_NOT_NEEDED);  // Back to normal state
  // Don't set gFirstTimeSetupPerformed = true - let WiFi connect normally
  
  broadcastOutput("");
  broadcastOutput("FIRST-TIME SETUP COMPLETE!");
  
  // If user disabled I2C, save settings and reboot so it takes effect from boot
  if (i2cDisabledByUser) {
    extern bool writeSettingsJson();
    broadcastOutput("");
    broadcastOutput("Saving settings and rebooting to apply I2C disabled setting...");
    writeSettingsJson();  // Ensure settings are persisted

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
