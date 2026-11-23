// HAL_Motion_LSM6DSOX.cpp - the Adafruit 4517 backend of the motion layer
// (HAL_MotionBackend.h): an ST LSM6DSOX (accelerometer + gyroscope) and an ST
// LIS3MDL (magnetometer) on one board, fused here with a Mahony filter
// (HAL_Motion_Fusion.h) at a fixed 50 Hz.
//
// Register level over the I2C manager; the Adafruit_LSM6DS / Adafruit_LIS3MDL
// libraries are not used (they allocate in internal RAM, wait without a bound
// while holding the bus and ignore failed reads). Each register access of the
// start is its own short transaction with every wait asleep and the bus
// released (HAL_Motion_LSM6DSOXCore.h); each sample is one transaction per
// chip (lsm4517ReadSample: the magnetometer first, the LSM6DSOX and its
// read-to-clear event flags last). A failed or short LSM6DSOX read fails the
// sample; a failed magnetometer read keeps the sample with the last good
// field. Both count in the chips' health records, and auto-disable sees them.
//
// First version: tilt, flip and tap. The magnetometer is read and reported,
// but the fusion runs on the gyroscope and accelerometer only (6 axes), so
// yaw is relative to the start and hasHeading is false until the magnetometer
// calibration lands (TODO(mag-cal) in HAL_Motion_Fusion.h).
#include "HAL_MotionBackend.h"
#include "System_BuildConfig.h"

#if ENABLE_IMU_LSM6DSOX

#include <Arduino.h>
#include <Wire.h>
#include <esp_attr.h>   // EXT_RAM_BSS_ATTR
#include <esp_timer.h>

#include "HAL_Motion_Fusion.h"
#include "HAL_Motion_LSM6DSOXCore.h"
#include "System_I2C.h"

namespace {

constexpr uint32_t kClockHz = 400000;
// A sample waits at most this for the bus per chip: longer than one OLED
// frame on a shared bus (about 23 ms at 400 kHz); a busy bus fails the
// sample, and the 20 ms schedule absorbs the late tick.
constexpr uint32_t kReadLockMs = 35;
// The layer samples every 20 ms (50 Hz) and publishes the cache every
// imuDevicePollMs.
constexpr uint16_t kSamplePeriodMs = 20;

const MotionBackendInfo kInfo{kMotionBackendLsm6dsox, "LSM6DSOX", kClockHz, kSamplePeriodMs, "LIS3MDL"};

// Driver state: the filter, the start-up bias estimate and the last step's
// time. PSRAM (NOLOAD: no initialisers run), so motionBackendInit sets it all.
struct Lsm4517State {
  MahonyFilter filter;
  GyroBiasEstimator bias;
  MagCalibration magCal;
  Lsm4517SampleState sample;  // event flags and field carried between samples
  int64_t lastUs;
  bool fusing;  // the bias is in and the filter started
  // Where the chips were started (for the power-down at stop); started is
  // false until a start succeeded and again after the stop.
  bool started;
  uint8_t bus, address, secondAddress;
};
EXT_RAM_BSS_ATTR Lsm4517State sState;

bool readRegs(TwoWire& wire, uint8_t address, uint8_t reg, uint8_t* out, uint8_t len) {
  wire.beginTransmission(address);
  wire.write(reg);
  if (wire.endTransmission(false) != 0) return false;
  if (wire.requestFrom(address, len) != len) return false;
  for (uint8_t i = 0; i < len; ++i) out[i] = (uint8_t)wire.read();
  return true;
}

bool writeRegs(TwoWire& wire, uint8_t address, uint8_t reg, const uint8_t* values, uint8_t len) {
  wire.beginTransmission(address);
  wire.write(reg);
  for (uint8_t i = 0; i < len; ++i) wire.write(values[i]);
  return wire.endTransmission() == 0;
}

TwoWire* wireFor(uint8_t bus) {
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  return mgr ? mgr->getWire(bus) : nullptr;
}

// The start sequence's bus: one short NACK-tolerant transaction per register
// access (a chip that is absent or still resetting is no device error; the
// health records start fresh once both chips are up). An access whose
// operation never ran found the bus busy (lock timeout).
struct StartIo {
  uint8_t bus;
  TwoWire* wire;
  bool lastBusy = false;
  bool read(uint8_t address, uint8_t reg, uint8_t* out, uint8_t len) {
    bool ok = false, ran = false;
    i2cTransactionNackTolerant(bus, address, kClockHz, kLsm4517StepLockMs,
                               [&]() { ran = true; ok = readRegs(*wire, address, reg, out, len); });
    lastBusy = !ran;
    return ok;
  }
  bool write(uint8_t address, uint8_t reg, const uint8_t* values, uint8_t len) {
    bool ok = false, ran = false;
    i2cTransactionNackTolerant(bus, address, kClockHz, kLsm4517StepLockMs,
                               [&]() { ran = true; ok = writeRegs(*wire, address, reg, values, len); });
    lastBusy = !ran;
    return ok;
  }
  bool busy() const { return lastBusy; }
  void sleepMs(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
  uint32_t nowMs() { return millis(); }
};

}  // namespace

const MotionBackendInfo& motionBackendInfo() { return kInfo; }

uint8_t motionBackendAddress(long savedSetting) { return lsm6dsoxAddressFromSetting(savedSetting); }
uint8_t motionBackendSecondAddress(long savedSetting) { return lis3mdlAddressFromSetting(savedSetting); }

bool motionBackendInit(uint8_t bus, uint8_t address, uint8_t secondAddress, char* why, size_t whyLen) {
  TwoWire* wire = wireFor(bus);
  if (!wire) {
    if (why && whyLen) {
      snprintf(why, whyLen, "Error: I2C bus %u is not running. Check 'imubus', then 'openimu'.", (unsigned)bus);
    }
    return false;
  }
  sState = Lsm4517State{};
  i2cResetGracePeriod(address, bus);
  i2cResetGracePeriod(secondAddress, bus);
  StartIo io{bus, wire};
  Lsm6Chip chip = Lsm6Chip::Dsox;
  if (!lsm4517StartSequence(io, bus, address, secondAddress, why, whyLen, &chip)) return false;
  sState.sample.chip = chip;
  // Name both health records after their chips, and start their counts fresh.
  if (I2cDeviceManager* mgr = I2cDeviceManager::getInstance()) {
    mgr->registerDevice(address, kInfo.part, bus);
    mgr->registerDevice(secondAddress, kInfo.secondPart, bus);
  }
  i2cResetGracePeriod(address, bus);
  i2cResetGracePeriod(secondAddress, bus);
  sState.bus = bus;
  sState.address = address;
  sState.secondAddress = secondAddress;
  sState.started = true;
  return true;
}

bool motionBackendRead(uint8_t bus, uint8_t address, uint8_t secondAddress, MotionSample& out) {
  TwoWire* wire = wireFor(bus);
  if (!wire) return false;
  Lsm4517Reading reading{};
  bool magFailed = false;
  const bool ok = lsm4517ReadSample(
      sState.sample,
      [&](uint8_t* field) -> bool {
        return i2cTaskWithTimeout(bus, secondAddress, kClockHz, kReadLockMs, [&]() -> bool {
          return readRegs(*wire, secondAddress, kLis3mdlRegOutX | kLis3mdlAutoIncrement, field, kLis3mdlDataBytes);
        });
      },
      [&](uint8_t* src, uint8_t* data, bool& srcRead) -> bool {
        return i2cTaskWithTimeout(bus, address, kClockHz, kReadLockMs, [&]() -> bool {
          srcRead = readRegs(*wire, address, kLsm6dsoxRegAllIntSrc, src, kLsm6dsoxIntSrcBytes);
          // An all-zero burst (the chip lost power and came back in
          // power-down) fails the transaction, so it counts as a device error.
          return srcRead && readRegs(*wire, address, kLsm6dsoxRegOutTemp, data, kLsm6dsoxDataBytes) &&
                 lsm6dsoxBurstAlive(data);
        });
      },
      reading, magFailed);
  out.secondFailed = magFailed;
  if (!ok) return false;
  const int64_t nowUs = esp_timer_get_time();
  magCorrect(sState.magCal, reading.mag);  // the identity until the calibration lands

  for (int i = 0; i < 3; ++i) {
    out.accel[i] = reading.accel[i];
    out.mag[i] = reading.mag[i];
  }
  out.temp = reading.temp;
  out.hasMag = reading.hasMag;
  out.hasOrientation = true;
  out.hasHeading = false;  // 6-axis fusion: yaw is relative to the start
  out.hasCal = false;
  out.hasHwTap = out.hasHwFreefall = true;
  out.hwTap = reading.tap;
  out.hwFreefall = reading.freefall;

  // The first second: the gyro bias, with the board (hopefully) still. No
  // reading is published until it is in.
  if (!sState.fusing) {
    if (!gyroBiasAdd(sState.bias, reading.gyro)) {
      for (int i = 0; i < 3; ++i) out.gyro[i] = reading.gyro[i];
      out.ready = false;
      return true;
    }
    for (int i = 0; i < 3; ++i) out.gyro[i] = reading.gyro[i] - sState.bias.bias[i];
    mahonyInitFromAccel(sState.filter, out.accel[0], out.accel[1], out.accel[2]);
    sState.lastUs = nowUs;
    sState.fusing = true;
  } else {
    for (int i = 0; i < 3; ++i) out.gyro[i] = reading.gyro[i] - sState.bias.bias[i];
    const float dt = (float)(nowUs - sState.lastUs) / 1e6f;
    sState.lastUs = nowUs;
    // A step mahonyUpdate refuses (a stall) leaves the estimate as it was.
    mahonyUpdate(sState.filter, out.gyro[0], out.gyro[1], out.gyro[2], out.accel[0], out.accel[1],
                 out.accel[2], dt);
  }
  const FusionEuler euler = mahonyEuler(sState.filter);
  out.yaw = euler.yaw;
  out.pitch = euler.pitch;
  out.roll = euler.roll;
  mahonyGravity(sState.filter, out.gravity);
  out.hasGravity = true;
  out.ready = true;
  return true;
}

// The state is static; nothing to free. Both chips are powered down (best
// effort, NACK-tolerant: the chip may be gone, and the next start resets
// them anyway), so a closed IMU stops drawing its measuring current.
void motionBackendStop() {
  sState.fusing = false;
  if (!sState.started) return;
  sState.started = false;
  const uint8_t bus = sState.bus, address = sState.address, secondAddress = sState.secondAddress;
  TwoWire* wire = wireFor(bus);
  if (!wire) return;
  i2cTransactionNackTolerant(bus, address, kClockHz, kReadLockMs, [&]() {
    writeRegs(*wire, address, kLsm6dsoxRegCtrl1Xl, kLsm6dsoxPowerDown, sizeof(kLsm6dsoxPowerDown));
  });
  if (secondAddress != 0) {
    i2cTransactionNackTolerant(bus, secondAddress, kClockHz, kReadLockMs, [&]() {
      writeRegs(*wire, secondAddress, kLis3mdlRegCtrl3, &kLis3mdlPowerDown, 1);
    });
  }
}

// Tap and free fall come from the LSM6DSOX; no heading until the magnetometer
// calibration lands. The gyro offset is this start's (flags the IMU task
// writes; read here without a lock).
void motionBackendStatus(MotionBackendStatus& out) {
  out = MotionBackendStatus{};
  out.hasHwTap = out.hasHwFreefall = true;
  out.hasMag = true;
  out.hasHeading = false;
  out.measuresGyroOffset = true;
  if (sState.started) {
    out.gyroOffsetDone = sState.bias.done;
    out.gyroOffsetStill = sState.bias.done && sState.bias.still;
  }
}

#endif  // ENABLE_IMU_LSM6DSOX
