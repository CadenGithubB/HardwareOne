// i2csensor-rda5807-oled.h - RDA5807 FM Radio OLED display functions
// Include this at the end of i2csensor-rda5807.cpp
#ifndef I2CSENSOR_RDA5807_OLED_H
#define I2CSENSOR_RDA5807_OLED_H

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include <Adafruit_SSD1306.h>

// FM Radio OLED display function - shows radio data
static void displayFmRadio() {
  extern Adafruit_SSD1306* oledDisplay;
  
  oledDisplay->setTextSize(1);
  oledDisplay->setCursor(0, 0);
  oledDisplay->println("=== FM RADIO ===");
  oledDisplay->println();
  
  if (!fmRadioConnected || !fmRadioEnabled) {
    oledDisplay->println("FM Radio not active");
    oledDisplay->println();
    oledDisplay->println("Press X to start");
    return;
  }
  
  // Line 1: Frequency (large)
  oledDisplay->setTextSize(2);
  oledDisplay->printf("%.1f MHz\n", fmRadioFrequency / 100.0);
  oledDisplay->setTextSize(1);
  
  // Line 3: Station name (if available)
  if (strlen(fmRadioStationName) > 0) {
    oledDisplay->printf("Station: %s\n", fmRadioStationName);
  } else {
    oledDisplay->println("No RDS Station");
  }
  
  // Line 4: RDS Radio Text (scrolling if needed)
  if (strlen(fmRadioStationText) > 0) {
    oledDisplay->printf("%s\n", fmRadioStationText);
  } else {
    oledDisplay->println();
  }
  
  // Line 5: Status bar with volume, RSSI, stereo, mute
  oledDisplay->print("Vol:");
  oledDisplay->print(fmRadioVolume);
  oledDisplay->print(" RSSI:");
  oledDisplay->print(fmRadioRSSI);
  
  if (fmRadioStereo) {
    oledDisplay->print(" ST");
  } else {
    oledDisplay->print(" MO");
  }
  
  if (fmRadioMuted) {
    oledDisplay->print(" MUTE");
  }
  
  oledDisplay->println();
}

// Availability check for FM Radio OLED mode
static bool fmRadioOLEDModeAvailable(String* outReason) {
  return true;  // Always allow navigation, display function handles "not active" state
}

static void fmRadioToggleConfirmed(void* userData) {
  (void)userData;
  extern bool enqueueSensorStart(SensorType sensor);
  extern bool isInQueue(SensorType sensor);

  if (fmRadioEnabled && fmRadioConnected) {
    Serial.println("[FM_RADIO] Confirmed: Stopping FM radio...");
    fmRadioEnabled = false;
  } else if (!isInQueue(SENSOR_FMRADIO)) {
    Serial.println("[FM_RADIO] Confirmed: Starting FM radio...");
    enqueueSensorStart(SENSOR_FMRADIO);
  }
}

// Input handler for FM Radio OLED mode - X button toggles radio
static bool fmRadioInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    if (fmRadioEnabled && fmRadioConnected) {
      oledConfirmRequest("Stop FM?", nullptr, fmRadioToggleConfirmed, nullptr, false);
    } else {
      oledConfirmRequest("Start FM?", nullptr, fmRadioToggleConfirmed, nullptr);
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
    "notify_cli",            // icon name
    displayFmRadio,          // displayFunc
    fmRadioOLEDModeAvailable,// availFunc
    fmRadioInputHandler,     // inputFunc - X toggles radio
    true,                    // showInMenu
    60                       // menuOrder
  }
};

// Auto-register FM Radio OLED mode
REGISTER_OLED_MODE_MODULE(fmRadioOLEDModes, sizeof(fmRadioOLEDModes) / sizeof(fmRadioOLEDModes[0]), "FMRadio");

#endif // I2CSENSOR_RDA5807_OLED_H
