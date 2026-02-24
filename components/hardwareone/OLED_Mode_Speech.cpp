// OLED_Mode_Speech.cpp - ESP-SR speech recognition display mode
// Provides status, control, and live detection feedback for ESP-SR

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY && ENABLE_ESP_SR

#include <Adafruit_SSD1306.h>
#include "OLED_Utils.h"
#include "HAL_Input.h"
#include "System_Settings.h"
#include "System_ESPSR.h"

// External references
extern bool oledConnected;
extern OLEDMode currentOLEDMode;
extern Settings gSettings;

// Speech menu state
static int speechMenuSelection = 0;
static const int SPEECH_MENU_ITEMS = 4;
static bool speechShowingStatus = false;

// Animation state for wake word indicator
static uint32_t lastWakeAnimFrame = 0;
static int wakeAnimPhase = 0;

// ============================================================================
// Speech Menu Display Functions
// ============================================================================

void displaySpeechStatus() {
  if (!oledDisplay || !oledConnected) return;
  
  oledDisplay->setTextSize(1);
  
  bool running = isESPSRRunning();
  bool wakeActive = isESPSRWakeActive();
  
  // Line 1: Status
  oledDisplay->print("SR: ");
  if (running) {
    oledDisplay->print("ON ");
    // Show voice state
    const char* state = getESPSRVoiceState();
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
    const char* cat = getESPSRCurrentCategory();
    const char* sub = getESPSRCurrentSubCategory();
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
  const char* lastCmd = getESPSRLastCommand();
  if (lastCmd && lastCmd[0]) {
    float conf = getESPSRLastConfidence();
    oledDisplay->print("Last: ");
    // Truncate if too long
    String cmdStr = lastCmd;
    if (cmdStr.length() > 10) {
      cmdStr = cmdStr.substring(0, 10) + "..";
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
  oledDisplay->print(getESPSRWakeCount());
  oledDisplay->print(" C:");
  oledDisplay->println(getESPSRCommandCount());
}

void displaySpeechInfo() {
  if (!oledDisplay || !oledConnected) return;
  
  oledDisplay->setTextSize(1);
  
  if (speechShowingStatus) {
    displaySpeechStatus();
    return;
  }
  
  // Header with running status
  oledDisplay->print("SPEECH ");
  bool running = isESPSRRunning();
  if (running) {
    bool wakeActive = isESPSRWakeActive();
    if (wakeActive) {
      oledDisplay->println("(wake!)");
    } else {
      oledDisplay->println("(on)");
    }
  } else {
    oledDisplay->println("(off)");
  }
  
  // Menu options
  const char* options[SPEECH_MENU_ITEMS];
  options[0] = "View Status";
  options[1] = running ? "Stop SR" : "Start SR";
  options[2] = "Models";
  options[3] = "Settings";
  
  // Draw menu items
  for (int i = 0; i < SPEECH_MENU_ITEMS; i++) {
    if (i == speechMenuSelection) {
      oledDisplay->fillRect(0, 10 + i * 10, 128, 10, DISPLAY_COLOR_WHITE);
      oledDisplay->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
    } else {
      oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    }
    oledDisplay->setCursor(2, 11 + i * 10);
    oledDisplay->print(options[i]);
  }
  
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  // Footer handled by global drawOLEDFooter()
}

// ============================================================================
// Speech Menu Navigation
// ============================================================================

void speechMenuUp() {
  if (speechShowingStatus) return;
  speechMenuSelection = (speechMenuSelection - 1 + SPEECH_MENU_ITEMS) % SPEECH_MENU_ITEMS;
  oledMarkDirty();
}

void speechMenuDown() {
  if (speechShowingStatus) return;
  speechMenuSelection = (speechMenuSelection + 1) % SPEECH_MENU_ITEMS;
  oledMarkDirty();
}

void speechMenuSelect() {
  if (speechShowingStatus) {
    speechShowingStatus = false;
    oledMarkDirty();
    return;
  }
  
  switch (speechMenuSelection) {
    case 0:  // View Status
      speechShowingStatus = true;
      break;
    case 1:  // Start/Stop SR
      if (isESPSRRunning()) {
        stopESPSR();
      } else {
        startESPSR();
      }
      break;
    case 2:  // Models - could show model info or open settings
      // For now, just show status
      speechShowingStatus = true;
      break;
    case 3:  // Settings - open settings editor to espsr module
      // TODO: openSettingsEditorForModule("espsr");
      break;
  }
  oledMarkDirty();
}

void speechMenuBack() {
  if (speechShowingStatus) {
    speechShowingStatus = false;
    oledMarkDirty();
  }
  // Top-level back is handled by global handler via oledMenuBack()
}

// ============================================================================
// Speech Input Handler
// ============================================================================

bool speechInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  (void)deltaX;
  
  // Navigation
  if (deltaY < 0) {
    speechMenuUp();
    return true;
  }
  if (deltaY > 0) {
    speechMenuDown();
    return true;
  }
  
  // Button handling - A/X for select, B for back
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    speechMenuSelect();
    return true;
  }
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    if (speechShowingStatus) {
      speechMenuBack();
      return true;
    }
    // Return false to let global handler call oledMenuBack()
    return false;
  }
  
  return false;
}

// ============================================================================
// OLED Mode Registration
// ============================================================================

static const OLEDModeEntry speechModeEntries[] = {
  {
    OLED_SPEECH,          // mode
    "Speech",             // name
    "mic",                // iconName (microphone icon)
    displaySpeechInfo,    // displayFunc
    nullptr,              // availFunc (always available when compiled)
    speechInputHandler,   // inputFunc
    true,                 // showInMenu
    50                    // menuOrder
  }
};

REGISTER_OLED_MODE_MODULE(speechModeEntries, 1, "Speech");

#endif // ENABLE_OLED_DISPLAY && ENABLE_ESP_SR
