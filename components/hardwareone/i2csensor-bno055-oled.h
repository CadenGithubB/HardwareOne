// i2csensor-bno055-oled.h - BNO055 IMU OLED display functions
// Include this at the end of i2csensor-bno055.cpp
#ifndef I2CSENSOR_BNO055_OLED_H
#define I2CSENSOR_BNO055_OLED_H

#include "OLED_Display.h"
#include <Adafruit_SSD1306.h>

// Forward declaration for IMU action detection
extern void updateIMUActions();

// IMU OLED display function - shows action detection
static void displayIMUActions() {
  extern Adafruit_SSD1306* oledDisplay;
  
  oledDisplay->setTextSize(1);
  oledDisplay->setCursor(0, 0);
  oledDisplay->println("=== IMU ===");
  oledDisplay->println();

  if (!imuConnected || !imuEnabled) {
    oledDisplay->println("IMU not active");
    oledDisplay->println();
    oledDisplay->println("Press X to start");
    return;
  }

  // Update detections
  updateIMUActions();

  // Display detected actions
  oledDisplay->println("Monitoring...");
  
  // Show orientation data from cache
  if (gImuCache.mutex && xSemaphoreTake(gImuCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    oledDisplay->print("Y:");
    oledDisplay->print((int)gImuCache.oriYaw);
    oledDisplay->print(" P:");
    oledDisplay->print((int)gImuCache.oriPitch);
    oledDisplay->print(" R:");
    oledDisplay->println((int)gImuCache.oriRoll);
    xSemaphoreGive(gImuCache.mutex);
  }
}

// Availability check for IMU OLED mode
static bool imuOLEDModeAvailable(String* outReason) {
  return true;  // Always allow navigation, display function handles "not active" state
}

// Input handler for IMU OLED mode - X button toggles sensor
static bool imuInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    extern bool enqueueSensorStart(SensorType sensor);
    extern bool isInQueue(SensorType sensor);
    
    if (imuEnabled && imuConnected) {
      Serial.println("[IMU] X button: Stopping IMU sensor...");
      imuEnabled = false;
    } else if (!isInQueue(SENSOR_IMU)) {
      Serial.println("[IMU] X button: Starting IMU sensor...");
      enqueueSensorStart(SENSOR_IMU);
    }
    return true;
  }
  return false;
}

// IMU OLED mode entry
static const OLEDModeEntry imuOLEDModes[] = {
  {
    OLED_IMU_ACTIONS,        // mode enum
    "IMU",                   // menu name
    "imu_axes",              // icon name
    displayIMUActions,       // displayFunc
    imuOLEDModeAvailable,    // availFunc
    imuInputHandler,         // inputFunc - X toggles sensor
    true,                    // showInMenu
    40                       // menuOrder
  }
};

// Auto-register IMU OLED mode
REGISTER_OLED_MODE_MODULE(imuOLEDModes, sizeof(imuOLEDModes) / sizeof(imuOLEDModes[0]), "IMU");

#endif // I2CSENSOR_BNO055_OLED_H
