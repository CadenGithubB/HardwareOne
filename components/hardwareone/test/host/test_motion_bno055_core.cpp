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
// waits) plus 1 ms per register access (one short transaction each).
struct Chip {
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
  bool read(uint8_t reg, uint8_t* out, uint8_t len) {
    now += 1;
    if (!answering()) return false;
    assert(len == 1);
    *out = reg == kBno055RegChipId ? id : reg == kBno055RegOprMode ? mode : 0;
    return true;
  }
  bool write(uint8_t reg, uint8_t value) {
    now += 1;
    if (!answering() || reg == failWriteTo) return false;
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

int main() {
  testStartsInOrder();
  testBootingChip();
  testNothingAnswers();
  testAnotherChip();
  testNoReturnAfterReset();
  testSlowReset();
  testStepFails();
  testModeDoesNotStick();
  testNoWhyBuffer();
  testDecode();
  testAddressRule();
  std::puts("BNO055 core: bounded start in order, waits outside the lock, one reason per failure, decode "
            "and addresses passed");
  return 0;
}
