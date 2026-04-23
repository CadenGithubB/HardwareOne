// i2csensor-rda5807-oled.h - RDA5807 FM Radio OLED display functions
// Include this at the end of i2csensor-rda5807.cpp
#ifndef I2CSENSOR_RDA5807_OLED_H
#define I2CSENSOR_RDA5807_OLED_H

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include <Adafruit_SSD1306.h>

// FM Radio OLED display function - shows radio data
static void displayFmRadio() {
  extern void oledDrawIcon(int x, int y, const char* iconName, int targetSize);
  extern void oledDrawLevelBars(int x, int y, int level, int maxBars, int barHeight);
  
  // Header is rendered by the system - content starts at OLED_CONTENT_START_Y
  int y = OLED_CONTENT_START_Y;
  oledDisplay->setTextSize(1);
  
  if (!gFmRadioConnected || !gRadioInitialized) {
    oledDrawIcon(48, y + 2, "vol_mute", 16);
    oledDisplay->setCursor(16, y + 22);
    oledDisplay->println("FM Radio not active");
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT - 8);
    oledDisplay->print("X: Start");
    return;
  }
  
  // Take a snapshot under the cache mutex so we don't tear strings mid-draw.
  FMRadioCache snap;
  if (gFmRadioCache.mutex && xSemaphoreTake(gFmRadioCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    snap = gFmRadioCache;
    xSemaphoreGive(gFmRadioCache.mutex);
  } else {
    snap = gFmRadioCache;  // best-effort
  }

  // Frequency (large)
  oledDisplay->setCursor(0, y);
  oledDisplay->setTextSize(2);
  oledDisplay->printf("%.1f MHz", snap.frequency / 100.0);
  oledDisplay->setTextSize(1);
  y += 18;

  // Station name (if available)
  oledDisplay->setCursor(0, y);
  if (strlen(snap.stationName) > 0) {
    oledDisplay->printf("Station: %s", snap.stationName);
  } else {
    oledDisplay->print("No RDS Station");
  }
  y += 10;

  // RDS Radio Text
  oledDisplay->setCursor(0, y);
  if (strlen(snap.stationText) > 0) {
    oledDisplay->print(snap.stationText);
  }
  y += 10;

  // Status bar with volume, RSSI, stereo
  oledDisplay->setCursor(0, y);
  oledDisplay->print("Vol:");
  oledDisplay->print(snap.volume);
  oledDisplay->print(snap.muted ? "M" : "");
  oledDisplay->print(" RSSI:");
  oledDisplay->print(snap.rssi);
  oledDisplay->print(snap.stereo ? " ST" : " MO");
}

// Availability check for FM Radio OLED mode
static bool fmRadioOLEDModeAvailable(String* outReason) {
  return true;  // Always allow navigation, display function handles "not active" state
}

static void fmRadioToggleConfirmed(void* userData) {
  (void)userData;
  extern void executeOLEDCommand(const String& argsInput);
  if (gRadioInitialized && gFmRadioConnected) {
    executeOLEDCommand("closefmradio");
  } else {
    executeOLEDCommand("openfmradio");
  }
}

// Input handler for FM Radio OLED mode - X button toggles radio
static bool fmRadioInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    if (gRadioInitialized && gFmRadioConnected) {
      oledConfirmRequest("Close FM?", nullptr, fmRadioToggleConfirmed, nullptr, false);
    } else {
      oledConfirmRequest("Open FM?", nullptr, fmRadioToggleConfirmed, nullptr);
    }
    return true;
  }
  return false;
}

// FM Radio OLED mode entry
static const OLEDModeEntry fmRadioOLEDModes[] = {
  {
    OLED_FM_RADIO,           // mode enum
    "FM Radio",              // menu name
    "radio",                 // icon name
    displayFmRadio,          // displayFunc
    fmRadioOLEDModeAvailable,// availFunc
    fmRadioInputHandler,     // inputFunc - X toggles radio
    true,                    // showInMenu
    60,                      // menuOrder
    nullptr                  // hints
  }
};

// Auto-register FM Radio OLED mode
REGISTER_OLED_MODE_MODULE(fmRadioOLEDModes, sizeof(fmRadioOLEDModes) / sizeof(fmRadioOLEDModes[0]), "FMRadio");

#endif // I2CSENSOR_RDA5807_OLED_H
