// i2csensor_apds9960_oled.h - APDS9960 OLED display functions
// Include this at the end of i2csensor_apds9960.cpp
#ifndef I2CSENSOR_APDS9960_OLED_H
#define I2CSENSOR_APDS9960_OLED_H

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "i2csensor_apds9960.h"
#include "HAL_Display.h"  // For gDisplay and oledDisplay macro alias

// Display function for APDS OLED mode
void displayApdsData() {
  if (!oledDisplay || !oledConnected) return;
  
  // Under the header bar, which shows the screen's name.
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  
  if (!gApdsConnected || (!gApdsColorRunning && !gApdsProximityRunning)) {
    oledDisplay->println("Not active");
    oledDisplay->println();
    oledDisplay->println("Press X to start");
  } else {
    ApdsCache reading{};
    if (apdsCacheSnapshot(reading)) {
      oledDisplay->print("R:");
      oledDisplay->print(reading.apdsRed);
      oledDisplay->print(" G:");
      oledDisplay->println(reading.apdsGreen);
      oledDisplay->print("B:");
      oledDisplay->print(reading.apdsBlue);
      oledDisplay->print(" C:");
      oledDisplay->println(reading.apdsClear);
      oledDisplay->print("Prox: ");
      oledDisplay->println(reading.apdsProximity);
    }
  }
  
  // Don't call display() here - let updateOledDisplay() render footer and display in same frame
}

// Availability check for APDS OLED mode
static bool apdsOledModeAvailable(String* outReason) {
  return true;  // Always allow navigation
}

static void apdsToggleConfirmed(void* userData) {
  (void)userData;
  if (gApdsColorRunning || gApdsProximityRunning) {
    executeOledCommand("closeapds");
  } else {
    executeOledCommand("openapds");
  }
}

// Input handler for APDS OLED mode - X button toggles sensor
static bool apdsInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    if (gApdsColorRunning || gApdsProximityRunning) {
      oledConfirmRequest("Close APDS?", nullptr, apdsToggleConfirmed, nullptr, false);
    } else {
      oledConfirmRequest("Open APDS?", nullptr, apdsToggleConfirmed, nullptr);
    }
    return true;
  }
  return false;
}

// APDS OLED mode entry
static const OledModeEntry apdsOledModes[] = {
  {
    OLED_APDS_DATA,          // mode enum
    "APDS",                  // menu name
    "notify_sensor",         // icon name
    displayApdsData,         // displayFunc
    apdsOledModeAvailable,   // availFunc
    apdsInputHandler,        // inputFunc - X toggles sensor
    true,                    // showInMenu
    35,                      // menuOrder
    nullptr                  // hints
  }
};

// Auto-register APDS OLED mode
REGISTER_OLED_MODE_MODULE(apdsOledModes, sizeof(apdsOledModes) / sizeof(apdsOledModes[0]), "APDS");

#endif // I2CSENSOR_APDS9960_OLED_H
