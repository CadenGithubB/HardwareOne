// Host harness for the automation conditions on the IMU.
//
// test_automation_imu_conditions.py inserts, from evaluateCondition
// (System_Automation.cpp): the `<variable> <operator> <value>` parse, the IMU_
// branch and the comparisons, as one function imuCondition(condStart) that
// takes the upper-cased text between IF and THEN. The condition values are the
// shipping rule's (HAL_MotionCore.h), the detector state the shipping
// detector's. Host boundaries: the IMU's running flags, its reading and its
// detector copy (each can be made unavailable), millis() and the log macro.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "HAL_MotionCore.h"

#define ENABLE_IMU_SENSOR 1
#define DEBUG_AUTOMATIONSF(...) ((void)0)

// The reading's fields the branch reads (HAL_Motion.h, ImuCache).
struct ImuCache {
  float oriPitch = 0, oriRoll = 0, oriYaw = 0;
  bool imuDataValid = false;
};

static bool gImuRunning = true, gImuConnected = true;
static ImuCache gReading;
static MotionActionState gActions;
static bool gCacheAnswers = true, gActionsAnswer = true;
static unsigned long gNow = 100000;
static unsigned gCacheReads = 0;

static unsigned long millis() { return gNow; }
static bool imuCacheSnapshot(ImuCache& out, uint32_t timeoutMs) {
  assert(timeoutMs == 50);
  ++gCacheReads;
  out = ImuCache{};
  if (!gCacheAnswers) return false;
  out = gReading;
  return true;
}
static bool imuActionsSnapshot(MotionActionState& out, uint32_t timeoutMs) {
  assert(timeoutMs == 50);
  out = MotionActionState{};
  if (!gActionsAnswer) return false;
  out = gActions;
  return true;
}

// INSERT_PRODUCTION

static int failures = 0, checks = 0;
static void check(bool ok, const std::string& what) {
  ++checks;
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

// The condition as evaluateCondition hands it on: trimmed and upper-cased.
static bool met(const char* condition) {
  char text[128];
  std::snprintf(text, sizeof(text), "%s", condition);
  for (char* p = text; *p; ++p) {
    if (*p >= 'a' && *p <= 'z') *p -= 32;
  }
  return imuCondition(text);
}

static void feed(const MotionActionSample& sample, int count, unsigned long stepMs = 20) {
  for (int i = 0; i < count; ++i) {
    motionActionsUpdate(gActions, sample, gNow, nullptr);
    gNow += stepMs;
  }
}

int main() {
  MotionActionSample flat;
  flat.az = 9.81f;
  gActions = MotionActionState{};
  feed(flat, 5);
  gReading.imuDataValid = true;
  gReading.oriPitch = -35.5f;
  gReading.oriRoll = 12.0f;
  gReading.oriYaw = 200.0f;

  // ---- Tilt: the reading's angles, numeric operators, negative values --------
  check(met("imu_pitch < -30"), "pitch below -30");
  check(!met("imu_pitch > -30"), "pitch not above -30");
  check(met("IMU_PITCH<=-35.5") && met("IMU_PITCH>=-35.5") && met("imu_pitch = -35.5"), "pitch at its value");
  check(met("imu_pitch != 0") && !met("imu_pitch != -35.5"), "pitch not equal");
  check(met("imu_roll > 10") && met("IMU_ROLL<12.5") && !met("imu_roll>12"), "roll");
  check(met("imu_tilt > 30") && !met("imu_tilt > 36"), "tilt is the larger of |pitch| and |roll|");
  gReading.oriPitch = 5.0f;
  gReading.oriRoll = -60.0f;
  check(met("imu_tilt >= 60") && met("imu_roll < -45"), "tilt follows roll when it is the larger");
  // A text operator on a number is no comparison.
  check(!met("imu_tilt CONTAINS 6"), "CONTAINS does not apply to a number");

  // ---- Pose (flip) ------------------------------------------------------------
  check(met("imu_pose = face-up") && met("IMU_POSE=FACE-UP") && !met("imu_pose = face-down"), "face up at the start");
  check(met("imu_pose != face-down") && met("imu_pose contains FACE"), "pose with != and CONTAINS");
  MotionActionSample down = flat;
  down.az = -9.81f;
  feed(down, 2);
  check(met("imu_pose = face-up"), "a pose is the one that held, not the latest sample");
  feed(down, 15);
  check(met("imu_pose = face-down") && met("imu_pose=FACE-DOWN") && !met("imu_pose = face-up"), "flipped over");
  check(met("imu_pose contains down") && !met("imu_pose contains portrait"), "flipped over, by CONTAINS");
  check(!met("imu_pose > 0") && !met("imu_pose < 0"), "a numeric operator on the pose is no comparison");
  feed(flat, 15);
  check(met("imu_pose = face-up"), "turned back");

  // ---- Tap ---------------------------------------------------------------------
  check(!met("imu_tap_age < 5") && !met("imu_tap_age > 5") && !met("imu_tap_age != 3"),
        "no tap since the IMU opened: every comparison is false");
  MotionActionSample tap = flat;
  tap.hasHwTap = true;  // the chip's own tap flag, as on the Adafruit 4517
  tap.hwTap = true;
  feed(tap, 1);
  feed(flat, 50);  // one second later
  check(met("imu_tap_age < 5") && met("imu_tap_age >= 1") && !met("imu_tap_age > 5"), "a second after a tap");
  gNow += 10000;
  check(!met("imu_tap_age < 5") && met("imu_tap_age > 10"), "eleven seconds after a tap");
  // The software tap (a chip without a tap engine: the BNO055) counts the same.
  gActions = MotionActionState{};
  feed(flat, 5);
  MotionActionSample knock = flat;
  knock.az = 40.0f;
  feed(knock, 1);
  feed(flat, 5);
  check(met("imu_tap_age < 2"), "a tap found in the readings");

  // ---- No value: the condition is false, whatever the operator ------------------
  const char* all[] = {"imu_pitch < 90", "imu_roll < 190", "imu_tilt >= 0", "imu_pose != none", "imu_tap_age >= 0"};
  for (const char* condition : all) check(met(condition), std::string("baseline: ") + condition);
  gImuRunning = false;
  gCacheReads = 0;
  for (const char* condition : all) check(!met(condition), std::string("IMU closed: ") + condition);
  check(gCacheReads == 0, "a closed IMU is not read");
  gImuRunning = true;
  gImuConnected = false;
  for (const char* condition : all) check(!met(condition), std::string("IMU not connected: ") + condition);
  gImuConnected = true;
  gReading.imuDataValid = false;  // open, no reading yet (the 4517's first second)
  for (const char* condition : all) check(!met(condition), std::string("no reading yet: ") + condition);
  gReading.imuDataValid = true;
  gCacheAnswers = false;  // the cache lock is busy
  for (const char* condition : all) check(!met(condition), std::string("cache busy: ") + condition);
  gCacheAnswers = true;
  gActionsAnswer = false;
  for (const char* condition : all) check(!met(condition), std::string("detector copy busy: ") + condition);
  gActionsAnswer = true;
  for (const char* condition : all) check(met(condition), std::string("available again: ") + condition);

  // Names the IMU does not answer: no heading condition yet, and no prefix match.
  for (const char* condition : {"imu_heading > 0", "imu_heading != 400", "imu_yaw < 400", "imu_ != 1",
                                "imu_pitchx < 90", "imu_tap >= 0"}) {
    check(!met(condition), std::string("unknown IMU variable: ") + condition);
  }

  if (failures) {
    std::fprintf(stderr, "automation IMU conditions: %d of %d checks failed\n", failures, checks);
    return 1;
  }
  std::printf("automation IMU conditions: %d checks passed (tilt, pose, tap age, no value is false)\n", checks);
  return 0;
}
