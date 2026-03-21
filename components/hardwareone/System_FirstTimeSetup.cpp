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
#if ENABLE_GAMEPAD_SENSOR
#include "HAL_Input.h"
#endif
#include "System_Settings.h"
#include "System_UserSettings.h"
#include "OLED_SetupWizard.h"
#include "System_SetupWizard.h"
#include "System_FeatureRegistry.h"
#include "System_WiFi.h"
#if ENABLE_WIFI
#include <WiFi.h>
#endif
#if ENABLE_HTTP_SERVER
#include "WebServer_MigrationTool.h"
#endif

// ============================================================================
// Global Variables
// ============================================================================

bool gFirstTimeSetupPerformed = false;
volatile FirstTimeSetupState gFirstTimeSetupState = SETUP_NOT_NEEDED;
volatile SetupProgressStage gSetupProgressStage = SETUP_PROMPT_USERNAME;
volatile bool gAcceptingRestore = false;
volatile bool gRestoreComplete = false;

// File paths
#define SETTINGS_JSON_FILE "/system/settings.json"
#define USERS_JSON_FILE "/system/users/users.json"
#define AUTOMATIONS_JSON_FILE "/system/automations.json"

// Global variables
extern uint32_t gNTPAnchorId;
extern uint32_t gBootCounter;
extern int gWifiNetworkCount;

// Utility functions
extern String waitForSerialInputBlocking();
extern String hashUserPassword(const String& plaintext);
#if ENABLE_AUTOMATION
extern bool writeAutomationsJsonAtomic(const String& json);
#endif
extern void runUnifiedSystemCommand(const String& argsInput);
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

static void clearOledIfActive() {
#if ENABLE_OLED_DISPLAY
  if (gDisplay && oledConnected && oledEnabled) {
    displayClear();
    displayUpdate();
  }
#endif
}

static void rebootWithMessage(const char* message) {
  broadcastOutput("");
  broadcastOutput(message);
  clearOledIfActive();
  delay(1000);
  ESP.restart();
}

// ============================================================================
// First-Time Setup Implementation
// ============================================================================

// Forward declaration for OLED setup mode selection
#if ENABLE_OLED_DISPLAY
extern bool getOLEDSetupModeSelection(int& setupMode);
#endif

// Migration restore handler registration (from WebServer_MigrationTool.cpp)
#if ENABLE_HTTP_SERVER
#include "WebServer_MigrationTool.h"
extern httpd_handle_t server;
#endif

void firstTimeSetupIfNeeded() {
  // Check current state instead of filesystem
  if (gFirstTimeSetupState == SETUP_NOT_NEEDED) {
    return;  // Already configured
  }

  // Outer restart loop — re-entered if user presses B / types 'back' during restore wait
  while (true) {

  bool goBack = false;

  // Update state for OLED animation
  setFirstTimeSetupState(SETUP_IN_PROGRESS);
  
  broadcastOutput("");
  broadcastOutput("FIRST-TIME SETUP");
  broadcastOutput("----------------");
  
  // ============================================================================
  // Setup Mode Selection: Basic vs Advanced vs Import from Backup
  // ============================================================================
  int setupMode = 0;  // 0 = Basic, 1 = Advanced, 2 = Import from Backup
  
#if ENABLE_OLED_DISPLAY
  // Use OLED selection if available
  if (oledEnabled && oledConnected) {
    getOLEDSetupModeSelection(setupMode);
  } else {
#endif
    // Serial-only mode selection
    broadcastOutput("");
    broadcastOutput("Select setup mode:");
    broadcastOutput("  1. Basic Setup        - Quick start (username + password only)");
    broadcastOutput("  2. Advanced Setup     - Full configuration wizard");
#if ENABLE_HTTP_SERVER && ENABLE_WIFI
    broadcastOutput("  3. Import from Backup - Restore settings from .hwbackup file");
    broadcastOutput("");
    broadcastOutput("Enter 1, 2, or 3 (default: 1): ");
#else
    broadcastOutput("");
    broadcastOutput("Enter 1 or 2 (default: 1): ");
#endif

    String modeInput = waitForSerialInputBlocking();
    modeInput.trim();
    if (modeInput == "2" || modeInput.equalsIgnoreCase("advanced")) {
      setupMode = 1;
#if ENABLE_HTTP_SERVER && ENABLE_WIFI
    } else if (modeInput == "3" || modeInput.equalsIgnoreCase("restore") || modeInput.equalsIgnoreCase("import")) {
      setupMode = 2;
#endif
    } else {
      setupMode = 0;
    }
#if ENABLE_OLED_DISPLAY
  }
#endif

  // ============================================================================
  // Handle "Import from Backup" mode
  // ============================================================================
  if (setupMode == 2) {
    broadcastOutput("Import from Backup selected.");
    broadcastOutput("");

#if ENABLE_HTTP_SERVER && ENABLE_WIFI
    // Step 1: Collect WiFi credentials — reuse the same scan+select UI as the setup wizard
    broadcastOutput("WiFi is required for the Migration Tool to connect.");
    broadcastOutput("");

    String restoreSSID = "";
    String restorePass = "";
    bool wifiSelected = false;

    while (!wifiSelected) {
      // Network selection (scan + pick from list, with serial fallback)
      if (!getOLEDWiFiSelection(restoreSSID)) {
        // User pressed B / cancelled — go back to setup mode selection
        goBack = true;
        break;
      }

      // Password entry (B goes back to network list)
      bool passwordCancelled = false;
#if ENABLE_OLED_DISPLAY
      if (oledEnabled && oledConnected) {
        restorePass = getOLEDTextInput("WiFi Password:", true, "", 64, &passwordCancelled);
      } else {
#endif
        broadcastOutput("Enter WiFi Password (blank for open, 'back' to re-select): ");
        restorePass = waitForSerialInputBlocking();
        restorePass.trim();
        if (restorePass.equalsIgnoreCase("back") || restorePass.equalsIgnoreCase("b")) {
          passwordCancelled = true;
        }
#if ENABLE_OLED_DISPLAY
      }
#endif

      if (passwordCancelled) {
        restoreSSID = "";
        restorePass = "";
        continue;  // Loop back to network selection
      }

      wifiSelected = true;
    }

    if (goBack) {
      continue;  // Restart outer setup loop
    }

    // Step 2: Save WiFi credentials and connect using the proven connection infrastructure
    broadcastOutput("Connecting to WiFi: " + restoreSSID);
#if ENABLE_OLED_DISPLAY
    showOLEDMessage(("Connecting WiFi:\n" + restoreSSID).c_str(), false);
#endif

    // Save credentials to the WiFi network list and persist to flash
    extern bool upsertWiFiNetwork(const String& ssid, const String& password, int priority, bool hidden);
    extern void sortWiFiByPriority();
    extern bool saveWiFiNetworks();
    upsertWiFiNetwork(restoreSSID, restorePass, 1, false);
    sortWiFiByPriority();
    saveWiFiNetworks();
    gSettings.wifiAutoReconnect = true;

    // Use the project's standard WiFi connection (ESP-IDF API internally)
    setupWiFi();

    if (WiFi.status() != WL_CONNECTED) {
      broadcastOutput("ERROR: Failed to connect to WiFi. Cannot use Import from Backup.");
      broadcastOutput("Falling back to Basic Setup.");
#if ENABLE_OLED_DISPLAY
      showOLEDMessage("WiFi connect\nfailed!\n\nFalling back to\nBasic Setup", true);
#endif
      setupMode = 0;
    } else {
      String ipStr = WiFi.localIP().toString();
      broadcastOutput("WiFi connected! IP: " + ipStr);

      // Step 3: Start the minimal restore-only HTTP server
      // This exposes ONLY /api/ping and /api/restore — no other pages are accessible.
      gAcceptingRestore = true;
      gRestoreComplete = false;
      startRestoreOnlyHttpServer();

      broadcastOutput("");
      broadcastOutput("========================================");
      broadcastOutput("  RESTORE MODE ACTIVE");
      broadcastOutput("========================================");
      broadcastOutput("");
      broadcastOutput("Device IP: " + ipStr);
      broadcastOutput("");
      broadcastOutput("IMPORTANT: Do NOT connect to this device directly.");
      broadcastOutput("Instead, use the HardwareOne Migration Tool broswer application");
      broadcastOutput("to send your .hwbackup file to this IP address.");
      broadcastOutput("");
      broadcastOutput("Download Migration Tool:");
      broadcastOutput("  https://github.com/CadenGithubB/HardwareOne-Migration-Tool");
      broadcastOutput("");
      broadcastOutput("Press 'back' (serial) or B (gamepad) to return to setup menu.");
      broadcastOutput("========================================");
      broadcastOutput("");

#if ENABLE_OLED_DISPLAY
      {
        String msg = "RESTORE MODE\n\nUse Migration Tool\nbrowser application to\nsend .hwbackup\n\nIP: " + ipStr + "\n\nB = back";
        showOLEDMessage(msg.c_str(), false);
      }
#endif

      // Step 5: Poll until restore completes or user presses B / types 'back'
#if ENABLE_GAMEPAD_SENSOR
      uint32_t lastBtnState = 0xFFFFFFFF;
      bool btnStateInit = false;
#endif
      while (!gRestoreComplete && !goBack) {
        delay(500);
        Serial.print(".");

        // Serial 'back' escape
        if (Serial.available()) {
          String line = Serial.readStringUntil('\n');
          line.trim();
          if (line.equalsIgnoreCase("back") || line.equalsIgnoreCase("cancel")) {
            goBack = true;
          }
        }

#if ENABLE_GAMEPAD_SENSOR
        // Gamepad B button escape (active-low, detect new press)
        if (!goBack && gControlCache.mutex &&
            xSemaphoreTake(gControlCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
          uint32_t btns = gControlCache.gamepadButtons;
          bool valid = gControlCache.gamepadDataValid;
          xSemaphoreGive(gControlCache.mutex);
          if (valid) {
            if (!btnStateInit) {
              lastBtnState = btns;
              btnStateInit = true;
            } else {
              uint32_t newPressed = ~btns & lastBtnState;  // active-low: was 1 (up), now 0 (down)
              if (newPressed & INPUT_MASK(INPUT_BUTTON_B)) {
                goBack = true;
              }
              lastBtnState = btns;
            }
          }
        }
#endif
      }
      Serial.println();

      // Gate 3: Stop the restore-only server entirely
      gAcceptingRestore = false;
      stopRestoreOnlyHttpServer();

      if (goBack) {
        broadcastOutput("");
        broadcastOutput("Returning to setup mode selection...");
#if ENABLE_OLED_DISPLAY
        showOLEDMessage("Returning to\nsetup menu...", false);
#endif
        continue;  // Restart outer while(true) loop
      }

      broadcastOutput("");
      broadcastOutput("Restore complete! Files written to device.");
      broadcastOutput("Rebooting to apply restored settings...");

      gFirstTimeSetupPerformed = true;
      setFirstTimeSetupState(SETUP_COMPLETE);
      rebootWithMessage("Rebooting with restored settings...");
      return;  // rebootWithMessage calls ESP.restart()
    }
#else
    broadcastOutput("ERROR: HTTP server or WiFi not enabled. Cannot use Import from Backup.");
    broadcastOutput("Falling back to Basic Setup.");
    setupMode = 0;
#endif
  }

  if (goBack) continue;  // Jump back to mode selection

  bool advancedSetup = (setupMode == 1);
  broadcastOutput(advancedSetup ? "Advanced setup selected." : "Basic setup selected.");
  broadcastOutput("");
  
  // Username stage
  setSetupProgressStage(SETUP_PROMPT_USERNAME);
  if (!(oledEnabled && oledConnected)) {
    broadcastOutput("Enter admin username (cannot be blank): ");
  }
  String u = "";
  while (u.length() == 0) {
#if ENABLE_OLED_DISPLAY
    u = getOLEDTextInput("Admin Username:", false, "", 32);
#else
    u = waitForSerialInputBlocking();
#endif
    u.trim();
    if (u.length() == 0) {
      if (!(oledEnabled && oledConnected)) {
        broadcastOutput("Username cannot be blank. Please enter admin username: ");
      }
#if ENABLE_OLED_DISPLAY
      showOLEDMessage("Username cannot\nbe blank!", true);
#endif
    }
  }

  // Password stage  
  setSetupProgressStage(SETUP_PROMPT_PASSWORD);
  String p = "";
  while (p.length() == 0) {
    if (!(oledEnabled && oledConnected)) {
      broadcastOutput("Enter admin password (cannot be blank): ");
    }
#if ENABLE_OLED_DISPLAY
    p = getOLEDTextInput("Admin Password:", true, "", 32);
#else
    p = waitForSerialInputBlocking();
#endif
    p.trim();
    if (p.length() == 0) {
      if (!(oledEnabled && oledConnected)) {
        broadcastOutput("Password cannot be blank. Please enter admin password: ");
      }
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

    wizardResult = runSetupWizard();

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
  } else {
    // Basic setup - use sensible defaults
    broadcastOutput("");
    broadcastOutput("Using default settings (Basic mode)");
    gSettings.wifiAutoReconnect = true;
    gSettings.httpAutoStart = true;
  }

  // ============================================================================
  // Device Name + Theme (both Basic and Advanced modes)
  // ============================================================================

  // Device name customization
  broadcastOutput("");
  broadcastOutput("========================================");
  broadcastOutput("       DEVICE NAME");
  broadcastOutput("========================================");
  broadcastOutput("Used for Bluetooth and ESP-NOW identity.");
  broadcastOutput("Press Enter to keep default [HardwareOne]");
  broadcastOutput("----------------------------------------");
  String deviceName = "";
#if ENABLE_OLED_DISPLAY
  deviceName = getOLEDTextInput("Device Name:", false, "HardwareOne", 20);
#else
  deviceName = waitForSerialInputBlocking();
#endif
  deviceName.trim();
  if (deviceName.length() == 0) {
    deviceName = "HardwareOne";
  }
  gSettings.bleDeviceName = deviceName;
  gSettings.espnowDeviceName = deviceName;
  broadcastOutput("Device name set to: " + deviceName);

#if ENABLE_HTTP_SERVER
  // Theme preference (for web UI only — skipped if web server is disabled)
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
  useDarkTheme = (themeInput == "2" || themeInput.equalsIgnoreCase("dark"));
  broadcastOutput(useDarkTheme ? "Theme set to: Dark" : "Theme set to: Light");
#endif // ENABLE_HTTP_SERVER
  
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
  admin["ntpAnchorId"] = gNTPAnchorId;
  admin["bootCount"] = 1;
  
  doc["bootAnchors"].to<JsonArray>();

  DEBUG_SYSTEMF("FTS: Writing initial users.json: bootCounter=%u (forced 1), admin.bootCount=%u, gNTPAnchorId=%lu",
                1, 1, (unsigned long)gNTPAnchorId);
  
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
  
  // Debug: Print sensor auto-start values before saving
  Serial.printf("[FTS] Before save: i2cBus=%d\n", gSettings.i2cBusEnabled ? 1 : 0);
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
    // Clear the OLED before reboot so the previous setup text doesn't remain
    // visible on the next boot when OLED init is skipped.
    rebootWithMessage("Rebooting to apply I2C disabled setting...");
    // Will not return - device reboots
  }
  
  // OLED and gamepad are always started during first time setup if hardware is
  // detected (OLED for boot animation, gamepad for menu navigation). If the
  // user did not select them, reboot so the next boot starts clean — stopping
  // them in-place would fragment the heap; a reboot is cheaper.
  bool needsRebootForHardware = false;
#if ENABLE_OLED_DISPLAY
  if (oledConnected && !gSettings.oledEnabled) {
    needsRebootForHardware = true;
  }
#endif
#if ENABLE_GAMEPAD_SENSOR
  if (gamepadEnabled && !gSettings.gamepadAutoStart) {
    needsRebootForHardware = true;
  }
#endif

  if (needsRebootForHardware) {
    rebootWithMessage("Rebooting to apply hardware settings...");
    // Will not return - device reboots
  }

  broadcastOutput("Starting WiFi connection...");
  broadcastOutput("");

  break;  // Setup completed normally — exit the restart loop
  } // end while(true) restart loop
}
