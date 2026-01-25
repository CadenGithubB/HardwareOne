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
  extern Adafruit_SSD1306* oledDisplay;
  
  oledDisplay->setTextSize(1);
  oledDisplay->setCursor(0, 0);
  oledDisplay->println("=== MICROPHONE ===");
  
  if (!micEnabled) {
    oledDisplay->println();
    oledDisplay->println("Mic not active");
    oledDisplay->println();
    oledDisplay->println("Press X to start");
    oledDisplay->println("Press Y to record");
    return;
  }
  
  // Get current audio level
  int level = getAudioLevel();
  
  // Status line
  oledDisplay->println();
  oledDisplay->printf("Status: %s\n", micRecording ? "RECORDING" : "Active");
  oledDisplay->printf("Rate: %d Hz\n", micSampleRate);
  
  // VU Meter visualization (horizontal bar)
  oledDisplay->println();
  oledDisplay->print("Level: ");
  
  // Draw VU meter bar
  int barWidth = 80;  // pixels for the bar
  int barX = 40;      // starting X position
  int barY = 40;      // Y position
  int barHeight = 8;
  int fillWidth = (level * barWidth) / 100;
  
  // Draw outline
  oledDisplay->drawRect(barX, barY, barWidth, barHeight, SSD1306_WHITE);
  
  // Fill based on level
  if (fillWidth > 0) {
    oledDisplay->fillRect(barX + 1, barY + 1, fillWidth - 2, barHeight - 2, SSD1306_WHITE);
  }
  
  // Level percentage
  oledDisplay->setCursor(barX + barWidth + 4, barY);
  oledDisplay->printf("%d%%", level);
  
  // Recording indicator (blinking if recording)
  if (micRecording) {
    static bool blinkState = false;
    static unsigned long lastBlink = 0;
    if (millis() - lastBlink > 500) {
      blinkState = !blinkState;
      lastBlink = millis();
    }
    if (blinkState) {
      oledDisplay->fillCircle(120, 4, 3, SSD1306_WHITE);  // Recording dot
    }
  }
  
  // Controls hint at bottom
  oledDisplay->setCursor(0, OLED_CONTENT_HEIGHT - 8);
  oledDisplay->print("X:Start/Stop Y:Record");
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
      oledConfirmRequest("Stop mic?", nullptr, microphoneToggleConfirmed, nullptr, false);
    } else {
      oledConfirmRequest("Start mic?", nullptr, microphoneToggleConfirmed, nullptr);
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
    "notify_speaker",            // icon name (closest available)
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
