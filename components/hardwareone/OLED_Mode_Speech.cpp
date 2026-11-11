// OLED_Mode_Speech.cpp - ESP-SR speech recognition display mode
// Provides status, control, and live detection feedback for ESP-SR.
//
// Umbrella-compliant (matches OLED_Mode_Power / OLED_Mode_Network):
//   • the control menu is an OledScrollState (not a raw int + options array),
//     navigated by oledScrollHandleNav and rebuilt per-frame with
//     oledScrollClearKeepSelection + oledScrollClampSelection;
//   • the live status detail is a pushed sub-mode (OLED_SPEECH_STATUS) entered
//     via requestOledMode and popped by the global B handler — not a
//     `speechShowingStatus` flag faking a second screen.

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY && ENABLE_ESP_SR

#include <Adafruit_SSD1306.h>
#include "OLED_Utils.h"
#include "HAL_Input.h"
#include "System_Settings.h"
#include "System_ESPSR.h"
#include "OLED_SettingsEditor.h"  // openSettingsEditorForModule

// Control menu — shared OledScrollState (same model as Power/Network menus).
EXT_RAM_BSS_ATTR static OledScrollState sSpeechMenuScroll;
static bool sSpeechMenuInit = false;

// Animation state for the wake-word "listening" indicator (status sub-mode).
static uint32_t lastWakeAnimFrame = 0;
static int wakeAnimPhase = 0;

// ============================================================================
// OLED_SPEECH_STATUS — live ESP-SR status detail (pushed sub-mode, read-only)
// ============================================================================

void displaySpeechStatus() {
  if (!oledDisplay || !oledConnected) return;

  oledDisplay->setTextSize(1);

  bool running = isEspSrRunning();
  bool wakeActive = isEspSrWakeActive();

  // Line 1: Status
  oledDisplay->print("SR: ");
  if (running) {
    oledDisplay->print("ON ");
    // Show voice state
    const char* state = getEspSrVoiceState();
    if (strcmp(state, "idle") == 0) {
      oledDisplay->println("(idle)");
    } else if (strcmp(state, "category") == 0) {
      oledDisplay->println("(await cat)");
    } else if (strcmp(state, "subcategory") == 0) {
      oledDisplay->println("(await sub)");
    } else if (strcmp(state, "target") == 0) {
      oledDisplay->println("(await tgt)");
    } else {
      oledDisplay->println();
    }
  } else {
    oledDisplay->println("OFF");
  }

  // Line 2: Current context (if in multi-stage)
  if (running && wakeActive) {
    const char* cat = getEspSrCurrentCategory();
    const char* sub = getEspSrCurrentSubCategory();
    if (cat && cat[0]) {
      oledDisplay->print(">");
      oledDisplay->print(cat);
      if (sub && sub[0]) {
        oledDisplay->print(">");
        oledDisplay->print(sub);
      }
      oledDisplay->println();
    } else {
      // Animate listening indicator
      if (millis() - lastWakeAnimFrame > 200) {
        lastWakeAnimFrame = millis();
        wakeAnimPhase = (wakeAnimPhase + 1) % 4;
      }
      const char* animChars[] = {"[.  ]", "[.. ]", "[...]", "[.. ]"};
      oledDisplay->print("Listening ");
      oledDisplay->println(animChars[wakeAnimPhase]);
    }
  } else if (running) {
    oledDisplay->println("Ready for wake word");
  } else {
    oledDisplay->println();
  }

  // Line 3: Last command + confidence
  const char* lastCmd = getEspSrLastCommand();
  if (lastCmd && lastCmd[0]) {
    float conf = getEspSrLastConfidence();
    oledDisplay->print("Last: ");
    String cmdStr = lastCmd;
    if (cmdStr.length() > 10) {
      cmdStr = cmdStr.substring(0, 10); cmdStr += "..";
    }
    oledDisplay->print(cmdStr);
    oledDisplay->print(" ");
    oledDisplay->print((int)(conf * 100));
    oledDisplay->println("%");
  } else {
    oledDisplay->println("Last: (none)");
  }

  // Line 4: Stats
  oledDisplay->print("W:");
  oledDisplay->print(getEspSrWakeCount());
  oledDisplay->print(" C:");
  oledDisplay->println(getEspSrCommandCount());

  // This screen animates (listening indicator) and has no input handler, so
  // keep it re-rendering while it's the active mode.
  oledMarkDirty();
}

// ============================================================================
// OLED_SPEECH — control menu
// ============================================================================

static void populateSpeechMenu() {
  if (!sSpeechMenuInit) {
    oledScrollInit(&sSpeechMenuScroll, nullptr, 4);
    sSpeechMenuInit = true;
  }
  oledScrollClearKeepSelection(&sSpeechMenuScroll);
  oledScrollAddItem(&sSpeechMenuScroll, "View Status");
  oledScrollAddItem(&sSpeechMenuScroll, isEspSrRunning() ? "Stop SR" : "Start SR");
  oledScrollAddItem(&sSpeechMenuScroll, "Models");
  oledScrollAddItem(&sSpeechMenuScroll, "Settings");
  oledScrollClampSelection(&sSpeechMenuScroll);
}

void displaySpeechInfo() {
  if (!oledDisplay || !oledConnected) return;

  oledDisplay->setTextSize(1);

  // Header with running status
  oledDisplay->print("SPEECH ");
  bool running = isEspSrRunning();
  if (running) {
    oledDisplay->println(isEspSrWakeActive() ? "(wake!)" : "(on)");
  } else {
    oledDisplay->println("(off)");
  }

  populateSpeechMenu();

  // Inverse-bar menu render (the mode's established look) — selection/items now
  // come from the scroll state instead of a raw int + options[] array.
  for (int i = 0; i < sSpeechMenuScroll.itemCount; i++) {
    if (i == sSpeechMenuScroll.selectedIndex) {
      oledDisplay->fillRect(0, 10 + i * 10, 128, 10, DISPLAY_COLOR_WHITE);
      oledDisplay->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
    } else {
      oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    }
    oledDisplay->setCursor(2, 11 + i * 10);
    if (sSpeechMenuScroll.items[i].line1) {
      oledDisplay->print(sSpeechMenuScroll.items[i].line1);
    }
  }

  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  // Footer handled by global drawOledFooter()
}

// ============================================================================
// Input
// ============================================================================

bool speechInputHandler(int /*deltaX*/, int /*deltaY*/, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;
  if (oledScrollHandleNav(&sSpeechMenuScroll)) return true;

  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    switch (sSpeechMenuScroll.selectedIndex) {
      case 0:  // View Status — push the live status sub-mode
        requestOledMode(OLED_SPEECH_STATUS, "speech.status");
        break;
      case 1:  // Start / Stop SR
        if (isEspSrRunning()) stopEspSr(); else startEspSr();
        break;
      case 2:  // Models (placeholder — shows status for now, as before)
        requestOledMode(OLED_SPEECH_STATUS, "speech.models");
        break;
      case 3:  // Settings — open the ESP-SR settings editor (was a dead TODO)
        if (openSettingsEditorForModule("espsr")) {
          requestOledMode(OLED_SETTINGS, "speech.settings");
        }
        break;
    }
    return true;
  }

  // B: return false so the global handler runs oledMenuBack() (pops the stack).
  return false;
}

// ============================================================================
// OLED Mode Registration
// ============================================================================

// Columns: mode, name, iconName, displayFunc, availFunc, inputFunc, showInMenu, menuOrder, hints
static const OledModeEntry speechModeEntries[] = {
  { OLED_SPEECH,        "Speech", "mic", displaySpeechInfo,   nullptr, speechInputHandler, true,  50, "A:Select B:Back" },
  { OLED_SPEECH_STATUS, "SR",     "mic", displaySpeechStatus, nullptr, nullptr,            false, -1, "B:Back" },
};

REGISTER_OLED_MODE_MODULE(speechModeEntries,
                          sizeof(speechModeEntries) / sizeof(speechModeEntries[0]),
                          "Speech");

#endif // ENABLE_OLED_DISPLAY && ENABLE_ESP_SR
