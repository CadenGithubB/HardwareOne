// i2csensor_bno055_oled.h - BNO055 IMU OLED display functions
// Include this at the end of i2csensor_bno055.cpp
#ifndef I2CSENSOR_BNO055_OLED_H
#define I2CSENSOR_BNO055_OLED_H

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "i2csensor_bno055.h"
#include <Adafruit_SSD1306.h>

// Forward declaration for IMU action detection
extern void imuUpdateActions(bool postEvents);

// IMU OLED display function - shows action detection
static void displayIMUActions() {
  // Header is rendered by the system - content starts at OLED_CONTENT_START_Y
  int y = OLED_CONTENT_START_Y;
  oledDisplay->setTextSize(1);

  if (!gImuConnected || !gImuRunning) {
    oledDisplay->setCursor(0, y);
    oledDisplay->println("IMU not active");
    oledDisplay->println();
    oledDisplay->println("Press X to start");
    return;
  }

  // Update detections
  imuUpdateActions();

  // Show orientation and acceleration data from cache
  ImuCache reading{};
  if (imuCacheSnapshot(reading)) {
    oledDisplay->setCursor(0, y);
    oledDisplay->print("Y:");
    oledDisplay->print((int)reading.oriYaw);
    oledDisplay->print(" P:");
    oledDisplay->print((int)reading.oriPitch);
    oledDisplay->print(" R:");
    oledDisplay->println((int)reading.oriRoll);
    y += 10;

    oledDisplay->setCursor(0, y);
    oledDisplay->print("Ax:");
    oledDisplay->print(reading.accelX, 1);
    oledDisplay->print(" Ay:");
    oledDisplay->println(reading.accelY, 1);
    y += 10;

    oledDisplay->setCursor(0, y);
    oledDisplay->print("Az:");
    oledDisplay->print(reading.accelZ, 1);
    y += 10;

    oledDisplay->setCursor(0, y);
    oledDisplay->print("Temp:");
    oledDisplay->print(reading.imuTemp, 1);
    oledDisplay->print("C");
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
  if (gImuRunning && gImuConnected) {
    executeOLEDCommand("closeimu");
  } else {
    executeOLEDCommand("openimu");
  }
}

// Input handler for IMU OLED mode - X button toggles sensor
static bool imuInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    if (gImuRunning && gImuConnected) {
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
    40,                      // menuOrder
    nullptr                  // hints
  }
};

// Auto-register IMU OLED mode
REGISTER_OLED_MODE_MODULE(imuOLEDModes, sizeof(imuOLEDModes) / sizeof(imuOLEDModes[0]), "IMU");

#endif // I2CSENSOR_BNO055_OLED_H
