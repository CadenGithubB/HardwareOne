// Input_IMUCore.h - the IMU as an input device: the pure part.
//
// Turns the motion layer's per-sample stream (HAL_MotionHookCore.h) into what
// every other input device publishes: buttons and a joystick.
//
//   Taps     a run of taps is counted until no tap follows for tapWindowMs,
//            then the count is the button. Taps on the side (the knock
//            detector tells side from face; a chip that cannot tell counts
//            as side): 1 = A, 2 = B, 3 = Y, 4 = X. Taps on the face: 2 =
//            START, 3 = SELECT (1 is nothing: setting the device down or
//            bumping its face knocks once). Other counts: nothing. A run is
//            of one kind: a tap of the other kind ends it (its button goes
//            out) and starts a new run. (Every button arrives tapWindowMs
//            after its last tap.)
//   Shake    a sustained shake starts the configuration (shakeCalibrate),
//            one flow in stages, each reported by Output::calEvent for a
//            prompt:
//              settle  set the device down in the pose it will be used in;
//                      still for kCalStillMs: that pose is the new centre
//                      (not yet in use). Never still within kCalTimeoutMs:
//                      cancelled.
//              side    (Config::teachKnocks) the next kTeachKnocks taps are
//                      side examples (knock::Teacher, as imutapteach side);
//              face    then kTeachKnocks face ("top") examples. Each knock
//                      stage gives up after knock::kTeachTimeoutMs.
//            Only when every stage is done is anything changed: the centre
//            goes into use here and the caller saves both surfaces (from
//            sideTeach() / faceTeach()) off the IMU task. A cancel or a
//            time-out changes nothing. Taps press no button and the joystick
//            is off throughout. A shake during the configuration cancels it,
//            and the next configuration can start only from a shake that
//            begins kCalRearmMs after that one ended.
//            A shake here is strong acceleration for more than half of the
//            last 500 ms, counted from the samples themselves: a run of taps
//            is a few single strong samples and never reaches it. (The
//            motion layer's own isShaking is not used: it looks at 2 s, and
//            the tap rule must not depend on thresholds tuned for events.)
//   Tilt     the joystick: how far the device leans away from its reference
//            pose, one axis at a time. The reference is the pose it had when
//            input started, or at the last calibration / recentre request.
//
// Tilt is measured on the "up" vector (which way is up, in the sensor's
// frame), not on Euler angles: the difference of two up vectors is a lean
// whatever the reference pose is, so a device re-centred lying flat, standing
// or on its side behaves the same. Turning it about the vertical does nothing.
//
// Directions, at the reference pose, in the sensor's own axes:
//   right  the +X end drops          left  the +X end rises
//   up     the device turns about its +X axis so that its +Y side rises
//          (lying flat: the far edge lifts)
// The mount (Config::mountTurns / mountMirror, `imuinputmount`) fits that to
// how the board sits against the panel: mirrored first, then turned, as
// Input_RotationCore.h does it for every input device. The screen's own
// rotation is applied later, by the input layer. A device re-centred with its
// X axis vertical uses Y in X's place.
//
// Teach step (`imuinputlearn`): the user tilts toward the top of the screen;
// the first lean past the engage angle held for the dwell, in the device's
// own frame, is reported (Output::learnEvent / learnDir) and the caller sets
// the mount from it (input_rotation::learnQuarterTurns). No lean within
// kLearnTimeoutMs: it ends and nothing changes; a shake or a configuration
// cancels it. (It is no longer offered after a shake configuration: its face
// tap would be taken by the face stage.)
//
// The engine runs at one sample every 20 ms (kEnginePeriodMs, the Adafruit
// 4517's rate). The windows that are counted in samples (strong samples, the
// dwell, the centre capture) assume about that; the times are real
// milliseconds from each sample. A faster feed (the BNO055 with IMU input: one
// sample every 10 ms) goes through the Decimator below, which hands the engine
// every second sample and carries each tap, with its kind, to the next one.
//
// Pure header (C library only); host-tested by test_input_imu_core.cpp.
#pragma once

#include <math.h>
#include <stdint.h>

#include "HAL_Motion_KnockCore.h"  // knock::Teacher: the configuration's knock stages
#include "Input_RotationCore.h"

namespace imuinput {

// Buttons, at the input layer's logical bit positions (HAL_Input.h: bit ==
// InputButton value).
constexpr uint8_t kButtonA = 1u << 0;
constexpr uint8_t kButtonB = 1u << 1;
constexpr uint8_t kButtonX = 1u << 2;
constexpr uint8_t kButtonY = 1u << 3;
constexpr uint8_t kButtonStart = 1u << 4;
constexpr uint8_t kButtonSelect = 1u << 5;

// Where a tap hit (Sample::tapKind; the values of HAL_MotionHookCore.h's
// kMotionTapKind*). Unknown counts as side.
constexpr uint8_t kTapUnknown = 0;
constexpr uint8_t kTapSide = 1;
constexpr uint8_t kTapFace = 2;

// The configuration (a shake): what Output::calEvent reports.
constexpr uint8_t kCalNone = 0;
constexpr uint8_t kCalStarted = 1;    // set the device down in the pose to use
constexpr uint8_t kCalCountdown = 2;  // still: calSecondsLeft to go
constexpr uint8_t kCalMoved = 3;      // it moved during the countdown: waiting again
constexpr uint8_t kCalDone = 4;       // configured: the new centre is in use (and, with teachKnocks, both
                                      // surfaces are in sideTeach() / faceTeach() for the caller to save)
constexpr uint8_t kCalCancelled = 5;  // never still long enough: nothing changed
constexpr uint8_t kCalSideStart = 6;  // the centre is taken (not yet in use): knock the side
constexpr uint8_t kCalSideKnock = 7;  // a side knock taken: calKnocks so far
constexpr uint8_t kCalFaceStart = 8;  // the side is done: knock the top (the face)
constexpr uint8_t kCalFaceKnock = 9;  // a face knock taken: calKnocks so far
constexpr uint8_t kCalTimedOut = 10;  // a knock stage (calStage) had too few knocks in time: nothing changed
constexpr uint8_t kCalShakeCancelled = 11;  // a shake during the configuration (calStage): nothing changed
constexpr uint32_t kCalStillMs = 3000;
constexpr uint32_t kCalTimeoutMs = 20000;
// After a shake cancelled a configuration, a new one starts only from a shake
// that begins this long after that shake ended (and so on: every shake in the
// meantime pushes it on).
constexpr uint32_t kCalRearmMs = 2000;

// The configuration's stages (Engine::calStage, Output::calStage).
constexpr uint8_t kStageNone = 0;
constexpr uint8_t kStageSettle = 1;  // waiting for the device to be still
constexpr uint8_t kStageCentre = 2;  // taking the centre from calm samples
constexpr uint8_t kStageSide = 3;    // side knocks
constexpr uint8_t kStageFace = 4;    // face ("top") knocks
inline const char* stageName(uint8_t stage) {
  switch (stage) {
    case kStageSettle: return "settle";
    case kStageCentre: return "centre";
    case kStageSide: return "side";
    case kStageFace: return "face";
    default: return "none";
  }
}
// Still, for the calibration: acceleration without gravity under this.
constexpr float kCalStillAccel = 1.2f;  // m/s^2

// The teach step: the stages Output::learnEvent reports.
constexpr uint8_t kLearnNone = 0;
constexpr uint8_t kLearnStarted = 1;   // tilt toward the top of the screen
constexpr uint8_t kLearnDone = 2;      // learnDir is the lean, in the device's frame
constexpr uint8_t kLearnTimedOut = 3;  // no lean: nothing changes
constexpr uint8_t kLearnCancelled = 5; // a shake or a configuration ended it
constexpr uint32_t kLearnTimeoutMs = 10000;

// The joystick's scale, the Seesaw gamepad's (i2csensor_seesaw.h): 0..1023,
// centre 512, and the navigation's dead zone of 200 around it.
constexpr int kJoyCentre = 512;
constexpr int kJoyDeadZone = 200;
constexpr int kJoyMinDeflection = 250;  // just engaged: clear of the dead zone
constexpr int kJoyMaxDeflection = 511;

constexpr float kGravity = 9.80665f;
constexpr float kRadToDeg = 57.29577951308232f;

// A sample later than this after the previous one follows a gap: what was
// being counted is dropped (the reference pose is kept). Pauses of 105-130 ms
// happen on a shared I2C bus (bench 2026-11-23: they dropped runs of taps at
// a 100 ms limit), and the knock detector keeps its taps across them.
constexpr uint32_t kGapMs = 300;
// A button is shown as held this long after its press, so a reader of the
// level (a status page, a peer) sees it; far below any hold gesture.
constexpr uint32_t kHeldMs = 100;
// Tilt is not re-evaluated this long after a tap: the knock itself rocks the
// device.
constexpr uint32_t kTapFreezeMs = 150;
// How long a tap's knock rings: strong samples this soon after a tap flag
// (and the one sample before it) are the knock, not movement.
constexpr uint32_t kKnockMs = 60;
// After a shake ends, taps are ignored this long (the hand is settling).
constexpr uint32_t kAfterShakeMs = 300;
// A finished run of taps waits for the device to be calm before it becomes a
// button; one that has waited this long is dropped.
constexpr uint32_t kTapPatienceMs = 1000;

// A sample whose acceleration without gravity exceeds this is "strong".
constexpr float kStrongAccel = 6.0f;  // m/s^2
// The last kMotionWindow samples (500 ms) are remembered as strong or not.
constexpr int kMotionWindow = 25;
constexpr int kMovingCount = 6;   // this many strong: the device is being moved about
constexpr int kShakeCount = 13;   // this many: a shake (strong more than half the time)
constexpr int kShakeEndCount = 2; // down to this many: the shake is over

// A lean engages the joystick after this many samples in a row past the
// engage angle (60 ms: a knock's rocking is shorter).
constexpr int kDwellSamples = 3;
// Past this lean from the reference the joystick is centred: the device has
// been put in another pose, and would otherwise scroll until re-centred.
constexpr float kMaxTiltDeg = 60.0f;
// The reference pose is the mean of this many calm samples (200 ms).
constexpr int kCentreSamples = 10;

struct Config {
  uint16_t tapWindowMs = 350;  // a run of taps ends this long after its last tap
  uint16_t tapMinGapMs = 80;   // a second tap flag sooner than this is the same knock
  float engageDeg = 15.0f;     // lean that deflects the joystick
  float releaseDeg = 11.0f;    // lean under which it returns to centre
  float fullDeg = 30.0f;       // lean of a full deflection
  uint8_t mountTurns = 0;    // the mount: quarter turns (imuinputmount)
  bool mountMirror = false;  // and mirrored
  bool shakeCalibrate = true;  // a shake starts the configuration (off: a shake does nothing)
  // The configuration also teaches the side and the face from knocks (the
  // feed's taps carry Sample::tapVec: the firmware's knock detector). Off: it
  // takes the centre only.
  bool teachKnocks = false;
};

// One sample (the fields of MotionHookSample this core reads).
struct Sample {
  uint32_t ms = 0;
  float up[3] = {0, 0, 1};     // unit vector, sensor frame
  float accel[3] = {0, 0, 0};  // m/s^2, sensor frame
  bool tap = false;            // at least one single tap since the previous sample
  uint8_t tapKind = kTapUnknown;  // where it hit
  int16_t tapVec[3] = {0, 0, 0};  // with tap: the knock's peak jump (mg, sensor frame); zero when unknown
};

struct Output {
  uint8_t pressed = 0;   // buttons pressed by this sample (kButton*)
  uint8_t held = 0;      // buttons shown as held (the pressed ones, for kHeldMs)
  int joyX = kJoyCentre;
  int joyY = kJoyCentre;
  bool activity = false; // something a user did: a press, or the joystick deflected
  bool recentred = false; // a new reference pose was taken on this sample
  uint8_t calEvent = kCalNone;  // a configuration stage reached on this sample
  uint8_t calSecondsLeft = 0;   // with kCalCountdown
  uint8_t calKnocks = 0;        // with kCalSideKnock / kCalFaceKnock / kCalTimedOut: knocks taken in the stage
  uint8_t calStage = kStageNone;  // with kCalTimedOut / kCalShakeCancelled / kCalCancelled: the stage it ended in
  uint8_t learnEvent = kLearnNone;  // a teach-step stage reached on this sample
  uint8_t learnDir = 0;             // with kLearnDone: input_rotation::Dir in the device's frame
};

// Why a run of taps made no button (Engine::lastDrop).
constexpr uint8_t kDropNone = 0;
constexpr uint8_t kDropNeverCalm = 1;  // the device never got calm within kTapPatienceMs
constexpr uint8_t kDropGap = 2;        // the samples stopped (a gap)
constexpr uint8_t kDropShake = 3;      // a shake or a configuration started
constexpr uint8_t kDropNoButton = 4;   // the count has no button (1 face tap, 5+ side taps)
inline const char* dropName(uint8_t why) {
  switch (why) {
    case kDropNeverCalm: return "never-calm";
    case kDropGap: return "gap";
    case kDropShake: return "shake";
    case kDropNoButton: return "no-button";
    default: return "none";
  }
}

// What a finished run of `count` taps is: a button mask (0: nothing).
inline uint8_t buttonForTaps(uint8_t count, bool face) {
  if (face) {
    switch (count) {
      case 2: return kButtonStart;
      case 3: return kButtonSelect;
      default: return 0;
    }
  }
  switch (count) {
    case 1: return kButtonA;
    case 2: return kButtonB;
    case 3: return kButtonY;
    case 4: return kButtonX;
    default: return 0;
  }
}

inline float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// The two lean axes of a reference pose `up0` (unit): e1 is the sensor's X
// axis laid into the plane across up0 (Y when X is close to vertical), e2
// completes the right-handed set (up0 x e1). False when up0 is not a direction.
inline bool leanAxes(const float up0[3], float e1[3], float e2[3]) {
  const float n0 = sqrtf(dot3(up0, up0));
  if (!(n0 > 0.5f && n0 < 2.0f)) return false;
  const float u[3] = {up0[0] / n0, up0[1] / n0, up0[2] / n0};
  const float seed[3] = {fabsf(u[0]) > 0.8f ? 0.0f : 1.0f, fabsf(u[0]) > 0.8f ? 1.0f : 0.0f, 0.0f};
  const float along = dot3(seed, u);
  float a[3] = {seed[0] - along * u[0], seed[1] - along * u[1], seed[2] - along * u[2]};
  const float na = sqrtf(dot3(a, a));
  if (!(na > 0.1f)) return false;
  for (int i = 0; i < 3; ++i) e1[i] = a[i] / na;
  e2[0] = u[1] * e1[2] - u[2] * e1[1];
  e2[1] = u[2] * e1[0] - u[0] * e1[2];
  e2[2] = u[0] * e1[1] - u[1] * e1[0];
  return true;
}

// The lean of `up` from the reference, in degrees: x positive to the right,
// y positive up (the directions at the top of this file, before the swap and
// the inversions). False past kMaxTiltDeg (x and y are then 0).
inline bool leanDegrees(const float up[3], const float up0[3], const float e1[3], const float e2[3], float& x,
                        float& y) {
  x = y = 0.0f;
  const float c = dot3(up, up0);
  if (c <= cosf(kMaxTiltDeg / kRadToDeg)) return false;
  x = -atan2f(dot3(up, e1), c) * kRadToDeg;
  y = atan2f(dot3(up, e2), c) * kRadToDeg;
  return true;
}

// A lean of `deg` (past the engage angle, or in the band above release while
// engaged) as a joystick offset from centre.
inline int deflectionFor(float deg, const Config& cfg) {
  const float span = cfg.fullDeg - cfg.engageDeg;
  float t = span > 0.5f ? (deg - cfg.engageDeg) / span : 1.0f;
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  return kJoyMinDeflection + static_cast<int>(t * (kJoyMaxDeflection - kJoyMinDeflection) + 0.5f);
}

// ---- Decimation ---------------------------------------------------------------
// The engine's sample period, and how many samples of a feed make one engine
// update: 1 at 20 ms and slower, 2 at 10 ms (a backend's samplePeriodMs; 0:
// it samples every imuDevicePollMs, slower than this).
constexpr uint32_t kEnginePeriodMs = 20;
inline uint8_t decimationFor(uint16_t samplePeriodMs) {
  if (samplePeriodMs == 0 || samplePeriodMs * 3u >= kEnginePeriodMs * 2u) return 1;  // 14 ms and slower
  const uint32_t n = (kEnginePeriodMs + samplePeriodMs / 2u) / samplePeriodMs;
  return static_cast<uint8_t>(n > 8 ? 8 : n);
}

// Hands every `factor`-th sample to the engine. No tap is lost: a tap on a
// sample that is not handed over waits, with its kind and peak jump, for the
// next one that is; taps waiting there go out one per engine update, oldest
// first (two taps within one engine period are two knocks the detector told
// apart). With factor 1 every sample passes unchanged. A gap (kGapMs) drops
// what waits, as the engine drops its run.
class Decimator {
 public:
  void reset() { *this = Decimator(); }
  // True when `s` makes an engine update: `out` is then the sample to feed.
  bool add(const Sample& s, uint8_t factor, Sample& out) {
    if (have_ && static_cast<uint32_t>(s.ms - lastMs_) > kGapMs) {
      count_ = 0;
      since_ = 0;
    }
    have_ = true;
    lastMs_ = s.ms;
    if (s.tap && count_ < kMaxWaiting) {
      kinds_[count_] = s.tapKind;
      for (int i = 0; i < 3; ++i) vecs_[count_][i] = s.tapVec[i];
      ++count_;
    }
    if (++since_ < (factor ? factor : 1)) return false;
    since_ = 0;
    out = s;
    out.tap = count_ > 0;
    out.tapKind = kTapUnknown;
    for (int i = 0; i < 3; ++i) out.tapVec[i] = 0;
    if (out.tap) {
      out.tapKind = kinds_[0];
      for (int i = 0; i < 3; ++i) out.tapVec[i] = vecs_[0][i];
      for (uint8_t k = 1; k < count_; ++k) {
        kinds_[k - 1] = kinds_[k];
        for (int i = 0; i < 3; ++i) vecs_[k - 1][i] = vecs_[k][i];
      }
      --count_;
    }
    return true;
  }
  uint8_t waiting() const { return count_; }

 private:
  static constexpr uint8_t kMaxWaiting = 8;
  bool have_ = false;
  uint32_t lastMs_ = 0;
  uint8_t since_ = 0;    // samples since the last engine update
  uint8_t count_ = 0;    // taps waiting, oldest first:
  uint8_t kinds_[kMaxWaiting] = {};      // their kinds
  int16_t vecs_[kMaxWaiting][3] = {};    // and peak jumps
};

class Engine {
 public:
  // Forget everything, the reference pose included (input was started).
  void reset() { *this = Engine(); }

  // Start the teach step (or start it over): the next lean held past the
  // engage angle is reported as the top of the screen.
  void startLearn(uint32_t ms, Output* out = nullptr) {
    learning_ = true;
    learnStartMs_ = ms;
    learnDwell_ = 0;
    learnDwellDir_ = -1;
    releaseJoystick();
    if (out) out->learnEvent = kLearnStarted;
  }
  bool learning() const { return learning_; }

  // Take the present pose as the centre, from the next calm samples.
  void recentre() {
    capturing_ = true;
    captured_ = 0;
    sum_[0] = sum_[1] = sum_[2] = 0.0f;
    releaseJoystick();
  }

  Output update(const Sample& s, const Config& cfg) {
    Output out;
    if (have_ && static_cast<uint32_t>(s.ms - lastMs_) > kGapMs) dropRun();
    if (!have_) recentre();  // the first sample: the pose input starts in
    have_ = true;
    lastMs_ = s.ms;

    // ---- How much the device is being moved -------------------------------
    const float lin[3] = {s.accel[0] - kGravity * s.up[0], s.accel[1] - kGravity * s.up[1],
                          s.accel[2] - kGravity * s.up[2]};
    const bool strong = dot3(lin, lin) > kStrongAccel * kStrongAccel;
    const uint32_t mask = (1u << kMotionWindow) - 1u;
    strongBits_ = ((strongBits_ << 1) | (strong ? 1u : 0u)) & mask;
    const int strongCount = __builtin_popcount(strongBits_);
    const bool moving = strongCount >= kMovingCount;
    // Whether the device moved since the last tap's knock ended: a run of
    // firm taps rings for a few samples per knock, which is not "being moved
    // about" (bench 2026-11-23: four quick taps were dropped as never calm).
    // Only the samples after the knock (kKnockMs) count, as a share.
    if (s.tap) {
      knockUntilMs_ = s.ms + kKnockMs;
      afterKnock_ = 0;
      strongAfterKnock_ = 0;
    } else if (static_cast<int32_t>(s.ms - knockUntilMs_) >= 0) {
      if (afterKnock_ < kMotionWindow) {
        ++afterKnock_;
        if (strong) ++strongAfterKnock_;
      }
    }
    const bool movedSinceKnock =
        afterKnock_ > 0 && strongAfterKnock_ * kMotionWindow >= kMovingCount * afterKnock_;

    // ---- Shake ------------------------------------------------------------
    if (!inShake_ && strongCount >= kShakeCount) {
      inShake_ = true;
      if (tapCount_ > 0) noteDrop(kDropShake);
      tapCount_ = 0;
      if (calState_ != CalState::Idle) {
        // A shake during the configuration cancels it (nothing changes), and
        // this shake does not start a new one.
        cancelConfig(out, kCalShakeCancelled);
        calBlocked_ = true;
      }
    } else if (inShake_ && strongCount <= kShakeEndCount) {
      inShake_ = false;
      shakeEndMs_ = s.ms;
      tapsFromMs_ = s.ms + kAfterShakeMs;
      tapsBlocked_ = true;
      if (cfg.shakeCalibrate && !calBlocked_) {
        calState_ = CalState::Waiting;
        calStartMs_ = s.ms;
        calStillSinceMs_ = s.ms;
        calStill_ = false;
        calShown_ = 0;
        sideTeach_.cancel();
        faceTeach_.cancel();
        out.calEvent = kCalStarted;
      }
    }
    if (calBlocked_ && !inShake_ && static_cast<uint32_t>(s.ms - shakeEndMs_) >= kCalRearmMs) calBlocked_ = false;
    if (tapsBlocked_ && static_cast<int32_t>(s.ms - tapsFromMs_) >= 0) tapsBlocked_ = false;

    // ---- Configuration: settle ---------------------------------------------
    if (calState_ == CalState::Waiting) {
      const bool still = !s.tap && dot3(lin, lin) < kCalStillAccel * kCalStillAccel;
      if (!still) {
        if (calStill_ && out.calEvent == kCalNone) out.calEvent = kCalMoved;
        calStill_ = false;
        calShown_ = 0;
        calStillSinceMs_ = s.ms;
      } else {
        calStill_ = true;
        const uint32_t held = static_cast<uint32_t>(s.ms - calStillSinceMs_);
        if (held >= kCalStillMs) {
          calState_ = CalState::Capturing;
          recentre();
        } else {
          const uint8_t left = static_cast<uint8_t>((kCalStillMs - held + 999u) / 1000u);
          if (left != calShown_ && out.calEvent == kCalNone) {
            calShown_ = left;
            out.calEvent = kCalCountdown;
            out.calSecondsLeft = left;
          }
        }
      }
      if (calState_ == CalState::Waiting && static_cast<uint32_t>(s.ms - calStartMs_) > kCalTimeoutMs) {
        cancelConfig(out, kCalCancelled);
      }
    }

    // ---- Configuration: the knock stages ------------------------------------
    // Every tap is an example of the stage's surface (as imutapteach takes
    // them), whichever surface the detector sorted it to; none is a button
    // (the one that finishes the configuration neither).
    bool tapTaken = false;
    if (calState_ == CalState::Side || calState_ == CalState::Face) {
      tapTaken = s.tap;
      const bool side = calState_ == CalState::Side;
      knock::Teacher& t = side ? sideTeach_ : faceTeach_;
      if (s.tap) {
        knock::Event e;
        e.verdict = knock::Verdict::Tap;
        for (int i = 0; i < 3; ++i) e.peakVec[i] = s.tapVec[i];
        if (t.add(e)) {
          out.calEvent = side ? kCalSideKnock : kCalFaceKnock;
          out.calKnocks = t.got.count;
        }
      }
      if (t.state == knock::TeachState::Done) {
        if (side) {
          calState_ = CalState::Face;
          faceTeach_.start(false, s.ms);
          out.calEvent = kCalFaceStart;
          out.calKnocks = 0;
        } else {
          finishConfig(out);
        }
      } else if (t.expire(s.ms)) {
        cancelConfig(out, kCalTimedOut);
      }
    }
    const bool calibrating = calState_ != CalState::Idle;

    // ---- Taps -------------------------------------------------------------
    if (inShake_ || calibrating) {
      if (tapCount_ > 0) noteDrop(kDropShake);
      tapCount_ = 0;
    } else if (s.tap && !tapsBlocked_ && !tapTaken) {
      const bool face = s.tapKind == kTapFace;
      // A tap of the other kind ends the run in progress: its button goes out
      // now (unless the device was being moved about), and this tap starts a
      // new run.
      if (tapCount_ > 0 && face != runFace_) {
        const uint8_t what = movedSinceKnock ? 0 : buttonForTaps(tapCount_, runFace_);
        if (!what) noteDrop(movedSinceKnock ? kDropNeverCalm : kDropNoButton);
        tapCount_ = 0;
        if (what) press(out, what, s.ms);
      }
      if (tapCount_ == 0 || static_cast<uint32_t>(s.ms - lastTapMs_) >= cfg.tapMinGapMs) {
        if (tapCount_ == 0) runFace_ = face;
        if (tapCount_ < 15) ++tapCount_;
        lastTapMs_ = s.ms;
        freezeUntilMs_ = s.ms + kTapFreezeMs;
        frozen_ = true;
      }
    }
    if (tapCount_ > 0) {
      const uint32_t since = static_cast<uint32_t>(s.ms - lastTapMs_);
      if (since > cfg.tapWindowMs + kTapPatienceMs) {
        noteDrop(kDropNeverCalm);
        tapCount_ = 0;  // never calm enough to mean it
      } else if (since > cfg.tapWindowMs && !movedSinceKnock) {
        const uint8_t what = buttonForTaps(tapCount_, runFace_);
        if (!what) noteDrop(kDropNoButton);
        tapCount_ = 0;
        if (what) press(out, what, s.ms);
      }
    }
    if (frozen_ && static_cast<int32_t>(s.ms - freezeUntilMs_) >= 0) frozen_ = false;

    // ---- The reference pose -----------------------------------------------
    if (capturing_) {
      if (strong || inShake_) {
        captured_ = 0;
        sum_[0] = sum_[1] = sum_[2] = 0.0f;
      } else {
        for (int i = 0; i < 3; ++i) sum_[i] += s.up[i];
        if (++captured_ >= kCentreSamples) {
          const float n = sqrtf(dot3(sum_, sum_));
          float up0[3] = {0, 0, 0};
          if (n > 0.5f * kCentreSamples) {
            for (int i = 0; i < 3; ++i) up0[i] = sum_[i] / n;
          }
          float e1[3], e2[3];
          if (leanAxes(up0, e1, e2)) {
            capturing_ = false;
            if (calState_ == CalState::Capturing) {
              // The configuration's centre: kept aside until every stage is done.
              for (int i = 0; i < 3; ++i) {
                candUp0_[i] = up0[i];
                candE1_[i] = e1[i];
                candE2_[i] = e2[i];
              }
              if (cfg.teachKnocks) {
                calState_ = CalState::Side;
                sideTeach_.start(true, s.ms);
                out.calEvent = kCalSideStart;
              } else {
                finishConfig(out);
              }
            } else {
              useReference(up0, e1, e2, out);
            }
          } else {  // not a direction (the samples cancelled out): try again
            captured_ = 0;
            sum_[0] = sum_[1] = sum_[2] = 0.0f;
          }
        }
      }
    }

    // ---- The teach step ----------------------------------------------------
    if (learning_) {
      if (inShake_ || calState_ != CalState::Idle) {
        learning_ = false;  // a new shake or configuration ends it
        out.learnEvent = kLearnCancelled;
      } else if (static_cast<uint32_t>(s.ms - learnStartMs_) > kLearnTimeoutMs) {
        learning_ = false;
        out.learnEvent = kLearnTimedOut;
      } else if (haveRef_ && !capturing_ && !moving && !frozen_) {
        float x = 0, y = 0;
        int dir = -1;
        if (leanDegrees(s.up, up0_, e1_, e2_, x, y)) {
          const bool xLeads = fabsf(x) >= fabsf(y);
          const float v = xLeads ? x : y;
          if (fabsf(v) >= cfg.engageDeg) {
            dir = xLeads ? (v > 0 ? input_rotation::kRight : input_rotation::kLeft)
                         : (v > 0 ? input_rotation::kUp : input_rotation::kDown);
          }
        }
        if (dir >= 0 && dir == learnDwellDir_) {
          if (++learnDwell_ >= kDwellSamples) {
            learning_ = false;
            out.learnEvent = kLearnDone;
            out.learnDir = static_cast<uint8_t>(dir);
          }
        } else {
          learnDwellDir_ = static_cast<int8_t>(dir);
          learnDwell_ = dir >= 0 ? 1 : 0;
        }
      }
    }

    // ---- Tilt -------------------------------------------------------------
    if (!haveRef_ || capturing_ || moving || inShake_ || calState_ != CalState::Idle || learning_) {
      releaseJoystick();
    } else if (!frozen_) {
      float x = 0, y = 0;
      const bool inRange = leanDegrees(s.up, up0_, e1_, e2_, x, y);
      input_rotation::orientVector(input_rotation::Mount{cfg.mountTurns, cfg.mountMirror}, 0, x, y);
      leanX_ = x;
      leanY_ = y;
      if (!inRange) {
        releaseJoystick();
      } else if (axis_ != 0) {
        const float v = (axis_ == 1 ? x : y) * static_cast<float>(sign_);
        if (v < cfg.releaseDeg) releaseJoystick();
        else deflection_ = deflectionFor(v, cfg);
      } else {
        const bool xLeads = fabsf(x) >= fabsf(y);
        const float v = xLeads ? x : y;
        const int8_t axis = xLeads ? 1 : 2;
        const int8_t sign = v < 0 ? -1 : 1;
        if (fabsf(v) >= cfg.engageDeg) {
          if (axis == dwellAxis_ && sign == dwellSign_) ++dwell_;
          else dwell_ = 1;
          dwellAxis_ = axis;
          dwellSign_ = sign;
          if (dwell_ >= kDwellSamples) {
            axis_ = axis;
            sign_ = sign;
            deflection_ = deflectionFor(fabsf(v), cfg);
            dwell_ = 0;
            dwellAxis_ = 0;
            out.activity = true;
          }
        } else {
          dwell_ = 0;
          dwellAxis_ = 0;
        }
      }
    }
    if (axis_ == 1) out.joyX = kJoyCentre + sign_ * deflection_;
    if (axis_ == 2) out.joyY = kJoyCentre + sign_ * deflection_;
    if (axis_ != 0) out.activity = true;

    // ---- Held buttons -----------------------------------------------------
    if (held_ && static_cast<int32_t>(s.ms - heldUntilMs_) >= 0) held_ = 0;
    out.held = held_;
    if (out.pressed) out.activity = true;
    return out;
  }

  // State for a status reply.
  uint8_t tapCount() const { return tapCount_; }
  bool hasReference() const { return haveRef_; }
  bool capturing() const { return capturing_; }
  bool shaking() const { return inShake_; }
  bool calibrating() const { return calState_ != CalState::Idle; }
  // The configuration's stage (kStage*) and, in a knock stage, its knocks so
  // far and the time it has left.
  uint8_t calStage() const {
    switch (calState_) {
      case CalState::Waiting: return kStageSettle;
      case CalState::Capturing: return kStageCentre;
      case CalState::Side: return kStageSide;
      case CalState::Face: return kStageFace;
      default: return kStageNone;
    }
  }
  uint8_t calKnocks() const {
    return calState_ == CalState::Side ? sideTeach_.got.count : calState_ == CalState::Face ? faceTeach_.got.count : 0;
  }
  uint32_t calMsLeft(uint32_t ms) const {
    return calState_ == CalState::Side ? sideTeach_.msLeft(ms) : calState_ == CalState::Face ? faceTeach_.msLeft(ms) : 0;
  }
  // A shake would not start a configuration now (one was just cancelled by a shake).
  bool calBlocked() const { return calBlocked_; }
  // After kCalDone with Config::teachKnocks: the two finished steps (Done),
  // for the caller to save (knock::judgeTaught). Otherwise not Done.
  const knock::Teacher& sideTeach() const { return sideTeach_; }
  const knock::Teacher& faceTeach() const { return faceTeach_; }
  bool runOnFace() const { return runFace_; }
  // The last run of taps that made no button, and why (kDrop*).
  uint32_t drops() const { return drops_; }
  uint8_t lastDropTaps() const { return lastDropTaps_; }
  bool lastDropFace() const { return lastDropFace_; }
  uint8_t lastDrop() const { return lastDrop_; }
  float leanX() const { return leanX_; }  // degrees, after the mount
  float leanY() const { return leanY_; }
  const float* reference() const { return up0_; }

 private:
  void useReference(const float up0[3], const float e1[3], const float e2[3], Output& out) {
    for (int i = 0; i < 3; ++i) {
      up0_[i] = up0[i];
      e1_[i] = e1[i];
      e2_[i] = e2[i];
    }
    haveRef_ = true;
    out.recentred = true;
  }
  // Every stage done: the centre goes into use (the caller saves the surfaces).
  void finishConfig(Output& out) {
    calState_ = CalState::Idle;
    useReference(candUp0_, candE1_, candE2_, out);
    releaseJoystick();
    // The last knock's echo is no tap either (as after a shake).
    tapsBlocked_ = true;
    tapsFromMs_ = lastMs_ + kAfterShakeMs;
    out.calEvent = kCalDone;
  }
  // Ended before every stage was done: nothing changes (the old centre stays
  // in use, the taught steps are dropped).
  void cancelConfig(Output& out, uint8_t event) {
    out.calEvent = event;
    out.calStage = calStage();
    out.calKnocks = calKnocks();
    if (calState_ == CalState::Capturing && capturing_) {
      capturing_ = false;
      captured_ = 0;
      sum_[0] = sum_[1] = sum_[2] = 0.0f;
    }
    calState_ = CalState::Idle;
    sideTeach_.cancel();
    faceTeach_.cancel();
  }
  void noteDrop(uint8_t why) {
    ++drops_;
    lastDropTaps_ = tapCount_;
    lastDropFace_ = runFace_;
    lastDrop_ = why;
  }
  void press(Output& out, uint8_t buttons, uint32_t ms) {
    out.pressed |= buttons;
    held_ |= buttons;
    heldUntilMs_ = ms + kHeldMs;
  }
  void releaseJoystick() {
    axis_ = 0;
    sign_ = 0;
    deflection_ = 0;
    dwell_ = 0;
    dwellAxis_ = 0;
    dwellSign_ = 0;
  }
  // After a gap: nothing that was being counted is still true.
  void dropRun() {
    if (tapCount_ > 0) noteDrop(kDropGap);
    tapCount_ = 0;
    if (calState_ == CalState::Waiting) {  // the countdown starts over
      calStill_ = false;
      calShown_ = 0;
      calStillSinceMs_ = lastMs_;
    }
    strongBits_ = 0;
    afterKnock_ = strongAfterKnock_ = 0;
    inShake_ = false;
    tapsBlocked_ = false;
    frozen_ = false;
    held_ = 0;
    releaseJoystick();
    if (capturing_) {
      captured_ = 0;
      sum_[0] = sum_[1] = sum_[2] = 0.0f;
    }
  }

  bool have_ = false;
  uint32_t lastMs_ = 0;

  uint32_t strongBits_ = 0;
  uint32_t knockUntilMs_ = 0;   // a tap's knock rings until then
  int afterKnock_ = 0, strongAfterKnock_ = 0;  // samples (and strong ones) since it ended
  bool inShake_ = false;
  bool tapsBlocked_ = false;
  uint32_t tapsFromMs_ = 0;

  uint8_t tapCount_ = 0;
  bool runFace_ = false;  // the run in progress is of face taps
  uint32_t drops_ = 0;
  uint8_t lastDropTaps_ = 0;
  bool lastDropFace_ = false;
  uint8_t lastDrop_ = kDropNone;
  uint32_t lastTapMs_ = 0;

  enum class CalState : uint8_t { Idle = 0, Waiting, Capturing, Side, Face };
  CalState calState_ = CalState::Idle;
  bool calBlocked_ = false;  // a shake cancelled a configuration: none starts until kCalRearmMs after the last shake
  uint32_t shakeEndMs_ = 0;
  knock::Teacher sideTeach_, faceTeach_;  // the knock stages (Idle = 0: fine zero-filled)
  float candUp0_[3] = {0, 0, 0};  // the configuration's centre, until every stage is done
  float candE1_[3] = {0, 0, 0};
  float candE2_[3] = {0, 0, 0};
  uint32_t calStartMs_ = 0, calStillSinceMs_ = 0;
  bool calStill_ = false;
  uint8_t calShown_ = 0;  // the countdown second last reported
  bool frozen_ = false;
  uint32_t freezeUntilMs_ = 0;

  uint8_t held_ = 0;
  uint32_t heldUntilMs_ = 0;

  bool haveRef_ = false;
  bool capturing_ = false;
  int captured_ = 0;
  float sum_[3] = {0, 0, 0};
  // All members start at zero, so an Engine may sit in a zero-filled section
  // (the firmware keeps it in PSRAM); the reference is read only with haveRef_.
  float up0_[3] = {0, 0, 0};
  float e1_[3] = {0, 0, 0};
  float e2_[3] = {0, 0, 0};

  int8_t axis_ = 0;  // 0 centred, 1 X, 2 Y
  int8_t sign_ = 0;
  int deflection_ = 0;
  int dwell_ = 0;
  int8_t dwellAxis_ = 0;
  int8_t dwellSign_ = 0;
  float leanX_ = 0.0f, leanY_ = 0.0f;

  bool learning_ = false;
  uint32_t learnStartMs_ = 0;
  int learnDwell_ = 0;
  int8_t learnDwellDir_ = 0;
};

}  // namespace imuinput
