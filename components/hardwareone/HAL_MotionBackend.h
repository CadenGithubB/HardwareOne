// HAL_MotionBackend.h - what a motion backend gives the shared IMU layer
// (HAL_Motion.cpp). Exactly one backend is compiled per build
// (IMU_DEVICE_TYPE); its .cpp is guarded in the file.
//
// The layer calls these on the IMU task only, so a backend keeps its state
// without a lock. Every bus access goes through the I2C manager in short
// transactions; no call holds the bus while it waits.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "System_BuildConfig.h"

// The backend's addresses, for the imuAddress setting and its command.
#if ENABLE_IMU_BNO055
#include "HAL_Motion_BNO055Core.h"
#define IMU_ADDRESS_DEFAULT kBno055AddressDefault
#define IMU_ADDRESS_MIN kBno055AddressDefault
#define IMU_ADDRESS_MAX kBno055AddressAlt
#define IMU_ADDRESS_OPTIONS BNO055_ADDRESS_OPTIONS
#define IMU_ADDRESS_CHOICES_TEXT "0x28|0x29"
#endif

// One sample in the ImuCache units (HAL_Motion.h): accel m/s^2, gyro rad/s,
// mag uT, Euler degrees before the orientation correction, temp Celsius.
struct MotionSample {
  float accel[3] = {0, 0, 0};
  float gyro[3] = {0, 0, 0};
  float mag[3] = {0, 0, 0};
  float yaw = 0, pitch = 0, roll = 0;
  float temp = 0;
  bool hasMag = false, hasOrientation = false, hasHeading = false, hasCal = false;
  uint8_t cal = 0;
};

struct MotionBackendInfo {
  uint8_t id;          // kMotionBackend* (HAL_MotionCore.h)
  const char* part;    // chip name for replies and the device table ("BNO055")
  uint32_t clockHz;    // bus clock of its transactions
};
const MotionBackendInfo& motionBackendInfo();

// The address imuAddress names for this backend (its rule for a saved value).
uint8_t motionBackendAddress(long savedSetting);

// Bring the chip up at (bus, address). Ends within kMotionInitBudgetMs
// (HAL_MotionCore.h). On failure, why holds "Error: <reason>".
bool motionBackendInit(uint8_t bus, uint8_t address, char* why, size_t whyLen);

// One sample, in its own short transaction(s); false on any failed or short
// read (the transaction records it for the health system).
bool motionBackendRead(uint8_t bus, uint8_t address, MotionSample& out);

// Release whatever the backend holds; the chip may already be gone.
void motionBackendStop();
