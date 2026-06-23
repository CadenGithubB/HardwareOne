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
#include "WebServer_Handle.h"
#include "System_VFS.h"   // VFS::*Guarded + systemAuth (Phase 2 perm refactor)

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
#if ENABLE_MIGRATION_TOOL
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
const char* PENDING_CRED_SETUP_FILE = "/system/.pending_credential_setup";

// File paths (defined in HardwareOne.cpp / System_User.cpp)
extern const char* SETTINGS_JSON_FILE;
extern const char* USERS_JSON_FILE;
extern const char* AUTOMATIONS_JSON_FILE;

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
  bool usersExist = VFS::existsGuarded(USERS_JSON_FILE, VFS::systemAuth("setup.detect"));
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
  if (gDisplay && oledConnected && gOledEnabled) {
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
extern int getOLEDArchetypeSelection();  // Level-2 deployment picker; -1 = back
#endif

#if ENABLE_WIFI
// Serial WiFi scan + pick for "Import from Backup" when ENABLE_OLED_DISPLAY is off
// (getOLEDWiFiSelection lives in OLED_FirstTimeSetup.cpp and is not built).
static bool serialWifiSelectionForRestore(String& outSSID) {
  outSSID = "";
  while (true) {
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks(false, true);

    // Number only NAMED networks; group hidden (empty-SSID) ones into a count —
    // mirrors the OLED picker so a numbered pick can never yield a blank SSID (a
    // blank SSID fails to connect, which is what crashed the import flow). Hidden
    // networks are joined by typing the exact SSID. Loop (not recurse) on rescan.
    int named[20];
    int namedCount = 0;
    int hiddenCount = 0;
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i).length() == 0) { hiddenCount++; continue; }
      if (namedCount < 20) named[namedCount++] = i;
    }

    if (namedCount > 0) {
      broadcastOutput(String("Found ") + namedCount + " network(s):");
      for (int j = 0; j < namedCount; j++) {
        char line[96];
        snprintf(line, sizeof(line), "  %d. %-24s  %lddBm",
                 j + 1, WiFi.SSID(named[j]).c_str(), (long)WiFi.RSSI(named[j]));
        broadcastOutput(line);
      }
    } else {
      broadcastOutput("No named WiFi networks found.");
    }
    if (hiddenCount > 0) {
      broadcastOutput(String("  (+") + hiddenCount + " hidden network" +
                      (hiddenCount == 1 ? "" : "s") + " - type the exact SSID to join one)");
    }
    broadcastOutput("Enter a number, type an SSID to join manually, 'rescan', or 'b' to go back:");

    String input = waitForSerialInputBlocking();
    input.trim();

    if (input.equalsIgnoreCase("b") || input.equalsIgnoreCase("back")) { WiFi.scanDelete(); return false; }
    if (input.length() == 0) { WiFi.scanDelete(); return false; }
    if (input.equalsIgnoreCase("rescan")) { WiFi.scanDelete(); continue; }

    // Bare in-range integer -> Nth named network; anything else -> typed SSID.
    String ssid = input;
    int idx = input.toInt();
    if (idx >= 1 && idx <= namedCount && String(idx) == input) {
      ssid = WiFi.SSID(named[idx - 1]);
    }
    WiFi.scanDelete();
    ssid.trim();
    if (ssid.length() == 0) {
      broadcastOutput("SSID cannot be empty - please try again.");
      continue;
    }
    outSSID = ssid;
    return true;
  }
}
#endif // ENABLE_WIFI

// Migration restore handler registration (from WebServer_MigrationTool.cpp)
#if ENABLE_MIGRATION_TOOL
#include "WebServer_MigrationTool.h"
#endif

#if ENABLE_MIGRATION_TOOL
// Create the initial admin user: users.json (id 1, bootCounter 1, nextId 2) plus the
// per-user settings file holding the hashed password and theme. Mirrors the inline
// creation in firstTimeSetupIfNeeded()'s Basic/Advanced path and is used by the
// post-restore streamlined credential flow. (If the first-admin shape changes, update
// both — a future cleanup could unify them.)
static bool createInitialAdminUser(const String& username, const String& plaintextPassword, bool useDarkTheme) {
  String hashedPassword = hashUserPassword(plaintextPassword);

  PSRAM_JSON_DOC(doc);
  doc["bootCounter"] = 1;
  doc["nextId"] = 2;

  JsonArray users = doc["users"].to<JsonArray>();
  JsonObject admin = users.add<JsonObject>();
  admin["id"] = 1;
  admin["username"] = username;
  admin["role"] = "admin";
  admin["createdAt"] = (const char*)nullptr;  // resolved lazily via boot anchor
  admin["createdBy"] = "firstsetup";          // provenance: onboarding wizard owner
  admin["createdAtSource"] = "pending";       // time-derivation status (see resolver)
  admin["createdMs"] = millis();
  admin["ntpAnchorId"] = gNTPAnchorId;
  admin["bootCount"] = 1;
  doc["bootAnchors"].to<JsonArray>();

  File file = VFS::openGuarded(USERS_JSON_FILE, "w", VFS::systemAuth("setup.users.create"));
  if (!file) {
    broadcastOutput("ERROR: Failed to create users.json");
    return false;
  }
  size_t written = serializeJson(doc, file);
  file.close();
  if (written == 0) {
    broadcastOutput("ERROR: Failed to write users.json");
    return false;
  }
  broadcastOutput("Saved /system/users/users.json");

  // Per-user settings hold the (hashed) password and theme.
  PSRAM_JSON_DOC(defaults);
  defaults["theme"] = useDarkTheme ? "dark" : "light";
  defaults["password"] = hashedPassword;
  if (!saveUserSettings(1, defaults)) {
    broadcastOutput("ERROR: Failed to create user settings");
  }

  gBootCounter = 1;  // keep in-memory counter in sync with what we wrote
  if (time(nullptr) > 0) {
    resolvePendingUserCreationTimes();  // resolve creation timestamp if NTP already synced
  }
  return true;
}

// Streamlined post-restore login setup. Runs when a cross-device (partial) restore
// left settings in place but no user credentials. Collects just username + password,
// creates the admin, and reboots into the (now fully configured) device. Returns
// false if the user backed out or creation failed, so the caller falls back to the
// normal setup menu.
static bool runPostRestoreCredentialSetup() {
  setFirstTimeSetupState(SETUP_IN_PROGRESS);

  broadcastOutput("");
  broadcastOutput("========================================");
  broadcastOutput("  RESTORE COMPLETE - CREATE YOUR LOGIN");
  broadcastOutput("========================================");
  broadcastOutput("Your settings were restored from a backup.");
  broadcastOutput("This device needs its own admin login.");
  broadcastOutput("");
#if ENABLE_OLED_DISPLAY
  if (gOledEnabled && oledConnected) {
    showOLEDMessage("Restore done!\n\nCreate your\nadmin login", false);
  }
#endif

  // Username (cancel/back -> fall back to the full setup menu)
  setSetupProgressStage(SETUP_PROMPT_USERNAME);
  String u = "";
  while (u.length() == 0) {
#if ENABLE_OLED_DISPLAY
    if (gOledEnabled && oledConnected) {
      bool cancelled = false;
      u = getOLEDTextInput("Admin Username:", false, "", 32, &cancelled, false);
      if (cancelled) return false;
    } else {
#endif
      broadcastOutput("Enter admin username: ");
      u = waitForSerialInputBlocking();
#if ENABLE_OLED_DISPLAY
    }
#endif
    u.trim();
  }

  // Password
  setSetupProgressStage(SETUP_PROMPT_PASSWORD);
  String p = "";
  while (p.length() == 0) {
#if ENABLE_OLED_DISPLAY
    if (gOledEnabled && oledConnected) {
      bool cancelled = false;
      p = getOLEDTextInput("Admin Password:", true, "", 32, &cancelled, false);
      if (cancelled) return false;
    } else {
#endif
      broadcastOutput("Enter admin password: ");
      p = waitForSerialInputBlocking();
#if ENABLE_OLED_DISPLAY
    }
#endif
    p.trim();
  }

  if (!createInitialAdminUser(u, p, /*useDarkTheme=*/false)) {
    broadcastOutput("ERROR: Could not create admin user. Falling back to full setup.");
    return false;
  }

  setSetupProgressStage(SETUP_FINISHED);
  setFirstTimeSetupState(SETUP_NOT_NEEDED);
  gFirstTimeSetupPerformed = true;  // suppress stale-cookie notice; WiFi connects next boot
  rebootWithMessage("Login created. Rebooting with your restored settings...");
  return true;  // unreached (rebootWithMessage restarts)
}
#endif  // ENABLE_MIGRATION_TOOL

void firstTimeSetupIfNeeded() {
  // Check current state instead of filesystem
  if (gFirstTimeSetupState == SETUP_NOT_NEEDED) {
    return;  // Already configured
  }

#if ENABLE_MIGRATION_TOOL
  // A cross-device (partial) restore writes settings but intentionally skips user
  // credentials, leaving users.json absent. Instead of the full Basic/Advanced/Import
  // menu, guide the user through creating just their admin login.
  if (VFS::existsGuarded(PENDING_CRED_SETUP_FILE, VFS::systemAuth("setup.restore.cred_marker"))) {
    // Clear the marker first so a power-cycle mid-flow falls back to the normal menu
    // (prevents a boot loop).
    VFS::removeGuarded(PENDING_CRED_SETUP_FILE, VFS::systemAuth("setup.restore.cred_marker"));
    if (runPostRestoreCredentialSetup()) {
      return;  // unreached (success path reboots)
    }
    // Backed out / failed -> continue to the normal setup menu below.
  }
#endif

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
  if (gOledEnabled && oledConnected) {
    getOLEDSetupModeSelection(setupMode);
  } else {
#endif
    // Serial-only mode selection
    broadcastOutput("");
    broadcastOutput("Select setup mode:");
    broadcastOutput("  1. Basic Setup        - Quick start (username + password only)");
    broadcastOutput("  2. Advanced Setup     - Full configuration wizard");
#if ENABLE_MIGRATION_TOOL && ENABLE_WIFI
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
#if ENABLE_MIGRATION_TOOL && ENABLE_WIFI
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

#if ENABLE_MIGRATION_TOOL && ENABLE_WIFI
    // Step 1: Collect WiFi credentials — reuse the same scan+select UI as the setup wizard
    broadcastOutput("WiFi is required for the Migration Tool to connect.");
    broadcastOutput("");

    String restoreSSID = "";
    String restorePass = "";
    bool wifiSelected = false;

    while (!wifiSelected) {
      // Network selection (scan + pick from list)
#if ENABLE_OLED_DISPLAY
      if (!getOLEDWiFiSelection(restoreSSID)) {
        // User pressed B / cancelled — go back to setup mode selection
        goBack = true;
        break;
      }
#elif ENABLE_WIFI
      if (!serialWifiSelectionForRestore(restoreSSID)) {
        goBack = true;
        break;
      }
#else
      broadcastOutput("ERROR: WiFi not enabled in this build; cannot import from backup.");
      goBack = true;
      break;
#endif

      // Password entry (B goes back to network list)
      bool passwordCancelled = false;
#if ENABLE_OLED_DISPLAY
      if (gOledEnabled && oledConnected) {
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
      // The full restore message doesn't fit the OLED (showOLEDMessage shows ~5
      // lines), so present it as flip-able pages — A = next, B = back. Serial
      // (above) shows the whole thing at once. Pages are rendered in the wait loop.
      String rtitles[3] = { "Restore Mode", "Device IP", "Get the Tool" };
      String rbodies[3];
      rbodies[0] = "Send a .hwbackup to this device using the HardwareOne Migration Tool.";
      rbodies[1] = "  " + ipStr + "\n\nPoint the Migration Tool at this address.";
      rbodies[2] = "github.com/\nCadenGithubB/\nHardwareOne-\nMigration-Tool";
      const int RPAGES = 3;
      int rpage = 0;
      int lastRpage = -1;
#endif

      // Step 5: Poll until restore completes or user presses B / types 'back'
#if ENABLE_GAMEPAD_SENSOR
      uint32_t lastBtnState = 0xFFFFFFFF;
      bool btnStateInit = false;
#endif
      while (!gRestoreComplete && !goBack) {
#if ENABLE_OLED_DISPLAY
        // Render the current help page (only when it changes, to avoid flicker)
        if (rpage != lastRpage) {
          drawSetupInfoPage(rtitles[rpage].c_str(), rbodies[rpage].c_str(),
                            "Joy:flip B:back", rpage + 1, RPAGES);
          lastRpage = rpage;
        }
#endif
        delay(100);

        // Serial 'back' escape
        if (Serial.available()) {
          String line = Serial.readStringUntil('\n');
          line.trim();
          if (line.equalsIgnoreCase("back") || line.equalsIgnoreCase("cancel")) {
            goBack = true;
          }
        }

#if ENABLE_GAMEPAD_SENSOR
#if ENABLE_OLED_DISPLAY
        // Joystick up/down flips the help pages — same control as the archetype cards.
        {
          JoystickNav nav = readWizardJoystickNav();
          if (nav.down)    { rpage = (rpage + 1) % RPAGES; }
          else if (nav.up) { rpage = (rpage + RPAGES - 1) % RPAGES; }
        }
#endif
        // Gamepad B button = back (active-low, detect new press)
        uint32_t btns = 0;
        bool valid = false;
        if (!goBack) {
          SensorCacheGuard g(gInputCache.mutex, pdMS_TO_TICKS(10), "fts.gamepadEscape");
          if (g.held) {
            btns = gInputCache.buttons;
            valid = gInputCache.dataValid;
          }
        }
        if (!goBack && valid) {
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
#endif
      }

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

  // Basic (setupMode 0) -> Level 2: pick a deployment archetype, seed its bundle.
  // (Advanced runs the classic manual wizard; Import was handled above.)
  if (setupMode == 0) {
    int archIdx = 0;
#if ENABLE_OLED_DISPLAY
    if (gOledEnabled && oledConnected) {
      archIdx = getOLEDArchetypeSelection();   // index, or -1 = back
    } else {
#endif
      broadcastOutput("");
      broadcastOutput("What will you use this device for?");
      int avail[16];
      int navail = 0;
      for (size_t i = 0; i < setupArchetypesCount && navail < 16; i++) {
        if (!setupArchetypeAvailable(&setupArchetypes[i])) continue;  // hide uncompiled
        avail[navail++] = (int)i;
        char ln[176];
        snprintf(ln, sizeof(ln), "  %d. %s - %s", navail,
                 setupArchetypes[i].name, setupArchetypes[i].blurb);
        broadcastOutput(ln);
      }
      broadcastOutput("Enter a number, or 'b' to go back (default: 1): ");
      String ai = waitForSerialInputBlocking();
      ai.trim();
      if (ai.equalsIgnoreCase("b") || ai.equalsIgnoreCase("back")) {
        archIdx = -1;
      } else {
        int num = ai.toInt();
        archIdx = (navail > 0 && num >= 1 && num <= navail) ? avail[num - 1]
                                                            : (navail > 0 ? avail[0] : 0);
      }
#if ENABLE_OLED_DISPLAY
    }
#endif
    if (archIdx < 0) continue;  // back -> re-show the Basic/Advanced/Import menu
    int seeded = applyArchetypeSeed(&setupArchetypes[archIdx]);
    char msg[80];
    snprintf(msg, sizeof(msg), "%s selected (%d features pre-enabled).",
             setupArchetypes[archIdx].name, seeded);
    broadcastOutput(msg);
  } else {
    broadcastOutput("Advanced setup selected.");
  }
  broadcastOutput("");
  
  // Username stage ('b' on serial / B button on OLED goes back to the menu)
  setSetupProgressStage(SETUP_PROMPT_USERNAME);
  if (!(gOledEnabled && oledConnected)) {
    broadcastOutput("Enter admin username ('b' to go back): ");
  }
  String u = "";
  bool setupBack = false;
  while (u.length() == 0) {
#if ENABLE_OLED_DISPLAY
    bool cancelled = false;
    u = getOLEDTextInput("Admin Username:", false, "", 32, &cancelled, false);
    if (cancelled) { setupBack = true; break; }
#else
    u = waitForSerialInputBlocking();
    u.trim();
    if (u.equalsIgnoreCase("b") || u.equalsIgnoreCase("back")) { setupBack = true; break; }
#endif
    u.trim();
    if (u.length() == 0) {
      if (!(gOledEnabled && oledConnected)) {
        broadcastOutput("Username cannot be blank ('b' to go back): ");
      }
#if ENABLE_OLED_DISPLAY
      showOLEDMessage("Username cannot\nbe blank!", true);
#endif
    }
  }
  if (setupBack) continue;  // back to the Basic/Advanced/Import menu

  // Password stage ('b' on serial / B button on OLED goes back to the menu)
  setSetupProgressStage(SETUP_PROMPT_PASSWORD);
  String p = "";
  setupBack = false;
  while (p.length() == 0) {
    if (!(gOledEnabled && oledConnected)) {
      broadcastOutput("Enter admin password ('b' to go back): ");
    }
#if ENABLE_OLED_DISPLAY
    bool cancelled = false;
    p = getOLEDTextInput("Admin Password:", true, "", 32, &cancelled, false);
    if (cancelled) { setupBack = true; break; }
#else
    p = waitForSerialInputBlocking();
    p.trim();
    if (p.equalsIgnoreCase("b") || p.equalsIgnoreCase("back")) { setupBack = true; break; }
#endif
    p.trim();
    if (p.length() == 0) {
      if (!(gOledEnabled && oledConnected)) {
        broadcastOutput("Password cannot be blank ('b' to go back): ");
      }
#if ENABLE_OLED_DISPLAY
      showOLEDMessage("Password cannot\nbe blank!", true);
#endif
    }
  }
  if (setupBack) continue;  // back to the Basic/Advanced/Import menu

  // Create users.json with admin (ID 1), nextId field, and empty bootAnchors array - hash the password
  String hashedPassword = hashUserPassword(p);
  // At first-time setup, users.json does not exist yet; seed bootCounter starting at 1 and set admin bootCount to 1
  
  // ============================================================================
  // Feature Configuration Wizard (Advanced mode only)
  // ============================================================================
  bool wifiConfigured = false;
  bool useDarkTheme = false;  // Theme preference (used when creating user settings)
  
  if (advancedSetup) {
    setSetupProgressStage(SETUP_PROMPT_HARDWARE);
    broadcastOutput("");
    broadcastOutput("Feature Configuration...");

    SetupWizardResult wizardResult = runAndApplyFeatureWizard();
    wifiConfigured = wizardResult.wifiConfigured && wizardResult.wifiSSID.length() > 0;
  } else {
    // Basic setup - use sensible defaults
    broadcastOutput("");
    broadcastOutput("Using default settings (Basic mode)");
    gSettings.wifiAutoReconnect = true;
    gSettings.httpAutoStart = true;
  }

  // ============================================================================
  // Theme (both Basic and Advanced modes)
  // Device name is set inside the ESP-NOW configure panel (wizard page).
  // ============================================================================

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
  if (gOledEnabled && oledConnected && getOLEDThemeSelection(darkSelected)) {
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
  
  // WiFi credentials are saved inside runAndApplyFeatureWizard() when configured.
  // When not configured, ensure auto-reconnect is off so the device doesn't
  // attempt to connect with no stored credentials.
  if (!wifiConfigured) {
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
  admin["createdAt"] = (const char*)nullptr;  // resolved lazily via boot anchor
  admin["createdBy"] = "firstsetup";          // provenance: onboarding wizard owner
  admin["createdAtSource"] = "pending";       // time-derivation status (see resolver)
  admin["createdMs"] = millis();
  admin["ntpAnchorId"] = gNTPAnchorId;
  admin["bootCount"] = 1;
  
  doc["bootAnchors"].to<JsonArray>();

  DEBUG_SYSTEMF("FTS: Writing initial users.json: bootCounter=%u (forced 1), admin.bootCount=%u, gNTPAnchorId=%lu",
                1, 1, (unsigned long)gNTPAnchorId);
  
  // Write to file
  File file = VFS::openGuarded(USERS_JSON_FILE, "w", VFS::systemAuth("setup.users.create"));
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
        PSRAM_JSON_DOC(defaults);
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
  if (!VFS::existsGuarded(AUTOMATIONS_JSON_FILE, VFS::systemAuth("setup.automations.init"))) {
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
  INFO_SYSTEMF("[FTS] Before save: i2cBus=%d", gSettings.i2cBusEnabled ? 1 : 0);
  INFO_SYSTEMF("[FTS] Sensors: thermal=%d tof=%d imu=%d gps=%d fmradio=%d apds=%d gamepad=%d rtc=%d presence=%d",
                gSettings.thermalAutoStart ? 1 : 0,
                gSettings.tofAutoStart ? 1 : 0,
                gSettings.imuAutoStart ? 1 : 0,
                gSettings.gpsAutoStart ? 1 : 0,
                gSettings.fmRadioAutoStart ? 1 : 0,
                gSettings.apdsAutoStart ? 1 : 0,
                gSettings.inputAutoStart ? 1 : 0,
                gSettings.rtcAutoStart ? 1 : 0,
                gSettings.presenceAutoStart ? 1 : 0);
  
  writeSettingsJson();
  applySettings();  // Apply log level and other debug settings immediately

  // Connect WiFi and sync NTP before any potential reboot.
  // WiFi was already initialized for network scanning during setup.
  // This ensures user creation timestamps are resolved to real dates
  // even when the setup wizard triggers a reboot (I2C/OLED/gamepad).
  if (wifiConfigured) {
    broadcastOutput("Connecting WiFi and syncing time before reboot...");
    connectToBestWiFiNetwork();  // Handles WiFi connect + NTP sync + timestamp resolution
  }

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
  // Also reboot if user enabled OLED but it's not connected (wasn't initialized
  // during setup because settings weren't loaded yet).
  bool needsRebootForHardware = false;
#if ENABLE_OLED_DISPLAY
  if (oledConnected && !gSettings.oledEnabled) {
    needsRebootForHardware = true;
  }
  if (!oledConnected && gSettings.oledEnabled) {
    needsRebootForHardware = true;
  }
#endif
#if ENABLE_GAMEPAD_SENSOR
  if (gInputEnabled && !gSettings.inputAutoStart) {
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
