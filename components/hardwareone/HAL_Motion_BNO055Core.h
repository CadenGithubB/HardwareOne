// HAL_Motion_BNO055Core.h - the BNO055 backend's pure parts: its addresses,
// registers, the bounded start sequence and the sample decode.
//
// HAL_Motion_BNO055.cpp runs the sequence over the I2C manager (each register
// access its own short transaction, every wait outside the bus lock); the
// host test runs it over a scripted chip (test_motion_bno055_core.cpp).
// Pure header: no Arduino, FreeRTOS or ESP-IDF dependency.
//
// Register values are from the Bosch BNO055 datasheet (BST-BNO055-DS000):
// page 0 map, OPR_MODE, SYS_TRIGGER and the default UNIT_SEL (m/s^2, dps,
// degrees, Celsius), which the sequence leaves as the chip resets it.
#pragma once

#include <stdint.h>
#include <stdio.h>

#include "HAL_MotionCore.h"

// ---- Addresses: 0x28, or 0x29 with the ADR pin high ------------------------
constexpr uint8_t kBno055AddressDefault = 0x28;
constexpr uint8_t kBno055AddressAlt = 0x29;
#define BNO055_ADDRESS_OPTIONS "40|0x28 (default),41|0x29 (ADR high)"

constexpr bool bno055AddressValid(long address) {
  return address == kBno055AddressDefault || address == kBno055AddressAlt;
}
// The address the backend opens for a saved value (the default for any other).
constexpr uint8_t bno055AddressFromSetting(long saved) {
  return bno055AddressValid(saved) ? static_cast<uint8_t>(saved) : kBno055AddressDefault;
}

// ---- Registers (page 0) -----------------------------------------------------
constexpr uint8_t kBno055RegChipId = 0x00;
constexpr uint8_t kBno055RegPageId = 0x07;
constexpr uint8_t kBno055RegAccelData = 0x08;  // accel, mag, gyro, euler: 24 bytes from here
// GRV_DATA_X_LSB: the fusion's gravity vector, x/y/z little-endian int16 in
// the accelerometer's unit (after QUA_DATA 0x20..0x27 and LIA_DATA
// 0x28..0x2D, before TEMP).
constexpr uint8_t kBno055RegGravityData = 0x2E;
constexpr uint8_t kBno055RegTemp = 0x34;       // temp, then CALIB_STAT at 0x35
constexpr uint8_t kBno055RegOprMode = 0x3D;
constexpr uint8_t kBno055RegPwrMode = 0x3E;
constexpr uint8_t kBno055RegSysTrigger = 0x3F;

constexpr uint8_t kBno055ChipId = 0xA0;
constexpr uint8_t kBno055ModeConfig = 0x00;
constexpr uint8_t kBno055ModeNdof = 0x0C;      // 9-axis fusion, absolute orientation
constexpr uint8_t kBno055PowerNormal = 0x00;
constexpr uint8_t kBno055TriggerReset = 0x20;  // RST_SYS
constexpr uint8_t kBno055TriggerExtClock = 0x80;  // CLK_SEL: the board's 32 kHz crystal

constexpr uint8_t kBno055DataBytes = 24;       // 0x08..0x1F
constexpr uint8_t kBno055StatusBytes = 2;      // 0x34..0x35
constexpr uint8_t kBno055VectorBytes = 6;      // one x/y/z vector: ACC_DATA 0x08..0x0D, GRV_DATA 0x2E..0x33

// The default UNIT_SEL (0x3B = 0x00, left as the reset sets it): acceleration
// and gravity in m/s^2, 1 m/s^2 = 100 LSB (datasheet, acceleration and
// gravity vector data representation).
constexpr float kBno055AccelLsbPerMs2 = 100.0f;
constexpr float kBno055StandardGravity = 9.80665f;  // m/s^2 per g

// ---- Fast sampling (a build with IMU input) ------------------------------------
// In NDOF the fusion outputs at 100 Hz, and the chip sets its accelerometer
// itself (4 g, 62.5 Hz bandwidth; datasheet, fusion modes): the knock
// detector reads ACC_DATA and GRV_DATA every 10 ms,
// and the full block (mag, gyro, Euler, temperature, CALIB_STAT) every
// kBno055FullEveryMs, its last values standing in between.
constexpr uint16_t kBno055FastPeriodMs = 10;
constexpr uint16_t kBno055FastHz = 100;
constexpr uint32_t kBno055FullEveryMs = 100;

// ---- Timing --------------------------------------------------------------------
// Power-on reset takes 650 ms (datasheet table 0-2); Adafruit's driver allows
// 850 ms for a first answer. Mode changes: 19 ms into CONFIG, 7 ms out of it.
constexpr uint32_t kBno055BootWaitMs = 900;    // first answer after power-up
constexpr uint32_t kBno055ResetWaitMs = 650;   // after RST_SYS, before the first poll
constexpr uint32_t kBno055PollEveryMs = 20;    // chip-id polls while it boots
constexpr uint32_t kBno055ConfigModeMs = 25;
constexpr uint32_t kBno055SettleMs = 50;       // after the reset answers, before configuring
constexpr uint32_t kBno055ClockMs = 10;
constexpr uint32_t kBno055FusionModeMs = 20;
// One register access, worst case: the backend waits up to kBno055StepLockMs
// for the bus, then a read is two transfers (address, then data), each of
// which the I2C manager gives up on after kBno055TransferTimeoutMs.
constexpr uint32_t kBno055StepLockMs = 50;
constexpr uint32_t kBno055TransferTimeoutMs = 100;  // I2C_WIRE_IO_TIMEOUT_MS (System_I2C_Manager.cpp)
constexpr uint32_t kBno055IoWorstMs = kBno055StepLockMs + 2 * kBno055TransferTimeoutMs;
// A write the chip did not take (or that found the bus busy) is tried again
// after this, while the deadline leaves room for it and for what follows.
constexpr uint32_t kBno055StepRetryMs = 10;
// The whole start ends within this, every access at its worst case included
// (HAL_MotionCore.h).
constexpr uint32_t kBno055InitDeadlineMs = kMotionInitBudgetMs;
// Worst case of what follows each step (the reserve it leaves when it
// retries): the configuration tail after the reset poll is the settle wait,
// power mode, page, crystal, clock wait, fusion mode, fusion wait and the
// mode read-back.
constexpr uint32_t kBno055AfterNdofMs = kBno055FusionModeMs + kBno055IoWorstMs;
constexpr uint32_t kBno055AfterCrystalMs = kBno055ClockMs + kBno055IoWorstMs + kBno055AfterNdofMs;
constexpr uint32_t kBno055AfterPageMs = kBno055IoWorstMs + kBno055AfterCrystalMs;
constexpr uint32_t kBno055AfterPowerMs = kBno055IoWorstMs + kBno055AfterPageMs;
constexpr uint32_t kBno055TailMs = kBno055SettleMs + kBno055IoWorstMs + kBno055AfterPowerMs;
// The reset write leaves room for the reset wait, one poll and the tail; the
// config-mode write also for its wait and the reset write.
constexpr uint32_t kBno055AfterResetMs = kBno055ResetWaitMs + kBno055IoWorstMs + kBno055TailMs;
constexpr uint32_t kBno055AfterConfigMs = kBno055ConfigModeMs + kBno055IoWorstMs + kBno055AfterResetMs;
// A chip that answers at the end of its boot window still gets a full start.
static_assert(kBno055BootWaitMs + kBno055IoWorstMs + kBno055AfterConfigMs <= kBno055InitDeadlineMs,
              "the start must fit its deadline with every access at its worst case");

// ---- Start sequence ---------------------------------------------------------------
// Io: one register access per call, each its own short bus transaction.
//   bool read(uint8_t reg, uint8_t* out, uint8_t len);   false: no answer / short read
//   bool write(uint8_t reg, uint8_t value);
//   bool busy();        the last access never reached the chip (bus lock timed out)
//   void sleepMs(uint32_t ms);                           with the bus lock released
//   uint32_t nowMs();
// Replaces Adafruit_BNO055::begin(), whose waits (and an unbounded chip-id
// loop after its reset) ran while the caller held the bus. Returns true with
// the chip fusing in NDOF mode on the external crystal; false with
// "Error: <reason>" in why. No access or wait starts unless it ends (at its
// worst case) inside kBno055InitDeadlineMs.
template <typename Io>
inline bool bno055StartSequence(Io& io, uint8_t bus, uint8_t address, char* why, size_t whyLen) {
  const uint32_t start = io.nowMs();
  auto elapsed = [&]() -> uint32_t { return io.nowMs() - start; };
  auto fits = [&](uint32_t ms) { return elapsed() + ms <= kBno055InitDeadlineMs; };
  const bool hasWhy = why && whyLen;
  auto busyWhy = [&](const char* what) {
    if (hasWhy) snprintf(why, whyLen, "Error: I2C bus %u stayed busy, so the BNO055 at 0x%02X could not be started "
                         "(%s). Run 'openimu' again.", (unsigned)bus, (unsigned)address, what);
    return false;
  };
  auto lateWhy = [&](const char* what) {
    if (hasWhy) snprintf(why, whyLen, "Error: The BNO055 at 0x%02X on bus %u did not finish starting in time (%s). "
                         "Run 'openimu' again.", (unsigned)address, (unsigned)bus, what);
    return false;
  };
  // Poll the chip id until it reads kBno055ChipId or the next poll would end
  // after `untilMs` (since start); a booting chip may not answer, or answer
  // another id, for a while. Returns 1 when it reads the id, 0 when the last
  // poll got no answer, -1 when the last poll answered another id (in *seen),
  // -2 when the bus stayed busy.
  auto waitChipId = [&](uint32_t untilMs, uint8_t* seen) -> int {
    while (true) {
      uint8_t id = 0;
      int last = 0;
      if (io.read(kBno055RegChipId, &id, 1)) {
        if (id == kBno055ChipId) return 1;
        *seen = id;
        last = -1;
      } else if (io.busy()) {
        last = -2;
      }
      if (elapsed() + kBno055PollEveryMs + kBno055IoWorstMs > untilMs) return last;
      io.sleepMs(kBno055PollEveryMs);
    }
  };
  // One register write, tried again while the deadline leaves room for it
  // and for `reserveMs` (the worst case of everything after it).
  auto step = [&](uint8_t reg, uint8_t value, const char* what, uint32_t reserveMs) {
    if (!fits(kBno055IoWorstMs + reserveMs)) return lateWhy(what);
    while (true) {
      if (io.write(reg, value)) return true;
      if (!fits(kBno055StepRetryMs + kBno055IoWorstMs + reserveMs)) break;
      io.sleepMs(kBno055StepRetryMs);
    }
    if (io.busy()) return busyWhy(what);
    if (hasWhy) {
      snprintf(why, whyLen, "Error: The BNO055 at 0x%02X on bus %u stopped answering (%s). Check its "
               "wiring, then 'openimu'.", (unsigned)address, (unsigned)bus, what);
    }
    return false;
  };

  uint8_t seen = 0;
  int answer = waitChipId(kBno055BootWaitMs, &seen);
  if (answer == -2) return busyWhy("chip id");
  if (answer == 0) {
    if (hasWhy) snprintf(why, whyLen, "Error: No BNO055 answers at 0x%02X on bus %u. Check the wiring, 'imubus' and "
                "'imuaddress', then 'openimu'.", (unsigned)address, (unsigned)bus);
    return false;
  }
  if (answer < 0) {
    if (hasWhy) snprintf(why, whyLen, "Error: The device at 0x%02X on bus %u is not a BNO055 (chip id 0x%02X, expected "
                "0x%02X). Check 'imuaddress'.", (unsigned)address, (unsigned)bus, (unsigned)seen,
                (unsigned)kBno055ChipId);
    return false;
  }

  if (!step(kBno055RegOprMode, kBno055ModeConfig, "config mode", kBno055AfterConfigMs)) return false;
  io.sleepMs(kBno055ConfigModeMs);
  if (!step(kBno055RegSysTrigger, kBno055TriggerReset, "reset", kBno055AfterResetMs)) return false;
  io.sleepMs(kBno055ResetWaitMs);
  // The last poll ends with room for the whole tail at its worst case.
  answer = waitChipId(kBno055InitDeadlineMs - kBno055TailMs, &seen);
  if (answer == -2) return busyWhy("chip id after the reset");
  if (answer <= 0) {
    if (hasWhy) snprintf(why, whyLen, "Error: The BNO055 at 0x%02X on bus %u did not come back after its reset. Check its "
                "power and wiring, then 'openimu'.", (unsigned)address, (unsigned)bus);
    return false;
  }
  io.sleepMs(kBno055SettleMs);

  if (!step(kBno055RegPwrMode, kBno055PowerNormal, "power mode", kBno055AfterPowerMs)) return false;
  if (!step(kBno055RegPageId, 0, "register page", kBno055AfterPageMs)) return false;
  if (!step(kBno055RegSysTrigger, kBno055TriggerExtClock, "crystal", kBno055AfterCrystalMs)) return false;
  io.sleepMs(kBno055ClockMs);
  if (!step(kBno055RegOprMode, kBno055ModeNdof, "fusion mode", kBno055AfterNdofMs)) return false;
  io.sleepMs(kBno055FusionModeMs);

  uint8_t mode = 0;
  if (!io.read(kBno055RegOprMode, &mode, 1) || mode != kBno055ModeNdof) {
    if (io.busy()) return busyWhy("mode read-back");
    if (hasWhy) snprintf(why, whyLen, "Error: The BNO055 at 0x%02X on bus %u did not enter its fusion mode. Run "
                "'openimu' again.", (unsigned)address, (unsigned)bus);
    return false;
  }
  return true;
}

// ---- Sample decode -----------------------------------------------------------------
// One reading in the ImuCache units. Euler: the chip's heading becomes yaw,
// its "roll" register (0x1C, +-90 deg) pitch and its "pitch" register (0x1E,
// +-180 deg) roll, the assignment the readings have always had (aerospace
// ranges: pitch +-90, roll +-180).
struct Bno055Reading {
  float accel[3];  // m/s^2 (1 LSB = 0.01)
  float mag[3];    // uT (1 LSB = 1/16)
  float gyro[3];   // rad/s (1 LSB = 1/16 dps)
  float yaw, pitch, roll;  // degrees (1 LSB = 1/16)
  int8_t temp;     // Celsius
  uint8_t cal;     // CALIB_STAT: sys, gyro, accel, mag, two bits each (3 = calibrated)
};

inline int16_t bno055Le16(const uint8_t* bytes) {
  return static_cast<int16_t>(static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8));
}

// One x/y/z vector of the accelerometer's unit (ACC_DATA, GRV_DATA) in m/s^2.
inline void bno055DecodeVector(const uint8_t* six, float out[3]) {
  for (int i = 0; i < 3; ++i) out[i] = bno055Le16(six + 2 * i) / kBno055AccelLsbPerMs2;
}
// m/s^2 to mg, for the knock detector.
inline void bno055AccelToMg(const float ms2[3], float mg[3]) {
  for (int i = 0; i < 3; ++i) mg[i] = ms2[i] * (1000.0f / kBno055StandardGravity);
}
// The gravity vector (m/s^2) as a direction in g (what the accelerometer reads
// at rest, divided by g), when it is one: the fusion reads zeros until it has
// started, and a length far from 1 g is no gravity estimate. False leaves
// `up` unchanged.
inline bool bno055GravityUp(const float grav[3], float up[3]) {
  float n2 = 0;
  for (int i = 0; i < 3; ++i) n2 += grav[i] * grav[i];
  const float g2 = kBno055StandardGravity * kBno055StandardGravity;
  if (!(n2 > 0.25f * g2 && n2 < 4.0f * g2)) return false;
  for (int i = 0; i < 3; ++i) up[i] = grav[i] / kBno055StandardGravity;
  return true;
}
// Whether the full block is due at nowMs: the first sample, then every
// kBno055FullEveryMs (within half a fast period, as the reads jitter).
inline bool bno055FullDue(bool haveFull, uint32_t lastFullMs, uint32_t nowMs) {
  return !haveFull || static_cast<uint32_t>(nowMs - lastFullMs) + kBno055FastPeriodMs / 2 >= kBno055FullEveryMs;
}

// data: the 24 bytes from 0x08; status: the 2 bytes from 0x34.
inline Bno055Reading bno055Decode(const uint8_t* data, const uint8_t* status) {
  Bno055Reading out{};
  for (int i = 0; i < 3; ++i) {
    out.accel[i] = bno055Le16(data + 2 * i) / 100.0f;
    out.mag[i] = bno055Le16(data + 6 + 2 * i) / 16.0f;
    out.gyro[i] = bno055Le16(data + 12 + 2 * i) / 16.0f * kMotionDegToRad;
  }
  out.yaw = bno055Le16(data + 18) / 16.0f;
  out.pitch = bno055Le16(data + 20) / 16.0f;
  out.roll = bno055Le16(data + 22) / 16.0f;
  out.temp = static_cast<int8_t>(status[0]);
  out.cal = status[1];
  return out;
}
