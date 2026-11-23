// HAL_Motion_LSM6DSOXCore.h - the Adafruit 4517 backend's pure parts: its two
// chips' addresses, registers and configuration bytes, the bounded start
// sequence, the sample decode and the on-chip tap / free-fall flags.
//
// The Adafruit 4517 board carries an ST LSM6DSOX (accelerometer + gyroscope,
// 0x6A or 0x6B) and an ST LIS3MDL (magnetometer, 0x1C or 0x1E) on one STEMMA
// QT bus. HAL_Motion_LSM6DSOX.cpp runs the sequence over the I2C manager (each
// register access its own short transaction, every wait outside the bus lock);
// the host test runs it over two scripted chips (test_motion_lsm6dsox_core.cpp).
// Pure header: no Arduino, FreeRTOS or ESP-IDF dependency.
//
// Register addresses, bit positions and scale factors are from the ST
// datasheets: LSM6DSOX (DS12814, register map section 9) and LIS3MDL
// (DocID024204, section 7). The host test pins every configuration byte to
// the named bit fields below.
#pragma once

#include <stdint.h>
#include <stdio.h>

#include "HAL_MotionCore.h"

// ---- Addresses -------------------------------------------------------------------
// LSM6DSOX: SDO/SA0 low 0x6A (the board's default), high 0x6B.
// LIS3MDL: SDO/SA1 low 0x1C, high 0x1E (the board's default is 0x1C).
constexpr uint8_t kLsm6dsoxAddressDefault = 0x6A;
constexpr uint8_t kLsm6dsoxAddressAlt = 0x6B;
constexpr uint8_t kLis3mdlAddressDefault = 0x1C;
constexpr uint8_t kLis3mdlAddressAlt = 0x1E;
#define LSM6DSOX_ADDRESS_OPTIONS "106|0x6A (default),107|0x6B (SA0 high)"
#define LIS3MDL_ADDRESS_OPTIONS "28|0x1C (default),30|0x1E (SA1 high)"

constexpr bool lsm6dsoxAddressValid(long address) {
  return address == kLsm6dsoxAddressDefault || address == kLsm6dsoxAddressAlt;
}
constexpr bool lis3mdlAddressValid(long address) {
  return address == kLis3mdlAddressDefault || address == kLis3mdlAddressAlt;
}
// The address the backend opens for a saved value (the default for any other).
constexpr uint8_t lsm6dsoxAddressFromSetting(long saved) {
  return lsm6dsoxAddressValid(saved) ? static_cast<uint8_t>(saved) : kLsm6dsoxAddressDefault;
}
constexpr uint8_t lis3mdlAddressFromSetting(long saved) {
  return lis3mdlAddressValid(saved) ? static_cast<uint8_t>(saved) : kLis3mdlAddressDefault;
}

// ---- LSM6DSOX registers ----------------------------------------------------------
constexpr uint8_t kLsm6dsoxRegWhoAmI = 0x0F;
constexpr uint8_t kLsm6dsoxRegCtrl1Xl = 0x10;   // CTRL1_XL, CTRL2_G, CTRL3_C follow
constexpr uint8_t kLsm6dsoxRegCtrl3C = 0x12;
constexpr uint8_t kLsm6dsoxRegCtrl9Xl = 0x18;
constexpr uint8_t kLsm6dsoxRegAllIntSrc = 0x1A;
constexpr uint8_t kLsm6dsoxRegOutTemp = 0x20;   // OUT_TEMP, OUTX/Y/Z_G, OUTX/Y/Z_A: 14 bytes
constexpr uint8_t kLsm6dsoxRegTapCfg0 = 0x56;   // TAP_CFG0 .. MD1_CFG (0x5E): 9 bytes
constexpr uint8_t kLsm6dsoxWhoAmI = 0x6C;
constexpr uint8_t kLsm6dsoxDataBytes = 14;

// CTRL1_XL: ODR_XL[7:4], FS_XL[3:2] (00 2 g, 01 16 g, 10 4 g, 11 8 g), LPF2_XL_EN[1].
constexpr uint8_t kLsm6dsoxOdr104Hz = 0x4;
constexpr uint8_t kLsm6dsoxOdr416Hz = 0x6;
constexpr uint8_t kLsm6dsoxFsXl4g = 0x2;
// CTRL2_G: ODR_G[7:4], FS_G[3:2] (00 250, 01 500, 10 1000, 11 2000 dps), FS_125[1].
constexpr uint8_t kLsm6dsoxFsG500 = 0x1;
// CTRL3_C bits.
constexpr uint8_t kLsm6dsoxCtrl3Boot = 0x80;
constexpr uint8_t kLsm6dsoxCtrl3Bdu = 0x40;     // block data update: a burst reads one sample
constexpr uint8_t kLsm6dsoxCtrl3IfInc = 0x04;   // register address auto-increment
constexpr uint8_t kLsm6dsoxCtrl3SwReset = 0x01;
// CTRL9_XL: DEN_X/Y/Z set at reset (0xE0); I3C_DISABLE is bit 1.
constexpr uint8_t kLsm6dsoxCtrl9Reset = 0xE0;
constexpr uint8_t kLsm6dsoxCtrl9I3cDisable = 0x02;
// TAP_CFG0 bits.
constexpr uint8_t kLsm6dsoxTapIntClrOnRead = 0x40;
constexpr uint8_t kLsm6dsoxTapXEn = 0x08, kLsm6dsoxTapYEn = 0x04, kLsm6dsoxTapZEn = 0x02;
constexpr uint8_t kLsm6dsoxTapLir = 0x01;       // latched interrupt sources
// TAP_CFG2 bit 7: the basic interrupt engines (tap, free fall, 6D) run.
constexpr uint8_t kLsm6dsoxInterruptsEnable = 0x80;
// WAKE_UP_THS bit 7: 0 = single tap only (the double-tap engine delays singles).
constexpr uint8_t kLsm6dsoxSingleDoubleTap = 0x80;
// MD1_CFG: INT1_SINGLE_TAP bit 6, INT1_FF bit 4. The board's INT1 pin is not
// on STEMMA QT; routing the events there keeps them latched as the datasheet
// describes latched mode, and ALL_INT_SRC is polled instead.
constexpr uint8_t kLsm6dsoxInt1SingleTap = 0x40;
constexpr uint8_t kLsm6dsoxInt1Ff = 0x10;
// ALL_INT_SRC bits (read clears the latched sources: LIR + INT_CLR_ON_READ).
constexpr uint8_t kLsm6dsoxSrcFreefall = 0x01;  // FF_IA
constexpr uint8_t kLsm6dsoxSrcSingleTap = 0x04; // SINGLE_TAP

// Tap: threshold per axis in FS_XL/32 steps (4 g: 125 mg each), shock and
// quiet windows in INT_DUR2 (at 416 Hz: SHOCK 2 = 8/ODR each, about 38 ms;
// QUIET 1 = 4/ODR, about 10 ms). Tuned for a knock on a desk sign; check on
// the bench.
constexpr uint8_t kLsm6dsoxTapThreshold = 8;    // 1.0 g above the slope
constexpr uint8_t kLsm6dsoxTapShock = 2;
constexpr uint8_t kLsm6dsoxTapQuiet = 1;
// Free fall: FF_THS 001 = 219 mg (the software detector uses 2 m/s^2, about
// 204 mg); FF_DUR in 1/ODR steps, 6 bits split over WAKE_UP_DUR[7] and
// FREE_FALL[7:3]: 62 at 416 Hz is about 150 ms, the software detector's wait.
constexpr uint8_t kLsm6dsoxFfThs219mg = 0x1;
constexpr uint8_t kLsm6dsoxFfDuration = 62;

// Scale factors (datasheet table 3): 4 g 0.122 mg/LSB, 500 dps 17.50 mdps/LSB,
// temperature 256 LSB/degree with 0 = 25 C.
constexpr float kLsm6dsoxAccelMgPerLsb = 0.122f;
constexpr float kLsm6dsoxGyroMdpsPerLsb = 17.50f;
constexpr float kMotionStandardGravity = 9.80665f;

// ---- LIS3MDL registers -----------------------------------------------------------
constexpr uint8_t kLis3mdlRegWhoAmI = 0x0F;
constexpr uint8_t kLis3mdlRegCtrl1 = 0x20;      // CTRL_REG1 .. CTRL_REG5: 5 bytes
constexpr uint8_t kLis3mdlRegCtrl2 = 0x21;
constexpr uint8_t kLis3mdlRegOutX = 0x28;       // OUT_X/Y/Z: 6 bytes
constexpr uint8_t kLis3mdlAutoIncrement = 0x80; // sub-address MSB: multi-byte access
constexpr uint8_t kLis3mdlWhoAmI = 0x3D;
constexpr uint8_t kLis3mdlDataBytes = 6;
// CTRL_REG1: TEMP_EN[7], OM[6:5] (11 ultra-high performance X/Y), DO[4:2] (111 80 Hz).
constexpr uint8_t kLis3mdlOmUltraHigh = 0x3;
constexpr uint8_t kLis3mdlDo80Hz = 0x7;
// CTRL_REG2: FS[6:5] (00 4 gauss), REBOOT[3], SOFT_RST[2].
constexpr uint8_t kLis3mdlFs4Gauss = 0x0;
constexpr uint8_t kLis3mdlCtrl2SoftReset = 0x04;
// CTRL_REG3: MD[1:0] 00 continuous conversion (03 power-down at reset).
constexpr uint8_t kLis3mdlModeContinuous = 0x0;
// CTRL_REG4: OMZ[3:2] (11 ultra-high performance Z).
// CTRL_REG5: BDU[6].
constexpr uint8_t kLis3mdlCtrl5Bdu = 0x40;
// 4 gauss: 6842 LSB/gauss; 1 gauss = 100 uT.
constexpr float kLis3mdlLsbPerGauss = 6842.0f;

// ---- Configuration bytes -----------------------------------------------------------
// LSM6DSOX CTRL1_XL..CTRL3_C, written as one burst after the reset. The
// accelerometer runs at 416 Hz for the tap engine (ST's guidance for the
// LSM6DSO family is ODR >= 417 Hz); BDU keeps each 50 Hz burst one coherent
// sample. The gyroscope runs at 104 Hz.
constexpr uint8_t kLsm6dsoxCtrl1Xl = (kLsm6dsoxOdr416Hz << 4) | (kLsm6dsoxFsXl4g << 2);
constexpr uint8_t kLsm6dsoxCtrl2G = (kLsm6dsoxOdr104Hz << 4) | (kLsm6dsoxFsG500 << 2);
constexpr uint8_t kLsm6dsoxCtrl3C = kLsm6dsoxCtrl3Bdu | kLsm6dsoxCtrl3IfInc;
constexpr uint8_t kLsm6dsoxCtrlBurst[3] = {kLsm6dsoxCtrl1Xl, kLsm6dsoxCtrl2G, kLsm6dsoxCtrl3C};
constexpr uint8_t kLsm6dsoxCtrl9 = kLsm6dsoxCtrl9Reset | kLsm6dsoxCtrl9I3cDisable;
// TAP_CFG0 (0x56) .. MD1_CFG (0x5E): single tap on all three axes and free
// fall, latched (LIR) and cleared by reading ALL_INT_SRC (INT_CLR_ON_READ).
constexpr uint8_t kLsm6dsoxEventBurst[9] = {
    kLsm6dsoxTapIntClrOnRead | kLsm6dsoxTapXEn | kLsm6dsoxTapYEn | kLsm6dsoxTapZEn | kLsm6dsoxTapLir,  // TAP_CFG0
    kLsm6dsoxTapThreshold,                                       // TAP_CFG1: priority X>Y>Z, X threshold
    kLsm6dsoxInterruptsEnable | kLsm6dsoxTapThreshold,           // TAP_CFG2: engines on, Y threshold
    kLsm6dsoxTapThreshold,                                       // TAP_THS_6D: Z threshold, 6D off
    (kLsm6dsoxTapQuiet << 2) | kLsm6dsoxTapShock,                // INT_DUR2 (DUR: double tap only)
    0x00,                                                        // WAKE_UP_THS: single tap only
    static_cast<uint8_t>((kLsm6dsoxFfDuration >> 5) << 7),       // WAKE_UP_DUR: FF_DUR[5]
    static_cast<uint8_t>(((kLsm6dsoxFfDuration & 0x1F) << 3) | kLsm6dsoxFfThs219mg),  // FREE_FALL
    kLsm6dsoxInt1SingleTap | kLsm6dsoxInt1Ff,                    // MD1_CFG
};
// LIS3MDL CTRL_REG1..CTRL_REG5 as one burst: ultra-high performance on all
// axes at 80 Hz, 4 gauss, continuous, BDU.
constexpr uint8_t kLis3mdlCtrlBurst[5] = {
    (kLis3mdlOmUltraHigh << 5) | (kLis3mdlDo80Hz << 2),  // CTRL_REG1
    kLis3mdlFs4Gauss << 5,                               // CTRL_REG2
    kLis3mdlModeContinuous,                              // CTRL_REG3
    kLis3mdlOmUltraHigh << 2,                            // CTRL_REG4 (OMZ)
    kLis3mdlCtrl5Bdu,                                    // CTRL_REG5
};

// ---- Timing ------------------------------------------------------------------------
// Both chips reset in well under 10 ms (the LSM6DSOX's SW_RESET in about
// 50 us); the wait sleeps with the bus released, then the reset bits are
// polled a bounded number of times.
constexpr uint32_t kLsm4517ResetWaitMs = 10;
constexpr uint32_t kLsm4517PollEveryMs = 5;
constexpr uint32_t kLsm4517RetryMs = 5;
// One register access, worst case: the lock wait, then two transfers (the
// sub-address, then the data), each given up on after the manager's timeout.
constexpr uint32_t kLsm4517StepLockMs = 50;
constexpr uint32_t kLsm4517TransferTimeoutMs = 100;  // I2C_WIRE_IO_TIMEOUT_MS (System_I2C_Manager.cpp)
constexpr uint32_t kLsm4517IoWorstMs = kLsm4517StepLockMs + 2 * kLsm4517TransferTimeoutMs;
constexpr uint32_t kLsm4517InitDeadlineMs = kMotionInitBudgetMs;
// The sequence: two identity reads, two reset writes, the reset wait, two
// reset polls, four configuration writes and two read-backs.
constexpr uint32_t kLsm4517Accesses = 12;
static_assert(kLsm4517Accesses * kLsm4517IoWorstMs + kLsm4517ResetWaitMs <= kLsm4517InitDeadlineMs,
              "the start must fit its deadline with every access at its worst case");

// ---- Start sequence ------------------------------------------------------------------
// Io: one register access per call, each its own short bus transaction.
//   bool read(uint8_t address, uint8_t reg, uint8_t* out, uint8_t len);   false: no answer / short read
//   bool write(uint8_t address, uint8_t reg, const uint8_t* values, uint8_t len);
//   bool busy();        the last access never reached the chip (bus lock timed out)
//   void sleepMs(uint32_t ms);                           with the bus lock released
//   uint32_t nowMs();
// Checks both chips' identity, resets both, writes the configuration above
// and reads it back. Returns true with both chips measuring; false with
// "Error: <reason>" in why. No access or wait starts unless it ends (at its
// worst case, with the accesses still to come) inside kLsm4517InitDeadlineMs.
template <typename Io>
inline bool lsm4517StartSequence(Io& io, uint8_t bus, uint8_t accelAddress, uint8_t magAddress,
                                 char* why, size_t whyLen) {
  const uint32_t start = io.nowMs();
  auto elapsed = [&]() -> uint32_t { return io.nowMs() - start; };
  // Room for `ms` plus `after` accesses at their worst case.
  auto fits = [&](uint32_t ms, uint32_t after) {
    return elapsed() + ms + after * kLsm4517IoWorstMs <= kLsm4517InitDeadlineMs;
  };
  const bool hasWhy = why && whyLen;
  auto chipName = [&](uint8_t address) { return address == accelAddress ? "LSM6DSOX" : "LIS3MDL"; };
  auto busyWhy = [&](const char* what) {
    if (hasWhy) snprintf(why, whyLen, "Error: I2C bus %u stayed busy, so the Adafruit 4517 could not be started "
                         "(%s). Run 'openimu' again.", (unsigned)bus, what);
    return false;
  };
  auto lateWhy = [&](const char* what) {
    if (hasWhy) snprintf(why, whyLen, "Error: The Adafruit 4517 on bus %u did not finish starting in time (%s). "
                         "Run 'openimu' again.", (unsigned)bus, what);
    return false;
  };
  auto goneWhy = [&](uint8_t address, const char* what) {
    if (hasWhy) snprintf(why, whyLen, "Error: The %s at 0x%02X on bus %u stopped answering (%s). Check its "
                         "wiring, then 'openimu'.", chipName(address), (unsigned)address, (unsigned)bus, what);
    return false;
  };
  // One access, tried again while the deadline leaves room for it and for
  // the `after` accesses that follow. The access is a callable returning bool.
  auto attempt = [&](auto&& access, uint8_t address, const char* what, uint32_t after) {
    if (!fits(kLsm4517IoWorstMs, after)) return lateWhy(what);
    while (true) {
      if (access()) return true;
      if (!fits(kLsm4517RetryMs + kLsm4517IoWorstMs, after)) break;
      io.sleepMs(kLsm4517RetryMs);
    }
    if (io.busy()) return busyWhy(what);
    return goneWhy(address, what);
  };
  // The identity read is tried again only while the bus is busy: a chip that
  // does not answer is not there.
  auto identify = [&](uint8_t address, uint8_t reg, uint8_t expected, const char* setting,
                      const char* role, uint32_t after) {
    if (!fits(kLsm4517IoWorstMs, after)) return lateWhy("chip id");
    uint8_t id = 0;
    bool answered = false;
    while (true) {
      answered = io.read(address, reg, &id, 1);
      if (answered || !io.busy()) break;
      if (!fits(kLsm4517RetryMs + kLsm4517IoWorstMs, after)) return busyWhy("chip id");
      io.sleepMs(kLsm4517RetryMs);
    }
    if (!answered) {
      if (hasWhy) snprintf(why, whyLen, "Error: No %s answers at 0x%02X on bus %u (the Adafruit 4517's %s). "
                           "Check the wiring, 'imubus' and '%s', then 'openimu'.", chipName(address),
                           (unsigned)address, (unsigned)bus, role, setting);
      return false;
    }
    if (id != expected) {
      if (hasWhy) snprintf(why, whyLen, "Error: The device at 0x%02X on bus %u is not an %s (WHO_AM_I 0x%02X, "
                           "expected 0x%02X). Check '%s'.", (unsigned)address, (unsigned)bus, chipName(address),
                           (unsigned)id, (unsigned)expected, setting);
      return false;
    }
    return true;
  };
  auto write = [&](uint8_t address, uint8_t reg, const uint8_t* values, uint8_t len, const char* what,
                   uint32_t after) {
    return attempt([&]() { return io.write(address, reg, values, len); }, address, what, after);
  };
  // Poll a reset bit until the chip clears it (a bounded number of times).
  auto waitReset = [&](uint8_t address, uint8_t reg, uint8_t bit, uint32_t after) {
    while (true) {
      uint8_t value = 0;
      if (!attempt([&]() { return io.read(address, reg, &value, 1); }, address, "reset", after)) return false;
      if (!(value & bit)) return true;
      if (!fits(kLsm4517PollEveryMs + kLsm4517IoWorstMs, after)) {
        if (hasWhy) snprintf(why, whyLen, "Error: The %s at 0x%02X on bus %u did not finish its reset. Check its "
                             "power, then 'openimu'.", chipName(address), (unsigned)address, (unsigned)bus);
        return false;
      }
      io.sleepMs(kLsm4517PollEveryMs);
    }
  };
  auto readBack = [&](uint8_t address, uint8_t reg, const uint8_t* expected, uint8_t len, uint32_t after) {
    uint8_t got[8] = {};
    if (!attempt([&]() { return io.read(address, reg, got, len); }, address, "read-back", after)) return false;
    for (uint8_t i = 0; i < len; ++i) {
      if (got[i] != expected[i]) {
        if (hasWhy) snprintf(why, whyLen, "Error: The %s at 0x%02X on bus %u did not keep its settings (register "
                             "0x%02X reads 0x%02X, expected 0x%02X). Run 'openimu' again.", chipName(address),
                             (unsigned)address, (unsigned)bus, (unsigned)((reg & 0x7F) + i), (unsigned)got[i],
                             (unsigned)expected[i]);
        return false;
      }
    }
    return true;
  };

  if (!identify(accelAddress, kLsm6dsoxRegWhoAmI, kLsm6dsoxWhoAmI, "imuaddress", "accelerometer and gyroscope", 11))
    return false;
  if (!identify(magAddress, kLis3mdlRegWhoAmI, kLis3mdlWhoAmI, "imumagaddress", "magnetometer", 10)) return false;

  const uint8_t accelReset = kLsm6dsoxCtrl3SwReset;
  const uint8_t magReset = kLis3mdlCtrl2SoftReset;
  if (!write(accelAddress, kLsm6dsoxRegCtrl3C, &accelReset, 1, "reset", 9)) return false;
  if (!write(magAddress, kLis3mdlRegCtrl2, &magReset, 1, "reset", 8)) return false;
  if (!fits(kLsm4517ResetWaitMs, 8)) return lateWhy("reset");
  io.sleepMs(kLsm4517ResetWaitMs);
  if (!waitReset(accelAddress, kLsm6dsoxRegCtrl3C, kLsm6dsoxCtrl3SwReset, 7)) return false;
  if (!waitReset(magAddress, kLis3mdlRegCtrl2, kLis3mdlCtrl2SoftReset, 6)) return false;

  if (!write(accelAddress, kLsm6dsoxRegCtrl1Xl, kLsm6dsoxCtrlBurst, sizeof(kLsm6dsoxCtrlBurst),
             "accelerometer and gyroscope", 5)) return false;
  if (!write(accelAddress, kLsm6dsoxRegCtrl9Xl, &kLsm6dsoxCtrl9, 1, "I3C off", 4)) return false;
  if (!write(accelAddress, kLsm6dsoxRegTapCfg0, kLsm6dsoxEventBurst, sizeof(kLsm6dsoxEventBurst),
             "tap and free fall", 3)) return false;
  if (!write(magAddress, kLis3mdlRegCtrl1 | kLis3mdlAutoIncrement, kLis3mdlCtrlBurst, sizeof(kLis3mdlCtrlBurst),
             "magnetometer", 2)) return false;

  if (!readBack(accelAddress, kLsm6dsoxRegCtrl1Xl, kLsm6dsoxCtrlBurst, sizeof(kLsm6dsoxCtrlBurst), 1)) return false;
  return readBack(magAddress, kLis3mdlRegCtrl1 | kLis3mdlAutoIncrement, kLis3mdlCtrlBurst,
                  sizeof(kLis3mdlCtrlBurst), 0);
}

// ---- Sample decode ---------------------------------------------------------------------
inline int16_t lsm4517Le16(const uint8_t* bytes) {
  return static_cast<int16_t>(static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8));
}

// One reading in the ImuCache units, in the LSM6DSOX frame (right-handed, +Z
// out of the chip's top: lying flat face up reads accel z = +9.81).
struct Lsm4517Reading {
  float accel[3];  // m/s^2
  float gyro[3];   // rad/s (before the bias is taken off)
  float mag[3];    // uT, in the LSM6DSOX frame (lis3mdlToAccelFrame)
  float temp;      // Celsius
  bool tap;        // the chip latched a single tap since the last read
  bool freefall;   // the chip latched a free fall since the last read
};

// data: the 14 bytes from OUT_TEMP (0x20); intSrc: ALL_INT_SRC.
inline void lsm6dsoxDecode(const uint8_t* data, uint8_t intSrc, Lsm4517Reading& out) {
  out.temp = lsm4517Le16(data) / 256.0f + 25.0f;
  const float gyroScale = kLsm6dsoxGyroMdpsPerLsb / 1000.0f * kMotionDegToRad;
  const float accelScale = kLsm6dsoxAccelMgPerLsb / 1000.0f * kMotionStandardGravity;
  for (int i = 0; i < 3; ++i) {
    out.gyro[i] = lsm4517Le16(data + 2 + 2 * i) * gyroScale;
    out.accel[i] = lsm4517Le16(data + 8 + 2 * i) * accelScale;
  }
  out.tap = (intSrc & kLsm6dsoxSrcSingleTap) != 0;
  out.freefall = (intSrc & kLsm6dsoxSrcFreefall) != 0;
}

// The LIS3MDL's axes in the LSM6DSOX frame (the magnetometer pipeline is raw
// -> this map -> hard/soft-iron correction -> fusion, the same order for a
// calibration sweep and at runtime). Taken as aligned on the 4517 board.
// TODO(bench): confirm against the board silkscreen with a compass before the
// magnetometer feeds the fusion.
constexpr int8_t kLis3mdlAxisMap[3] = {0, 1, 2};   // accel-frame axis i <- mag axis map[i]
constexpr int8_t kLis3mdlAxisSign[3] = {1, 1, 1};

// data: the 6 bytes from OUT_X (0x28).
inline void lis3mdlDecode(const uint8_t* data, Lsm4517Reading& out) {
  float raw[3];
  for (int i = 0; i < 3; ++i) raw[i] = lsm4517Le16(data + 2 * i) * 100.0f / kLis3mdlLsbPerGauss;
  for (int i = 0; i < 3; ++i) out.mag[i] = kLis3mdlAxisSign[i] * raw[kLis3mdlAxisMap[i]];
}
