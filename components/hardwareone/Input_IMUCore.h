// Input_IMUCore.h - the IMU as an input device: the pure part.
//
// Turns the motion layer's per-sample stream (HAL_MotionHookCore.h) into what
// every other input device publishes: buttons and a joystick.
//
//   Taps     a run of taps is counted until no tap follows for tapWindowMs,
//            then the count is the button: 1 = A, 2 = B, 3 = Y, 4 = X,
//            5 = take the present pose as the new centre. More than 5: nothing.
//            (So every button arrives tapWindowMs after its last tap.)
//   Shake    a sustained shake is START (shakeStart; off: a shake does nothing).
//            A shake here is strong acceleration for more than half of the
//            last 500 ms, counted from the samples themselves: a run of taps
//            is a few single strong samples and never reaches it. (The
//            motion layer's own isShaking is not used: it looks at 2 s, and
//            the tap rule must not depend on thresholds tuned for events.)
//   Tilt     the joystick: how far the device leans away from its reference
//            pose, one axis at a time. The reference is the pose it had when
//            input started, or at the last 5 taps / recentre request.
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
// swapXY / invertX / invertY fit that to how the board sits in its enclosure.
// A device re-centred with its X axis vertical uses Y in X's place.
//
// The rates are the Adafruit 4517's: one sample every 20 ms. The windows that
// are counted in samples (strong samples, the dwell, the centre capture)
// assume about that; the times are real milliseconds from each sample.
//
// Pure header (C library only); host-tested by test_input_imu_core.cpp.
#pragma once

#include <math.h>
#include <stdint.h>

namespace imuinput {

// Buttons, at the input layer's logical bit positions (HAL_Input.h: bit ==
// InputButton value).
constexpr uint8_t kButtonA = 1u << 0;
constexpr uint8_t kButtonB = 1u << 1;
constexpr uint8_t kButtonX = 1u << 2;
constexpr uint8_t kButtonY = 1u << 3;
constexpr uint8_t kButtonStart = 1u << 4;

// The joystick's scale, the Seesaw gamepad's (i2csensor_seesaw.h): 0..1023,
// centre 512, and the navigation's dead zone of 200 around it.
constexpr int kJoyCentre = 512;
constexpr int kJoyDeadZone = 200;
constexpr int kJoyMinDeflection = 250;  // just engaged: clear of the dead zone
constexpr int kJoyMaxDeflection = 511;

constexpr float kGravity = 9.80665f;
constexpr float kRadToDeg = 57.29577951308232f;

// A sample later than this after the previous one follows a gap: what was
// being counted is dropped (the reference pose is kept).
constexpr uint32_t kGapMs = 100;
// A button is shown as held this long after its press, so a reader of the
// level (a status page, a peer) sees it; far below any hold gesture.
constexpr uint32_t kHeldMs = 100;
// Tilt is not re-evaluated this long after a tap: the knock itself rocks the
// device.
constexpr uint32_t kTapFreezeMs = 150;
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
  bool swapXY = false;
  bool invertX = false;
  bool invertY = false;
  bool shakeStart = true;
};

// One sample (the fields of MotionHookSample this core reads).
struct Sample {
  uint32_t ms = 0;
  float up[3] = {0, 0, 1};     // unit vector, sensor frame
  float accel[3] = {0, 0, 0};  // m/s^2, sensor frame
  bool tap = false;            // at least one single tap since the previous sample
};

struct Output {
  uint8_t pressed = 0;   // buttons pressed by this sample (kButton*)
  uint8_t held = 0;      // buttons shown as held (the pressed ones, for kHeldMs)
  int joyX = kJoyCentre;
  int joyY = kJoyCentre;
  bool activity = false; // something a user did: a press, or the joystick deflected
  bool recentred = false; // a new reference pose was taken on this sample
};

// What a finished run of `count` taps is: a button mask, or kTapsRecentre.
constexpr uint8_t kTapsRecentre = 0x80;
inline uint8_t buttonForTaps(uint8_t count) {
  switch (count) {
    case 1: return kButtonA;
    case 2: return kButtonB;
    case 3: return kButtonY;
    case 4: return kButtonX;
    case 5: return kTapsRecentre;
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

class Engine {
 public:
  // Forget everything, the reference pose included (input was started).
  void reset() { *this = Engine(); }

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
    strongBits_ = ((strongBits_ << 1) | (strong ? 1u : 0u)) & ((1u << kMotionWindow) - 1u);
    const int strongCount = __builtin_popcount(strongBits_);
    const bool moving = strongCount >= kMovingCount;

    // ---- Shake ------------------------------------------------------------
    if (!inShake_ && strongCount >= kShakeCount) {
      inShake_ = true;
      tapCount_ = 0;
      if (cfg.shakeStart) press(out, kButtonStart, s.ms);
    } else if (inShake_ && strongCount <= kShakeEndCount) {
      inShake_ = false;
      tapsFromMs_ = s.ms + kAfterShakeMs;
      tapsBlocked_ = true;
    }
    if (tapsBlocked_ && static_cast<int32_t>(s.ms - tapsFromMs_) >= 0) tapsBlocked_ = false;

    // ---- Taps -------------------------------------------------------------
    if (inShake_) {
      tapCount_ = 0;
    } else if (s.tap && !tapsBlocked_) {
      if (tapCount_ == 0 || static_cast<uint32_t>(s.ms - lastTapMs_) >= cfg.tapMinGapMs) {
        if (tapCount_ < 15) ++tapCount_;
        lastTapMs_ = s.ms;
        freezeUntilMs_ = s.ms + kTapFreezeMs;
        frozen_ = true;
      }
    }
    if (tapCount_ > 0) {
      const uint32_t since = static_cast<uint32_t>(s.ms - lastTapMs_);
      if (since > cfg.tapWindowMs + kTapPatienceMs) {
        tapCount_ = 0;  // never calm enough to mean it
      } else if (since > cfg.tapWindowMs && !moving) {
        const uint8_t what = buttonForTaps(tapCount_);
        tapCount_ = 0;
        if (what == kTapsRecentre) recentre();
        else if (what) press(out, what, s.ms);
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
          if (leanAxes(up0, e1_, e2_)) {
            for (int i = 0; i < 3; ++i) up0_[i] = up0[i];
            haveRef_ = true;
            capturing_ = false;
            out.recentred = true;
          } else {  // not a direction (the samples cancelled out): try again
            captured_ = 0;
            sum_[0] = sum_[1] = sum_[2] = 0.0f;
          }
        }
      }
    }

    // ---- Tilt -------------------------------------------------------------
    if (!haveRef_ || capturing_ || moving || inShake_) {
      releaseJoystick();
    } else if (!frozen_) {
      float x = 0, y = 0;
      const bool inRange = leanDegrees(s.up, up0_, e1_, e2_, x, y);
      if (cfg.swapXY) {
        const float t = x;
        x = y;
        y = t;
      }
      if (cfg.invertX) x = -x;
      if (cfg.invertY) y = -y;
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
  float leanX() const { return leanX_; }  // degrees, after the swap and the inversions
  float leanY() const { return leanY_; }
  const float* reference() const { return up0_; }

 private:
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
    tapCount_ = 0;
    strongBits_ = 0;
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
  bool inShake_ = false;
  bool tapsBlocked_ = false;
  uint32_t tapsFromMs_ = 0;

  uint8_t tapCount_ = 0;
  uint32_t lastTapMs_ = 0;
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
};

}  // namespace imuinput
