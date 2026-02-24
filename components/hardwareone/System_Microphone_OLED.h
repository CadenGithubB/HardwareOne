// System_Microphone_OLED.h - PDM Microphone OLED display functions
#ifndef SYSTEM_MICROPHONE_OLED_H
#define SYSTEM_MICROPHONE_OLED_H

#include "System_BuildConfig.h"

#if ENABLE_MICROPHONE_SENSOR && ENABLE_OLED_DISPLAY

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "System_Microphone.h"
#include <Adafruit_SSD1306.h>

// Microphone OLED display function - shows VU meter and recording status
static void displayMicrophone() {
  extern void oledDrawIcon(int x, int y, const char* iconName, int targetSize);
  extern void oledDrawLevelBars(int x, int y, int level, int maxBars, int barHeight);
  
  // Header is rendered by the system - content starts at OLED_CONTENT_START_Y
  int y = OLED_CONTENT_START_Y;
  oledDisplay->setTextSize(1);
  
  if (!micEnabled) {
    // Show muted volume icon when mic is off
    oledDrawIcon(48, y + 2, "vol_mute", 16);
    oledDisplay->setCursor(20, y + 22);
    oledDisplay->println("Mic not active");
    return;
  }
  
  // Get current audio level (0-100)
  int level = getAudioLevel();
  
  // Recording indicator (blinking if recording)
  if (micRecording) {
    static bool blinkState = false;
    static unsigned long lastBlink = 0;
    if (millis() - lastBlink > 500) {
      blinkState = !blinkState;
      lastBlink = millis();
    }
    if (blinkState) {
      oledDisplay->fillCircle(SCREEN_WIDTH - 8, y + 3, 3, SSD1306_WHITE);
    }
  }
  
  // Status line
  oledDisplay->setCursor(0, y);
  oledDisplay->printf("%s %dHz", micRecording ? "REC" : "Active", micSampleRate);
  y += 10;
  
  // VU Meter visualization (horizontal bar)
  int barWidth = SCREEN_WIDTH - 28;
  int barX = 0;
  int barHeight = 10;
  int fillWidth = (level * (barWidth - 2)) / 100;
  
  // Draw outline
  oledDisplay->drawRect(barX, y, barWidth, barHeight, SSD1306_WHITE);
  
  // Fill based on level
  if (fillWidth > 0) {
    oledDisplay->fillRect(barX + 1, y + 1, fillWidth, barHeight - 2, SSD1306_WHITE);
  }
  
  // Level percentage
  oledDisplay->setCursor(barX + barWidth + 4, y + 1);
  oledDisplay->printf("%d%%", level);
}

// Availability check for Microphone OLED mode
static bool microphoneOLEDModeAvailable(String* outReason) {
  return true;  // Always allow navigation, display function handles "not active" state
}

static void microphoneToggleConfirmed(void* userData) {
  (void)userData;
  if (micEnabled) {
    Serial.println("[MICROPHONE] Confirmed: Stopping microphone...");
    stopMicrophone();
  } else {
    Serial.println("[MICROPHONE] Confirmed: Starting microphone...");
    initMicrophone();
  }
}

// Input handler for Microphone OLED mode
static bool microphoneInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  // X button: Start/Stop microphone
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    if (micEnabled) {
      oledConfirmRequest("Close mic?", nullptr, microphoneToggleConfirmed, nullptr, false);
    } else {
      oledConfirmRequest("Open mic?", nullptr, microphoneToggleConfirmed, nullptr);
    }
    return true;
  }
  
  // Y button: Toggle recording
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y)) {
    if (!micEnabled) {
      Serial.println("[MICROPHONE] Y button: Starting mic first...");
      initMicrophone();
    }
    micRecording = !micRecording;
    Serial.printf("[MICROPHONE] Y button: Recording %s\n", micRecording ? "started" : "stopped");
    return true;
  }
  
  return false;
}

// Microphone OLED mode entry - use a new enum value
// Note: OLED_MICROPHONE needs to be added to OLEDMode enum in OLED_Display.h
static const OLEDModeEntry microphoneOLEDModes[] = {
  {
    OLED_MICROPHONE,             // mode enum
    "Microphone",                // menu name
    "mic",                      // icon name (closest available)
    displayMicrophone,           // displayFunc
    microphoneOLEDModeAvailable, // availFunc
    microphoneInputHandler,      // inputFunc
    true,                        // showInMenu
    65                           // menuOrder (after FM Radio at 60)
  }
};

// Auto-register Microphone OLED mode
REGISTER_OLED_MODE_MODULE(microphoneOLEDModes, sizeof(microphoneOLEDModes) / sizeof(microphoneOLEDModes[0]), "Microphone");

#endif // ENABLE_MICROPHONE_SENSOR && ENABLE_OLED_DISPLAY

#endif // SYSTEM_MICROPHONE_OLED_H
