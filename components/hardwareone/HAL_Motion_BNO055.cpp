// HAL_Motion_BNO055.cpp - the BNO055 backend of the motion layer
// (HAL_MotionBackend.h): Bosch's 9-axis chip with its own fusion (NDOF).
//
// Register level over the I2C manager; the Adafruit_BNO055 library is no
// longer used. Its begin() waited (up to about a second, and without a bound
// after its reset) while the caller held the bus, freezing every other device
// on that bus. Here each register access is its own short transaction and
// every wait sleeps with the bus released (HAL_Motion_BNO055Core.h).
#include "HAL_MotionBackend.h"
#include "System_BuildConfig.h"

#if ENABLE_IMU_BNO055

#include <Arduino.h>
#include <Wire.h>

#include "HAL_Motion_BNO055Core.h"
#include "System_I2C.h"

namespace {

// The chip stretches the clock on some reads: 100 kHz, as before.
constexpr uint32_t kClockHz = 100000;
// Each start step waits at most this for the bus; a reading fails fast and
// is retried at the next poll rather than waiting long.
constexpr uint32_t kStepLockMs = 50;
constexpr uint32_t kReadLockMs = 100;

const MotionBackendInfo kInfo{kMotionBackendBno055, "BNO055", kClockHz};

// Raw register access on a bus the caller holds through one transaction.
bool readRegs(TwoWire& wire, uint8_t address, uint8_t reg, uint8_t* out, uint8_t len) {
  wire.beginTransmission(address);
  wire.write(reg);
  if (wire.endTransmission(false) != 0) return false;
  if (wire.requestFrom(address, len) != len) return false;
  for (uint8_t i = 0; i < len; ++i) out[i] = (uint8_t)wire.read();
  return true;
}

bool writeReg(TwoWire& wire, uint8_t address, uint8_t reg, uint8_t value) {
  wire.beginTransmission(address);
  wire.write(reg);
  wire.write(value);
  return wire.endTransmission() == 0;
}

TwoWire* wireFor(uint8_t bus) {
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  return mgr ? mgr->getWire(bus) : nullptr;
}

// The start sequence's bus: one short transaction per register access. A
// booting chip does not answer, so these do not count as device errors
// (NACK-tolerant); the health record starts fresh once the chip is up.
struct StartIo {
  uint8_t bus;
  uint8_t address;
  TwoWire* wire;
  bool read(uint8_t reg, uint8_t* out, uint8_t len) {
    bool ok = false;
    i2cTransactionNackTolerant(bus, address, kClockHz, kStepLockMs,
                               [&]() { ok = readRegs(*wire, address, reg, out, len); });
    return ok;
  }
  bool write(uint8_t reg, uint8_t value) {
    bool ok = false;
    i2cTransactionNackTolerant(bus, address, kClockHz, kStepLockMs,
                               [&]() { ok = writeReg(*wire, address, reg, value); });
    return ok;
  }
  void sleepMs(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
  uint32_t nowMs() { return millis(); }
};

}  // namespace

const MotionBackendInfo& motionBackendInfo() { return kInfo; }

uint8_t motionBackendAddress(long savedSetting) { return bno055AddressFromSetting(savedSetting); }

bool motionBackendInit(uint8_t bus, uint8_t address, char* why, size_t whyLen) {
  TwoWire* wire = wireFor(bus);
  if (!wire) {
    if (why && whyLen) {
      snprintf(why, whyLen, "Error: I2C bus %u is not running. Check 'imubus', then 'openimu'.",
               (unsigned)bus);
    }
    return false;
  }
  i2cResetGracePeriod(address, bus);
  StartIo io{bus, address, wire};
  if (!bno055StartSequence(io, bus, address, why, whyLen)) return false;
  // Name the health record after the chip, and start its count fresh.
  if (I2cDeviceManager* mgr = I2cDeviceManager::getInstance()) mgr->registerDevice(address, kInfo.part, bus);
  i2cResetGracePeriod(address, bus);
  return true;
}

bool motionBackendRead(uint8_t bus, uint8_t address, MotionSample& out) {
  TwoWire* wire = wireFor(bus);
  if (!wire) return false;
  uint8_t data[kBno055DataBytes];
  uint8_t status[kBno055StatusBytes];
  const bool ok = i2cTaskWithTimeout(bus, address, kClockHz, kReadLockMs, [&]() -> bool {
    return readRegs(*wire, address, kBno055RegAccelData, data, sizeof(data)) &&
           readRegs(*wire, address, kBno055RegTemp, status, sizeof(status));
  });
  if (!ok) return false;
  const Bno055Reading reading = bno055Decode(data, status);
  for (int i = 0; i < 3; ++i) {
    out.accel[i] = reading.accel[i];
    out.gyro[i] = reading.gyro[i];
    out.mag[i] = reading.mag[i];
  }
  out.yaw = reading.yaw;
  out.pitch = reading.pitch;
  out.roll = reading.roll;
  out.temp = (float)reading.temp;
  out.cal = reading.cal;
  out.hasMag = out.hasOrientation = out.hasHeading = out.hasCal = true;
  return true;
}

// Nothing to release: the backend keeps no object. The chip keeps fusing
// until the next start resets it.
void motionBackendStop() {}

#endif  // ENABLE_IMU_BNO055
