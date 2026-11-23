// HAL_Motion_Fusion.h - orientation from a gyroscope and an accelerometer for
// the motion backends that do not fuse on the chip (the Adafruit 4517).
//
// - A Mahony complementary filter, written from the published equations
//   (R. Mahony, T. Hamel, J.-M. Pflimlin, "Nonlinear Complementary Filters on
//   the Special Orthogonal Group", IEEE TAC 53(5), 2008): the gyro rate is
//   corrected by the cross product of the measured and the estimated gravity
//   direction (proportional and integral terms), then integrated as the
//   quaternion kinematics q' = 1/2 q (x) (0, w).
// - A start-up gyro bias estimate (an average over about a second while the
//   board is still).
// - The Euler angles in the readings' convention.
// - The magnetometer's hard/soft-iron correction hook (no calibration yet).
//
// Frame: the sensor frame, right-handed, +Z out of the chip's top (lying flat
// face up the accelerometer reads +g on Z). The filter's quaternion turns
// sensor vectors into a frame whose +Z is up.
//
// Output angles (degrees), the ranges the BNO055 readings use:
//   pitch -90..90  positive when the board's +X end rises;
//   roll -180..180 positive when its +Y edge rises (right-hand turn about +X);
//   yaw 0..360     growing clockwise seen from above. Without a magnetometer
//                  in the filter it is relative to the start and drifts
//                  slowly (hasHeading false).
// Pure header: no Arduino, FreeRTOS or ESP-IDF dependency; float maths only.
#pragma once

#include <math.h>
#include <stdint.h>

// ---- Mahony filter ---------------------------------------------------------------
// Gains: Kp (rad/s per unit of gravity-direction error) sets how fast the
// tilt follows the accelerometer (about 1/Kp seconds); Ki slowly learns the
// gyro bias the start-up average missed (tilt axes only: a 6-axis filter
// cannot see a bias about the vertical).
constexpr float kMahonyKp = 1.0f;
constexpr float kMahonyKi = 0.02f;
constexpr float kMahonyIntegralLimit = 0.1f;  // rad/s: the learned bias stays small
// The accelerometer corrects the tilt only while it reads about 1 g (not in
// a shake, a knock or a fall); the gyro carries the estimate meanwhile.
constexpr float kMahonyAccelGateLow = 0.75f;  // x 1 g
constexpr float kMahonyAccelGateHigh = 1.25f;
constexpr float kMahonyGravity = 9.80665f;
// A step longer than this (a stall, a missed tick) is not integrated.
constexpr float kMahonyMaxDtS = 0.25f;
constexpr float kFusionRadToDeg = 57.29577951308232f;

struct MahonyFilter {
  float q0 = 1.0f, q1 = 0.0f, q2 = 0.0f, q3 = 0.0f;  // sensor -> up frame
  float ix = 0.0f, iy = 0.0f, iz = 0.0f;            // integral feedback (rad/s)
  float kp = kMahonyKp, ki = kMahonyKi;
};

inline float fusionClamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline void mahonyNormalize(MahonyFilter& f) {
  const float n = sqrtf(f.q0 * f.q0 + f.q1 * f.q1 + f.q2 * f.q2 + f.q3 * f.q3);
  if (!(n > 0.0f)) {
    f.q0 = 1.0f;
    f.q1 = f.q2 = f.q3 = 0.0f;
    return;
  }
  f.q0 /= n;
  f.q1 /= n;
  f.q2 /= n;
  f.q3 /= n;
}

// Start from the tilt one accelerometer reading shows (yaw 0), so the first
// angles are right without waiting for the filter to converge. False (and
// the filter at rest) when the reading has no direction.
inline bool mahonyInitFromAccel(MahonyFilter& f, float ax, float ay, float az) {
  f = MahonyFilter{};
  const float n = sqrtf(ax * ax + ay * ay + az * az);
  if (!(n > 0.0f)) return false;
  const float roll = atan2f(ay, az);                        // about +X
  const float pitch = atan2f(-ax, sqrtf(ay * ay + az * az));  // about +Y (positive tips +X down)
  const float cr = cosf(roll * 0.5f), sr = sinf(roll * 0.5f);
  const float cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f);
  // Z-Y-X rotation with yaw 0.
  f.q0 = cr * cp;
  f.q1 = sr * cp;
  f.q2 = cr * sp;
  f.q3 = -sr * sp;
  mahonyNormalize(f);
  return true;
}

// One step: gyro in rad/s (bias already taken off), accel in any unit
// (m/s^2 here), dt in seconds. False, and nothing changes, for a dt that is
// zero, negative, not a number or longer than kMahonyMaxDtS.
inline bool mahonyUpdate(MahonyFilter& f, float gx, float gy, float gz, float ax, float ay, float az,
                         float dt) {
  if (!(dt > 0.0f && dt <= kMahonyMaxDtS)) return false;
  const float norm = sqrtf(ax * ax + ay * ay + az * az);
  const float g = norm / kMahonyGravity;
  if (norm > 0.0f && g >= kMahonyAccelGateLow && g <= kMahonyAccelGateHigh) {
    ax /= norm;
    ay /= norm;
    az /= norm;
    // "Up" in the sensor frame as the quaternion has it (the third row of
    // its rotation matrix).
    const float vx = 2.0f * (f.q1 * f.q3 - f.q0 * f.q2);
    const float vy = 2.0f * (f.q0 * f.q1 + f.q2 * f.q3);
    const float vz = f.q0 * f.q0 - f.q1 * f.q1 - f.q2 * f.q2 + f.q3 * f.q3;
    // Error: measured x estimated.
    const float ex = ay * vz - az * vy;
    const float ey = az * vx - ax * vz;
    const float ez = ax * vy - ay * vx;
    if (f.ki > 0.0f) {
      f.ix = fusionClamp(f.ix + f.ki * ex * dt, -kMahonyIntegralLimit, kMahonyIntegralLimit);
      f.iy = fusionClamp(f.iy + f.ki * ey * dt, -kMahonyIntegralLimit, kMahonyIntegralLimit);
      f.iz = fusionClamp(f.iz + f.ki * ez * dt, -kMahonyIntegralLimit, kMahonyIntegralLimit);
    }
    gx += f.kp * ex + f.ix;
    gy += f.kp * ey + f.iy;
    gz += f.kp * ez + f.iz;
  } else {
    gx += f.ix;
    gy += f.iy;
    gz += f.iz;
  }
  // q' = 1/2 q (x) (0, w)
  const float h = 0.5f * dt;
  const float a = f.q0, b = f.q1, c = f.q2, d = f.q3;
  f.q0 += h * (-b * gx - c * gy - d * gz);
  f.q1 += h * (a * gx + c * gz - d * gy);
  f.q2 += h * (a * gy - b * gz + d * gx);
  f.q3 += h * (a * gz + b * gy - c * gx);
  mahonyNormalize(f);
  return true;
}

struct FusionEuler {
  float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;  // degrees, see the header
};

// The quaternion as the readings' Euler angles (Z-Y-X order). The filter's
// own Z-Y-X angles have pitch positive with +X tipping down and yaw growing
// counter-clockwise; both signs are turned to the readings' convention.
inline FusionEuler mahonyEuler(const MahonyFilter& f) {
  FusionEuler out;
  const float q0 = f.q0, q1 = f.q1, q2 = f.q2, q3 = f.q3;
  out.roll = atan2f(2.0f * (q0 * q1 + q2 * q3), 1.0f - 2.0f * (q1 * q1 + q2 * q2)) * kFusionRadToDeg;
  out.pitch = -asinf(fusionClamp(2.0f * (q0 * q2 - q3 * q1), -1.0f, 1.0f)) * kFusionRadToDeg;
  float yaw = -atan2f(2.0f * (q0 * q3 + q1 * q2), 1.0f - 2.0f * (q2 * q2 + q3 * q3)) * kFusionRadToDeg;
  if (yaw < 0.0f) yaw += 360.0f;
  if (yaw >= 360.0f) yaw -= 360.0f;
  out.yaw = yaw;
  return out;
}

// ---- Gyro bias at start ------------------------------------------------------------
// About a second of samples at the fusion rate (50 Hz). When the spread shows
// the board was moving, no bias is taken (zero) and the filter's integral
// term learns the tilt-axis part instead.
constexpr uint16_t kGyroBiasSamples = 50;
constexpr float kGyroBiasMaxStdRadS = 0.02f;  // about 1.1 deg/s

struct GyroBiasEstimator {
  uint16_t count = 0;
  float sum[3] = {0, 0, 0};
  float sumSq[3] = {0, 0, 0};
  float bias[3] = {0, 0, 0};
  bool done = false;
  bool still = false;  // the bias came from a still board (false: zero bias)
};

// Add one gyro sample (rad/s); true once the estimate is done (bias set).
inline bool gyroBiasAdd(GyroBiasEstimator& e, const float g[3]) {
  if (e.done) return true;
  for (int i = 0; i < 3; ++i) {
    e.sum[i] += g[i];
    e.sumSq[i] += g[i] * g[i];
  }
  if (++e.count < kGyroBiasSamples) return false;
  bool still = true;
  float mean[3];
  for (int i = 0; i < 3; ++i) {
    mean[i] = e.sum[i] / e.count;
    const float variance = e.sumSq[i] / e.count - mean[i] * mean[i];
    if (variance > kGyroBiasMaxStdRadS * kGyroBiasMaxStdRadS) still = false;
  }
  for (int i = 0; i < 3; ++i) e.bias[i] = still ? mean[i] : 0.0f;
  e.still = still;
  e.done = true;
  return true;
}

// ---- Magnetometer correction hook -----------------------------------------------------
// Hard-iron offset (uT) and diagonal soft-iron scale, applied in the
// accelerometer frame after the board's axis map. Not calibrated yet: the
// identity, and the backends report hasHeading false.
// TODO(mag-cal): a later change adds 'imucalibrate mag' (a timed min/max
// sweep on the IMU task, stored as one settings batch by the command), sets
// valid, feeds the corrected field to a 9-axis update and reports a heading.
struct MagCalibration {
  float offset[3] = {0, 0, 0};
  float scale[3] = {1, 1, 1};
  bool valid = false;
};

inline void magCorrect(const MagCalibration& cal, float m[3]) {
  if (!cal.valid) return;
  for (int i = 0; i < 3; ++i) m[i] = (m[i] - cal.offset[i]) * cal.scale[i];
}
