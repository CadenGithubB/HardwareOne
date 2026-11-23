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
// The Adafruit 4517: the LSM6DSOX is the IMU's address (imuAddress), the
// LIS3MDL its second address (imuMagAddress).
#if ENABLE_IMU_LSM6DSOX
#include "HAL_Motion_LSM6DSOXCore.h"
#define IMU_ADDRESS_DEFAULT kLsm6dsoxAddressDefault
#define IMU_ADDRESS_MIN kLsm6dsoxAddressDefault
#define IMU_ADDRESS_MAX kLsm6dsoxAddressAlt
#define IMU_ADDRESS_OPTIONS LSM6DSOX_ADDRESS_OPTIONS
#define IMU_ADDRESS_CHOICES_TEXT "0x6A|0x6B"
#define IMU_MAG_ADDRESS_DEFAULT kLis3mdlAddressDefault
#define IMU_MAG_ADDRESS_MIN kLis3mdlAddressDefault
#define IMU_MAG_ADDRESS_MAX kLis3mdlAddressAlt
#define IMU_MAG_ADDRESS_OPTIONS LIS3MDL_ADDRESS_OPTIONS
#define IMU_MAG_ADDRESS_CHOICES_TEXT "0x1C|0x1E"
#endif

// One sample in the ImuCache units (HAL_Motion.h): accel m/s^2, gyro rad/s,
// mag uT, Euler degrees before the orientation correction, temp Celsius.
struct MotionSample {
  float accel[3] = {0, 0, 0};
  float gyro[3] = {0, 0, 0};
  float mag[3] = {0, 0, 0};
  float yaw = 0, pitch = 0, roll = 0;
  float temp = 0;
  // Which way is up in the sensor frame, a unit vector (hasGravity): for the
  // per-sample hook (HAL_MotionHookCore.h). A backend that fuses here has it
  // from its filter.
  float gravity[3] = {0, 0, 0};
  bool hasGravity = false;
  bool hasMag = false, hasOrientation = false, hasHeading = false, hasCal = false;
  uint8_t cal = 0;
  // The chip's own event engines (hasHw*): a latched single tap / free fall
  // since the previous sample. The action detector takes them in place of
  // its software rule (HAL_MotionCore.h).
  bool hasHwTap = false, hwTap = false;
  bool hasHwFreefall = false, hwFreefall = false;
  // False while the backend is still starting (the 4517's gyro bias): the
  // read worked, but there is nothing to publish or detect yet.
  bool ready = true;
  // The second chip's read failed (the 4517's LIS3MDL): the sample stands
  // with the last good field, and the task checks that chip's health.
  bool secondFailed = false;
};

struct MotionBackendInfo {
  uint8_t id;               // kMotionBackend* (HAL_MotionCore.h)
  const char* part;         // chip name for replies and the device table ("BNO055")
  uint32_t clockHz;         // bus clock of its transactions
  // 0: one sample every imuDevicePollMs (the chip fuses on its own). Else the
  // layer samples at this fixed period (the backend fuses each sample) and
  // publishes the cache every imuDevicePollMs.
  uint16_t samplePeriodMs;
  const char* secondPart;   // a second chip's name ("LIS3MDL"), or nullptr
};
const MotionBackendInfo& motionBackendInfo();

// The address imuAddress names for this backend (its rule for a saved value),
// and the second chip's from imuMagAddress (0: the backend has one chip).
uint8_t motionBackendAddress(long savedSetting);
uint8_t motionBackendSecondAddress(long savedSetting);

// Bring the chip(s) up at (bus, address[, secondAddress]). Ends within
// kMotionInitBudgetMs (HAL_MotionCore.h). On failure, why holds
// "Error: <reason>".
bool motionBackendInit(uint8_t bus, uint8_t address, uint8_t secondAddress, char* why, size_t whyLen);

// One sample, in its own short transaction(s), one per chip; false on a
// failed or short read of the main chip (the transaction records it for the
// health system). A failed second-chip read sets out.secondFailed and keeps
// the sample.
bool motionBackendRead(uint8_t bus, uint8_t address, uint8_t secondAddress, MotionSample& out);

// Release whatever the backend holds and power the chip(s) down, best
// effort; the chip may already be gone.
void motionBackendStop();

// What the backend can do and how its start went, for 'imustatus'. Asked from
// the command's task while the IMU task may be running: the backend answers
// from constants and single flags, so it takes no lock.
struct MotionBackendStatus {
  bool hasHwTap = false;        // the chip detects taps itself
  bool hasHwFreefall = false;   // ...and free falls
  bool hasMag = false;          // a magnetometer is read
  bool hasHeading = false;      // yaw is a heading from magnetic north
  // The gyro offset: a backend the layer fuses measures it at each start
  // (measuresGyroOffset); a chip that fuses on its own keeps its own.
  bool measuresGyroOffset = false;
  bool gyroOffsetDone = false;   // the measurement of this start ended
  bool gyroOffsetStill = false;  // ...on a board that held still (false: no offset, yaw drifts faster)
};
void motionBackendStatus(MotionBackendStatus& out);
