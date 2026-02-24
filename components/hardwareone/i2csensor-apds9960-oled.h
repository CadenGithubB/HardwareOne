// i2csensor-apds9960-oled.h - APDS9960 OLED display functions
// Include this at the end of i2csensor-apds9960.cpp
#ifndef I2CSENSOR_APDS9960_OLED_H
#define I2CSENSOR_APDS9960_OLED_H

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "HAL_Display.h"  // For gDisplay and oledDisplay macro alias

// Display function for APDS OLED mode
void displayAPDSData() {
  if (!oledDisplay || !oledConnected) return;
  
  oledDisplay->clearDisplay();
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(0, 0);
  
  if (!apdsConnected || (!apdsColorEnabled && !apdsProximityEnabled)) {
    oledDisplay->println("== APDS SENSOR ==");
    oledDisplay->println();
    oledDisplay->println("Not active");
    oledDisplay->println();
    oledDisplay->println("Press X to start");
  } else {
    oledDisplay->println("== APDS DATA ==");
    oledDisplay->println();
    
    if (gPeripheralCache.mutex && xSemaphoreTake(gPeripheralCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      oledDisplay->print("R:");
      oledDisplay->print(gPeripheralCache.apdsRed);
      oledDisplay->print(" G:");
      oledDisplay->println(gPeripheralCache.apdsGreen);
      oledDisplay->print("B:");
      oledDisplay->print(gPeripheralCache.apdsBlue);
      oledDisplay->print(" C:");
      oledDisplay->println(gPeripheralCache.apdsClear);
      oledDisplay->print("Prox: ");
      oledDisplay->println(gPeripheralCache.apdsProximity);
      xSemaphoreGive(gPeripheralCache.mutex);
    }
  }
  
  // Don't call display() here - let updateOLEDDisplay() render footer and display in same frame
}

// Availability check for APDS OLED mode
static bool apdsOLEDModeAvailable(String* outReason) {
  return true;  // Always allow navigation
}

static void apdsToggleConfirmed(void* userData) {
  (void)userData;
  // enqueueDeviceStart, isInQueue provided by System_I2C.h (included in parent .cpp)

  if (apdsColorEnabled || apdsProximityEnabled) {
    Serial.println("[APDS] Confirmed: Stopping APDS sensor...");
    apdsColorEnabled = false;
    apdsProximityEnabled = false;
  } else if (!isInQueue(I2C_DEVICE_APDS)) {
    Serial.println("[APDS] Confirmed: Starting APDS sensor...");
    enqueueDeviceStart(I2C_DEVICE_APDS);
  }
}

// Input handler for APDS OLED mode - X button toggles sensor
static bool apdsInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    if (apdsColorEnabled || apdsProximityEnabled) {
      oledConfirmRequest("Close APDS?", nullptr, apdsToggleConfirmed, nullptr, false);
    } else {
      oledConfirmRequest("Open APDS?", nullptr, apdsToggleConfirmed, nullptr);
    }
    return true;
  }
  return false;
}

// APDS OLED mode entry
static const OLEDModeEntry apdsOLEDModes[] = {
  {
    OLED_APDS_DATA,          // mode enum
    "APDS",                  // menu name
    "notify_sensor",         // icon name
    displayAPDSData,         // displayFunc
    apdsOLEDModeAvailable,   // availFunc
    apdsInputHandler,        // inputFunc - X toggles sensor
    true,                    // showInMenu
    35                       // menuOrder
  }
};

// Auto-register APDS OLED mode
REGISTER_OLED_MODE_MODULE(apdsOLEDModes, sizeof(apdsOLEDModes) / sizeof(apdsOLEDModes[0]), "APDS");

#endif // I2CSENSOR_APDS9960_OLED_H
