// The BNO055 backend's pure parts (HAL_Motion_BNO055Core.h) over a scripted
// chip: the bounded start sequence (register order, waits outside the bus
// lock, one reason per failure, the deadline), the sample decode and the
// address rule.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "HAL_Motion_BNO055Core.h"

// A BNO055 on a bus. Time moves only in sleepMs (the sequence's unlocked
// waits) plus ioMs per register access (one short transaction each: 1 ms, or
// kBno055IoWorstMs for a bus at its worst case).
struct Chip {
  uint32_t ioMs = 1;
  uint8_t nackOnceTo = 0xFF;       // this register's first write is not acknowledged
  uint32_t busyUntil = 0;          // the bus lock times out on every access before this
  bool lastBusy = false;
  std::vector<uint8_t> nacked;
  uint32_t now = 0;
  uint32_t answersFrom = 0;        // boot: no answer before this
  uint32_t resetFor = 400;         // after RST_SYS: no answer for this long
  bool comesBack = true;           // answers again after a reset
  uint8_t id = kBno055ChipId;
  uint8_t mode = kBno055ModeConfig;
  bool modeSticks = true;          // OPR_MODE reads back what was written
  uint8_t failWriteTo = 0xFF;      // this register's write is not acknowledged
  uint32_t silentFrom = UINT32_MAX;
  std::vector<std::pair<uint8_t, uint8_t>> writes;
  uint32_t longestAccess = 0, sleeps = 0;

  bool answering() const { return now >= answersFrom && now < silentFrom; }
  bool busy() const { return lastBusy; }
  // The lock wait: a busy bus costs the whole lock wait and never reaches the chip.
  bool locked() {
    lastBusy = now < busyUntil;
    if (lastBusy) now += kBno055StepLockMs;
    return !lastBusy;
  }
  bool read(uint8_t reg, uint8_t* out, uint8_t len) {
    if (!locked()) return false;
    now += ioMs;
    if (!answering()) return false;
    assert(len == 1);
    *out = reg == kBno055RegChipId ? id : reg == kBno055RegOprMode ? mode : 0;
    return true;
  }
  bool write(uint8_t reg, uint8_t value) {
    if (!locked()) return false;
    now += ioMs;
    if (!answering() || reg == failWriteTo) return false;
    if (reg == nackOnceTo) {
      nackOnceTo = 0xFF;
      nacked.push_back(reg);
      return false;
    }
    writes.emplace_back(reg, value);
    if (reg == kBno055RegOprMode && modeSticks) mode = value;
    if (reg == kBno055RegSysTrigger && (value & kBno055TriggerReset)) {
      answersFrom = now + resetFor;
      if (!comesBack) silentFrom = now;
      mode = kBno055ModeConfig;
    }
    return true;
  }
  void sleepMs(uint32_t ms) { now += ms; ++sleeps; }
  uint32_t nowMs() { return now; }
};

static bool start(Chip& chip, std::string& why, uint8_t bus = 1, uint8_t address = 0x29) {
  char text[160] = "untouched";
  const bool ok = bno055StartSequence(chip, bus, address, text, sizeof(text));
  why = text;
  return ok;
}

static void testStartsInOrder() {
  Chip chip;
  std::string why;
  assert(start(chip, why));
  const std::vector<std::pair<uint8_t, uint8_t>> expected = {
    {kBno055RegOprMode, kBno055ModeConfig}, {kBno055RegSysTrigger, kBno055TriggerReset},
    {kBno055RegPwrMode, kBno055PowerNormal}, {kBno055RegPageId, 0},
    {kBno055RegSysTrigger, kBno055TriggerExtClock}, {kBno055RegOprMode, kBno055ModeNdof}};
  assert(chip.writes == expected);
  assert(chip.mode == kBno055ModeNdof);
  // The reset wait and the polls are sleeps, not one long access.
  assert(chip.now < kBno055InitDeadlineMs && chip.now >= kBno055ResetWaitMs);
  assert(chip.sleeps >= 4);
}

static void testBootingChip() {
  Chip chip;
  chip.answersFrom = 600;  // still booting after power-up
  std::string why;
  assert(start(chip, why) && chip.mode == kBno055ModeNdof);
  assert(chip.now <= kBno055InitDeadlineMs);
}

static void testNothingAnswers() {
  Chip chip;
  chip.answersFrom = UINT32_MAX;
  std::string why;
  assert(!start(chip, why));
  assert(why.find("Error: No BNO055 answers at 0x29 on bus 1.") == 0);
  assert(why.find("'imuaddress'") != std::string::npos);
  assert(chip.writes.empty() && chip.now <= kBno055BootWaitMs + kBno055PollEveryMs);
}

static void testAnotherChip() {
  Chip chip;
  chip.id = 0x6C;  // an LSM6DSOX answering at the address
  std::string why;
  assert(!start(chip, why, 0, 0x28));
  assert(why.find("Error: The device at 0x28 on bus 0 is not a BNO055 (chip id 0x6C, expected 0xA0).") == 0);
  assert(chip.writes.empty());
}

static void testNoReturnAfterReset() {
  Chip chip;
  chip.comesBack = false;
  std::string why;
  assert(!start(chip, why));
  assert(why.find("did not come back after its reset") != std::string::npos);
  assert(chip.now <= kBno055InitDeadlineMs);  // bounded, unlike the library's loop
  assert(chip.writes.size() == 2);
}

static void testSlowReset() {
  Chip chip;
  chip.resetFor = 1200;  // longer than the datasheet, still inside the deadline
  std::string why;
  assert(start(chip, why) && chip.now <= kBno055InitDeadlineMs);
}

static void testStepFails() {
  Chip chip;
  chip.failWriteTo = kBno055RegPwrMode;
  std::string why;
  assert(!start(chip, why));
  assert(why == "Error: The BNO055 at 0x29 on bus 1 stopped answering (power mode). Check its wiring, then "
                "'openimu'.");
}

// A write the chip misses once is tried again: the start goes on.
static void testStepRetried() {
  Chip chip;
  chip.nackOnceTo = kBno055RegPwrMode;
  std::string why;
  assert(start(chip, why) && chip.mode == kBno055ModeNdof);
  assert(chip.nacked.size() == 1 && chip.writes.size() == 6);
}

// Another device holding the bus past one lock wait does not fail the start.
static void testBusBusyForAWhile() {
  Chip chip;
  chip.busyUntil = 300;
  std::string why;
  assert(start(chip, why) && chip.mode == kBno055ModeNdof);
}

// A bus that never frees is reported as busy, not as a missing chip.
static void testBusStaysBusy() {
  Chip chip;
  chip.busyUntil = UINT32_MAX;
  std::string why;
  assert(!start(chip, why));
  assert(why == "Error: I2C bus 1 stayed busy, so the BNO055 at 0x29 could not be started (chip id). Run "
                "'openimu' again.");
  assert(chip.now <= kBno055InitDeadlineMs && chip.writes.empty());
}

// The bus frees for the chip id and the first steps, then stays busy for
// good: after the reset write (the poll meets it) or after the chip is back
// (the power-mode step meets it). Each says the bus was busy, inside the
// deadline.
struct BusyLater {
  Chip& c;
  bool afterPoll;
  bool resetSent = false;
  bool read(uint8_t reg, uint8_t* out, uint8_t len) {
    const bool ok = c.read(reg, out, len);
    if (ok && resetSent && afterPoll) c.busyUntil = UINT32_MAX;
    return ok;
  }
  bool write(uint8_t reg, uint8_t value) {
    const bool ok = c.write(reg, value);
    if (ok && reg == kBno055RegSysTrigger && (value & kBno055TriggerReset)) {
      resetSent = true;
      if (!afterPoll) c.busyUntil = UINT32_MAX;
    }
    return ok;
  }
  bool busy() const { return c.busy(); }
  void sleepMs(uint32_t ms) { c.sleepMs(ms); }
  uint32_t nowMs() { return c.nowMs(); }
};

static void testBusBusyLater() {
  for (bool afterPoll : {false, true}) {
    Chip chip;
    BusyLater io{chip, afterPoll};
    char text[160] = "";
    assert(!bno055StartSequence(io, 1, 0x29, text, sizeof(text)));
    const std::string why = text;
    assert(why == (afterPoll ? "Error: I2C bus 1 stayed busy, so the BNO055 at 0x29 could not be started (power "
                               "mode). Run 'openimu' again."
                             : "Error: I2C bus 1 stayed busy, so the BNO055 at 0x29 could not be started (chip id "
                               "after the reset). Run 'openimu' again."));
    assert(chip.now <= kBno055InitDeadlineMs);
  }
}

// Every access at its worst case, the chip answering only at the end of its
// boot window and slow after its reset: the start still ends in the deadline,
// so the start command reports the backend's own result.
static void testWorstCaseFitsTheDeadline() {
  Chip chip;
  chip.ioMs = kBno055IoWorstMs;
  chip.answersFrom = kBno055BootWaitMs - kBno055IoWorstMs;
  chip.resetFor = 900;
  std::string why;
  assert(start(chip, why) && chip.now <= kBno055InitDeadlineMs);
  static_assert(kBno055InitDeadlineMs + 200 <= kMotionStartWaitMs, "");
}

// A chip that stops answering in the tail at the worst access time: the
// failure (with retries) still ends inside the deadline.
static void testWorstCaseFailureFitsTheDeadline() {
  for (uint8_t reg : {kBno055RegPwrMode, kBno055RegPageId, kBno055RegOprMode}) {
    Chip chip;
    chip.ioMs = kBno055IoWorstMs;
    chip.failWriteTo = reg;
    std::string why;
    assert(!start(chip, why) && chip.now <= kBno055InitDeadlineMs);
    assert(why.find("Error: The BNO055 at 0x29 on bus 1 ") == 0);
  }
}

static void testModeDoesNotStick() {
  Chip chip;
  chip.modeSticks = false;
  std::string why;
  assert(!start(chip, why));
  assert(why.find("did not enter its fusion mode") != std::string::npos);
}

static void testNoWhyBuffer() {
  Chip chip;
  chip.answersFrom = UINT32_MAX;
  assert(!bno055StartSequence(chip, 0, 0x28, nullptr, 0));
}

static void put16(uint8_t* at, int16_t value) {
  at[0] = static_cast<uint8_t>(value & 0xFF);
  at[1] = static_cast<uint8_t>((static_cast<uint16_t>(value) >> 8) & 0xFF);
}

static void testDecode() {
  uint8_t data[kBno055DataBytes] = {};
  put16(data + 0, 12);     // accel x 0.12 m/s^2
  put16(data + 2, -3);
  put16(data + 4, 981);    // 9.81
  put16(data + 6, 344);    // mag x 21.5 uT
  put16(data + 8, -68);
  put16(data + 10, -641);
  put16(data + 12, 16);    // gyro x 1 dps
  put16(data + 14, -1600); // -100 dps
  put16(data + 16, 0);
  put16(data + 18, 1440);  // heading 90
  put16(data + 20, -160);  // the chip's roll register: pitch -10
  put16(data + 22, 2720);  // the chip's pitch register: roll 170
  const uint8_t status[kBno055StatusBytes] = {0xE7, 0xFF};  // -25 C, fully calibrated
  const Bno055Reading r = bno055Decode(data, status);
  assert(std::fabs(r.accel[0] - 0.12f) < 1e-5f && std::fabs(r.accel[1] + 0.03f) < 1e-5f &&
         std::fabs(r.accel[2] - 9.81f) < 1e-5f);
  assert(std::fabs(r.mag[0] - 21.5f) < 1e-5f && std::fabs(r.mag[1] + 4.25f) < 1e-5f &&
         std::fabs(r.mag[2] + 40.0625f) < 1e-5f);
  assert(std::fabs(r.gyro[0] - 0.0174533f) < 1e-5f);              // rad/s
  assert(std::fabs(r.gyro[1] + 100.0f * kMotionDegToRad) < 1e-5f);
  assert(r.yaw == 90.0f && r.pitch == -10.0f && r.roll == 170.0f);
  assert(r.temp == -25 && r.cal == 0xFF);
}

static void testAddressRule() {
  static_assert(bno055AddressValid(0x28) && bno055AddressValid(0x29) && !bno055AddressValid(0x2A), "");
  static_assert(bno055AddressFromSetting(0x29) == 0x29, "");
  static_assert(bno055AddressFromSetting(0x30) == kBno055AddressDefault, "");
  static_assert(bno055AddressFromSetting(0) == kBno055AddressDefault, "");
  assert(std::strcmp(BNO055_ADDRESS_OPTIONS, "40|0x28 (default),41|0x29 (ADR high)") == 0);
}

// The fast path's reads (a build with IMU input): ACC_DATA and GRV_DATA,
// 100 LSB per m/s^2, the accelerometer in mg for the knock detector, gravity
// as a direction, the full block every 100 ms.
static void testFastSampling() {
  // Page 0 registers (datasheet register map): ACC_DATA 0x08..0x0D, GRV_DATA
  // 0x2E..0x33, TEMP 0x34 and CALIB_STAT 0x35.
  static_assert(kBno055RegAccelData == 0x08 && kBno055RegGravityData == 0x2E && kBno055RegTemp == 0x34, "");
  static_assert(kBno055VectorBytes == 6 && kBno055RegGravityData + kBno055VectorBytes == kBno055RegTemp, "");
  static_assert(kBno055FastPeriodMs == 10 && kBno055FastHz == 100 && kBno055FullEveryMs == 100, "");
  // Flat, face up: gravity 9.81 m/s^2 on +Z (981 LSB); a knock adds -2.45
  // m/s^2 on Y (-245 LSB, two's complement 0xFF0B).
  const uint8_t acc[6] = {0x00, 0x00, 0x0B, 0xFF, 0xD5, 0x03};  // x 0, y -245, z 981
  float ms2[3];
  bno055DecodeVector(acc, ms2);
  assert(ms2[0] == 0.0f && std::fabs(ms2[1] + 2.45f) < 1e-4f && std::fabs(ms2[2] - 9.81f) < 1e-4f);
  float mg[3];
  bno055AccelToMg(ms2, mg);
  assert(std::fabs(mg[2] - 1000.34f) < 0.05f);   // 9.81 m/s^2 is 1000.3 mg
  assert(std::fabs(mg[1] + 249.83f) < 0.05f);    // -2.45 m/s^2 is -249.8 mg
  // The full block's accelerometer is the same 6 bytes, so both paths agree.
  uint8_t block[kBno055DataBytes] = {};
  std::memcpy(block, acc, sizeof(acc));
  const uint8_t status[2] = {25, 0xFF};
  const Bno055Reading r = bno055Decode(block, status);
  for (int i = 0; i < 3; ++i) assert(r.accel[i] == ms2[i]);
  // Gravity: a direction in g.
  const uint8_t grv[6] = {0x00, 0x00, 0x00, 0x00, 0xD5, 0x03};  // z 9.81 m/s^2
  float g[3];
  bno055DecodeVector(grv, g);
  float up[3] = {7, 7, 7};
  assert(bno055GravityUp(g, up));
  assert(std::fabs(up[0]) < 1e-6f && std::fabs(up[2] - 1.0003f) < 1e-3f);
  // Upside down on its side (-X), and the zeros of a fusion that has not started.
  const uint8_t side[6] = {0x2B, 0xFC, 0x00, 0x00, 0x00, 0x00};  // x -981
  bno055DecodeVector(side, g);
  assert(bno055GravityUp(g, up) && std::fabs(up[0] + 1.0003f) < 1e-3f);
  const uint8_t zeros[6] = {};
  bno055DecodeVector(zeros, g);
  float keep[3] = {0, 0, 1};
  assert(!bno055GravityUp(g, keep) && keep[2] == 1.0f);  // unchanged
  const float huge[3] = {0, 0, 40.0f};
  assert(!bno055GravityUp(huge, keep));
  // The full block: the first sample, then every 100 ms (reads jitter by a
  // few ms either way).
  assert(bno055FullDue(false, 0, 12345));
  assert(!bno055FullDue(true, 1000, 1010));
  assert(!bno055FullDue(true, 1000, 1090));
  assert(bno055FullDue(true, 1000, 1096));
  assert(bno055FullDue(true, 1000, 1100));
  assert(bno055FullDue(true, 0xFFFFFFF0u, 0x0000005Au));  // across the millisecond wrap (106 ms)
}

int main() {
  testFastSampling();
  testStartsInOrder();
  testBootingChip();
  testNothingAnswers();
  testAnotherChip();
  testNoReturnAfterReset();
  testSlowReset();
  testStepFails();
  testStepRetried();
  testBusBusyForAWhile();
  testBusStaysBusy();
  testBusBusyLater();
  testWorstCaseFitsTheDeadline();
  testWorstCaseFailureFitsTheDeadline();
  testModeDoesNotStick();
  testNoWhyBuffer();
  testDecode();
  testAddressRule();
  std::puts("BNO055 core: bounded start in order, waits outside the lock, one reason per failure, decode "
            "and addresses passed");
  return 0;
}
