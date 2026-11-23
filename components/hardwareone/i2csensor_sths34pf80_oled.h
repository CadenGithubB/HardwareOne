// i2csensor_sths34pf80_oled.h - STHS34PF80 OLED display functions
// Include this at the end of i2csensor_sths34pf80.cpp
#ifndef I2CSENSOR_STHS34PF80_OLED_H
#define I2CSENSOR_STHS34PF80_OLED_H

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "HAL_Display.h"  // For gDisplay and oledDisplay macro alias
#include "i2csensor_sths34pf80.h"

// Display function for Presence OLED mode
void displayPresenceData() {
  if (!oledDisplay || !oledConnected) return;
  
  // Under the header bar, which shows the screen's name.
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  
  if (!gPresenceConnected || !gPresenceRunning) {
    oledDisplay->println("Not active");
    oledDisplay->println();
    oledDisplay->println("Press X to start");
  } else {
    PresenceCache presence{};
    if (presenceCacheSnapshot(presence)) {
      // Ambient temperature
      oledDisplay->print("Ambient: ");
      oledDisplay->print(presence.ambientTemp, 1);
      oledDisplay->println("C");
      
      // Presence detection with value
      oledDisplay->print("Presence: ");
      oledDisplay->print(presence.presenceValue);
      if (presence.presenceDetected) {
        oledDisplay->println(" [!]");
      } else {
        oledDisplay->println();
      }
      
      // Motion detection with value
      oledDisplay->print("Motion: ");
      oledDisplay->print(presence.motionValue);
      if (presence.motionDetected) {
        oledDisplay->println(" [!]");
      } else {
        oledDisplay->println();
      }
      
      // Temperature shock
      oledDisplay->print("TShock: ");
      oledDisplay->print(presence.tempShockValue);
      if (presence.tempShockDetected) {
        oledDisplay->println(" [!]");
      } else {
        oledDisplay->println();
      }
    }
  }
  
  // Don't call display() here - let updateOledDisplay() render footer and display in same frame
}

// Availability check for Presence OLED mode
static bool presenceOledModeAvailable(String* outReason) {
  return true;  // Always allow navigation
}

static void presenceToggleConfirmed(void* userData) {
  (void)userData;
  if (gPresenceRunning) {
    executeOledCommand("closepresence");
  } else {
    executeOledCommand("openpresence");
  }
}

// Input handler for Presence OLED mode - X button toggles sensor
static bool presenceInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    if (gPresenceRunning) {
      oledConfirmRequest("Close Presence?", nullptr, presenceToggleConfirmed, nullptr, false);
    } else {
      oledConfirmRequest("Open Presence?", nullptr, presenceToggleConfirmed, nullptr);
    }
    return true;
  }
  return false;
}

// Presence OLED mode entry
static const OledModeEntry presenceOledModes[] = {
  {
    OLED_PRESENCE_DATA,          // mode enum
    "Presence",                  // menu name
    "notify_sensor",             // icon name
    displayPresenceData,         // displayFunc
    presenceOledModeAvailable,   // availFunc
    presenceInputHandler,        // inputFunc - X toggles sensor
    true,                        // showInMenu
    36,                          // menuOrder (after APDS at 35)
    nullptr                      // hints
  }
};

// Auto-register Presence OLED mode
REGISTER_OLED_MODE_MODULE(presenceOledModes, sizeof(presenceOledModes) / sizeof(presenceOledModes[0]), "Presence");

#endif // I2CSENSOR_STHS34PF80_OLED_H
