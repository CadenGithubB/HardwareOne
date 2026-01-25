// i2csensor-sths34pf80-oled.h - STHS34PF80 OLED display functions
// Include this at the end of i2csensor-sths34pf80.cpp
#ifndef I2CSENSOR_STHS34PF80_OLED_H
#define I2CSENSOR_STHS34PF80_OLED_H

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "HAL_Display.h"  // For gDisplay and oledDisplay macro alias

// Display function for Presence OLED mode
void displayPresenceData() {
  if (!oledDisplay || !oledConnected) return;
  
  oledDisplay->clearDisplay();
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(0, 0);
  
  if (!presenceConnected || !presenceEnabled) {
    oledDisplay->println("== PRESENCE ==");
    oledDisplay->println();
    oledDisplay->println("Not active");
    oledDisplay->println();
    oledDisplay->println("Press X to start");
  } else {
    oledDisplay->println("== PRESENCE ==");
    
    if (gPresenceCache.mutex && xSemaphoreTake(gPresenceCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      // Ambient temperature
      oledDisplay->print("Ambient: ");
      oledDisplay->print(gPresenceCache.ambientTemp, 1);
      oledDisplay->println("C");
      
      // Presence detection with value
      oledDisplay->print("Presence: ");
      oledDisplay->print(gPresenceCache.presenceValue);
      if (gPresenceCache.presenceDetected) {
        oledDisplay->println(" [!]");
      } else {
        oledDisplay->println();
      }
      
      // Motion detection with value
      oledDisplay->print("Motion: ");
      oledDisplay->print(gPresenceCache.motionValue);
      if (gPresenceCache.motionDetected) {
        oledDisplay->println(" [!]");
      } else {
        oledDisplay->println();
      }
      
      // Temperature shock
      oledDisplay->print("TShock: ");
      oledDisplay->print(gPresenceCache.tempShockValue);
      if (gPresenceCache.tempShockDetected) {
        oledDisplay->println(" [!]");
      } else {
        oledDisplay->println();
      }
      
      xSemaphoreGive(gPresenceCache.mutex);
    }
  }
  
  // Don't call display() here - let updateOLEDDisplay() render footer and display in same frame
}

// Availability check for Presence OLED mode
static bool presenceOLEDModeAvailable(String* outReason) {
  return true;  // Always allow navigation
}

static void presenceToggleConfirmed(void* userData) {
  (void)userData;

  if (presenceEnabled) {
    Serial.println("[PRESENCE] Confirmed: Stopping presence sensor...");
    presenceEnabled = false;
  } else {
    extern bool startPresenceSensorInternal();
    Serial.println("[PRESENCE] Confirmed: Starting presence sensor...");
    startPresenceSensorInternal();
  }
}

// Input handler for Presence OLED mode - X button toggles sensor
static bool presenceInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    if (presenceEnabled) {
      oledConfirmRequest("Stop Presence?", nullptr, presenceToggleConfirmed, nullptr, false);
    } else {
      oledConfirmRequest("Start Presence?", nullptr, presenceToggleConfirmed, nullptr);
    }
    return true;
  }
  return false;
}

// Presence OLED mode entry
static const OLEDModeEntry presenceOLEDModes[] = {
  {
    OLED_PRESENCE_DATA,          // mode enum
    "Presence",                  // menu name
    "notify_sensor",             // icon name
    displayPresenceData,         // displayFunc
    presenceOLEDModeAvailable,   // availFunc
    presenceInputHandler,        // inputFunc - X toggles sensor
    true,                        // showInMenu
    36                           // menuOrder (after APDS at 35)
  }
};

// Auto-register Presence OLED mode
REGISTER_OLED_MODE_MODULE(presenceOLEDModes, sizeof(presenceOLEDModes) / sizeof(presenceOLEDModes[0]), "Presence");

#endif // I2CSENSOR_STHS34PF80_OLED_H
