// System_SetupWizardMode.cpp
//
// CLIMode-based setup wizard. See System_SetupWizardMode.h for the
// design rationale and how it differs from the legacy synchronous
// runSetupWizard() (which is still used by FTS-at-boot).
//
// The state-machine breakdown:
//
//   WizardSubMode::PAGE_GROUP     user is on a shared settings group;
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
#include "HAL_Input.h"
#include "System_SetupWizard.h"   // existing state mutators + renderers
#include "System_CLIMode.h"
#include "System_Debug.h"

#if ENABLE_OLED_DISPLAY
#include "OLED_Display.h"          // oledDisplay, oledConnected
#include "OLED_SetupWizard.h"      // shared group rendering and input
#endif

#include <Arduino.h>

// ============================================================================
// Wizard mode state (single instance — only one CLIMode active at a time)
// ============================================================================

enum class WizardSubMode : uint8_t {
  // Shared settings groups -- n/b/<number>, with a device-name text detour.
  PAGE_GROUP = 0,
  PAGE_DEVICE_NAME,

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
  if (wizardIsGroupPage(page)) return WizardSubMode::PAGE_GROUP;
  switch (page) {
    case WIZARD_PAGE_WEBMODE: return WizardSubMode::PAGE_WEBMODE;
    case WIZARD_PAGE_BTMODE:  return WizardSubMode::PAGE_BTMODE;
    case WIZARD_PAGE_ESPNOW:  return WizardSubMode::ESPNOW_INTRO;
    case WIZARD_PAGE_MQTT:    return WizardSubMode::MQTT_INTRO;
    case WIZARD_PAGE_WIFI:    return WizardSubMode::WIFI_SSID;
    default:                  return WizardSubMode::DONE;
  }
}

// Paint the prompt appropriate for the current sub-mode (called after
// every state transition). Returns the response string that cmd_exec
// should reply to the user's input with (e.g., "Enter MQTT host: ").
static const char* paintAfterTransition() {
  switch (sWizard.subMode) {
    case WizardSubMode::PAGE_GROUP:
      paintCurrentPage();
      return "";  // page status already printed via broadcastOutput

    case WizardSubMode::PAGE_DEVICE_NAME: {
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
      const String& savedName = getWizardDraftValues().deviceName;
      String cur = savedName.length() > 0 ? savedName : String("HardwareOne");
      snprintf(prompt, sizeof(prompt),
               "Device Name (for Bluetooth + ESP-NOW) [%s]: ", cur.c_str());
      return prompt;
    }
    case WizardSubMode::ESPNOW_ROOM: {
      static char prompt[112];
      snprintf(prompt, sizeof(prompt), "Room [%s], blank keeps it, 'n' to finish, 'b' back: ",
               getWizardDraftValues().espnowRoom.c_str());
      return prompt;
    }
    case WizardSubMode::ESPNOW_ZONE: {
      static char prompt[112];
      snprintf(prompt, sizeof(prompt), "Zone [%s], blank keeps it, 'n' to finish, 'b' back: ",
               getWizardDraftValues().espnowZone.c_str());
      return prompt;
    }
    case WizardSubMode::ESPNOW_STATIONARY:
      return getWizardDraftValues().espnowStationary
               ? "The device will be — (m)obile or (s)tationary [s]: "
               : "The device will be — (m)obile or (s)tationary [m]: ";

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
    case WizardSubMode::MQTT_HOST: {
      static char prompt[144];
      snprintf(prompt, sizeof(prompt), "MQTT broker host [%s], blank keeps it, 'n' to finish, 'b' back: ",
               getWizardDraftValues().mqttHost.c_str());
      return prompt;
    }
    case WizardSubMode::MQTT_PORT: {
      static char prompt[96];
      snprintf(prompt, sizeof(prompt), "MQTT broker port [%d], blank keeps it, 'n' to finish, 'b' back: ",
               getWizardDraftValues().mqttPort);
      return prompt;
    }
    case WizardSubMode::MQTT_USER: {
      static char prompt[128];
      snprintf(prompt, sizeof(prompt), "MQTT username [%s], blank keeps it, 'n' to finish, 'b' back: ",
               getWizardDraftValues().mqttUser.c_str());
      return prompt;
    }
    case WizardSubMode::MQTT_PASS:
      return "MQTT password (blank keeps it), 'b' back: ";

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
    case WizardSubMode::PAGE_GROUP:
      return dispatchTopLevelPage(line, out, outSize);

    case WizardSubMode::PAGE_WEBMODE:
    case WizardSubMode::PAGE_BTMODE:
      return dispatchModePage(line, out, outSize);

    case WizardSubMode::PAGE_DEVICE_NAME: {
      String name = line;
      name.trim();
      if (name.length() > 0) {
        char* buf = getWizardDeviceNameBuf();
        strncpy(buf, name.c_str(), 19);
        buf[19] = '\0';
      }
      sWizard.subMode = subModeForPage(getWizardCurrentPage());
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
      // HANDLED_AND_EXIT. Preserve the completion decision from the core.
      return CLI_MODE_HANDLED_AND_EXIT;
  }

  snprintf(out, outSize, "Wizard: unknown state (bug); aborting.");
  sWizard.result.completed = false;
  return CLI_MODE_HANDLED_AND_EXIT;
}

// ---------- Shared settings group dispatch
static CLIModeInputResult dispatchTopLevelPage(const String& line,
                                               char* out, size_t outSize) {
  String lc = line;
  lc.toLowerCase();
  lc.trim();

  if (lc == "n" || lc == "next") {
    bool advanced = wizardNextPage(sWizard.result);
    if (!advanced) {
      // Reached end -- finalize wizard
      snprintf(out, outSize, "Applying feature configuration...");
      return CLI_MODE_HANDLED_AND_EXIT;
    }
    sWizard.subMode = subModeForPage(getWizardCurrentPage());
    appendPromptTo(out, outSize);
    return CLI_MODE_HANDLED;
  }
  if (lc == "b" || lc == "back") {
    if (wizardIsFirstPage()) {
      requestWizardCancel();
      sWizard.result.completed = false;
      snprintf(out, outSize, "Wizard cancelled. No changes saved.");
      return CLI_MODE_HANDLED_AND_EXIT;
    }
    wizardPrevPage();
    sWizard.subMode = subModeForPage(getWizardCurrentPage());
    appendPromptTo(out, outSize);
    return CLI_MODE_HANDLED;
  }
  const long number = lc.toInt();
  if (number > 0 && (size_t)number <= getWizardMenuItemCount(getWizardCurrentPage()) && String(number) == lc) {
    const int index = (int)(number - 1);
    const WizardMenuItem* item = getWizardMenuItem(getWizardCurrentPage(), index);
    setWizardCurrentSelection(index);
    if (item->type == WIZARD_ITEM_DEVICE_NAME) sWizard.subMode = WizardSubMode::PAGE_DEVICE_NAME;
    else wizardActivateCurrentItem();
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
      snprintf(out, outSize, "Applying feature configuration...");
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
      sWizard.subMode = WizardSubMode::DONE;
    } else {
      sWizard.subMode = subModeForPage(getWizardCurrentPage());
    }
  };
  auto stageAndAdvance = [&]() {
    if (!wizardStageESPNowSettings(sWizard.result)) {
      snprintf(out, outSize, "Error: ESP-NOW choices could not be staged. Try again or cancel.");
      return;
    }
    broadcastOutput("ESP-NOW identity staged.");
    goNextPage();
  };

  switch (sWizard.subMode) {
    case WizardSubMode::ESPNOW_INTRO:
      if (isBack)         { goBack(); appendPromptTo(out, outSize); return CLI_MODE_HANDLED; }
      if (lc == "c" || lc == "configure") {
        sWizard.espnowConfiguring = true;
        sWizard.result.espnowFriendlyName = "";
        sWizard.result.espnowRoom = "";
        sWizard.result.espnowZone = "";
        sWizard.result.espnowStationary = getWizardDraftValues().espnowStationary;
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
        wizardSetFeatureEnabled("espnow", false);
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
      sWizard.result.espnowRoom = "";
      sWizard.result.espnowZone = "";
      sWizard.result.espnowStationary = getWizardDraftValues().espnowStationary;
      sWizard.result.espnowFriendlyName = raw;
      sWizard.subMode = WizardSubMode::ESPNOW_ROOM;
      broadcastOutput("----------------------------------------");
      broadcastOutput("All fields optional. Enter to skip field, "
                      "'n' to finish, 'b' to go back.");
      broadcastOutput("----------------------------------------");
      appendPromptTo(out, outSize); return CLI_MODE_HANDLED;

    case WizardSubMode::ESPNOW_NAME:
      if (isBack) { goBack(); appendPromptTo(out, outSize); return CLI_MODE_HANDLED; }
      if (isNext) {
        sWizard.result.espnowFriendlyName = "";  // Match serial early finish, including after Back.
        stageAndAdvance();
        appendPromptTo(out, outSize);
        return sWizard.subMode == WizardSubMode::DONE ? CLI_MODE_HANDLED_AND_EXIT : CLI_MODE_HANDLED;
      }
      // Empty -> keep current. Non-empty -> use as friendly name.
      if (raw.length() > 0) sWizard.result.espnowFriendlyName = raw;
      else                  sWizard.result.espnowFriendlyName = getWizardDraftValues().deviceName.length() > 0
                                                                ? getWizardDraftValues().deviceName : String("HardwareOne");
      sWizard.subMode = WizardSubMode::ESPNOW_ROOM;
      appendPromptTo(out, outSize); return CLI_MODE_HANDLED;

    case WizardSubMode::ESPNOW_ROOM:
      if (isBack) { sWizard.subMode = WizardSubMode::ESPNOW_NAME; appendPromptTo(out, outSize); return CLI_MODE_HANDLED; }
      if (isNext) {
        stageAndAdvance();
        appendPromptTo(out, outSize);
        return sWizard.subMode == WizardSubMode::DONE ? CLI_MODE_HANDLED_AND_EXIT : CLI_MODE_HANDLED;
      }
      sWizard.result.espnowRoom = raw;
      sWizard.subMode = WizardSubMode::ESPNOW_ZONE;
      appendPromptTo(out, outSize); return CLI_MODE_HANDLED;

    case WizardSubMode::ESPNOW_ZONE:
      if (isBack) { sWizard.subMode = WizardSubMode::ESPNOW_ROOM; appendPromptTo(out, outSize); return CLI_MODE_HANDLED; }
      if (isNext) {
        stageAndAdvance();
        appendPromptTo(out, outSize);
        return sWizard.subMode == WizardSubMode::DONE ? CLI_MODE_HANDLED_AND_EXIT : CLI_MODE_HANDLED;
      }
      sWizard.result.espnowZone = raw;
      sWizard.subMode = WizardSubMode::ESPNOW_STATIONARY;
      appendPromptTo(out, outSize); return CLI_MODE_HANDLED;

    case WizardSubMode::ESPNOW_STATIONARY:
      if (isBack) { sWizard.subMode = WizardSubMode::ESPNOW_ZONE; appendPromptTo(out, outSize); return CLI_MODE_HANDLED; }
      if (raw.length() > 0) sWizard.result.espnowStationary = (lc == "s" || lc == "stationary");
      stageAndAdvance();
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
      sWizard.subMode = WizardSubMode::DONE;
    } else {
      sWizard.subMode = subModeForPage(getWizardCurrentPage());
    }
  };
  auto stageAndAdvance = [&]() {
    if (!wizardStageMQTTSettings(sWizard.result)) {
      snprintf(out, outSize, "Error: MQTT choices could not be staged. Try again or cancel.");
      return;
    }
    broadcastOutput("MQTT broker staged.");
    goNextPage();
  };

  switch (sWizard.subMode) {
    case WizardSubMode::MQTT_INTRO:
      if (isBack) { goBack(); appendPromptTo(out, outSize); return CLI_MODE_HANDLED; }
      if (lc == "c" || lc == "configure") {
        sWizard.result.mqttHost = "";
        sWizard.result.mqttPort = 0;
        sWizard.result.mqttUser = "";
        sWizard.result.mqttPassword = "";
        sWizard.subMode = WizardSubMode::MQTT_HOST;
        broadcastOutput("----------------------------------------");
        broadcastOutput("Enter MQTT broker fields. Blank keeps the current value; 'n' finishes, 'b' goes back.");
        broadcastOutput("----------------------------------------");
        appendPromptTo(out, outSize); return CLI_MODE_HANDLED;
      }
      // Skipping broker configuration also leaves auto-start disabled.
      wizardSetMQTTAutoStart(false);
      goNextPage();
      appendPromptTo(out, outSize);
      return (sWizard.subMode == WizardSubMode::DONE) ? CLI_MODE_HANDLED_AND_EXIT
                                                       : CLI_MODE_HANDLED;

    case WizardSubMode::MQTT_HOST:
      if (isBack) { goBack(); appendPromptTo(out, outSize); return CLI_MODE_HANDLED; }
      if (isNext) {
        stageAndAdvance();
        appendPromptTo(out, outSize);
        return sWizard.subMode == WizardSubMode::DONE ? CLI_MODE_HANDLED_AND_EXIT : CLI_MODE_HANDLED;
      }
      sWizard.result.mqttHost = raw;
      sWizard.subMode = WizardSubMode::MQTT_PORT;
      appendPromptTo(out, outSize); return CLI_MODE_HANDLED;

    case WizardSubMode::MQTT_PORT:
      if (isBack) { sWizard.subMode = WizardSubMode::MQTT_HOST; appendPromptTo(out, outSize); return CLI_MODE_HANDLED; }
      if (isNext) {
        stageAndAdvance();
        appendPromptTo(out, outSize);
        return sWizard.subMode == WizardSubMode::DONE ? CLI_MODE_HANDLED_AND_EXIT : CLI_MODE_HANDLED;
      }
      if (!wizardParseMQTTPort(raw, sWizard.result.mqttPort)) {
        snprintf(out, outSize, "Error: MQTT port must be 1..65535, or blank to keep it.");
        appendPromptTo(out, outSize);
        return CLI_MODE_HANDLED;
      }
      sWizard.subMode = WizardSubMode::MQTT_USER;
      appendPromptTo(out, outSize); return CLI_MODE_HANDLED;

    case WizardSubMode::MQTT_USER:
      if (isBack) { sWizard.subMode = WizardSubMode::MQTT_PORT; appendPromptTo(out, outSize); return CLI_MODE_HANDLED; }
      if (isNext) {
        stageAndAdvance();
        appendPromptTo(out, outSize);
        return sWizard.subMode == WizardSubMode::DONE ? CLI_MODE_HANDLED_AND_EXIT : CLI_MODE_HANDLED;
      }
      sWizard.result.mqttUser = raw;
      sWizard.subMode = WizardSubMode::MQTT_PASS;
      appendPromptTo(out, outSize); return CLI_MODE_HANDLED;

    case WizardSubMode::MQTT_PASS:
      if (isBack) { sWizard.subMode = WizardSubMode::MQTT_USER; appendPromptTo(out, outSize); return CLI_MODE_HANDLED; }
      sWizard.result.mqttPassword = raw;
      stageAndAdvance();
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
  auto advance = [&]() {
    clearWizardWifiChoices();
    sWizard.wifiPendingSsid = "";
    if (!wizardNextPage(sWizard.result)) {
      sWizard.subMode = WizardSubMode::DONE;
      return CLI_MODE_HANDLED_AND_EXIT;
    }
    sWizard.subMode = subModeForPage(getWizardCurrentPage());
    appendPromptTo(out, outSize);
    return CLI_MODE_HANDLED;
  };

  switch (sWizard.subMode) {
    case WizardSubMode::WIFI_SSID: {
#if ENABLE_WIFI
      if (lc == "b" || lc == "back") {
        clearWizardWifiChoices();
        sWizard.wifiPendingSsid = "";
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
        sWizard.result.wifiConfigured = false;
        sWizard.result.wifiSSID = "";
        sWizard.result.wifiPassword = "";
        snprintf(out, outSize, "WiFi configuration skipped.");
        return advance();
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
      return advance();
#endif
    }

    case WizardSubMode::WIFI_PASSWORD: {
      if (lc == "b" || lc == "back") {
        sWizard.wifiPendingSsid = "";
        sWizard.subMode = WizardSubMode::WIFI_SSID;
        appendPromptTo(out, outSize);
        return CLI_MODE_HANDLED;
      }
      sWizard.result.wifiSSID       = sWizard.wifiPendingSsid;
      sWizard.result.wifiPassword   = raw;
      sWizard.result.wifiConfigured = true;
      snprintf(out, outSize, "WiFi credentials captured for '%s'.",
               sWizard.wifiPendingSsid.c_str());
      return advance();
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
  // CLIMode owns inactivity/session expiry; reset the blocking input escape.
  setSerialWaitTimeout(0);
  // Reset the wizard's state (existing initSetupWizard rebuilds the
  // features list etc.).
  initSetupWizard();

  // Page scratch is separate from the shared draft initialized above.
  sWizard.subMode = subModeForPage(getWizardCurrentPage());
  sWizard.result = SetupWizardResult{};
  clearWizardWifiChoices();
  sWizard.wifiPendingSsid = "";
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

static void wizardMode_onExit(void* /*ud*/) {
  // onExit also covers timeout/session-owner loss and cliExitMode().
  clearWizardWifiChoices();
  bool applied = false;
  if (!cliModeExitCanComplete() || isWizardCancelRequested()) {
    sWizard.result.completed = false;
  }
  if (sWizard.result.completed) {
    applied = wizardSaveAndApplySettings(sWizard.result);
    if (applied) {
      broadcastOutput("");
      broadcastOutput("Feature configuration complete.");
      broadcastOutput(String("Timezone: ") + sWizard.result.timezoneAbbrev);
    }
  } else {
    broadcastOutput("Feature setup cancelled. No changes saved.");
  }
  discardSetupWizard();
  setSerialWaitTimeout(0);
  sWizard.wifiPendingSsid = "";
  sWizard.result = SetupWizardResult{};
  sWizard.espnowConfiguring = false;
  sWizard.subMode = WizardSubMode::DONE;
  DEBUGF(DEBUG_CLI, "[wizardMode] exited (applied=%d)", applied ? 1 : 0);
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
  if (isWizardCancelRequested()) { sWizard.result.completed = false; cliExitMode(); return; }
#if ENABLE_OLED_DISPLAY
  if (!oledDisplay || !oledConnected) return;

  // ---- 1. Joystick poll ----
  const uint32_t newButtons = inputConsumeButtonPresses(sLastButtons, sLastButtonsValid);
  if (sLastButtonsValid) {

    JoystickNav nav = readWizardJoystickNav();
    bool hasInput = (newButtons != 0) || nav.up || nav.down || nav.left || nav.right;

    if (hasInput) {
      (void)cliModeNoteActivity();
      // Translate joystick event into wizard state change. Only meaningful
      // on the top-level pages -- sub-pages (ESPNOW / MQTT / WiFi) require
      // text entry which the joystick cannot provide.
      switch (sWizard.subMode) {
        case WizardSubMode::PAGE_GROUP:
          handleWizardPageInput(newButtons, nav, sWizard.result);
          if (sWizard.result.completed && !isWizardCancelRequested()) {
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
              sWizard.wifiPendingSsid = "";
            }
            wizardPrevPage();
            sWizard.subMode = subModeForPage(getWizardCurrentPage());
          }
          break;
      }
      if (isWizardCancelRequested()) { sWizard.result.completed = false; cliExitMode(); return; }
      // Sync page navigation and repaint values changed without moving a row.
      sWizard.subMode = subModeForPage(getWizardCurrentPage());
      sLastRendered = WIZARD_PAGE_COUNT;
    }
  }

  // ---- 2. OLED render for current sub-mode (only when state changed) ----
  SetupWizardPage curPage = getWizardCurrentPage();
  int curSel = getWizardCurrentSelection();
  if (curPage != sLastRendered || curSel != sLastRenderedSel) {
    if (sWizard.subMode == WizardSubMode::PAGE_GROUP) renderWizardPage();
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
