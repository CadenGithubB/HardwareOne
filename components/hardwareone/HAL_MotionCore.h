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
#include <string.h>

// ---- Start contract ---------------------------------------------------------
// imuStartInternal() waits kMotionStartWaitMs for the IMU task's init. Every
// backend's init ends by itself within kMotionInitBudgetMs (its own deadline,
// waits outside the bus lock), so the start always reports the backend's own
// reason and never a bare timeout while an init is still running. The budget
// counts the worst case of every bus access (a busy bus, a transfer that
// times out), not only the nominal waits; a normal start takes under a second.
constexpr uint32_t kMotionStartWaitMs = 4500;
constexpr uint32_t kMotionInitBudgetMs = 4000;
static_assert(kMotionInitBudgetMs + 200 <= kMotionStartWaitMs,
              "a backend init must end well inside the start wait");

// ---- The open address ----------------------------------------------------------
// While the IMU is open it reads, health-checks and claims the bus and
// address it was opened at; a changed imuaddress applies at the next open
// (as the ANO encoder's address does). Closed, the setting is what counts.
struct MotionOpenTarget {
  bool open = false;
  uint8_t bus = 0;
  uint8_t address = 0;
  uint8_t secondAddress = 0;  // a two-chip backend's second chip (the 4517's LIS3MDL), else 0
};

inline uint8_t motionActiveAddress(const MotionOpenTarget& target, uint8_t configured) {
  return target.open ? target.address : configured;
}
inline uint8_t motionActiveBus(const MotionOpenTarget& target, uint8_t configured) {
  return target.open ? target.bus : configured;
}
inline uint8_t motionActiveSecondAddress(const MotionOpenTarget& target, uint8_t configured) {
  return target.open ? target.secondAddress : configured;
}

// ---- Sampling ------------------------------------------------------------------
// A backend that fuses on the chip (the BNO055) is read once per
// imuDevicePollMs. A backend the layer fuses (the 4517) is read at its fixed
// period, the detector runs on every sample, and the cache (and so every
// reader) is refreshed once per imuDevicePollMs.
inline unsigned long motionSamplePeriodMs(uint16_t backendPeriodMs, unsigned long pollMs) {
  return backendPeriodMs ? backendPeriodMs : pollMs;
}
// Publish this sample? The first one always; then once pollMs has passed,
// within half a sample period (the reads jitter by a tick or two, so a
// sample read every pollMs is published every time).
inline bool motionPublishDue(bool publishedBefore, unsigned long lastPublishMs, unsigned long now,
                             unsigned long pollMs, unsigned long sampleMs) {
  return !publishedBefore || now - lastPublishMs + sampleMs / 2 >= pollMs;
}

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
  // A backend whose chip detects taps / free falls itself (the 4517's
  // LSM6DSOX, latched between samples) declares it (hasHw*); its flags then
  // replace the software rule, which a 50 Hz read would miss a ~10 ms knock by.
  bool hasHwTap = false, hwTap = false;
  bool hasHwFreefall = false, hwFreefall = false;
  // The time between samples (ms; 0: 200). The shake window's slot is at
  // least one sample long, so a slow poll still sees 10 samples.
  uint16_t periodMs = 0;
};

// Thresholds. Rotation is compared in rad/s, the unit the gyro is stored in.
constexpr float kMotionDegToRad = 0.017453292519943295f;
// Shake: the variance of |accel| over every sample of the last 10 slots of
// max(200 ms, one sample) each: 2 s whatever the rate (at the BNO055's 200 ms
// poll, the last 10 samples as before; at the 4517's 50 Hz, about 100, so a
// single knock and its ringing weigh a tenth as much and stay under the
// threshold, while a sustained shake has the same variance at either rate).
constexpr float kMotionShakeVariance = 15.0f;        // (m/s^2)^2
constexpr int kMotionShakeSlots = 10;
constexpr unsigned long kMotionShakeSlotMs = 200;
constexpr float kMotionTiltDeg = 30.0f;
constexpr float kMotionTapAccel = 25.0f;             // m/s^2
constexpr float kMotionRotationDps = 100.0f;         // deg/s
constexpr float kMotionRotationRadS = kMotionRotationDps * kMotionDegToRad;
constexpr float kMotionFreefallAccel = 2.0f;         // m/s^2
constexpr unsigned long kMotionFreefallEventMs = 150;
constexpr float kMotionStepPeak = 12.0f, kMotionStepValley = 8.0f;
constexpr unsigned long kMotionStepMinMs = 200, kMotionStepMaxMs = 2000;
// Walking needs at least two steps, each within kMotionStepMaxMs of the last
// (one peak-and-valley is also what a knock's ringing looks like).
constexpr uint32_t kMotionWalkMinSteps = 2;
// After a chip-detected tap, the knock's own samples (spike and ringing) are
// kept out of the shake window and the step detector for this long.
constexpr unsigned long kMotionTapQuietMs = 150;
// Pose and walking events wait for the new state to hold for 3 samples and at
// least 200 ms (at the BNO055's 200 ms poll, 3 samples; at the 4517's 50 Hz,
// about 11).
constexpr uint8_t kMotionDebounceSamples = 3;
constexpr unsigned long kMotionDebounceMs = 200;
// A chip-detected tap has no strength reading: it reports as full strength.
constexpr float kMotionHwTapStrength = 1.0f;

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
  unsigned long prevStepMs = 0;  // the step before lastStepMs
  float stepFrequency = 0;  // steps per minute, over the last minute
  float lastAccelMag = 0;
  bool stepPeakDetected = false;
  unsigned long stepWindowStartMs = 0;
  uint32_t stepsInWindow = 0;
  bool walkPosted = false;      // the walking state the last event announced
  bool walkCandidate = false;
  uint8_t walkStable = 0;
  unsigned long walkCandidateMs = 0;  // when the candidate state was first seen

  // 'P'ortrait, 'L'andscape, 'U'pside-down, 'R'everse landscape, 'F'ace-up, 'D'own
  char orientation = 'F';
  char lastOrientation = 'F';
  unsigned long lastOrientationChangeMs = 0;
  char posePosted = 0;          // the pose the last event announced (0: none yet)
  char poseCandidate = 0;
  uint8_t poseStable = 0;
  unsigned long poseCandidateMs = 0;  // when the candidate pose was first seen

  // The shake window: per slot, the sum and sum of squares of (|accel| - g)
  // and the sample count; shakeSlot is the current slot's number (time /
  // slot length), shakeSlotLenMs the slot length it was counted in.
  float shakeSum[kMotionShakeSlots] = {};
  float shakeSumSq[kMotionShakeSlots] = {};
  uint16_t shakeSamples[kMotionShakeSlots] = {};
  unsigned long shakeSlot = 0;
  unsigned long shakeSlotLenMs = 0;  // 0: no sample yet

  bool seeded = false;  // the first sample seeds lastAccelMag (a start is not a step)

  // A chip-detected tap's quiet time (kMotionTapQuietMs).
  bool tapQuiet = false;
  unsigned long tapQuietStartMs = 0;
};

// Add |accel| at `now` to the shake window and return the window's variance.
// A start is not a shake: the window holds only real samples.
inline float motionShakeWindowAdd(MotionActionState& s, float accelMag, unsigned long now, uint16_t periodMs,
                                  bool add) {
  const unsigned long slotMs = periodMs > kMotionShakeSlotMs ? periodMs : kMotionShakeSlotMs;
  const unsigned long slot = now / slotMs;
  if (s.shakeSlotLenMs != slotMs) {
    // First sample, or the poll period changed: start the window again.
    for (int i = 0; i < kMotionShakeSlots; ++i) {
      s.shakeSum[i] = s.shakeSumSq[i] = 0.0f;
      s.shakeSamples[i] = 0;
    }
    s.shakeSlotLenMs = slotMs;
    s.shakeSlot = slot;
  } else if (slot != s.shakeSlot) {
    // Clear the slots the window moved past (all of them after a long gap).
    const unsigned long moved = slot - s.shakeSlot;
    const unsigned long clear = moved < (unsigned long)kMotionShakeSlots ? moved : (unsigned long)kMotionShakeSlots;
    for (unsigned long k = 1; k <= clear; ++k) {
      const int i = (int)((s.shakeSlot + k) % kMotionShakeSlots);
      s.shakeSum[i] = s.shakeSumSq[i] = 0.0f;
      s.shakeSamples[i] = 0;
    }
    s.shakeSlot = slot;
  }
  if (add) {
    const int i = (int)(slot % kMotionShakeSlots);
    const float d = accelMag - 9.80665f;  // around g: the sums stay small
    s.shakeSum[i] += d;
    s.shakeSumSq[i] += d * d;
    if (s.shakeSamples[i] < 0xFFFF) s.shakeSamples[i]++;
  }
  float sum = 0.0f, sumSq = 0.0f;
  uint32_t n = 0;
  for (int i = 0; i < kMotionShakeSlots; ++i) {
    sum += s.shakeSum[i];
    sumSq += s.shakeSumSq[i];
    n += s.shakeSamples[i];
  }
  if (n == 0) return 0.0f;
  const float mean = sum / (float)n;
  const float variance = sumSq / (float)n - mean * mean;
  return variance > 0.0f ? variance : 0.0f;
}

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

  if (!s.seeded) {
    s.lastAccelMag = accelMag;
    s.seeded = true;
  }

  // A chip-detected tap starts a quiet time: the knock's own samples (the
  // spike and its ringing) stay out of the shake window and the steps.
  if (in.hasHwTap && in.hwTap) {
    s.tapQuiet = true;
    s.tapQuietStartMs = now;
    s.stepPeakDetected = false;
  } else if (s.tapQuiet && now - s.tapQuietStartMs > kMotionTapQuietMs) {
    s.tapQuiet = false;
  }

  // 1. Shake: variance of |accel| over the last 2 s (motionShakeWindowAdd).
  const float variance = motionShakeWindowAdd(s, accelMag, now, in.periodMs, !s.tapQuiet);
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

  // 3. Tap / knock: a sharp |accel| spike (or the chip's latched tap), at
  // most one per 200 ms.
  const bool tapSeen = in.hasHwTap ? in.hwTap : accelMag > kMotionTapAccel;
  if (tapSeen && (now - s.lastTapMs) > 200) {
    s.tapDetected = true;
    s.lastTapMs = now;
    s.tapCount++;
    s.tapStrength = in.hasHwTap ? kMotionHwTapStrength : fminf((accelMag - kMotionTapAccel) / 20.0f, 1.0f);
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
  // drives |accel| through zero at each reversal. A chip that latched a free
  // fall applied the same wait itself (the 4517: FF_DUR), so its flag posts
  // at once, reported as at least that long.
  const bool chipFall = in.hasHwFreefall && in.hwFreefall;
  if (accelMag < kMotionFreefallAccel || chipFall) {
    if (!s.isFreefalling) s.freefallStartMs = now;
    s.isFreefalling = true;
    s.freefallDurationMs = (uint32_t)(now - s.freefallStartMs);
    if (!s.freefallPosted && (s.freefallDurationMs >= kMotionFreefallEventMs || chipFall)) {
      s.freefallPosted = true;
      if (events) {
        char text[16];
        const uint32_t shown = s.freefallDurationMs >= kMotionFreefallEventMs
                                   ? s.freefallDurationMs : (uint32_t)kMotionFreefallEventMs;
        snprintf(text, sizeof(text), "%lums", (unsigned long)shown);
        events->add(MotionEventKind::Freefall, text);
      }
    }
  } else {
    s.isFreefalling = false;
    s.freefallDurationMs = 0;
    s.freefallPosted = false;
  }

  // 6. Steps: a peak above 12 m/s^2 then a valley below 8 m/s^2 (none in a
  // tap's quiet time). Walking: at least two steps, each within
  // kMotionStepMaxMs of the one before, the last one recent.
  if (!s.tapQuiet) {
    if (accelMag > kMotionStepPeak && s.lastAccelMag < kMotionStepPeak) {
      if (!s.stepPeakDetected && (now - s.lastStepMs) > kMotionStepMinMs) s.stepPeakDetected = true;
    }
    if (accelMag < kMotionStepValley && s.stepPeakDetected) {
      s.stepCount++;
      s.prevStepMs = s.lastStepMs;
      s.lastStepMs = now;
      s.stepPeakDetected = false;
      s.stepsInWindow++;
    }
  }
  const bool stepsInRow = s.stepCount >= kMotionWalkMinSteps && s.lastStepMs - s.prevStepMs < kMotionStepMaxMs;
  if (stepsInRow && now - s.lastStepMs < kMotionStepMaxMs) {
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

  // Walking started/stopped once the new state held (kMotionDebounce*).
  if (s.isWalking == s.walkPosted) {
    s.walkStable = 0;
    s.walkCandidate = s.isWalking;
  } else if (s.isWalking == s.walkCandidate && s.walkStable > 0) {
    if (++s.walkStable >= kMotionDebounceSamples && now - s.walkCandidateMs >= kMotionDebounceMs) {
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
    s.walkCandidateMs = now;
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
  // The pose event once the new pose held (kMotionDebounce*: it chatters at
  // the 45-degree boundaries). The first sample seeds it without an event.
  if (s.posePosted == 0) s.posePosted = pose;
  if (pose == s.posePosted) {
    s.poseStable = 0;
    s.poseCandidate = 0;
  } else if (pose == s.poseCandidate) {
    if (++s.poseStable >= kMotionDebounceSamples && now - s.poseCandidateMs >= kMotionDebounceMs) {
      const char previous = s.posePosted;
      s.posePosted = pose;
      s.poseStable = 0;
      s.poseCandidate = 0;
      if (events) events->add(MotionEventKind::Orientation, motionPoseName(pose), motionPoseName(previous));
    }
  } else {
    s.poseCandidate = pose;
    s.poseStable = 1;
    s.poseCandidateMs = now;
  }
}

// ---- Gyro offset state ----------------------------------------------------------
// How the start's gyro offset went, as 'imustatus' reports it ("gyroOffset"
// in its JSON):
//   chip       the chip keeps its own (a backend that fuses on the chip)
//   closed     measured at each start; the IMU is not open
//   measuring  being measured now (about a second with the board still)
//   still      measured on a board that held still
//   moving     the board moved for the whole measurement: no offset, and yaw
//              drifts faster until the IMU is opened again on a still board
inline const char* motionGyroOffsetState(bool measures, bool running, bool done, bool still) {
  if (!measures) return "chip";
  if (!running) return "closed";
  if (!done) return "measuring";
  return still ? "still" : "moving";
}
// The same state as a sentence part for the text reply.
inline const char* motionGyroOffsetText(const char* state) {
  if (strcmp(state, "chip") == 0) return "kept by the chip";
  if (strcmp(state, "closed") == 0) return "measured at each start";
  if (strcmp(state, "measuring") == 0) return "being measured (keep the board still)";
  if (strcmp(state, "still") == 0) return "measured while the board was still";
  return "not taken, the board was moving (yaw drifts faster; hold it still, then 'closeimu' and 'openimu')";
}

// ---- Compass point -------------------------------------------------------------
// The eight-point name of a heading (degrees clockwise from north), for the
// surfaces that show one. Only a reading with hasHeading has a heading.
inline const char* motionCompassPoint(float headingDeg) {
  static const char* const kPoints[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  if (!(headingDeg == headingDeg) || headingDeg > 1.0e6f || headingDeg < -1.0e6f) return "?";
  float h = fmodf(headingDeg, 360.0f);
  if (h < 0.0f) h += 360.0f;
  return kPoints[static_cast<int>((h + 22.5f) / 45.0f) % 8];
}

// ---- Automation condition variables ----------------------------------------------
// What an automation condition can ask the IMU (System_Automation.cpp,
// evaluateCondition; the variable name arrives upper-cased):
//   IMU_PITCH, IMU_ROLL  degrees, after the orientation correction (tilt)
//   IMU_TILT             the larger of |pitch| and |roll|, degrees
//   IMU_POSE             FACE-UP, FACE-DOWN, PORTRAIT, PORTRAIT-INV, LANDSCAPE-R
//                        or LANDSCAPE-L: the pose the last imu_orientation
//                        event announced (held for kMotionDebounce*), so
//                        IMU_POSE=FACE-DOWN is "flipped over"
//   IMU_TAP_AGE          seconds since the last tap
// There is no heading variable yet: yaw is a heading only with a calibrated
// magnetometer (hasHeading).
struct MotionConditionValue {
  bool isText = false;
  float number = 0.0f;
  char text[16] = "";  // upper case, as the condition's value is compared
};

inline bool motionConditionIsVariable(const char* name) {
  if (!name) return false;
  static const char* const kNames[] = {"IMU_PITCH", "IMU_ROLL", "IMU_TILT", "IMU_POSE", "IMU_TAP_AGE"};
  for (const char* known : kNames) {
    if (strcmp(name, known) == 0) return true;
  }
  return false;
}

// The value of one variable. False: there is none, so the condition is false
// (an unknown name, no valid reading, no pose yet, no tap since the IMU opened).
// pitch and roll are the reading's (ImuCache, after the orientation
// correction), `actions` the detector's state, `now` millis().
inline bool motionConditionValue(const char* name, bool readingValid, float pitch, float roll,
                                 const MotionActionState& actions, unsigned long now,
                                 MotionConditionValue& out) {
  out = MotionConditionValue{};
  if (!readingValid || !motionConditionIsVariable(name)) return false;
  if (strcmp(name, "IMU_PITCH") == 0) {
    out.number = pitch;
    return true;
  }
  if (strcmp(name, "IMU_ROLL") == 0) {
    out.number = roll;
    return true;
  }
  if (strcmp(name, "IMU_TILT") == 0) {
    out.number = fmaxf(fabsf(pitch), fabsf(roll));
    return true;
  }
  if (strcmp(name, "IMU_POSE") == 0) {
    if (actions.posePosted == 0) return false;
    const char* pose = motionPoseName(actions.posePosted);
    size_t i = 0;
    for (; pose[i] && i + 1 < sizeof(out.text); ++i) {
      const char c = pose[i];
      out.text[i] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
    }
    out.text[i] = '\0';
    out.isText = true;
    return true;
  }
  // IMU_TAP_AGE
  if (actions.tapCount == 0) return false;
  out.number = static_cast<float>(now - actions.lastTapMs) / 1000.0f;
  return true;
}
