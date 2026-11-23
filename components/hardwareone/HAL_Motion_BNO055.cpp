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
// Each start access waits at most kBno055StepLockMs for the bus (a busy
// step is tried again inside the start's deadline, HAL_Motion_BNO055Core.h);
// a reading fails fast and is retried at the next poll rather than waiting long.
constexpr uint32_t kReadLockMs = 100;

const MotionBackendInfo kInfo{kMotionBackendBno055, "BNO055", kClockHz, 0, nullptr};

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
// (NACK-tolerant); the health record starts fresh once the chip is up. An
// access whose operation never ran found the bus busy (lock timeout), which
// the sequence reports apart from a chip that does not answer.
struct StartIo {
  uint8_t bus;
  uint8_t address;
  TwoWire* wire;
  bool lastBusy = false;
  bool read(uint8_t reg, uint8_t* out, uint8_t len) {
    bool ok = false, ran = false;
    i2cTransactionNackTolerant(bus, address, kClockHz, kBno055StepLockMs,
                               [&]() { ran = true; ok = readRegs(*wire, address, reg, out, len); });
    lastBusy = !ran;
    return ok;
  }
  bool write(uint8_t reg, uint8_t value) {
    bool ok = false, ran = false;
    i2cTransactionNackTolerant(bus, address, kClockHz, kBno055StepLockMs,
                               [&]() { ran = true; ok = writeReg(*wire, address, reg, value); });
    lastBusy = !ran;
    return ok;
  }
  bool busy() const { return lastBusy; }
  void sleepMs(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
  uint32_t nowMs() { return millis(); }
};

}  // namespace

const MotionBackendInfo& motionBackendInfo() { return kInfo; }

uint8_t motionBackendAddress(long savedSetting) { return bno055AddressFromSetting(savedSetting); }
uint8_t motionBackendSecondAddress(long) { return 0; }  // one chip

bool motionBackendInit(uint8_t bus, uint8_t address, uint8_t, char* why, size_t whyLen) {
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

bool motionBackendRead(uint8_t bus, uint8_t address, uint8_t, MotionSample& out) {
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

void motionBackendSetTapTuning(long, long, long) {}  // the BNO055 has no tap engine
void motionBackendSetKnockTuning(long, long, long) {}
bool motionBackendKnockTrace(knock::Trace&, knock::Config&) { return false; }
void motionBackendKnockTraceClear() {}

// The chip fuses nine axes itself: a heading from its magnetometer, its own
// calibration (the reading's cal byte), no tap or free-fall engine.
void motionBackendStatus(MotionBackendStatus& out) {
  out = MotionBackendStatus{};
  out.hasMag = out.hasHeading = true;
}

#endif  // ENABLE_IMU_BNO055
