// i2csensor-vl53l4cx-oled.h - VL53L4CX ToF OLED display functions
// Include this at the end of i2csensor-vl53l4cx.cpp
#ifndef I2CSENSOR_VL53L4CX_OLED_H
#define I2CSENSOR_VL53L4CX_OLED_H

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include <Adafruit_SSD1306.h>

// ToF OLED display function - shows distance data
static void displayToFData() {
  // Header is rendered by the system - content starts at OLED_CONTENT_START_Y
  int y = OLED_CONTENT_START_Y;
  oledDisplay->setTextSize(1);

  if (!tofConnected || !tofEnabled) {
    oledDisplay->setCursor(0, y);
    oledDisplay->println("ToF not active");
    oledDisplay->println();
    oledDisplay->println("Press X to start");
    return;
  }

  if (gTofCache.mutex && xSemaphoreTake(gTofCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (gTofCache.tofDataValid && gTofCache.tofTotalObjects > 0) {
      // Show first detected object distance
      int distMM = gTofCache.tofObjects[0].distance_mm;
      oledDisplay->setCursor(0, y);
      oledDisplay->setTextSize(2);
      oledDisplay->print(distMM);
      oledDisplay->println(" mm");
      oledDisplay->setTextSize(1);
      y += 20;
      
      // Visual bar representation (0-2000mm range)
      int barWidth = map(constrain(distMM, 0, 2000), 0, 2000, 0, 120);
      oledDisplay->drawRect(0, y, 124, 10, DISPLAY_COLOR_WHITE);
      oledDisplay->fillRect(2, y + 2, barWidth, 6, DISPLAY_COLOR_WHITE);
      
      oledDisplay->setCursor(0, y + 12);
      oledDisplay->print("0");
      oledDisplay->setCursor(100, y + 12);
      oledDisplay->print("2000mm");
    } else {
      oledDisplay->setCursor(0, y);
      oledDisplay->println("Waiting for data...");
    }
    xSemaphoreGive(gTofCache.mutex);
  } else {
    oledDisplay->setCursor(0, y);
    oledDisplay->println("ToF: Busy");
  }
}

// Availability check for ToF OLED mode
static bool tofOLEDModeAvailable(String* outReason) {
  return true;  // Always allow navigation, display function handles "not active" state
}

static void tofToggleConfirmed(void* userData) {
  (void)userData;
  // enqueueDeviceStart, isInQueue provided by System_I2C.h (included in parent .cpp)

  if (tofEnabled && tofConnected) {
    Serial.println("[TOF] Confirmed: Stopping ToF sensor...");
    tofEnabled = false;
  } else if (!isInQueue(I2C_DEVICE_TOF)) {
    Serial.println("[TOF] Confirmed: Starting ToF sensor...");
    enqueueDeviceStart(I2C_DEVICE_TOF);
  }
}

// Input handler for ToF OLED mode - X button toggles sensor
static bool tofInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    if (tofEnabled && tofConnected) {
      oledConfirmRequest("Close ToF?", nullptr, tofToggleConfirmed, nullptr, false);
    } else {
      oledConfirmRequest("Open ToF?", nullptr, tofToggleConfirmed, nullptr);
    }
    return true;
  }
  return false;
}

// ToF OLED mode entry
static const OLEDModeEntry tofOLEDModes[] = {
  {
    OLED_TOF_DATA,           // mode enum
    "ToF",                   // menu name
    "tof_radar",             // icon name
    displayToFData,          // displayFunc
    tofOLEDModeAvailable,    // availFunc
    tofInputHandler,         // inputFunc - X toggles sensor
    true,                    // showInMenu
    30                       // menuOrder
  }
};

// Auto-register ToF OLED mode
REGISTER_OLED_MODE_MODULE(tofOLEDModes, sizeof(tofOLEDModes) / sizeof(tofOLEDModes[0]), "ToF");

#endif // I2CSENSOR_VL53L4CX_OLED_H
