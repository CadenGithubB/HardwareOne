// The Adafruit 4517 backend's pure parts (HAL_Motion_LSM6DSOXCore.h): the
// configuration bytes pinned to the ST datasheet values, the bounded start
// sequence over two scripted chips (order, waits outside the bus lock, one
// reason per failure, the deadline), the sample decode and the address rules.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "HAL_Motion_LSM6DSOXCore.h"

static bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

// ---- Configuration bytes against the datasheets ------------------------------------
static void testConfigurationBytes() {
  // LSM6DSOX CTRL1_XL: ODR_XL 0110 (416 Hz), FS_XL 10 (+-4 g), LPF2_XL_EN (bit 1).
  assert(kLsm6dsoxCtrl1Xl == 0x6A);
  // CTRL2_G: ODR_G 0100 (104 Hz), FS_G 01 (+-500 dps), FS_125 off.
  assert(kLsm6dsoxCtrl2G == 0x44);
  // CTRL3_C: BDU (bit 6) + IF_INC (bit 2); SW_RESET is bit 0, BOOT bit 7.
  assert(kLsm6dsoxCtrl3C == 0x44 && kLsm6dsoxCtrl3SwReset == 0x01 && kLsm6dsoxCtrl3Boot == 0x80);
  assert(kLsm6dsoxCtrlBurst[0] == 0x6A && kLsm6dsoxCtrlBurst[1] == 0x44 && kLsm6dsoxCtrlBurst[2] == 0x44);
  // CTRL9_XL: reset value 0xE0 (DEN_X/Y/Z) plus I3C_DISABLE (bit 1).
  assert(kLsm6dsoxCtrl9 == 0xE2);
  // CTRL8_XL: HPCF_XL 010 (LPF2 at ODR/20, about 21 Hz: under the 50 Hz
  // read's Nyquist limit), HP_SLOPE_XL_EN 0, written with CTRL9_XL.
  assert(kLsm6dsoxCtrl8 == 0x40 && kLsm6dsoxCtrl89Burst[0] == 0x40 && kLsm6dsoxCtrl89Burst[1] == 0xE2);
  assert(kLsm6dsoxRegCtrl8Xl == 0x17 && 416.0f / 20.0f < 25.0f);
  // The sample reads ALL_INT_SRC, WAKE_UP_SRC and TAP_SRC (0x1A..0x1C).
  assert(kLsm6dsoxIntSrcBytes == 3);
  // Power-down at stop: ODR_XL / ODR_G 0000; LIS3MDL CTRL_REG3 (0x22) MD 11.
  assert(kLsm6dsoxPowerDown[0] == 0 && kLsm6dsoxPowerDown[1] == 0 && kLis3mdlRegCtrl3 == 0x22 &&
         kLis3mdlPowerDown == 0x03);
  // The accelerometer runs at >= 416 Hz for the tap engine.
  assert((kLsm6dsoxCtrl1Xl >> 4) >= 0x6);
  // TAP_CFG0: INT_CLR_ON_READ (6) + TAP_X/Y/Z_EN (3..1) + LIR (0).
  assert(kLsm6dsoxEventBurst[0] == 0x4F);
  // TAP_CFG1: priority 000, X threshold 8; TAP_CFG2: INTERRUPTS_ENABLE (7) + Y threshold 8;
  // TAP_THS_6D: D4D off, Z threshold 8.
  assert(kLsm6dsoxEventBurst[1] == 0x05 && kLsm6dsoxEventBurst[2] == 0x85 && kLsm6dsoxEventBurst[3] == 0x05);
  // INT_DUR2: DUR 0, QUIET 01, SHOCK 10.
  assert(kLsm6dsoxEventBurst[4] == 0x06);
  // WAKE_UP_THS: SINGLE_DOUBLE_TAP 0 (single tap only).
  assert(kLsm6dsoxEventBurst[5] == 0x00 && !(kLsm6dsoxEventBurst[5] & kLsm6dsoxSingleDoubleTap));
  // FF_DUR 62 (0b111110): FF_DUR5 in WAKE_UP_DUR bit 7, FF_DUR[4:0] 11110 in
  // FREE_FALL[7:3], FF_THS 001 (219 mg).
  assert(kLsm6dsoxEventBurst[6] == 0x80 && kLsm6dsoxEventBurst[7] == 0xF1);
  const unsigned ffDur = ((kLsm6dsoxEventBurst[6] >> 7) << 5) | (kLsm6dsoxEventBurst[7] >> 3);
  assert(ffDur == 62 && near(ffDur / 416.0f * 1000.0f, 150.0f, 2.0f));
  // MD1_CFG: INT1_SINGLE_TAP (6) + INT1_FF (4).
  assert(kLsm6dsoxEventBurst[8] == 0x50);
  // The burst covers TAP_CFG0 (0x56) .. MD1_CFG (0x5E).
  assert(kLsm6dsoxRegTapCfg0 + sizeof(kLsm6dsoxEventBurst) - 1 == 0x5E);
  // ALL_INT_SRC: FF_IA bit 0, SINGLE_TAP bit 2.
  assert(kLsm6dsoxSrcFreefall == 0x01 && kLsm6dsoxSrcSingleTap == 0x04);
  // Registers.
  assert(kLsm6dsoxRegWhoAmI == 0x0F && kLsm6dsoxWhoAmI == 0x6C && kLsm6dsoxRegCtrl1Xl == 0x10 &&
         kLsm6dsoxRegCtrl3C == 0x12 && kLsm6dsoxRegAllIntSrc == 0x1A &&
         kLsm6dsoxRegOutTemp == 0x20 && kLsm6dsoxDataBytes == 14);

  // LIS3MDL CTRL_REG1: OM 11 (ultra-high X/Y), DO 111 (80 Hz), FAST_ODR off.
  assert(kLis3mdlCtrlBurst[0] == 0x7C);
  // CTRL_REG2: FS 00 (+-4 gauss); SOFT_RST is bit 2.
  assert(kLis3mdlCtrlBurst[1] == 0x00 && kLis3mdlCtrl2SoftReset == 0x04);
  // CTRL_REG3: MD 00 continuous; CTRL_REG4: OMZ 11; CTRL_REG5: BDU (bit 6).
  assert(kLis3mdlCtrlBurst[2] == 0x00 && kLis3mdlCtrlBurst[3] == 0x0C && kLis3mdlCtrlBurst[4] == 0x40);
  assert(kLis3mdlRegWhoAmI == 0x0F && kLis3mdlWhoAmI == 0x3D && kLis3mdlRegCtrl1 == 0x20 &&
         kLis3mdlRegCtrl2 == 0x21 && kLis3mdlRegOutX == 0x28 && kLis3mdlDataBytes == 6);
  // Multi-byte access: the sub-address MSB.
  assert((kLis3mdlRegOutX | kLis3mdlAutoIncrement) == 0xA8 && (kLis3mdlRegCtrl1 | kLis3mdlAutoIncrement) == 0xA0);
}

// ---- Addresses ---------------------------------------------------------------------------
static void testAddresses() {
  assert(lsm6dsoxAddressFromSetting(0x6A) == 0x6A && lsm6dsoxAddressFromSetting(0x6B) == 0x6B);
  assert(lsm6dsoxAddressFromSetting(0x28) == 0x6A && lsm6dsoxAddressFromSetting(0) == 0x6A);
  assert(lis3mdlAddressFromSetting(0x1C) == 0x1C && lis3mdlAddressFromSetting(0x1E) == 0x1E);
  assert(lis3mdlAddressFromSetting(0x1D) == 0x1C && lis3mdlAddressFromSetting(0x6A) == 0x1C);
  assert(lsm6dsoxAddressValid(0x6B) && !lsm6dsoxAddressValid(0x6C));
  assert(lis3mdlAddressValid(0x1E) && !lis3mdlAddressValid(0x1F));
}

// ---- The start over two scripted chips -------------------------------------------------
// Time moves only in sleepMs (the unlocked waits) plus ioMs per access (one
// short transaction each).
struct Bus {
  struct Chip {
    bool present = true;
    uint8_t whoAmIReg = 0x0F;
    uint8_t id = 0;
    std::map<uint8_t, uint8_t> regs;
    uint8_t resetReg = 0, resetBit = 0;
    uint32_t resetClearsAt = 0;   // the reset bit reads set until this time
    uint32_t resetFor = 0;        // how long a reset keeps the bit set
    bool ignoreWritesTo = false;  // the configuration does not stick
    uint8_t ignoredReg = 0;
  };
  uint8_t accelAddress = 0x6A, magAddress = 0x1C;
  Chip accel, mag;
  uint32_t now = 0;
  uint32_t ioMs = 1;
  uint32_t busyUntil = 0;
  bool lastBusy = false;
  uint32_t sleeps = 0;
  std::vector<std::string> log;

  Bus() {
    accel.id = kLsm6dsoxWhoAmI;
    accel.resetReg = kLsm6dsoxRegCtrl3C;
    accel.resetBit = kLsm6dsoxCtrl3SwReset;
    accel.regs[kLsm6dsoxRegCtrl3C] = 0x04;
    mag.id = kLis3mdlWhoAmI;
    mag.resetReg = kLis3mdlRegCtrl2;
    mag.resetBit = kLis3mdlCtrl2SoftReset;
  }
  Chip* at(uint8_t address) {
    if (address == accelAddress) return &accel;
    if (address == magAddress) return &mag;
    return nullptr;
  }
  bool locked() {
    lastBusy = now < busyUntil;
    if (lastBusy) now += kLsm4517StepLockMs;
    return !lastBusy;
  }
  // The LIS3MDL increments only with the sub-address MSB set.
  static uint8_t regAt(const Chip& chip, uint8_t reg, uint8_t i, bool isMag) {
    if (isMag) return (reg & kLis3mdlAutoIncrement) ? (reg & 0x7F) + i : reg;
    (void)chip;
    return reg + i;
  }
  bool read(uint8_t address, uint8_t reg, uint8_t* out, uint8_t len) {
    if (!locked()) return false;
    now += ioMs;
    Chip* chip = at(address);
    if (!chip || !chip->present) return false;
    char line[48];
    snprintf(line, sizeof(line), "r %02X %02X %u", address, reg, len);
    log.push_back(line);
    for (uint8_t i = 0; i < len; ++i) {
      const uint8_t r = regAt(*chip, reg, i, chip == &mag);
      if (r == chip->whoAmIReg) {
        out[i] = chip->id;
      } else {
        uint8_t value = chip->regs[r];
        if (r == chip->resetReg && now < chip->resetClearsAt) value |= chip->resetBit;
        out[i] = value;
      }
    }
    return true;
  }
  bool write(uint8_t address, uint8_t reg, const uint8_t* values, uint8_t len) {
    if (!locked()) return false;
    now += ioMs;
    Chip* chip = at(address);
    if (!chip || !chip->present) return false;
    char line[48];
    snprintf(line, sizeof(line), "w %02X %02X %u", address, reg, len);
    log.push_back(line);
    for (uint8_t i = 0; i < len; ++i) {
      const uint8_t r = regAt(*chip, reg, i, chip == &mag);
      if (r == chip->resetReg && (values[i] & chip->resetBit)) {
        chip->regs.clear();
        if (chip == &accel) chip->regs[kLsm6dsoxRegCtrl3C] = 0x04;
        chip->resetClearsAt = now + chip->resetFor;
        continue;
      }
      if (chip->ignoreWritesTo && r == chip->ignoredReg) continue;
      chip->regs[r] = values[i];
    }
    return true;
  }
  bool busy() const { return lastBusy; }
  void sleepMs(uint32_t ms) {
    now += ms;
    ++sleeps;
    log.push_back("sleep " + std::to_string(ms));
  }
  uint32_t nowMs() { return now; }
};

static bool start(Bus& bus, std::string& why) {
  char text[200] = "";
  const bool ok = lsm4517StartSequence(bus, 1, bus.accelAddress, bus.magAddress, text, sizeof(text));
  why = text;
  return ok;
}

static void testStartConfiguresBothChips() {
  Bus bus;
  std::string why;
  assert(start(bus, why) && why.empty());
  // Order: identities, resets, one unlocked wait, reset polls, configuration, read-backs.
  const std::vector<std::string> expected = {
      "r 6A 0F 1", "r 1C 0F 1", "w 6A 12 1", "w 1C 21 1", "sleep 10", "r 6A 12 1", "r 1C 21 1",
      "w 6A 10 3", "w 6A 17 2", "w 6A 56 9", "w 1C A0 5", "r 6A 10 3", "r 6A 17 2", "r 6A 56 9", "r 1C A0 5",
  };
  assert(bus.log == expected);
  assert(bus.accel.regs[0x10] == 0x6A && bus.accel.regs[0x11] == 0x44 && bus.accel.regs[0x12] == 0x44);
  assert(bus.accel.regs[0x17] == 0x40 && bus.accel.regs[0x18] == 0xE2 && bus.accel.regs[0x56] == 0x4F && bus.accel.regs[0x5E] == 0x50);
  assert(bus.mag.regs[0x20] == 0x7C && bus.mag.regs[0x22] == 0x00 && bus.mag.regs[0x23] == 0x0C &&
         bus.mag.regs[0x24] == 0x40);
  assert(bus.now < 100);  // about 25 ms on a free bus
}

static void testSlowResetIsPolled() {
  Bus bus;
  bus.accel.resetFor = 22;  // still resetting after the 10 ms wait
  std::string why;
  assert(start(bus, why));
  int polls = 0;
  for (const std::string& line : bus.log) polls += line == "r 6A 12 1";
  assert(polls >= 3);
  // A reset that never ends is reported, inside the deadline.
  Bus stuck;
  stuck.mag.resetFor = 100000;
  assert(!start(stuck, why));
  assert(why == "Error: The LIS3MDL at 0x1C on bus 1 did not finish its reset. Check its power, then 'openimu'.");
  assert(stuck.now <= kLsm4517InitDeadlineMs);
}

static void testMissingOrWrongChips() {
  std::string why;
  Bus noAccel;
  noAccel.accel.present = false;
  assert(!start(noAccel, why));
  assert(why == "Error: No LSM6DSOX answers at 0x6A on bus 1 (the Adafruit 4517's accelerometer and gyroscope). "
                "Check the wiring, 'imubus' and 'imuaddress', then 'openimu'.");
  assert(noAccel.log.empty());  // no write reached anything
  Bus noMag;
  noMag.mag.present = false;
  assert(!start(noMag, why));
  assert(why == "Error: No LIS3MDL answers at 0x1C on bus 1 (the Adafruit 4517's magnetometer). Check the wiring, "
                "'imubus' and 'imumagaddress', then 'openimu'.");
  // A 4692 (LSM6DSO32) shares 0x6C; another chip answering is refused.
  Bus other;
  other.accel.id = 0x69;
  assert(!start(other, why));
  assert(why == "Error: The device at 0x6A on bus 1 is not an LSM6DSOX or LSM6DS3TR-C (WHO_AM_I 0x69, expected "
                "0x6C or 0x6A). Check 'imuaddress'.");
  Bus otherMag;
  otherMag.mag.id = 0x33;
  assert(!start(otherMag, why));
  assert(why.find("is not an LIS3MDL (WHO_AM_I 0x33, expected 0x3D). Check 'imumagaddress'.") != std::string::npos);
  // The other addresses (both jumpers).
  Bus alt;
  alt.accelAddress = 0x6B;
  alt.magAddress = 0x1E;
  assert(start(alt, why));
}

static void testConfigurationThatDoesNotStick() {
  Bus bus;
  bus.accel.ignoreWritesTo = true;
  bus.accel.ignoredReg = 0x11;
  std::string why;
  assert(!start(bus, why));
  assert(why == "Error: The LSM6DSOX at 0x6A on bus 1 did not keep its settings (register 0x11 reads 0x00, "
                "expected 0x44). Run 'openimu' again.");
  // A lost event write (tap and free fall silently off) is caught too.
  Bus events;
  events.accel.ignoreWritesTo = true;
  events.accel.ignoredReg = 0x58;  // TAP_CFG2: INTERRUPTS_ENABLE
  assert(!start(events, why));
  assert(why == "Error: The LSM6DSOX at 0x6A on bus 1 did not keep its settings (register 0x58 reads 0x00, "
                "expected 0x85). Run 'openimu' again.");
  // So is a lost filter (CTRL8_XL) or I3C-off (CTRL9_XL) write.
  Bus filter;
  filter.accel.ignoreWritesTo = true;
  filter.accel.ignoredReg = 0x17;
  assert(!start(filter, why));
  assert(why == "Error: The LSM6DSOX at 0x6A on bus 1 did not keep its settings (register 0x17 reads 0x00, "
                "expected 0x40). Run 'openimu' again.");
  Bus i3c;
  i3c.accel.ignoreWritesTo = true;
  i3c.accel.ignoredReg = 0x18;
  assert(!start(i3c, why));
  assert(why == "Error: The LSM6DSOX at 0x6A on bus 1 did not keep its settings (register 0x18 reads 0x00, "
                "expected 0xE2). Run 'openimu' again.");
  Bus mag;
  mag.mag.ignoreWritesTo = true;
  mag.mag.ignoredReg = 0x24;
  assert(!start(mag, why));
  assert(why.find("LIS3MDL at 0x1C on bus 1 did not keep its settings (register 0x24 reads 0x00, expected 0x40)") !=
         std::string::npos);
}

static void testBusyBusAndDeadline() {
  // A bus that stays busy is reported as busy, not as a missing chip.
  Bus busy;
  busy.busyUntil = 100000;
  std::string why;
  assert(!start(busy, why));
  assert(why == "Error: I2C bus 1 stayed busy, so the Adafruit 4517 could not be started (chip id). Run 'openimu' "
                "again.");
  // Every access at its worst case: the start still ends inside its deadline
  // (it gives up early rather than overrun).
  Bus slow;
  slow.ioMs = kLsm4517IoWorstMs;
  assert(start(slow, why));
  assert(slow.now <= kLsm4517InitDeadlineMs);
  // Busy for a while, then free: the busy steps are tried again.
  Bus later;
  later.busyUntil = 3;  // the first access waits, then the bus frees
  assert(start(later, why));
}

// ---- Decode -----------------------------------------------------------------------------
static void put16(uint8_t* p, int16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((static_cast<uint16_t>(v) >> 8) & 0xFF);
}

static void testDecode() {
  uint8_t data[14] = {};
  put16(data, 512);             // +2 C over 25
  put16(data + 2, 5714);        // gyro X: 5714 * 17.5 mdps = 100 dps
  put16(data + 4, -5714);
  put16(data + 6, 0);
  put16(data + 8, 0);           // accel X
  put16(data + 10, 0);
  put16(data + 12, 8197);       // accel Z: 1 g (0.122 mg/LSB), flat face up
  Lsm4517Reading r{};
  lsm6dsoxDecode(data, kLsm6dsoxSrcSingleTap, r);
  assert(near(r.temp, 27.0f, 0.01f));
  assert(near(r.gyro[0], 100.0f * kMotionDegToRad, 0.001f) && near(r.gyro[1], -100.0f * kMotionDegToRad, 0.001f));
  assert(near(r.accel[2], 9.81f, 0.01f) && near(r.accel[0], 0.0f, 0.0001f));
  assert(r.tap && !r.freefall);
  lsm6dsoxDecode(data, kLsm6dsoxSrcFreefall, r);
  assert(!r.tap && r.freefall);
  // Full scale: -32768 is -4 g.
  put16(data + 8, -32768);
  lsm6dsoxDecode(data, 0, r);
  assert(near(r.accel[0], -3.998f * 9.80665f, 0.01f));

  uint8_t field[6] = {};
  put16(field, 6842);    // 1 gauss = 100 uT
  put16(field + 2, -3421);
  put16(field + 4, 342);
  lis3mdlDecode(field, r);
  assert(near(r.mag[0], 100.0f, 0.01f) && near(r.mag[1], -50.0f, 0.01f) && near(r.mag[2], 5.0f, 0.02f));
}

// ---- One sample: read order and nothing lost ----------------------------------------------
struct SampleBus {
  bool magOk = true;
  bool srcOk = true;    // the LSM6DSOX transaction reads the source bytes
  bool dataOk = true;   // ... then the 14-byte burst
  uint8_t intSrc = 0;   // latched on the chip until the source bytes are read
  int16_t accelZ = 8197;
  int16_t magX = 6842;
  std::vector<std::string> order;
  bool readMag(uint8_t* field) {
    order.push_back("mag");
    if (!magOk) return false;
    std::memset(field, 0, 6);
    field[0] = static_cast<uint8_t>(magX & 0xFF);
    field[1] = static_cast<uint8_t>((static_cast<uint16_t>(magX) >> 8) & 0xFF);
    return true;
  }
  bool readAccel(uint8_t* src, uint8_t* data, bool& srcRead) {
    order.push_back("accel");
    if (!srcOk) return false;
    src[0] = intSrc;
    src[1] = src[2] = 0;
    intSrc = 0;  // read-to-clear
    srcRead = true;
    if (!dataOk) return false;
    std::memset(data, 0, 14);
    data[12] = static_cast<uint8_t>(accelZ & 0xFF);
    data[13] = static_cast<uint8_t>((static_cast<uint16_t>(accelZ) >> 8) & 0xFF);
    return true;
  }
};

static bool sample(SampleBus& bus, Lsm4517SampleState& st, Lsm4517Reading& r, bool& magFailed) {
  return lsm4517ReadSample(
      st, [&](uint8_t* field) { return bus.readMag(field); },
      [&](uint8_t* src, uint8_t* data, bool& srcRead) { return bus.readAccel(src, data, srcRead); }, r, magFailed);
}

// The same board with an LSM6DS3TR-C (WHO_AM_I 0x6A): CTRL1_XL without the
// LPF2 bit, CTRL8/9_XL untouched, the tap engine at TAP_CFG (0x58).
static void testDs3trcVariant() {
  Bus bus;
  bus.accel.id = 0x6A;
  std::string why;
  Lsm6Chip chip = Lsm6Chip::Dsox;
  char buf[256] = {};
  assert(lsm4517StartSequence(bus, 1, bus.accelAddress, bus.magAddress, buf, sizeof(buf), &chip));
  assert(chip == Lsm6Chip::Ds3trc && buf[0] == 0);
  const std::vector<std::string> expected = {
      "r 6A 0F 1", "r 1C 0F 1", "w 6A 12 1", "w 1C 21 1", "sleep 10", "r 6A 12 1", "r 1C 21 1",
      "w 6A 10 3", "w 6A 58 7", "w 1C A0 5", "r 6A 10 3", "r 6A 58 7", "r 1C A0 5",
  };
  assert(bus.log == expected);
  assert(bus.accel.regs[0x10] == 0x68 && bus.accel.regs[0x58] == 0x8F && bus.accel.regs[0x59] == 0x05 &&
         bus.accel.regs[0x5B] == 0x00 && bus.accel.regs[0x5E] == 0x50);
  assert(bus.accel.regs[0x17] == 0 && bus.accel.regs[0x56] == 0);  // not written
  // Events come from WAKE_UP_SRC (src[1]) and TAP_SRC (src[2]).
  Lsm4517SampleState st;
  st.chip = Lsm6Chip::Ds3trc;
  Lsm4517Reading r{};
  bool magFailed = false;
  uint8_t field[6] = {0x5A, 0x02, 0, 0, 0, 0};
  uint8_t data[14] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x8F, 0x13};
  auto readMag = [&](uint8_t* f) { memcpy(f, field, 6); return true; };
  uint8_t srcBytes[3] = {0x00, 0x00, kLsm6ds3SrcSingleTap};
  auto readAccel = [&](uint8_t* src, uint8_t* d, bool& srcRead) {
    memcpy(src, srcBytes, 3); srcRead = true; memcpy(d, data, 14); return true; };
  assert(lsm4517ReadSample(st, readMag, readAccel, r, magFailed) && r.tap && !r.freefall);
  srcBytes[2] = 0; srcBytes[1] = kLsm6ds3SrcFreefall;
  assert(lsm4517ReadSample(st, readMag, readAccel, r, magFailed) && !r.tap && r.freefall);
  srcBytes[1] = 0; srcBytes[0] = 0x04;  // MASTER_CONFIG bits are not events on this chip
  assert(lsm4517ReadSample(st, readMag, readAccel, r, magFailed) && !r.tap && !r.freefall);
}

static void testSampleKeepsTheTap() {
  Lsm4517SampleState st;
  Lsm4517Reading r{};
  bool magFailed = false;
  SampleBus bus;
  // The magnetometer first (no side effect), the read-to-clear chip last.
  assert(sample(bus, st, r, magFailed) && !magFailed && r.hasMag && !r.tap);
  assert((bus.order == std::vector<std::string>{"mag", "accel"}));
  assert(near(r.mag[0], 100.0f, 0.01f) && near(r.accel[2], 9.81f, 0.01f));

  // A knock while an OLED frame holds the bus: the magnetometer read times
  // out, the accel/gyro sample and the tap still arrive, with the last field.
  bus.intSrc = kLsm6dsoxSrcSingleTap;
  bus.magOk = false;
  bus.magX = 0;
  assert(sample(bus, st, r, magFailed) && magFailed && r.tap && r.hasMag && near(r.mag[0], 100.0f, 0.01f));
  bus.magOk = true;

  // The source bytes read (the chip cleared its flags), then the burst
  // failed: the sample fails, and the tap and free fall arrive with the next.
  bus.intSrc = kLsm6dsoxSrcSingleTap | kLsm6dsoxSrcFreefall;
  bus.dataOk = false;
  assert(!sample(bus, st, r, magFailed) && bus.intSrc == 0 && st.pendingTap && st.pendingFreefall);
  bus.dataOk = true;
  assert(sample(bus, st, r, magFailed) && r.tap && r.freefall);
  assert(sample(bus, st, r, magFailed) && !r.tap && !r.freefall);  // once

  // The LSM6DSOX transaction never ran (bus busy): the flags stay on the chip.
  bus.intSrc = kLsm6dsoxSrcSingleTap;
  bus.srcOk = false;
  assert(!sample(bus, st, r, magFailed) && bus.intSrc == kLsm6dsoxSrcSingleTap && !st.pendingTap);
  bus.srcOk = true;
  assert(sample(bus, st, r, magFailed) && r.tap);

  // The chip lost its power and came back in power-down (a bus-rail cut):
  // it answers, with 14 zero bytes. That is no reading: the sample fails
  // (zeros would read as a free fall), a tap read before stays for the next
  // good sample, and one non-zero byte is a reading again.
  uint8_t zeros[14] = {};
  assert(!lsm6dsoxBurstAlive(zeros));
  for (int i = 0; i < 14; ++i) {
    zeros[i] = 1;
    assert(lsm6dsoxBurstAlive(zeros));
    zeros[i] = 0;
  }
  bus.intSrc = kLsm6dsoxSrcSingleTap;
  bus.accelZ = 0;  // the scripted burst is all zero now
  Lsm4517Reading kept = r;
  assert(!sample(bus, st, r, magFailed) && st.pendingTap);
  assert(r.accel[2] == kept.accel[2] && r.tap == kept.tap);  // out untouched
  assert(!sample(bus, st, r, magFailed) && !sample(bus, st, r, magFailed));  // every such read fails
  bus.accelZ = 8197;
  assert(sample(bus, st, r, magFailed) && r.tap && near(r.accel[2], 9.81f, 0.01f));

  // No good field yet: no magnetometer reading.
  Lsm4517SampleState fresh;
  SampleBus noMag;
  noMag.magOk = false;
  assert(sample(noMag, fresh, r, magFailed) && magFailed && !r.hasMag);
}

int main() {
  testConfigurationBytes();
  testAddresses();
  testStartConfiguresBothChips();
  testSlowResetIsPolled();
  testMissingOrWrongChips();
  testConfigurationThatDoesNotStick();
  testBusyBusAndDeadline();
  testDecode();
  testSampleKeepsTheTap();
  testDs3trcVariant();
  std::puts("motion_lsm6dsox_core_tests: all passed");
  return 0;
}
