// HAL_Motion_OLED.h - the IMU's OLED page (readings from the cache)
// Include this at the end of HAL_Motion.cpp
#ifndef HAL_MOTION_OLED_H
#define HAL_MOTION_OLED_H

#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "HAL_Motion.h"
#include "HAL_MotionCore.h"  // motionCompassPoint
#include <Adafruit_SSD1306.h>
#include <math.h>

// IMU OLED display function - shows the latest reading
static void displayImuActions() {
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

  // The latest reading from the cache: the chip, the angles, the
  // acceleration, and a heading line only when yaw is a heading.
  ImuCache reading{};
  if (imuCacheSnapshot(reading)) {
    char line[32];
    if (!reading.imuDataValid) {
      // Open, no reading yet (the 4517 measures its gyro offset first).
      oledDisplay->setCursor(0, y);
      oledDisplay->println(imuBackendName());
      oledDisplay->println("Waiting for data...");
      return;
    }
    // Row 1: which chip, and its temperature.
    snprintf(line, sizeof(line), "%s %dC", imuBackendName(), (int)reading.imuTemp);
    oledDisplay->setCursor(0, y);
    oledDisplay->println(line);
    y += 10;

    // Row 2: the angles. Without a heading, yaw counts from the IMU's start:
    // it goes last and is marked relative.
    if (reading.hasHeading) {
      snprintf(line, sizeof(line), "Y:%d P:%d R:%d", (int)reading.oriYaw, (int)reading.oriPitch,
               (int)reading.oriRoll);
    } else {
      snprintf(line, sizeof(line), "P:%d R:%d Yrel:%d", (int)reading.oriPitch, (int)reading.oriRoll,
               (int)reading.oriYaw);
    }
    oledDisplay->setCursor(0, y);
    oledDisplay->println(line);
    y += 10;

    // Row 3: acceleration, m/s^2.
    snprintf(line, sizeof(line), "A %.1f %.1f %.1f", (double)reading.accelX, (double)reading.accelY,
             (double)reading.accelZ);
    oledDisplay->setCursor(0, y);
    oledDisplay->println(line);
    y += 10;

    // Row 4: the heading with its compass point, the magnetometer's
    // calibration when the chip reports one (BNO055: c0..c3) and the field
    // strength. Only with a heading.
    if (reading.hasHeading) {
      int n = snprintf(line, sizeof(line), "Hdg:%d %s", (int)reading.oriYaw, motionCompassPoint(reading.oriYaw));
      if (reading.hasCal && n > 0 && n < (int)sizeof(line)) {
        n += snprintf(line + n, sizeof(line) - n, " c%u", (unsigned)(reading.cal & 0x03));
      }
      if (reading.hasMag && n > 0 && n < (int)sizeof(line)) {
        const float field = sqrtf(reading.magX * reading.magX + reading.magY * reading.magY +
                                  reading.magZ * reading.magZ);
        snprintf(line + n, sizeof(line) - n, " %duT", (int)field);
      }
      oledDisplay->setCursor(0, y);
      oledDisplay->println(line);
    }
  } else {
    oledDisplay->setCursor(0, y);
    oledDisplay->println("Reading...");
  }
}

// Availability check for IMU OLED mode
static bool imuOledModeAvailable(String* outReason) {
  return true;  // Always allow navigation, display function handles "not active" state
}

static void imuToggleConfirmed(void* userData) {
  (void)userData;
  if (gImuRunning && gImuConnected) {
    executeOledCommand("closeimu");
  } else {
    executeOledCommand("openimu");
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
static const OledModeEntry imuOledModes[] = {
  {
    OLED_IMU_ACTIONS,        // mode enum
    "IMU",                   // menu name
    "imu_axes",              // icon name
    displayImuActions,       // displayFunc
    imuOledModeAvailable,    // availFunc
    imuInputHandler,         // inputFunc - X toggles sensor
    true,                    // showInMenu
    40,                      // menuOrder
    nullptr,                 // hints
    nullptr,                 // onEnterFunc
    nullptr                  // headerFunc
  }
};

// Auto-register IMU OLED mode
REGISTER_OLED_MODE_MODULE(imuOledModes, sizeof(imuOledModes) / sizeof(imuOledModes[0]), "IMU");

#endif // HAL_MOTION_OLED_H
