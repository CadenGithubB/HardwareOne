// System_SetupWizardMode.cpp
//
// CLIMode-based setup wizard. See System_SetupWizardMode.h for the
// design rationale and how it differs from the legacy synchronous
// runSetupWizard() (which is still used by FTS-at-boot).
//
// The state-machine breakdown:
//
//   WizardSubMode::PAGE_*         user is on one of the top-level pages
//                                 (Features / Sensors / Network / System);
//                                 inputs: 'n' / 'b' / '<number>'
//
//   WizardSubMode::ESPNOW_*       multi-step ESP-NOW identity entry:
//                                 NAME -> ROOM -> ZONE -> STATIONARY,
//                                 each step is one user input
//
//   WizardSubMode::MQTT_*         multi-step MQTT broker entry:
//                                 HOST -> PORT -> USER -> PASS
//
//   WizardSubMode::WIFI_*         WiFi credentials:
//                                 SSID (number from scan OR raw SSID, or
//                                       'rescan' / 'skip' / 'back') ->
//                                 PASSWORD (or 'back')
//
// At each step we either advance, retreat, or apply settings and exit
// the wizard. The mode returns CLI_MODE_HANDLED while progressing and
// CLI_MODE_HANDLED_AND_EXIT on the final transition (or on cancel).

#include "System_SetupWizardMode.h"
#include "System_SetupWizard.h"   // existing state mutators + renderers
#include "System_CLIMode.h"
#include "System_Settings.h"      // gSettings, setSetting (applied at finalize)
#include "System_Utils.h"          // broadcastOutput
#include "System_Debug.h"

#if ENABLE_WIFI
#include <WiFi.h>
#endif

#if ENABLE_OLED_DISPLAY
#include "OLED_Display.h"          // oledDisplay, oledConnected
#include "OLED_SetupWizard.h"      // renderFeaturesPage / handleFeaturesInput / etc.
#include "System_Mutex.h"          // SensorCacheGuard for gInputCache
#include "i2csensor_seesaw.h"      // gInputCache
#endif

#include <Arduino.h>

// ============================================================================
// Wizard mode state (single instance — only one CLIMode active at a time)
// ============================================================================

enum class WizardSubMode : uint8_t {
  // Top-level pages -- input is n/b/<number> with optional field-entry
  // detour on the System page (device name).
  PAGE_FEATURES = 0,
  PAGE_SENSORS,
  PAGE_NETWORK,
  PAGE_SYSTEM,
  PAGE_SYSTEM_DEVICENAME,   // detour after user picks "Device Name" item

  // Web Interface / Bluetooth mode picker (one numbered choice). Shared mode
  // table with the FTS engine — see System_SetupWizard.cpp wizardModeMenu*.
  PAGE_WEBMODE,
  PAGE_BTMODE,

  // ESP-NOW identity page (linear field walk)
  ESPNOW_INTRO,             // c=Configure / n=Skip / b=Back
  ESPNOW_NAME,
  ESPNOW_ROOM,
  ESPNOW_ZONE,
  ESPNOW_STATIONARY,

  // MQTT broker page (linear field walk)
  MQTT_INTRO,
  MQTT_HOST,
  MQTT_PORT,
  MQTT_USER,
  MQTT_PASS,

  // WiFi credentials page
  WIFI_SSID,                // numbered scan list, type number or raw SSID
                            // or 'rescan' or 'skip' or 'back'
  WIFI_PASSWORD,            // password for selected SSID, or 'back'

  // Sentinel "done" -- onInput returns HANDLED_AND_EXIT next time
  DONE,
};

static struct {
  WizardSubMode subMode;
  SetupWizardResult result;

  // WiFi sub-mode scratch. The numbered SSIDs themselves live in the shared
  // PSRAM choice cache owned by System_SetupWizard.cpp; no Arduino scan index
  // or global scan result survives between painting this prompt and parsing
  // the later user response.
  int   wifiNamedCount;      // valid numeric range for the printed list
  // Gates scan/list printing so an idle repaint does not repeat a blocking
  // scan and console dump. Cleared whenever the copied choice list is
  // consumed or abandoned, so genuine (re)entry rescans.
  bool  wifiScanValid;
  String wifiPendingSsid;    // SSID selected, awaiting password

  // Tracks whether the user used the "configure" path on ESP-NOW so the
  // skip path can disable ESP-NOW entirely.
  bool espnowConfiguring;
} sWizard;

static void clearWizardWifiChoices() {
#if ENABLE_WIFI
  wifiScanClearNamed();
#endif
  sWizard.wifiNamedCount = 0;
  sWizard.wifiScanValid = false;
}

// OLED tick state -- declared up-front because both wizardMode_onEnter
// (which resets them) and wizardMode_onTick (which reads/writes them)
// reference them, and C++ requires the declarations to precede first use.
#if ENABLE_OLED_DISPLAY
static uint32_t        sLastButtons      = 0;
static bool            sLastButtonsValid = false;
static SetupWizardPage sLastRendered     = WIZARD_PAGE_COUNT;
static int             sLastRenderedSel  = -1;
#endif

bool setupWizardMode_isActive();   // forward (defined below)

// ============================================================================
// Page rendering helpers (thin wrappers around existing render functions)
// ============================================================================

static void paintCurrentPage() {
  // The printSerialPageStatus() helper (declared in System_SetupWizard.h)
  // writes the standard n/b/<number> page banner to broadcastOutput,
  // which fans out to every connected transport (serial, web, BLE, etc.).
  // That's exactly the behavior we want -- the user invoking featuresetup
  // from web sees the same paint that a serial user does, and the
  // dashboard's CLI box shows the same prompt.
  printSerialPageStatus();
}

static void paintHeader() {
  broadcastOutput("");
  broadcastOutput("========================================");
  broadcastOutput("       FEATURE CONFIGURATION WIZARD    ");
  broadcastOutput("========================================");
  broadcastOutput("Configure which features to enable.");
  broadcastOutput("");
}

// Map current top-level page to its corresponding sub-mode enum.
static WizardSubMode subModeForPage(SetupWizardPage page) {
  switch (page) {
    case WIZARD_PAGE_FEATURES: return WizardSubMode::PAGE_FEATURES;
    case WIZARD_PAGE_WEBMODE:  return WizardSubMode::PAGE_WEBMODE;
    case WIZARD_PAGE_BTMODE:   return WizardSubMode::PAGE_BTMODE;
    case WIZARD_PAGE_SENSORS:  return WizardSubMode::PAGE_SENSORS;
    case WIZARD_PAGE_NETWORK:  return WizardSubMode::PAGE_NETWORK;
    case WIZARD_PAGE_SYSTEM:   return WizardSubMode::PAGE_SYSTEM;
    case WIZARD_PAGE_ESPNOW:   return WizardSubMode::ESPNOW_INTRO;
    case WIZARD_PAGE_MQTT:     return WizardSubMode::MQTT_INTRO;
    case WIZARD_PAGE_WIFI:     return WizardSubMode::WIFI_SSID;
    default:                   return WizardSubMode::DONE;
  }
}

// Paint the prompt appropriate for the current sub-mode (called after
// every state transition). Returns the response string that cmd_exec
// should reply to the user's input with (e.g., "Enter MQTT host: ").
static const char* paintAfterTransition() {
  switch (sWizard.subMode) {
    case WizardSubMode::PAGE_FEATURES:
    case WizardSubMode::PAGE_SENSORS:
    case WizardSubMode::PAGE_NETWORK:
    case WizardSubMode::PAGE_SYSTEM:
      paintCurrentPage();
      return "";  // page status already printed via broadcastOutput

    case WizardSubMode::PAGE_SYSTEM_DEVICENAME: {
      static char prompt[80];
      snprintf(prompt, sizeof(prompt), "Device Name [%s]: ", getWizardDeviceName());
      return prompt;
    }

    case WizardSubMode::PAGE_WEBMODE:
    case WizardSubMode::PAGE_BTMODE: {
      SetupWizardPage page = getWizardCurrentPage();
      const WizardModeMenu* m = wizardModeMenuForPage(page);
      if (m) {
        char header[80];
        snprintf(header, sizeof(header), "=== %s (SETUP %d/%d) ===", m->title,
                 getWizardPageNumber(page), getWizardTotalPages());
        broadcastOutput("");
        broadcastOutput(header);
        int curIdx = wizardModeCurrentIndex(m);
        for (int i = 0; i < m->modeCount; i++) {
          char ln[64];
          snprintf(ln, sizeof(ln), " %d. [%s] %s", i + 1,
                   i == curIdx ? "X" : " ", m->modes[i].label);
          broadcastOutput(ln);
        }
        broadcastOutput("----------------------------------------");
      }
      return "Enter number to select, 'n' to keep current, 'b' back: ";
    }

    case WizardSubMode::ESPNOW_INTRO: {
      int pageNum = getWizardPageNumber(WIZARD_PAGE_ESPNOW);
      int totalPages = getWizardTotalPages();
      char header[80];
      snprintf(header, sizeof(header), "=== ESP-NOW Identity (SETUP %d/%d) ===",
               pageNum, totalPages);
      broadcastOutput("");
      broadcastOutput(header);
      broadcastOutput("Assign an optional identity for this device in the ESP-NOW mesh.");
      broadcastOutput("----------------------------------------");
      broadcastOutput(" c = Configure (enter fields)");
      broadcastOutput(" n = Next (skip)");
      broadcastOutput(" b = Back");
      return "Choice: ";
    }
    case WizardSubMode::ESPNOW_NAME: {
      static char prompt[80];
      String cur = gSettings.espnowDeviceName.length() > 0
                     ? gSettings.espnowDeviceName : String("HardwareOne");
      snprintf(prompt, sizeof(prompt),
               "Device Name (for Bluetooth + ESP-NOW) [%s]: ", cur.c_str());
      return prompt;
    }
    case WizardSubMode::ESPNOW_ROOM:
      return "Room (e.g. 'Living Room'), 'n' to finish, 'b' to go back: ";
    case WizardSubMode::ESPNOW_ZONE:
      return "Zone (e.g. 'North Wall'), 'n' to finish, 'b' to go back: ";
    case WizardSubMode::ESPNOW_STATIONARY:
      return "The device will be — (m)obile or (s)tationary [m]: ";

    case WizardSubMode::MQTT_INTRO: {
      int pageNum = getWizardPageNumber(WIZARD_PAGE_MQTT);
      int totalPages = getWizardTotalPages();
      char header[80];
      snprintf(header, sizeof(header), "=== MQTT Broker (SETUP %d/%d) ===",
               pageNum, totalPages);
      broadcastOutput("");
      broadcastOutput(header);
      broadcastOutput("Configure the MQTT broker for telemetry.");
      broadcastOutput("----------------------------------------");
      broadcastOutput(" c = Configure (enter broker fields)");
      broadcastOutput(" n = Next (skip)");
      broadcastOutput(" b = Back");
      return "Choice: ";
    }
    case WizardSubMode::MQTT_HOST:
      return "MQTT broker host (e.g. mqtt.example.com), 'n' to skip, 'b' back: ";
    case WizardSubMode::MQTT_PORT:
      return "MQTT broker port [1883], 'n' to skip, 'b' back: ";
    case WizardSubMode::MQTT_USER:
      return "MQTT username (blank = none), 'n' to finish, 'b' back: ";
    case WizardSubMode::MQTT_PASS:
      return "MQTT password (blank = none), 'b' back: ";

    case WizardSubMode::WIFI_SSID: {
#if ENABLE_WIFI
      // Scan + dump the list once per view. The coordinated helper copies the
      // numbered SSIDs to PSRAM and releases Arduino's scan result before it
      // returns, so waiting for input holds no WiFi scan allocation.
      if (!sWizard.wifiScanValid) {
        int pageNum = getWizardPageNumber(WIZARD_PAGE_WIFI);
        int totalPages = getWizardTotalPages();
        char header[80];
        snprintf(header, sizeof(header), "=== WiFi Setup (SETUP %d/%d) ===",
                 pageNum, totalPages);
        broadcastOutput("");
        broadcastOutput(header);
        sWizard.wifiNamedCount = wifiScanPrintNamed(24);
        broadcastOutput("----------------------------------------");
        sWizard.wifiScanValid = true;
      }
      return "Enter number, SSID directly, 'rescan', 'skip', or 'b' back: ";
#else
      broadcastOutput("WiFi not compiled in this build.");
      return "";
#endif
    }
    case WizardSubMode::WIFI_PASSWORD:
      return "Enter WiFi password (or 'b' to go back): ";

    case WizardSubMode::DONE:
      return "";
  }
  return "";
}

// ============================================================================
// onInput dispatch -- one user line per state transition
// ============================================================================

static CLIModeInputResult dispatchTopLevelPage(const String& line,
                                               char* out, size_t outSize);
static CLIModeInputResult dispatchESPNow(const String& line,
                                         char* out, size_t outSize);
static CLIModeInputResult dispatchMQTT(const String& line,
                                       char* out, size_t outSize);
static CLIModeInputResult dispatchWiFi(const String& line,
                                       char* out, size_t outSize);
static CLIModeInputResult dispatchModePage(const String& line,
                                           char* out, size_t outSize);

// Tail-paint helper: after a state transition, render the prompt for the
// new sub-mode and stuff it into `out` for the dispatcher to return to
// the user. `out` may already contain stuff (e.g. an error from the
// transition) — we append the prompt.
static void appendPromptTo(char* out, size_t outSize) {
  const char* prompt = paintAfterTransition();
  if (!prompt || prompt[0] == '\0') return;
  size_t cur = out ? strlen(out) : 0;
  if (out && cur < outSize - 1) {
    if (cur > 0) {
      out[cur] = '\n';
      cur++;
    }
    strncpy(out + cur, prompt, outSize - cur - 1);
    out[outSize - 1] = '\0';
  }
}

static CLIModeInputResult wizardMode_onInput(const String& line, void* /*ud*/,
                                             char* out, size_t outSize) {
  if (out && outSize > 0) out[0] = '\0';

  // Universal cancel: 'cancel' at ANY sub-mode aborts the wizard with
  // no changes saved.
  if (line.equalsIgnoreCase("cancel")) {
    clearWizardWifiChoices();
    snprintf(out, outSize, "Wizard cancelled. No changes saved.");
    sWizard.result.completed = false;
    return CLI_MODE_HANDLED_AND_EXIT;
  }

  switch (sWizard.subMode) {
    case WizardSubMode::PAGE_FEATURES:
    case WizardSubMode::PAGE_SENSORS:
    case WizardSubMode::PAGE_NETWORK:
    case WizardSubMode::PAGE_SYSTEM:
      return dispatchTopLevelPage(line, out, outSize);

    case WizardSubMode::PAGE_WEBMODE:
    case WizardSubMode::PAGE_BTMODE:
      return dispatchModePage(line, out, outSize);

    case WizardSubMode::PAGE_SYSTEM_DEVICENAME: {
      String name = line;
      name.trim();
      if (name.length() > 0) {
        char* buf = getWizardDeviceNameBuf();
        strncpy(buf, name.c_str(), 20);
        buf[20] = '\0';
      }
      // Return to system page
      sWizard.subMode = WizardSubMode::PAGE_SYSTEM;
      appendPromptTo(out, outSize);
      return CLI_MODE_HANDLED;
    }

    case WizardSubMode::ESPNOW_INTRO:
    case WizardSubMode::ESPNOW_NAME:
    case WizardSubMode::ESPNOW_ROOM:
    case WizardSubMode::ESPNOW_ZONE:
    case WizardSubMode::ESPNOW_STATIONARY:
      return dispatchESPNow(line, out, outSize);

    case WizardSubMode::MQTT_INTRO:
    case WizardSubMode::MQTT_HOST:
    case WizardSubMode::MQTT_PORT:
    case WizardSubMode::MQTT_USER:
    case WizardSubMode::MQTT_PASS:
      return dispatchMQTT(line, out, outSize);

    case WizardSubMode::WIFI_SSID:
    case WizardSubMode::WIFI_PASSWORD:
      return dispatchWiFi(line, out, outSize);

    case WizardSubMode::DONE:
      // Shouldn't happen — the previous transition should have returned
      // HANDLED_AND_EXIT. Bail safely.
      sWizard.result.completed = true;
      return CLI_MODE_HANDLED_AND_EXIT;
  }

  snprintf(out, outSize, "Wizard: unknown state (bug); aborting.");
  sWizard.result.completed = false;
  return CLI_MODE_HANDLED_AND_EXIT;
}

// ---------- Top-level page dispatch (Features / Sensors / Network / System)
static CLIModeInputResult dispatchTopLevelPage(const String& line,
                                               char* out, size_t outSize) {
  String lc = line;
  lc.toLowerCase();
  lc.trim();

  if (lc == "n" || lc == "next") {
    bool advanced = wizardNextPage(sWizard.result);
    if (!advanced) {
      // Reached end -- finalize wizard
      sWizard.result.completed = true;
      snprintf(out, outSize, "Feature configuration complete.");
      return CLI_MODE_HANDLED_AND_EXIT;
    }
    sWizard.subMode = subModeForPage(getWizardCurrentPage());
    appendPromptTo(out, outSize);
    return CLI_MODE_HANDLED;
  }
  if (lc == "b" || lc == "back") {
    wizardPrevPage();
    sWizard.subMode = subModeForPage(getWizardCurrentPage());
    appendPromptTo(out, outSize);
    return CLI_MODE_HANDLED;
  }
  // Numeric: toggle/select item
  int num = lc.toInt();
  if (num > 0) {
    setWizardCurrentSelection(num - 1);
    SetupWizardPage page = getWizardCurrentPage();
    if (page == WIZARD_PAGE_SYSTEM) {
      if (getSystemItemAt(num - 1) == SYS_ITEM_DEVICE_NAME) {
        // Field-entry detour: ask for device name next
        sWizard.subMode = WizardSubMode::PAGE_SYSTEM_DEVICENAME;
        appendPromptTo(out, outSize);
        return CLI_MODE_HANDLED;
      }
      wizardCycleOption();
    } else {
      wizardToggleCurrentItem();
    }
    appendPromptTo(out, outSize);
    return CLI_MODE_HANDLED;
  }

  snprintf(out, outSize, "Unrecognized input. Use 'n' (next), 'b' (back), "
                         "a number to toggle/select, or 'cancel' to abort the wizard.");
  return CLI_MODE_HANDLED;
}

// ---------- Web / Bluetooth mode-picker page (one numbered choice) ----------
static CLIModeInputResult dispatchModePage(const String& line,
                                           char* out, size_t outSize) {
  String lc = line; lc.toLowerCase(); lc.trim();
  const WizardModeMenu* m = wizardModeMenuForPage(getWizardCurrentPage());

  if (lc == "b" || lc == "back") {
    wizardPrevPage();
    sWizard.subMode = subModeForPage(getWizardCurrentPage());
    appendPromptTo(out, outSize);
    return CLI_MODE_HANDLED;
  }

  if (lc == "n" || lc == "next") {
    // Advance, committing whatever mode is currently selected.
    if (!wizardNextPage(sWizard.result)) {
      sWizard.result.completed = true;
      snprintf(out, outSize, "Feature configuration complete.");
      return CLI_MODE_HANDLED_AND_EXIT;
    }
    sWizard.subMode = subModeForPage(getWizardCurrentPage());
    appendPromptTo(out, outSize);
    return CLI_MODE_HANDLED;
  }

  // Numeric: SELECT that mode and STAY on the page (re-render shows the moved
  // [X]) — exactly like the feature/sensor pages' toggle-and-stay. Advancing is
  // 'n'/'next' only, matching the page's "# to select, 'n' next" hint. (Was:
  // select-and-auto-advance, which was the odd-one-out in the wizard.)
  int num = lc.toInt();
  if (m && num >= 1 && num <= m->modeCount) {
    wizardModeApply(m, num - 1);
    appendPromptTo(out, outSize);
    return CLI_MODE_HANDLED;
  }

  snprintf(out, outSize, "Enter a mode number to select, 'n' for next, or 'b' for back.");
  return CLI_MODE_HANDLED;
}

// ---------- ESP-NOW identity sub-page (NAME -> ROOM -> ZONE -> STATIONARY)
static CLIModeInputResult dispatchESPNow(const String& line,
                                         char* out, size_t outSize) {
  String raw = line; raw.trim();
  String lc = raw; lc.toLowerCase();
  const bool isBack = (lc == "b" || lc == "back");
  const bool isNext = (lc == "n" || lc == "next");

  auto goBack = [&]() {
    wizardPrevPage();
    sWizard.subMode = subModeForPage(getWizardCurrentPage());
  };
  auto goNextPage = [&]() {
    if (!wizardNextPage(sWizard.result)) {
      sWizard.result.completed = true;
    }
    sWizard.subMode = subModeForPage(getWizardCurrentPage());
  };
  auto applyAndAdvance = [&]() {
    // Apply collected fields to gSettings (mirrors handleSerialESPNowPage)
    gSettings.bleDeviceName    = sWizard.result.espnowFriendlyName;
    gSettings.espnowDeviceName = sWizard.result.espnowFriendlyName;
    if (sWizard.result.espnowFriendlyName.length() > 0)
      gSettings.espnowFriendlyName = sWizard.result.espnowFriendlyName;
    if (sWizard.result.espnowRoom.length() > 0)
      gSettings.espnowRoom = sWizard.result.espnowRoom;
    if (sWizard.result.espnowZone.length() > 0)
      gSettings.espnowZone = sWizard.result.espnowZone;
    gSettings.espnowStationary = sWizard.result.espnowStationary;
    broadcastOutput("ESP-NOW identity configured.");
    goNextPage();
  };

  switch (sWizard.subMode) {
    case WizardSubMode::ESPNOW_INTRO:
      if (isBack)         { goBack(); appendPromptTo(out, outSize); return CLI_MODE_HANDLED; }
      if (lc == "c" || lc == "configure") {
        sWizard.espnowConfiguring = true;
        sWizard.subMode = WizardSubMode::ESPNOW_NAME;
        broadcastOutput("----------------------------------------");
        broadcastOutput("All fields optional. Enter to skip field, "
                        "'n' to finish, 'b' to go back.");
        broadcastOutput("----------------------------------------");
        appendPromptTo(out, outSize); return CLI_MODE_HANDLED;
      }
      if (isNext || raw.length() == 0) {
        // Explicit skip (or blank/Enter) — disable ESP-NOW since it's
        // unconfigured; it can't start without a name.
        gSettings.espnowEnabled = false;
        goNextPage();
        appendPromptTo(out, outSize);
        return (sWizard.subMode == WizardSubMode::DONE) ? CLI_MODE_HANDLED_AND_EXIT
                                                         : CLI_MODE_HANDLED;
      }
      // Anything else looks like the user typed their device name directly
      // instead of picking from the c/n/b menu — treat it as the name and
      // continue into the configure flow (matches handleSerialESPNowPage)
      // rather than silently skipping (which used to also disable ESP-NOW)
      // on what's very likely a menu mix-up, not an intentional skip.
      sWizard.espnowConfiguring = true;
      sWizard.result.espnowFriendlyName = raw;
      sWizard.subMode = WizardSubMode::ESPNOW_ROOM;
      broadcastOutput("----------------------------------------");
      broadcastOutput("All fields optional. Enter to skip field, "
                      "'n' to finish, 'b' to go back.");
      broadcastOutput("----------------------------------------");
      appendPromptTo(out, outSize); return CLI_MODE_HANDLED;

    case WizardSubMode::ESPNOW_NAME:
      if (isBack) { goBack(); appendPromptTo(out, outSize); return CLI_MODE_HANDLED; }
      if (isNext) { applyAndAdvance(); appendPromptTo(out, outSize); return CLI_MODE_HANDLED; }
      // Empty -> keep current. Non-empty -> use as friendly name.
      if (raw.length() > 0) sWizard.result.espnowFriendlyName = raw;
      else                  sWizard.result.espnowFriendlyName = gSettings.espnowDeviceName.length() > 0
                                                                ? gSettings.espnowDeviceName : String("HardwareOne");
      sWizard.subMode = WizardSubMode::ESPNOW_ROOM;
      appendPromptTo(out, outSize); return CLI_MODE_HANDLED;

    case WizardSubMode::ESPNOW_ROOM:
      if (isBack) { sWizard.subMode = WizardSubMode::ESPNOW_NAME; appendPromptTo(out, outSize); return CLI_MODE_HANDLED; }
      if (isNext) { applyAndAdvance(); appendPromptTo(out, outSize); return CLI_MODE_HANDLED; }
      sWizard.result.espnowRoom = raw;
      sWizard.subMode = WizardSubMode::ESPNOW_ZONE;
      appendPromptTo(out, outSize); return CLI_MODE_HANDLED;

    case WizardSubMode::ESPNOW_ZONE:
      if (isBack) { sWizard.subMode = WizardSubMode::ESPNOW_ROOM; appendPromptTo(out, outSize); return CLI_MODE_HANDLED; }
      if (isNext) { applyAndAdvance(); appendPromptTo(out, outSize); return CLI_MODE_HANDLED; }
      sWizard.result.espnowZone = raw;
      sWizard.subMode = WizardSubMode::ESPNOW_STATIONARY;
      appendPromptTo(out, outSize); return CLI_MODE_HANDLED;

    case WizardSubMode::ESPNOW_STATIONARY:
      if (isBack) { sWizard.subMode = WizardSubMode::ESPNOW_ZONE; appendPromptTo(out, outSize); return CLI_MODE_HANDLED; }
      sWizard.result.espnowStationary = (lc == "s" || lc == "stationary");
      applyAndAdvance();
      appendPromptTo(out, outSize);
      return (sWizard.subMode == WizardSubMode::DONE) ? CLI_MODE_HANDLED_AND_EXIT
                                                       : CLI_MODE_HANDLED;

    default: break;
  }
  // Shouldn't reach
  snprintf(out, outSize, "Wizard: ESP-NOW dispatch error");
  return CLI_MODE_HANDLED;
}

// ---------- MQTT broker sub-page (HOST -> PORT -> USER -> PASS)
static CLIModeInputResult dispatchMQTT(const String& line,
                                       char* out, size_t outSize) {
  String raw = line; raw.trim();
  String lc = raw; lc.toLowerCase();
  const bool isBack = (lc == "b" || lc == "back");
  const bool isNext = (lc == "n" || lc == "next");

  auto goBack = [&]() {
    wizardPrevPage();
    sWizard.subMode = subModeForPage(getWizardCurrentPage());
  };
  auto goNextPage = [&]() {
    if (!wizardNextPage(sWizard.result)) {
      sWizard.result.completed = true;
    }
    sWizard.subMode = subModeForPage(getWizardCurrentPage());
  };
  auto applyAndAdvance = [&]() {
    if (sWizard.result.mqttHost.length() > 0)     gSettings.mqttHost     = sWizard.result.mqttHost;
    if (sWizard.result.mqttPort > 0)              gSettings.mqttPort     = sWizard.result.mqttPort;
    if (sWizard.result.mqttUser.length() > 0)     gSettings.mqttUser     = sWizard.result.mqttUser;
    if (sWizard.result.mqttPassword.length() > 0) gSettings.mqttPassword = sWizard.result.mqttPassword;
    broadcastOutput("MQTT broker configured.");
    goNextPage();
  };

  switch (sWizard.subMode) {
    case WizardSubMode::MQTT_INTRO:
      if (isBack) { goBack(); appendPromptTo(out, outSize); return CLI_MODE_HANDLED; }
      if (lc == "c" || lc == "configure") {
        sWizard.subMode = WizardSubMode::MQTT_HOST;
        broadcastOutput("----------------------------------------");
        broadcastOutput("Enter MQTT broker fields. 'n' skips, 'b' goes back.");
        broadcastOutput("----------------------------------------");
        appendPromptTo(out, outSize); return CLI_MODE_HANDLED;
      }
      // skip
      goNextPage();
      appendPromptTo(out, outSize);
      return (sWizard.subMode == WizardSubMode::DONE) ? CLI_MODE_HANDLED_AND_EXIT
                                                       : CLI_MODE_HANDLED;

    case WizardSubMode::MQTT_HOST:
      if (isBack) { goBack(); appendPromptTo(out, outSize); return CLI_MODE_HANDLED; }
      if (isNext) { applyAndAdvance(); appendPromptTo(out, outSize); return CLI_MODE_HANDLED; }
      sWizard.result.mqttHost = raw;
      sWizard.subMode = WizardSubMode::MQTT_PORT;
      appendPromptTo(out, outSize); return CLI_MODE_HANDLED;

    case WizardSubMode::MQTT_PORT:
      if (isBack) { sWizard.subMode = WizardSubMode::MQTT_HOST; appendPromptTo(out, outSize); return CLI_MODE_HANDLED; }
      if (isNext) { applyAndAdvance(); appendPromptTo(out, outSize); return CLI_MODE_HANDLED; }
      if (raw.length() == 0) sWizard.result.mqttPort = 1883;
      else                   sWizard.result.mqttPort = raw.toInt();
      sWizard.subMode = WizardSubMode::MQTT_USER;
      appendPromptTo(out, outSize); return CLI_MODE_HANDLED;

    case WizardSubMode::MQTT_USER:
      if (isBack) { sWizard.subMode = WizardSubMode::MQTT_PORT; appendPromptTo(out, outSize); return CLI_MODE_HANDLED; }
      if (isNext) { applyAndAdvance(); appendPromptTo(out, outSize); return CLI_MODE_HANDLED; }
      sWizard.result.mqttUser = raw;
      sWizard.subMode = WizardSubMode::MQTT_PASS;
      appendPromptTo(out, outSize); return CLI_MODE_HANDLED;

    case WizardSubMode::MQTT_PASS:
      if (isBack) { sWizard.subMode = WizardSubMode::MQTT_USER; appendPromptTo(out, outSize); return CLI_MODE_HANDLED; }
      sWizard.result.mqttPassword = raw;
      applyAndAdvance();
      appendPromptTo(out, outSize);
      return (sWizard.subMode == WizardSubMode::DONE) ? CLI_MODE_HANDLED_AND_EXIT
                                                       : CLI_MODE_HANDLED;

    default: break;
  }
  snprintf(out, outSize, "Wizard: MQTT dispatch error");
  return CLI_MODE_HANDLED;
}

// ---------- WiFi credentials sub-page (SSID -> PASSWORD)
static CLIModeInputResult dispatchWiFi(const String& line,
                                       char* out, size_t outSize) {
  String raw = line; raw.trim();
  String lc = raw; lc.toLowerCase();

  switch (sWizard.subMode) {
    case WizardSubMode::WIFI_SSID: {
#if ENABLE_WIFI
      if (lc == "b" || lc == "back") {
        clearWizardWifiChoices();
        wizardPrevPage();
        sWizard.subMode = subModeForPage(getWizardCurrentPage());
        appendPromptTo(out, outSize);
        return CLI_MODE_HANDLED;
      }
      if (lc == "rescan") {
        clearWizardWifiChoices();             // force a fresh scan on repaint
        appendPromptTo(out, outSize);  // re-paints scan results
        return CLI_MODE_HANDLED;
      }
      if (lc == "skip" || raw.length() == 0) {
        clearWizardWifiChoices();
        sWizard.result.completed = true;
        snprintf(out, outSize, "WiFi configuration skipped.");
        return CLI_MODE_HANDLED_AND_EXIT;
      }
      // Bare in-range integer -> Nth NAMED network; digit-leading SSIDs
      // (e.g. "2WIRE123") fall through to typed-SSID handling.
      String ssid = raw;
      int idx = raw.toInt();
      if (idx >= 1 && idx <= sWizard.wifiNamedCount && String(idx) == raw) {
        if (!wifiScanGetNamedSSID(idx - 1, ssid)) {
          clearWizardWifiChoices();
          snprintf(out, outSize, "WiFi list expired; scanning again.");
          appendPromptTo(out, outSize);
          return CLI_MODE_HANDLED;
        }
      }
      clearWizardWifiChoices();
      if (ssid.length() == 0) {
        snprintf(out, outSize, "Empty SSID; type a number, an SSID, "
                               "or 'rescan'/'skip'/'b'.");
        return CLI_MODE_HANDLED;
      }
      sWizard.wifiPendingSsid = ssid;
      sWizard.subMode = WizardSubMode::WIFI_PASSWORD;
      char line2[64];
      snprintf(line2, sizeof(line2), "Selected SSID: %s", ssid.c_str());
      broadcastOutput(line2);
      appendPromptTo(out, outSize);
      return CLI_MODE_HANDLED;
#else
      snprintf(out, outSize, "WiFi not compiled in this build.");
      sWizard.result.completed = true;
      return CLI_MODE_HANDLED_AND_EXIT;
#endif
    }

    case WizardSubMode::WIFI_PASSWORD: {
      if (lc == "b" || lc == "back") {
        sWizard.subMode = WizardSubMode::WIFI_SSID;
        appendPromptTo(out, outSize);
        return CLI_MODE_HANDLED;
      }
      sWizard.result.wifiSSID       = sWizard.wifiPendingSsid;
      sWizard.result.wifiPassword   = raw;
      sWizard.result.wifiConfigured = true;
      sWizard.result.completed      = true;
      snprintf(out, outSize, "WiFi credentials captured for '%s'.",
               sWizard.wifiPendingSsid.c_str());
      return CLI_MODE_HANDLED_AND_EXIT;
    }

    default: break;
  }
  snprintf(out, outSize, "Wizard: WiFi dispatch error");
  return CLI_MODE_HANDLED;
}

// ============================================================================
// Lifecycle callbacks
// ============================================================================

static void wizardMode_onEnter(void* /*ud*/) {
  // Reset the wizard's state (existing initSetupWizard rebuilds the
  // features list etc.).
  initSetupWizard();

  // Sync timezone selection from current settings (same as runSetupWizard).
  const TimezoneEntry* tzList = getTimezones();
  size_t tzCount = getTimezoneCount();
  for (size_t i = 0; i < tzCount; i++) {
    if (tzList[i].offsetMinutes == gSettings.tzOffsetMinutes) {
      setWizardTimezoneSelection(i);
      break;
    }
  }

  // Reset our own state
  sWizard.subMode           = WizardSubMode::PAGE_FEATURES;
  sWizard.result            = SetupWizardResult();
  sWizard.result.completed  = false;
  sWizard.result.deviceName = "HardwareOne";
  sWizard.result.timezoneOffset = -240;
  sWizard.result.timezoneAbbrev = "EDT";
  clearWizardWifiChoices();
  sWizard.wifiPendingSsid   = "";
  sWizard.espnowConfiguring = false;

#if ENABLE_OLED_DISPLAY
  // Reset joystick edge-detect state for this run -- prevents the first
  // tick from interpreting button state at entry as a fresh press.
  sLastButtons        = 0;
  sLastButtonsValid   = false;
  sLastRendered       = WIZARD_PAGE_COUNT;
  sLastRenderedSel    = -1;
  resetWizardJoystickState();
#endif

  paintHeader();
  paintCurrentPage();
  broadcastOutput("Type 'cancel' at any time to abort the wizard.");
}

// External WiFi save helpers (definitions in System_WiFi.cpp). Mirror the
// approach used by runAndApplyFeatureWizard in System_SetupWizard.cpp:
// the wizard's WiFi credentials go through the saved-networks list
// (network.wifi.networks[] in settings.json), which the runtime
// connect-prefer-saved logic reads on next boot.
extern bool upsertWiFiNetwork(const String& ssid, const String& password,
                              int priority, bool hidden);
extern void sortWiFiByPriority();
extern bool saveWiFiNetworks();
extern bool writeSettingsJson();
extern void applySettings();

static void wizardMode_onExit(void* /*ud*/) {
  // onExit also covers timeout/session-owner loss and cliExitMode().
  clearWizardWifiChoices();
  if (sWizard.result.completed) {
    // Apply collected results (mirrors what runAndApplyFeatureWizard does
    // after runSetupWizard returns).
    broadcastOutput("");
    broadcastOutput("Feature configuration complete.");
    broadcastOutput(String("Timezone: ") + sWizard.result.timezoneAbbrev);

    // Push timezone / log level / NTP / LED / device name into gSettings.
    // wizardFinalize is the shared helper used by the legacy wizard too;
    // it reads from the wizard-state singletons (timezoneSelection, etc.)
    // and writes to gSettings.
    wizardFinalize(sWizard.result);

#if ENABLE_WIFI
    if (sWizard.result.wifiConfigured && sWizard.result.wifiSSID.length() > 0) {
      // Use the saved-networks list, matching runAndApplyFeatureWizard:
      // upsert the entry, re-sort by priority, persist to disk. This
      // writes network.wifi.networks[] in settings.json, which the runtime
      // connect-saved-networks loop reads.
      upsertWiFiNetwork(sWizard.result.wifiSSID, sWizard.result.wifiPassword,
                        /*priority=*/1, /*hidden=*/false);
      sortWiFiByPriority();
      saveWiFiNetworks();
      setSetting(gSettings.wifiAutoStart, true);
      broadcastOutput("WiFi credentials saved: " + sWizard.result.wifiSSID);
    }
#endif

    // Flush settings.json and re-apply runtime settings (output flags,
    // log level, etc.) so the wizard's choices take effect immediately
    // without requiring a reboot. Matches the legacy flow.
    writeSettingsJson();
    applySettings();
#if ENABLE_WIFI
    // Re-register SNTP with the wizard's NTP choice — applySettings() does
    // not reach setupNTP(), so without this the new server only took effect
    // on the next reboot.
    if (WiFi.isConnected()) setupNTP();
#endif
  } else {
    broadcastOutput("Feature setup cancelled. No changes saved.");
  }
  DEBUGF(DEBUG_CLI, "[wizardMode] exited (completed=%d)", sWizard.result.completed ? 1 : 0);
}

// Periodic tick — called from HardwareOne.cpp loop() every iteration
// while the wizard mode is active. Used for:
//   1. OLED joystick polling on devices with a display attached
//   2. OLED page rendering for the current sub-mode
//
// On non-OLED builds this is a no-op. Text input from the exact session that
// opened the wizard flows through wizardMode_onInput via cliModeDispatchInput.
//
// Keep this fast -- runs every loop tick. The persistent tick-state
// (sLastButtons, sLastRendered, ...) is declared at the top of the
// file alongside sWizard so onEnter can reset it.
static void wizardMode_onTick(void* /*ud*/) {
#if ENABLE_OLED_DISPLAY
  if (!oledDisplay || !oledConnected) return;

  // ---- 1. Joystick poll ----
  uint32_t buttons = sLastButtons;
  bool haveButtons = false;
  {
    SensorCacheGuard g(gInputCache.mutex, pdMS_TO_TICKS(5), "wizardMode.buttonRead");
    if (g.held && gInputCache.dataValid) {
      buttons = gInputCache.buttons;
      haveButtons = true;
    }
  }
  // First read just establishes the baseline; the first edge will be
  // detected next tick.
  if (haveButtons && !sLastButtonsValid) {
    sLastButtons = buttons;
    sLastButtonsValid = true;
  } else if (haveButtons) {
    uint32_t pressedNow  = ~buttons;
    uint32_t pressedLast = ~sLastButtons;
    uint32_t newButtons  = pressedNow & ~pressedLast;
    sLastButtons = buttons;

    JoystickNav nav = readWizardJoystickNav();
    bool hasInput = (newButtons != 0) || nav.up || nav.down || nav.left || nav.right;

    if (hasInput) {
      (void)cliModeNoteActivity();
      // Translate joystick event into wizard state change. Only meaningful
      // on the top-level pages -- sub-pages (ESPNOW / MQTT / WiFi) require
      // text entry which the joystick cannot provide.
      switch (sWizard.subMode) {
        case WizardSubMode::PAGE_FEATURES:
          handleFeaturesInput(newButtons, nav);
          break;
        case WizardSubMode::PAGE_SENSORS:
          handleSensorsInput(newButtons, nav);
          break;
        case WizardSubMode::PAGE_NETWORK:
          handleNetworkInput(newButtons, nav);
          break;
        case WizardSubMode::PAGE_SYSTEM:
          if (!handleSystemInput(newButtons, nav, sWizard.result)) {
            // System page returns false to signal "user finished"
            sWizard.result.completed = true;
            cliExitMode();
            return;
          }
          break;
        default:
          // Text-entry sub-pages cannot safely borrow another transport's
          // login: that would defeat exact-session ownership. Left always
          // provides a physical escape back to the prior page; to enter text,
          // cancel and reopen the wizard from a text-capable interface.
          if (nav.left) {
            if (sWizard.subMode == WizardSubMode::WIFI_SSID ||
                sWizard.subMode == WizardSubMode::WIFI_PASSWORD) {
              clearWizardWifiChoices();
            }
            wizardPrevPage();
            sWizard.subMode = subModeForPage(getWizardCurrentPage());
          }
          break;
      }
      // Sync our sub-mode to whatever wizardNextPage/Prev changed under us.
      sWizard.subMode = subModeForPage(getWizardCurrentPage());
    }
  }

  // ---- 2. OLED render for current sub-mode (only when state changed) ----
  SetupWizardPage curPage = getWizardCurrentPage();
  int curSel = getWizardCurrentSelection();
  if (curPage != sLastRendered || curSel != sLastRenderedSel) {
    switch (sWizard.subMode) {
      case WizardSubMode::PAGE_FEATURES: renderFeaturesPage(); break;
      case WizardSubMode::PAGE_SENSORS:  renderSensorsPage();  break;
      case WizardSubMode::PAGE_NETWORK:  renderNetworkPage();  break;
      case WizardSubMode::PAGE_SYSTEM:   renderSystemPage();   break;
      default: break;  // sub-page screens are text-based on serial
    }
    sLastRendered    = curPage;
    sLastRenderedSel = curSel;
  }
#endif  // ENABLE_OLED_DISPLAY
}

static const CLIMode kWizardMode = {
  "wizard",
  wizardMode_onEnter,
  wizardMode_onInput,
  wizardMode_onExit,
  wizardMode_onTick,
  nullptr,  // userData lives in sWizard static
  10u * 60u * 1000u,
};

// ============================================================================
// Public API
// ============================================================================

bool setupWizardMode_start() {
  if (cliInModeActive()) {
    DEBUGF(DEBUG_CLI, "[wizardMode] start rejected: '%s' already active",
           cliCurrentMode() && cliCurrentMode()->name ? cliCurrentMode()->name : "(unnamed)");
    return false;
  }
  return cliEnterMode(&kWizardMode);
}

bool setupWizardMode_isActive() {
  return cliCurrentMode() == &kWizardMode;
}
