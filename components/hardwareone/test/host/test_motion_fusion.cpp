// The motion layer's own fusion (HAL_Motion_Fusion.h) on known poses: a flat
// board reads pitch/roll 0 with accel z +9.81; the +X end raised gives
// positive pitch, the +Y edge raised positive roll, a turn to the right (seen
// from above) a growing yaw; the filter converges from a wrong start, ignores
// a zero or huge step, keeps its output ranges, and the start-up gyro bias
// takes a still board's offset but not a moving board's.
#include <cassert>
#include <cmath>
#include <cstdio>

#include "HAL_Motion_Fusion.h"
#include "HAL_Motion_LSM6DSOXCore.h"

static bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }
static constexpr float kG = 9.80665f;
static constexpr float kDt = 0.02f;  // 50 Hz
static float rad(float deg) { return deg * kMotionDegToRad; }

// Accelerometer reading of a still board with the +X end raised by `pitch`
// degrees and then the +Y edge raised by `roll` degrees.
static void stillAccel(float pitch, float roll, float a[3]) {
  const float p = rad(pitch), r = rad(roll);
  a[0] = kG * std::sin(p);
  a[1] = kG * std::cos(p) * std::sin(r);
  a[2] = kG * std::cos(p) * std::cos(r);
}

static void run(MahonyFilter& f, const float gyro[3], const float accel[3], int steps) {
  for (int i = 0; i < steps; ++i) {
    assert(mahonyUpdate(f, gyro[0], gyro[1], gyro[2], accel[0], accel[1], accel[2], kDt));
  }
}

static void checkRanges(const FusionEuler& e) {
  assert(e.yaw >= 0.0f && e.yaw < 360.0f);
  assert(e.pitch >= -90.0f && e.pitch <= 90.0f);
  assert(e.roll >= -180.0f && e.roll <= 180.0f);
}

static void testFlat() {
  // A flat board: the decoded accelerometer reads +1 g on Z.
  uint8_t data[14] = {};
  data[12] = 8197 & 0xFF;
  data[13] = 8197 >> 8;
  Lsm4517Reading reading{};
  lsm6dsoxDecode(data, 0, reading);
  assert(near(reading.accel[2], 9.81f, 0.01f));
  MahonyFilter f;
  assert(mahonyInitFromAccel(f, reading.accel[0], reading.accel[1], reading.accel[2]));
  FusionEuler e = mahonyEuler(f);
  assert(near(e.pitch, 0.0f, 0.01f) && near(e.roll, 0.0f, 0.01f) && near(e.yaw, 0.0f, 0.01f));
  const float still[3] = {0, 0, 0};
  run(f, still, reading.accel, 500);
  e = mahonyEuler(f);
  assert(near(e.pitch, 0.0f, 0.01f) && near(e.roll, 0.0f, 0.01f));
  checkRanges(e);
}

static void testKnownPoses() {
  struct Pose { float pitch, roll; };
  const Pose poses[] = {{30, 0}, {-30, 0}, {0, 30}, {0, -45}, {20, 40}, {60, -10}, {0, 150}, {0, -120}};
  for (const Pose& pose : poses) {
    float a[3];
    stillAccel(pose.pitch, pose.roll, a);
    MahonyFilter f;
    assert(mahonyInitFromAccel(f, a[0], a[1], a[2]));
    FusionEuler e = mahonyEuler(f);
    assert(near(e.pitch, pose.pitch, 0.05f) && near(e.roll, pose.roll, 0.05f));
    checkRanges(e);
  }
  // The +X end raised reads +X up on the accelerometer and positive pitch.
  float a[3];
  stillAccel(30, 0, a);
  assert(a[0] > 0);
  // Face down: roll 180 (either sign), pitch 0.
  MahonyFilter down;
  assert(mahonyInitFromAccel(down, 0, 0, -kG));
  const FusionEuler e = mahonyEuler(down);
  assert(near(std::fabs(e.roll), 180.0f, 0.05f) && near(e.pitch, 0.0f, 0.05f));
  // No direction: refused.
  MahonyFilter none;
  assert(!mahonyInitFromAccel(none, 0, 0, 0));
}

// From a level start the filter follows the accelerometer to a tilted pose.
static void testConvergesFromWrongStart() {
  MahonyFilter f;  // identity: believes the board is flat
  float a[3];
  stillAccel(30, -20, a);
  const float still[3] = {0, 0, 0};
  run(f, still, a, 50 * 10);  // 10 s at 50 Hz
  const FusionEuler e = mahonyEuler(f);
  assert(near(e.pitch, 30.0f, 0.5f) && near(e.roll, -20.0f, 0.5f));
}

// A gyro-only turn integrates the expected angle; turning right (clockwise
// seen from above: -Z rate) grows yaw.
static void testGyroTurn() {
  MahonyFilter f;
  float a[3];
  stillAccel(0, 0, a);
  const float right[3] = {0, 0, -rad(90)};  // 90 deg/s clockwise
  run(f, right, a, 50);                     // 1 s
  FusionEuler e = mahonyEuler(f);
  assert(near(e.yaw, 90.0f, 0.5f));
  const float left[3] = {0, 0, rad(90)};
  run(f, left, a, 100);  // 2 s back the other way: -90 -> 270
  e = mahonyEuler(f);
  assert(near(e.yaw, 270.0f, 0.5f));
  checkRanges(e);
  // A turn about +Y (the +X end going down) with the accelerometer
  // ignored (a 0 g reading is outside the gate): pitch -20 after 1 s.
  MahonyFilter g;
  const float tipDown[3] = {0, rad(20), 0};
  const float none[3] = {0, 0, 0};
  run(g, tipDown, none, 50);
  assert(near(mahonyEuler(g).pitch, -20.0f, 0.3f));
}

static void testBadStepsAreIgnored() {
  MahonyFilter f;
  float a[3];
  stillAccel(10, 0, a);
  assert(mahonyInitFromAccel(f, a[0], a[1], a[2]));
  const MahonyFilter before = f;
  assert(!mahonyUpdate(f, 1, 1, 1, a[0], a[1], a[2], 0.0f));
  assert(!mahonyUpdate(f, 1, 1, 1, a[0], a[1], a[2], -0.02f));
  assert(!mahonyUpdate(f, 1, 1, 1, a[0], a[1], a[2], 5.0f));
  assert(!mahonyUpdate(f, 1, 1, 1, a[0], a[1], a[2], NAN));
  assert(f.q0 == before.q0 && f.q1 == before.q1 && f.q2 == before.q2 && f.q3 == before.q3);
  assert(mahonyUpdate(f, 0, 0, 0, a[0], a[1], a[2], kMahonyMaxDtS));
}

// A knock or a fall (|a| far from 1 g) does not pull the tilt.
static void testAccelGate() {
  MahonyFilter f;
  float a[3];
  stillAccel(0, 0, a);
  assert(mahonyInitFromAccel(f, a[0], a[1], a[2]));
  const float knock[3] = {3 * kG, 0, 0};
  const float still[3] = {0, 0, 0};
  run(f, still, knock, 25);
  const FusionEuler e = mahonyEuler(f);
  assert(near(e.pitch, 0.0f, 0.01f) && near(e.roll, 0.0f, 0.01f));
}

// The integral term learns a tilt-axis bias the start-up average missed.
static void testIntegralLearnsBias() {
  MahonyFilter f;
  float a[3];
  stillAccel(0, 0, a);
  assert(mahonyInitFromAccel(f, a[0], a[1], a[2]));
  const float biased[3] = {rad(1.0f), 0, 0};  // 1 deg/s left in the X rate
  run(f, biased, a, 50 * 120);               // 2 minutes
  const FusionEuler e = mahonyEuler(f);
  assert(std::fabs(e.roll) < 0.5f);
  assert(near(f.ix, -rad(1.0f), rad(0.2f)));
}

static void testGyroBias() {
  GyroBiasEstimator still;
  const float offset[3] = {0.01f, -0.02f, 0.005f};
  for (uint16_t i = 0; i + 1 < kGyroBiasSamples; ++i) {
    const float g[3] = {offset[0] + ((i & 1) ? 0.001f : -0.001f), offset[1], offset[2]};
    assert(!gyroBiasAdd(still, g));
  }
  assert(gyroBiasAdd(still, offset) && still.done && still.still);
  assert(near(still.bias[0], 0.01f, 0.0005f) && near(still.bias[1], -0.02f, 0.0001f) &&
         near(still.bias[2], 0.005f, 0.0001f));
  assert(gyroBiasAdd(still, offset));  // done stays done

  // Picked up while it started: no bias (the integral term handles it).
  GyroBiasEstimator moving;
  for (uint16_t i = 0; i < kGyroBiasSamples; ++i) {
    const float g[3] = {(i & 1) ? 0.5f : -0.5f, 0, 0};
    gyroBiasAdd(moving, g);
  }
  assert(moving.done && !moving.still && moving.bias[0] == 0.0f && moving.bias[1] == 0.0f);
}

static void testMagHook() {
  MagCalibration cal;  // not calibrated: the identity
  float m[3] = {10, -20, 30};
  magCorrect(cal, m);
  assert(m[0] == 10 && m[1] == -20 && m[2] == 30);
  cal.valid = true;
  cal.offset[0] = 5;
  cal.scale[1] = 2;
  magCorrect(cal, m);
  assert(m[0] == 5 && m[1] == -40 && m[2] == 30);
}

int main() {
  testFlat();
  testKnownPoses();
  testConvergesFromWrongStart();
  testGyroTurn();
  testBadStepsAreIgnored();
  testAccelGate();
  testIntegralLearnsBias();
  testGyroBias();
  testMagHook();
  std::puts("motion_fusion_tests: all passed");
  return 0;
}
