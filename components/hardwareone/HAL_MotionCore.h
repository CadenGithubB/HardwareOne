// HAL_MotionCore.h - the motion (IMU) layer's pure rules, shared by every
// backend: the start contract, the orientation correction and the motion
// action detector (shake, tilt, tap, rotation, free fall, steps, pose).
//
// HAL_Motion.cpp runs these on the IMU task; the host tests run them on
// synthetic samples (test_motion_core.cpp). Pure header: no Arduino, FreeRTOS
// or ESP-IDF dependency, nothing allocated.
#pragma once

#include <math.h>
#include <stdint.h>
#include <stdio.h>

// ---- Start contract ---------------------------------------------------------
// imuStartInternal() waits kMotionStartWaitMs for the IMU task's init. Every
// backend's init ends by itself within kMotionInitBudgetMs (its own deadline,
// waits outside the bus lock), so the start always reports the backend's own
// reason and never a bare timeout while an init is still running.
constexpr uint32_t kMotionStartWaitMs = 3000;
constexpr uint32_t kMotionInitBudgetMs = 2500;
static_assert(kMotionInitBudgetMs + 200 <= kMotionStartWaitMs,
              "a backend init must end well inside the start wait");

// Which backend filled a reading (ImuCache::backend). One backend per build
// (IMU_DEVICE_TYPE in System_BuildConfig.h).
constexpr uint8_t kMotionBackendNone = 0;
constexpr uint8_t kMotionBackendBno055 = 1;
constexpr uint8_t kMotionBackendLsm6dsox = 2;  // the Adafruit 4517 (LSM6DSOX + LIS3MDL)

// ImuCache::cal when the backend reports no calibration state.
constexpr uint8_t kMotionCalNone = 0xFF;

// ---- Orientation correction --------------------------------------------------
// The mounting correction applied to every reading before it is stored, when
// switched on (off by default: the readings are the chip's own). The web 3D
// view clamps its own pitch for drawing; the stored data is never clamped.
struct MotionOrientationSettings {
  bool enabled = false;
  int mode = 0;  // 0..8, see the settings options (imuorientationmode)
  float pitchOffset = 0.0f, rollOffset = 0.0f, yawOffset = 0.0f;
};

inline void motionApplyOrientationCorrection(const MotionOrientationSettings& settings,
                                             float& pitch, float& roll, float& yaw) {
  if (!settings.enabled) return;

  // Manual offsets first.
  pitch += settings.pitchOffset;
  roll += settings.rollOffset;
  yaw += settings.yawOffset;

  switch (settings.mode) {
    case 0:  // Normal - no correction
      break;
    case 1:  // Flip pitch (device upside down)
      pitch = -pitch;
      break;
    case 2:  // Flip roll (device rotated 180 degrees around the forward axis)
      roll = -roll;
      break;
    case 3:  // Flip yaw (device facing backwards)
      yaw = yaw + 180.0f;
      if (yaw > 360.0f) yaw -= 360.0f;
      break;
    case 4:  // Flip pitch and roll (device upside down and rotated)
      pitch = -pitch;
      roll = -roll;
      break;
    case 5:  // Roll values flipped around +-180 degrees
      if (roll > 90.0f) {
        roll = 180.0f - roll;
      } else if (roll < -90.0f) {
        roll = -180.0f - roll;
      }
      break;
    case 6: {  // Rotated 90 degrees counter-clockwise (pitch and roll swapped)
      const float origPitch = pitch;
      pitch = roll;
      roll = origPitch;
      break;
    }
    case 7: {  // Alternative mapping for extreme pitch values
      const float origPitch = pitch, origYaw = yaw;
      pitch = origYaw - 270.0f;
      if (pitch < -180.0f) pitch += 360.0f;
      if (pitch > 180.0f) pitch -= 360.0f;
      yaw = origPitch + 180.0f;
      if (yaw < 0.0f) yaw += 360.0f;
      if (yaw >= 360.0f) yaw -= 360.0f;
      break;
    }
    case 8: {  // Upside down: roll around +-180 degrees, pitch small
      const float origPitch = pitch;
      float normalizedRoll = roll;
      if (normalizedRoll > 90.0f) {
        normalizedRoll = 180.0f - normalizedRoll;
      } else if (normalizedRoll < -90.0f) {
        normalizedRoll = -180.0f - normalizedRoll;
      }
      pitch = normalizedRoll;
      roll = origPitch;
      break;
    }
    default:
      break;
  }

  // Normalize: yaw 0..360, roll -180..180. Pitch is not clamped.
  while (yaw < 0.0f) yaw += 360.0f;
  while (yaw >= 360.0f) yaw -= 360.0f;
  if (roll > 180.0f) roll -= 360.0f;
  if (roll < -180.0f) roll += 360.0f;
}

// ---- Motion actions -----------------------------------------------------------
// One sample as the detector reads it: accel in m/s^2, gyro in rad/s (the
// ImuCache units), pitch and roll in degrees after the orientation correction.
struct MotionActionSample {
  float ax = 0, ay = 0, az = 0;
  float gx = 0, gy = 0, gz = 0;
  float pitch = 0, roll = 0;
};

// Thresholds. Rotation is compared in rad/s, the unit the gyro is stored in.
constexpr float kMotionDegToRad = 0.017453292519943295f;
constexpr float kMotionShakeVariance = 15.0f;        // (m/s^2)^2 over the last 10 samples
constexpr float kMotionTiltDeg = 30.0f;
constexpr float kMotionTapAccel = 25.0f;             // m/s^2
constexpr float kMotionRotationDps = 100.0f;         // deg/s
constexpr float kMotionRotationRadS = kMotionRotationDps * kMotionDegToRad;
constexpr float kMotionFreefallAccel = 2.0f;         // m/s^2
constexpr unsigned long kMotionFreefallEventMs = 150;
constexpr float kMotionStepPeak = 12.0f, kMotionStepValley = 8.0f;
constexpr unsigned long kMotionStepMinMs = 200, kMotionStepMaxMs = 2000;
constexpr uint8_t kMotionDebounceSamples = 3;        // pose and walking events
constexpr int kMotionHistory = 10;

// Detector state: what the commands and the OLED page show, plus the
// detector's own history and debounce. One owner (the IMU task) updates it;
// readers copy it under the IMU cache lock (imuActionsSnapshot).
struct MotionActionState {
  bool isShaking = false;
  unsigned long lastShakeMs = 0;
  uint32_t shakeCount = 0;
  float shakeIntensity = 0;

  bool isTilted = false;
  float tiltAngle = 0;
  char tiltDirection = 'N';  // 'F'orward, 'B'ack, 'L'eft, 'R'ight, 'N'one

  bool tapDetected = false;
  unsigned long lastTapMs = 0;
  uint32_t tapCount = 0;
  float tapStrength = 0;

  bool isRotating = false;
  float rotationRateDps = 0;  // deg/s, for display
  char rotationAxis = 'N';    // 'X', 'Y', 'Z' or 'N'

  bool isFreefalling = false;
  unsigned long freefallStartMs = 0;
  uint32_t freefallDurationMs = 0;
  bool freefallPosted = false;

  bool isWalking = false;
  uint32_t stepCount = 0;
  unsigned long lastStepMs = 0;
  float stepFrequency = 0;  // steps per minute, over the last minute
  float lastAccelMag = 0;
  bool stepPeakDetected = false;
  unsigned long stepWindowStartMs = 0;
  uint32_t stepsInWindow = 0;
  bool walkPosted = false;      // the walking state the last event announced
  bool walkCandidate = false;
  uint8_t walkStable = 0;

  // 'P'ortrait, 'L'andscape, 'U'pside-down, 'R'everse landscape, 'F'ace-up, 'D'own
  char orientation = 'F';
  char lastOrientation = 'F';
  unsigned long lastOrientationChangeMs = 0;
  char posePosted = 0;          // the pose the last event announced (0: none yet)
  char poseCandidate = 0;
  uint8_t poseStable = 0;

  float accelHistory[kMotionHistory] = {};
  int accelHistoryIndex = 0;
  bool historySeeded = false;   // the first sample fills the history
};

enum class MotionEventKind : uint8_t { Shake, Tap, Freefall, Walking, Orientation };
struct MotionEvent {
  MotionEventKind kind;
  char subject[16];
  char detail[20];  // empty: the event has no detail
};
// At most one event of each kind per sample.
struct MotionEvents {
  MotionEvent items[5];
  uint8_t count = 0;
  void add(MotionEventKind kind, const char* subject, const char* detail = "") {
    if (count >= sizeof(items) / sizeof(items[0])) return;
    MotionEvent& event = items[count++];
    event.kind = kind;
    snprintf(event.subject, sizeof(event.subject), "%s", subject ? subject : "");
    snprintf(event.detail, sizeof(event.detail), "%s", detail ? detail : "");
  }
};

inline const char* motionPoseName(char pose) {
  switch (pose) {
    case 'F': return "face-up";
    case 'D': return "face-down";
    case 'P': return "portrait";
    case 'U': return "portrait-inv";
    case 'R': return "landscape-r";
    case 'L': return "landscape-l";
    default:  return "?";
  }
}

// Run the detector on one new sample at `now` (ms). The events of this sample
// go to `events` (may be null). Call it once per sample, from the one task
// that owns the state; a renderer copies the state instead of calling this.
inline void motionActionsUpdate(MotionActionState& s, const MotionActionSample& in,
                                unsigned long now, MotionEvents* events) {
  const float accelMag = sqrtf(in.ax * in.ax + in.ay * in.ay + in.az * in.az);

  // History of |accel|; the first sample fills it, so a start is not a shake.
  if (!s.historySeeded) {
    for (float& value : s.accelHistory) value = accelMag;
    s.lastAccelMag = accelMag;
    s.historySeeded = true;
  }
  s.accelHistory[s.accelHistoryIndex] = accelMag;
  s.accelHistoryIndex = (s.accelHistoryIndex + 1) % kMotionHistory;

  // 1. Shake: variance of |accel| over the history.
  float mean = 0.0f, variance = 0.0f;
  for (float value : s.accelHistory) mean += value;
  mean /= kMotionHistory;
  for (float value : s.accelHistory) variance += (value - mean) * (value - mean);
  variance /= kMotionHistory;
  const float intensity = fminf(variance / 50.0f, 1.0f);
  if (variance > kMotionShakeVariance) {
    if (!s.isShaking) {
      s.shakeCount++;
      if (events) {
        char text[12];
        snprintf(text, sizeof(text), "%.2f", (double)intensity);
        events->add(MotionEventKind::Shake, text);
      }
    }
    s.isShaking = true;
    s.lastShakeMs = now;
    s.shakeIntensity = intensity;
  } else if (now - s.lastShakeMs > 500) {
    s.isShaking = false;
    s.shakeIntensity = 0.0f;
  }

  // 2. Tilt past the threshold, by the larger of pitch and roll.
  const float maxTilt = fmaxf(fabsf(in.roll), fabsf(in.pitch));
  s.tiltAngle = maxTilt;
  if (maxTilt > kMotionTiltDeg) {
    s.isTilted = true;
    if (fabsf(in.pitch) > fabsf(in.roll)) {
      s.tiltDirection = (in.pitch > 0) ? 'F' : 'B';
    } else {
      s.tiltDirection = (in.roll > 0) ? 'R' : 'L';
    }
  } else {
    s.isTilted = false;
    s.tiltDirection = 'N';
  }

  // 3. Tap / knock: a sharp |accel| spike, at most one per 200 ms.
  if (accelMag > kMotionTapAccel && (now - s.lastTapMs) > 200) {
    s.tapDetected = true;
    s.lastTapMs = now;
    s.tapCount++;
    s.tapStrength = fminf((accelMag - kMotionTapAccel) / 20.0f, 1.0f);
    if (events) {
      char text[12];
      snprintf(text, sizeof(text), "%.2f", (double)s.tapStrength);
      events->add(MotionEventKind::Tap, text);
    }
  } else if (now - s.lastTapMs > 500) {
    s.tapDetected = false;
    s.tapStrength = 0.0f;
  }

  // 4. Rotation: the fastest axis against the threshold, both in rad/s.
  const float maxGyro = fmaxf(fabsf(in.gx), fmaxf(fabsf(in.gy), fabsf(in.gz)));
  s.rotationRateDps = maxGyro / kMotionDegToRad;
  if (maxGyro > kMotionRotationRadS) {
    s.isRotating = true;
    if (fabsf(in.gx) > fabsf(in.gy) && fabsf(in.gx) > fabsf(in.gz)) {
      s.rotationAxis = 'X';
    } else if (fabsf(in.gy) > fabsf(in.gz)) {
      s.rotationAxis = 'Y';
    } else {
      s.rotationAxis = 'Z';
    }
  } else {
    s.isRotating = false;
    s.rotationAxis = 'N';
  }

  // 5. Free fall: near-zero |accel|. The event waits 150 ms, because a shake
  // drives |accel| through zero at each reversal.
  if (accelMag < kMotionFreefallAccel) {
    if (!s.isFreefalling) s.freefallStartMs = now;
    s.isFreefalling = true;
    s.freefallDurationMs = (uint32_t)(now - s.freefallStartMs);
    if (!s.freefallPosted && s.freefallDurationMs >= kMotionFreefallEventMs) {
      s.freefallPosted = true;
      if (events) {
        char text[16];
        snprintf(text, sizeof(text), "%lums", (unsigned long)s.freefallDurationMs);
        events->add(MotionEventKind::Freefall, text);
      }
    }
  } else {
    s.isFreefalling = false;
    s.freefallDurationMs = 0;
    s.freefallPosted = false;
  }

  // 6. Steps: a peak above 12 m/s^2 then a valley below 8 m/s^2.
  if (accelMag > kMotionStepPeak && s.lastAccelMag < kMotionStepPeak) {
    if (!s.stepPeakDetected && (now - s.lastStepMs) > kMotionStepMinMs) s.stepPeakDetected = true;
  }
  if (accelMag < kMotionStepValley && s.stepPeakDetected) {
    s.stepCount++;
    s.lastStepMs = now;
    s.stepPeakDetected = false;
    s.stepsInWindow++;
  }
  if (s.stepCount > 0 && now - s.lastStepMs < kMotionStepMaxMs) {
    s.isWalking = true;
    if (now - s.stepWindowStartMs > 60000) {
      s.stepFrequency = (float)s.stepsInWindow;
      s.stepWindowStartMs = now;
      s.stepsInWindow = 0;
    }
  } else {
    s.isWalking = false;
    if (now - s.stepWindowStartMs > 60000) {
      s.stepFrequency = 0.0f;
      s.stepWindowStartMs = now;
      s.stepsInWindow = 0;
    }
  }
  s.lastAccelMag = accelMag;

  // Walking started/stopped once the new state held for 3 samples.
  if (s.isWalking == s.walkPosted) {
    s.walkStable = 0;
    s.walkCandidate = s.isWalking;
  } else if (s.isWalking == s.walkCandidate) {
    if (++s.walkStable >= kMotionDebounceSamples) {
      s.walkPosted = s.isWalking;
      s.walkStable = 0;
      if (events) {
        char cadence[20];
        snprintf(cadence, sizeof(cadence), "%.0f steps/min", (double)s.stepFrequency);
        events->add(MotionEventKind::Walking, s.isWalking ? "started" : "stopped", cadence);
      }
    }
  } else {
    s.walkCandidate = s.isWalking;
    s.walkStable = 1;
  }

  // 7. Pose: the axis nearest vertical.
  char pose = 'F';
  if (fabsf(in.az) > 7.0f) {
    pose = (in.az > 0) ? 'F' : 'D';
  } else if (fabsf(in.pitch) > 45.0f) {
    pose = (in.pitch > 0) ? 'P' : 'U';
  } else if (fabsf(in.roll) > 45.0f) {
    pose = (in.roll > 0) ? 'R' : 'L';
  }
  if (pose != s.orientation) {
    s.lastOrientation = s.orientation;
    s.orientation = pose;
    s.lastOrientationChangeMs = now;
  }
  // The pose event once the new pose held for 3 samples (it chatters at the
  // 45-degree boundaries). The first sample seeds it without an event.
  if (s.posePosted == 0) s.posePosted = pose;
  if (pose == s.posePosted) {
    s.poseStable = 0;
    s.poseCandidate = 0;
  } else if (pose == s.poseCandidate) {
    if (++s.poseStable >= kMotionDebounceSamples) {
      const char previous = s.posePosted;
      s.posePosted = pose;
      s.poseStable = 0;
      s.poseCandidate = 0;
      if (events) events->add(MotionEventKind::Orientation, motionPoseName(pose), motionPoseName(previous));
    }
  } else {
    s.poseCandidate = pose;
    s.poseStable = 1;
  }
}
