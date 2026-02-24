// i2csensor-bno055-oled.h - BNO055 IMU OLED display functions
// Include this at the end of i2csensor-bno055.cpp
#ifndef I2CSENSOR_BNO055_OLED_H
#define I2CSENSOR_BNO055_OLED_H

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include <Adafruit_SSD1306.h>

// Forward declaration for IMU action detection
extern void updateIMUActions();

// IMU OLED display function - shows action detection
static void displayIMUActions() {
  // Header is rendered by the system - content starts at OLED_CONTENT_START_Y
  int y = OLED_CONTENT_START_Y;
  oledDisplay->setTextSize(1);

  if (!imuConnected || !imuEnabled) {
    oledDisplay->setCursor(0, y);
    oledDisplay->println("IMU not active");
    oledDisplay->println();
    oledDisplay->println("Press X to start");
    return;
  }

  // Update detections
  updateIMUActions();

  // Show orientation and acceleration data from cache
  if (gImuCache.mutex && xSemaphoreTake(gImuCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    oledDisplay->setCursor(0, y);
    oledDisplay->print("Y:");
    oledDisplay->print((int)gImuCache.oriYaw);
    oledDisplay->print(" P:");
    oledDisplay->print((int)gImuCache.oriPitch);
    oledDisplay->print(" R:");
    oledDisplay->println((int)gImuCache.oriRoll);
    y += 10;

    oledDisplay->setCursor(0, y);
    oledDisplay->print("Ax:");
    oledDisplay->print(gImuCache.accelX, 1);
    oledDisplay->print(" Ay:");
    oledDisplay->println(gImuCache.accelY, 1);
    y += 10;

    oledDisplay->setCursor(0, y);
    oledDisplay->print("Az:");
    oledDisplay->print(gImuCache.accelZ, 1);
    y += 10;

    oledDisplay->setCursor(0, y);
    oledDisplay->print("Temp:");
    oledDisplay->print(gImuCache.imuTemp, 1);
    oledDisplay->print("C");

    xSemaphoreGive(gImuCache.mutex);
  } else {
    oledDisplay->setCursor(0, y);
    oledDisplay->println("Reading...");
  }
}

// Availability check for IMU OLED mode
static bool imuOLEDModeAvailable(String* outReason) {
  return true;  // Always allow navigation, display function handles "not active" state
}

static void imuToggleConfirmed(void* userData) {
  (void)userData;
  // enqueueDeviceStart, isInQueue provided by System_I2C.h (included in parent .cpp)

  if (imuEnabled && imuConnected) {
    Serial.println("[IMU] Confirmed: Stopping IMU sensor...");
    imuEnabled = false;
  } else if (!isInQueue(I2C_DEVICE_IMU)) {
    Serial.println("[IMU] Confirmed: Starting IMU sensor...");
    enqueueDeviceStart(I2C_DEVICE_IMU);
  }
}

// Input handler for IMU OLED mode - X button toggles sensor
static bool imuInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    if (imuEnabled && imuConnected) {
      oledConfirmRequest("Close IMU?", nullptr, imuToggleConfirmed, nullptr);
    } else {
      oledConfirmRequest("Open IMU?", nullptr, imuToggleConfirmed, nullptr);
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
