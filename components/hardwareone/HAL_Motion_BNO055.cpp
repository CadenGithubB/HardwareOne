// HAL_Motion_BNO055.cpp - the BNO055 backend of the motion layer
// (HAL_MotionBackend.h): Bosch's 9-axis chip with its own fusion (NDOF).
//
// Register level over the I2C manager; the Adafruit_BNO055 library is no
// longer used. Its begin() waited (up to about a second, and without a bound
// after its reset) while the caller held the bus, freezing every other device
// on that bus. Here each register access is its own short transaction and
// every wait sleeps with the bus released (HAL_Motion_BNO055Core.h).
//
// Sampling: without IMU input in the build, one read of the whole block every
// imuDevicePollMs, as always. With IMU input (ENABLE_IMU_INPUT) the chip is
// read every 10 ms, its fusion rate: each sample is one short transaction of
// ACC_DATA and GRV_DATA (12 bytes) for the software knock detector
// (HAL_Motion_KnockFeed.h) and the input's gravity; the full block (mag, gyro,
// Euler, temperature, calibration) joins that transaction every 100 ms and
// its last values stand in between.
#include "HAL_MotionBackend.h"
#include "System_BuildConfig.h"

#if ENABLE_IMU_BNO055

#include <Arduino.h>
#include <Wire.h>
#if ENABLE_IMU_KNOCK_DETECTOR
#include <esp_attr.h>   // EXT_RAM_BSS_ATTR
#include <esp_timer.h>

#include "HAL_Motion_KnockFeed.h"
#endif

#include "HAL_Motion_BNO055Core.h"
#include "System_I2C.h"

namespace {

// The chip stretches the clock on some reads: 100 kHz, as before.
constexpr uint32_t kClockHz = 100000;
// Each start access waits at most kBno055StepLockMs for the bus (a busy
// step is tried again inside the start's deadline, HAL_Motion_BNO055Core.h);
// a reading fails fast and is retried at the next poll rather than waiting long.
constexpr uint32_t kReadLockMs = 100;

#if ENABLE_IMU_KNOCK_DETECTOR
// Fast sampling: the layer reads every 10 ms and publishes the cache every
// imuDevicePollMs. A sample waits at most this for the bus: longer than one
// OLED frame on a shared bus; a busy bus fails the sample, and the 10 ms
// schedule absorbs the late tick.
constexpr uint32_t kFastReadLockMs = 35;
const MotionBackendInfo kInfo{kMotionBackendBno055, "BNO055", kClockHz, kBno055FastPeriodMs, nullptr};

// What stands between full reads: the last full block, and the last gravity
// direction the fusion gave. PSRAM (NOLOAD), set by motionBackendInit.
struct Bno055FastState {
  Bno055Reading full;
  bool haveFull;
  uint32_t lastFullMs;
  float up[3];
  bool haveUp;
};
EXT_RAM_BSS_ATTR Bno055FastState sFast;
#else
const MotionBackendInfo kInfo{kMotionBackendBno055, "BNO055", kClockHz, 0, nullptr};
#endif

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
#if ENABLE_IMU_KNOCK_DETECTOR
  sFast = Bno055FastState{};
  knockFeedStart(true, kBno055FastHz);
#endif
  if (!bno055StartSequence(io, bus, address, why, whyLen)) return false;
  // Name the health record after the chip, and start its count fresh.
  if (I2cDeviceManager* mgr = I2cDeviceManager::getInstance()) mgr->registerDevice(address, kInfo.part, bus);
  i2cResetGracePeriod(address, bus);
  return true;
}

#if ENABLE_IMU_KNOCK_DETECTOR
bool motionBackendRead(uint8_t bus, uint8_t address, uint8_t, MotionSample& out) {
  TwoWire* wire = wireFor(bus);
  if (!wire) return false;
  const uint32_t nowMs = millis();
  const bool full = bno055FullDue(sFast.haveFull, sFast.lastFullMs, nowMs);
  // One transaction: the accelerometer and gravity (12 bytes), with the
  // whole block (ACC_DATA is its first 6 bytes) and the status when due.
  uint8_t data[kBno055DataBytes];
  uint8_t grav[kBno055VectorBytes];
  uint8_t status[kBno055StatusBytes];
  const bool ok = i2cTaskWithTimeout(bus, address, kClockHz, kFastReadLockMs, [&]() -> bool {
    return readRegs(*wire, address, kBno055RegAccelData, data, full ? kBno055DataBytes : kBno055VectorBytes) &&
           readRegs(*wire, address, kBno055RegGravityData, grav, sizeof(grav)) &&
           (!full || readRegs(*wire, address, kBno055RegTemp, status, sizeof(status)));
  });
  if (!ok) {
    KnockFeedRead failed;
    knockFeedRecord(failed, nullptr, 0);
    return false;
  }
  const uint32_t readUs = (uint32_t)esp_timer_get_time();
  if (full) {
    sFast.full = bno055Decode(data, status);
    sFast.haveFull = true;
    sFast.lastFullMs = nowMs;
  }
  float accel[3], gravity[3];
  bno055DecodeVector(data, accel);
  bno055DecodeVector(grav, gravity);
  if (bno055GravityUp(gravity, sFast.up)) sFast.haveUp = true;

  // The knock detector: this sample in mg at its read time.
  float mg[3];
  bno055AccelToMg(accel, mg);
  knock::Feed& feed = knockFeed();
  knock::Event ev;
  const uint8_t ended = feed.add(knockFeedConfig(), mg, feed.clock.at(readUs), ev) ? 1 : 0;
  KnockFeedRead read;
  read.ok = true;
  read.samples = 1;
  read.haveLast = true;
  for (int i = 0; i < 3; ++i) read.lastMg[i] = (int16_t)(mg[i] > 32767 ? 32767 : mg[i] < -32767 ? -32767 : mg[i]);
  knockFeedRecord(read, &ev, ended);

  const Bno055Reading& r = sFast.full;
  for (int i = 0; i < 3; ++i) {
    out.accel[i] = accel[i];
    out.gyro[i] = r.gyro[i];
    out.mag[i] = r.mag[i];
    out.gravity[i] = sFast.up[i];
  }
  out.yaw = r.yaw;
  out.pitch = r.pitch;
  out.roll = r.roll;
  out.temp = (float)r.temp;
  out.cal = r.cal;
  out.hasMag = out.hasOrientation = out.hasHeading = out.hasCal = true;
  out.hasGravity = true;
  out.hasHwTap = true;
  out.hwTap = knockFeedTakeTap(out.hwTapKind, out.hwTapVec);
  // Nothing to publish until the fusion gives gravity (it reads zeros just
  // after the start).
  out.ready = sFast.haveUp;
  return true;
}
#else
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
#endif

// Nothing to release: the backend keeps no object. The chip keeps fusing
// until the next start resets it.
void motionBackendStop() {}

// The BNO055 has no tap engine of its own; in a build with IMU input the
// software knock detector finds the taps (its tuning and trace:
// HAL_Motion_KnockFeed.cpp).
void motionBackendSetTapTuning(long, long, long) {}
#if !ENABLE_IMU_KNOCK_DETECTOR
void motionBackendSetKnockTuning(long, long, long) {}
void motionBackendSetKnockSide(long, long) {}
bool motionBackendKnockTrace(knock::Trace&, knock::Config&) { return false; }
void motionBackendKnockTraceClear() {}
#endif

// The chip fuses nine axes itself: a heading from its magnetometer, its own
// calibration (the reading's cal byte), no free-fall engine. Taps: the
// firmware's knock detector, in a build with IMU input.
void motionBackendStatus(MotionBackendStatus& out) {
  out = MotionBackendStatus{};
  out.hasMag = out.hasHeading = true;
#if ENABLE_IMU_KNOCK_DETECTOR
  out.hasHwTap = true;
  out.tapsInSoftware = true;  // its only tap detection
  out.knockHz = kBno055FastHz;
#endif
}

#endif  // ENABLE_IMU_BNO055
