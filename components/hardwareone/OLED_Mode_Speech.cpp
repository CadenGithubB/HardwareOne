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
#include "OLED_TextField.h"
#include "HAL_Input.h"
#include "System_Settings.h"
#include "System_ESPSR.h"
#include "OLED_SettingsEditor.h"  // openSettingsEditorForModule

// Control menu — shared OledScrollState (same model as Power/Network menus).
EXT_RAM_BSS_ATTR static OledScrollState sSpeechMenuScroll;
static bool sSpeechMenuInit = false;

// Animation state for the wake-word "listening" indicator (status sub-mode).
static uint32_t sLastWakeAnimFrame = 0;
static int sWakeAnimPhase = 0;

// ============================================================================
// OLED_SPEECH_STATUS — live ESP-SR status detail (pushed sub-mode, read-only)
// ============================================================================

void displaySpeechStatus() {
  if (!oledDisplay || !oledConnected) return;

  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);

  bool running = isEspSrRunning();
  bool wakeActive = isEspSrWakeActive();
  int y = OLED_CONTENT_START_Y;
  auto emit = [&](const char* text) {
    const auto g = oled_layout::geometryFor(SCREEN_WIDTH, SCREEN_HEIGHT);
    const int room = oled_layout::rowsInBand(g, y, 8);
    if (room < 1) return;
    if (!text || !text[0]) {
      y += 8;
      return;
    }
    y = oledPrintWrapped(oledDisplay, 0, y, text, room, 8);
  };

  // Line 1: Status
  if (running) {
    const char* state = getEspSrVoiceState();
    const char* tag = nullptr;
    if (strcmp(state, "idle") == 0) tag = "(idle)";
    else if (strcmp(state, "category") == 0) tag = "(await cat)";
    else if (strcmp(state, "subcategory") == 0) tag = "(await sub)";
    else if (strcmp(state, "target") == 0) tag = "(await tgt)";
    char line[32];
    if (tag) snprintf(line, sizeof(line), "SR: ON %s", tag);
    else snprintf(line, sizeof(line), "SR: ON");
    emit(line);
  } else {
    emit("SR: OFF");
  }

  // Line 2: Current context (if in multi-stage)
  if (running && wakeActive) {
    // Copies: the recognizer task rewrites these texts while this page draws.
    char cat[kEspSrPhraseTextSize];
    char sub[kEspSrPhraseTextSize];
    getEspSrCurrentCategory(cat, sizeof(cat));
    getEspSrCurrentSubCategory(sub, sizeof(sub));
    if (cat[0]) {
      char line[kEspSrPhraseTextSize * 2 + 4];
      if (sub[0]) snprintf(line, sizeof(line), ">%s>%s", cat, sub);
      else snprintf(line, sizeof(line), ">%s", cat);
      emit(line);
    } else {
      // Animate listening indicator
      if (millis() - sLastWakeAnimFrame > 200) {
        sLastWakeAnimFrame = millis();
        sWakeAnimPhase = (sWakeAnimPhase + 1) % 4;
      }
      const char* animChars[] = {"[.  ]", "[.. ]", "[...]", "[.. ]"};
      char line[24];
      snprintf(line, sizeof(line), "Listening %s", animChars[sWakeAnimPhase]);
      emit(line);
    }
  } else if (running) {
    emit("Ready for wake word");
  } else {
    emit("");
  }

  // Line 3: Last command + confidence
  // The line shows the first 10 characters; 15 are enough to tell that the
  // command is longer.
  char lastCmd[16];
  getEspSrLastCommand(lastCmd, sizeof(lastCmd));
  if (lastCmd[0]) {
    float conf = getEspSrLastConfidence();
    char shown[16];
    snprintf(shown, sizeof(shown), "%s", lastCmd);
    if (strlen(shown) > 10) {
      shown[10] = '\0';
      strncat(shown, "..", sizeof(shown) - 11);
    }
    char line[40];
    snprintf(line, sizeof(line), "Last: %s %d%%", shown, (int)(conf * 100));
    emit(line);
  } else {
    emit("Last: (none)");
  }

  // Line 4: Stats
  char stats[24];
  snprintf(stats, sizeof(stats), "W:%d C:%d", (int)getEspSrWakeCount(), (int)getEspSrCommandCount());
  emit(stats);

  // This screen animates (listening indicator) and has no input handler, so
  // keep it re-rendering while it's the active mode.
  oledMarkDirty();
}

// ============================================================================
// OLED_SPEECH — control menu
// ============================================================================

static void populateSpeechMenu() {
  if (!sSpeechMenuInit) {
    // 8 px rows below the status line, as many as fit above the footer (all
    // four on the 64 px OLED; the window grows with a taller canvas).
    oledScrollInit(&sSpeechMenuScroll, nullptr, oledVisibleRows(8, 8));
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
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);

  bool running = isEspSrRunning();
  char status[24];
  snprintf(status, sizeof(status), "SPEECH %s",
           running ? (isEspSrWakeActive() ? "(wake!)" : "(on)") : "(off)");
  const int listY = oledPrintStatus(oledDisplay, OLED_CONTENT_START_Y, status);

  populateSpeechMenu();

  // The shared single-line list on the first row under the status block.
  oledScrollRenderSimple(oledDisplay, &sSpeechMenuScroll, true, listY);
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
static const OledModeEntry kSpeechModeEntries[] = {
  { OLED_SPEECH,        "Speech", "mic", displaySpeechInfo,   nullptr, speechInputHandler, true,  50, "A:Select B:Back" },
  { OLED_SPEECH_STATUS, "SR",     "mic", displaySpeechStatus, nullptr, nullptr,            false, -1, "B:Back" },
};

REGISTER_OLED_MODE_MODULE(kSpeechModeEntries,
                          sizeof(kSpeechModeEntries) / sizeof(kSpeechModeEntries[0]),
                          "Speech");

#endif // ENABLE_OLED_DISPLAY && ENABLE_ESP_SR
