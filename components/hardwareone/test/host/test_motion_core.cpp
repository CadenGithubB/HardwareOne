// The motion layer's pure rules (HAL_MotionCore.h) on synthetic samples: the
// orientation correction (off by default, no pitch clamp) and the action
// detector (rotation in rad/s, a start that is not a shake, tap, free fall,
// tilt, steps and the pose debounce), the automation condition variables
// (IMU_PITCH, IMU_ROLL, IMU_TILT, IMU_POSE, IMU_TAP_AGE), the compass point
// and the gyro offset states 'imustatus' reports.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "HAL_MotionCore.h"

static bool near(float a, float b, float tolerance = 0.01f) { return std::fabs(a - b) <= tolerance; }

static MotionActionSample still() {
  MotionActionSample s;
  s.az = 9.81f;  // lying flat, face up
  return s;
}

// Feed `count` copies of a sample every `stepMs`, starting at `now`; returns
// the time after the last one and collects every event.
struct Feed {
  MotionActionState state;
  unsigned long now = 100000;
  MotionEvents all;
  void run(const MotionActionSample& sample, int count = 1, unsigned long stepMs = 20) {
    for (int i = 0; i < count; ++i) {
      MotionEvents events;
      motionActionsUpdate(state, sample, now, &events);
      for (uint8_t k = 0; k < events.count; ++k) {
        if (all.count < sizeof(all.items) / sizeof(all.items[0])) all.items[all.count++] = events.items[k];
      }
      now += stepMs;
    }
  }
  int count(MotionEventKind kind) const {
    int n = 0;
    for (uint8_t i = 0; i < all.count; ++i) n += all.items[i].kind == kind;
    return n;
  }
  const MotionEvent* first(MotionEventKind kind) const {
    for (uint8_t i = 0; i < all.count; ++i) {
      if (all.items[i].kind == kind) return &all.items[i];
    }
    return nullptr;
  }
};

// imuaddress while the IMU runs: reads stay at the open address; the new
// one counts once the IMU is closed (and so at the next open).
static void testOpenAddressIsLatched() {
  MotionOpenTarget target;
  assert(motionActiveAddress(target, 0x28) == 0x28 && motionActiveBus(target, 1) == 1);
  target.open = true;
  target.bus = 1;
  target.address = 0x28;
  assert(motionActiveAddress(target, 0x29) == 0x28);  // imuaddress 0x29 saved while running
  assert(motionActiveBus(target, 0) == 1);
  target.open = false;
  assert(motionActiveAddress(target, 0x29) == 0x29 && motionActiveBus(target, 0) == 0);
  // A two-chip backend's second address (the 4517's magnetometer) the same way.
  target.open = true;
  target.secondAddress = 0x1C;
  assert(motionActiveSecondAddress(target, 0x1E) == 0x1C);
  target.open = false;
  assert(motionActiveSecondAddress(target, 0x1E) == 0x1E);
}

// The chip-fused backend (BNO055) is read every imuDevicePollMs; the 4517 at
// its own 20 ms, publishing the first sample and then once per poll period.
static void testSamplingAndPublish() {
  assert(motionSamplePeriodMs(0, 200) == 200 && motionSamplePeriodMs(20, 200) == 20);
  assert(motionPublishDue(false, 0, 5, 200, 20));          // the first sample
  assert(!motionPublishDue(true, 1000, 1180, 200, 20));    // 9 samples in: not yet
  assert(motionPublishDue(true, 1000, 1200, 200, 20));
  // Read every poll period, a late previous read does not skip a sample.
  assert(motionPublishDue(true, 1205, 1400, 200, 200));
  assert(!motionPublishDue(true, 1205, 1300, 200, 200));
  // At 50 Hz with a 200 ms poll, one sample in ten is published, also with
  // the reads a tick late now and then.
  unsigned long last = 0;
  bool once = false;
  int published = 0;
  for (unsigned long tick = 1000; tick < 3000; tick += 20) {
    const unsigned long now = tick + ((tick / 20) % 3 == 0 ? 1 : 0);
    if (motionPublishDue(once, last, now, 200, 20)) {
      once = true;
      last = now;
      ++published;
    }
  }
  assert(published == 10);
}

static void testOrientationCorrection() {
  MotionOrientationSettings settings;  // the defaults: off, mode 0
  assert(!settings.enabled && settings.mode == 0);
  float pitch = 80.0f, roll = -170.0f, yaw = 359.0f;
  motionApplyOrientationCorrection(settings, pitch, roll, yaw);
  assert(pitch == 80.0f && roll == -170.0f && yaw == 359.0f);  // untouched, never clamped

  settings.enabled = true;  // mode 0 with offsets: no clamp, yaw wraps
  settings.pitchOffset = 5.0f;
  settings.yawOffset = 10.0f;
  pitch = 80.0f; roll = 10.0f; yaw = 355.0f;
  motionApplyOrientationCorrection(settings, pitch, roll, yaw);
  assert(near(pitch, 85.0f) && near(roll, 10.0f) && near(yaw, 5.0f));

  settings = MotionOrientationSettings{};
  settings.enabled = true;
  settings.mode = 8;  // upside down: pitch from the folded roll, roll from pitch
  pitch = 12.0f; roll = 170.0f; yaw = 90.0f;
  motionApplyOrientationCorrection(settings, pitch, roll, yaw);
  assert(near(pitch, 10.0f) && near(roll, 12.0f) && near(yaw, 90.0f));

  settings.mode = 1;
  pitch = 20.0f; roll = 0.0f; yaw = -30.0f;
  motionApplyOrientationCorrection(settings, pitch, roll, yaw);
  assert(near(pitch, -20.0f) && near(yaw, 330.0f));
}

static void testStartIsNotAShake() {
  Feed feed;
  feed.run(still(), 20);
  assert(feed.all.count == 0);
  assert(!feed.state.isShaking && feed.state.shakeCount == 0);
  assert(feed.state.orientation == 'F' && !feed.state.isTilted && !feed.state.isRotating);
}

static void testRotationIsRadPerSecond() {
  Feed feed;
  MotionActionSample s = still();
  s.gz = 1.0f;  // rad/s = 57 deg/s: below 100 deg/s
  feed.run(s);
  assert(!feed.state.isRotating && feed.state.rotationAxis == 'N');
  assert(near(feed.state.rotationRateDps, 57.2958f, 0.01f));
  s.gz = 2.0f;  // 114.6 deg/s
  feed.run(s);
  assert(feed.state.isRotating && feed.state.rotationAxis == 'Z');
  assert(near(feed.state.rotationRateDps, 114.59f, 0.01f));
  s = still();
  s.gx = -2.5f;
  feed.run(s);
  assert(feed.state.isRotating && feed.state.rotationAxis == 'X');
  assert(near(kMotionRotationRadS, 1.74533f, 0.0001f));
}

static void testShake() {
  Feed feed;
  feed.run(still(), 10);
  MotionActionSample hard = still(), soft = still();
  hard.az = 22.0f;
  soft.az = 2.5f;
  for (int i = 0; i < 6; ++i) {
    feed.run(hard);
    feed.run(soft);
  }
  assert(feed.count(MotionEventKind::Shake) == 1);  // one event per shaking episode
  assert(feed.state.isShaking && feed.state.shakeCount == 1);
  // Still again: the shaky samples leave the 2 s window, and the shake ends
  // 500 ms after the variance fell under the threshold.
  feed.run(still(), 40);
  assert(feed.state.isShaking);  // 0.8 s: still in the window
  feed.run(still(), 90);         // 2.6 s
  assert(!feed.state.isShaking && feed.state.shakeIntensity == 0.0f);
}

static MotionActionSample at50Hz(MotionActionSample s, bool hwFlags) {
  s.periodMs = 20;
  s.hasHwTap = s.hasHwFreefall = hwFlags;
  return s;
}

// The 4517 at 50 Hz: a knock on the sign is one tap, not a shake and not a
// walk, with the chip's tap flag (its spike and ringing are kept out of the
// shake window and the steps) and without it (the 2 s window outweighs it,
// and walking needs two steps).
static void testKnockAt50Hz() {
  const float ringing[] = {30.0f, 3.0f, 18.0f, 5.0f, 14.0f, 7.0f};
  for (int pass = 0; pass < 2; ++pass) {
    const bool hwFlags = pass == 0;
    Feed feed;
    feed.run(at50Hz(still(), hwFlags), 100);  // 2 s still
    for (size_t i = 0; i < sizeof(ringing) / sizeof(ringing[0]); ++i) {
      MotionActionSample s = at50Hz(still(), hwFlags);
      s.az = ringing[i];
      s.hwTap = hwFlags && i == 0;  // the chip latched the knock
      feed.run(s);
    }
    feed.run(at50Hz(still(), hwFlags), 200);  // 4 s still
    assert(feed.count(MotionEventKind::Tap) == 1 && feed.state.tapCount == 1);
    assert(feed.count(MotionEventKind::Shake) == 0 && feed.state.shakeCount == 0);
    assert(feed.count(MotionEventKind::Walking) == 0 && !feed.state.isWalking);
    if (hwFlags) assert(feed.state.stepCount == 0);  // the ringing fell in the quiet time
  }
}

// A real shake at 50 Hz still posts a shake (once), with the chip's flags in
// play but no tap.
static void testShakeAt50Hz() {
  Feed feed;
  feed.run(at50Hz(still(), true), 50);
  MotionActionSample hard = at50Hz(still(), true), soft = at50Hz(still(), true);
  hard.az = 22.0f;
  soft.az = 2.5f;
  for (int i = 0; i < 25; ++i) {  // 1 s
    feed.run(hard);
    feed.run(soft);
  }
  assert(feed.count(MotionEventKind::Shake) == 1 && feed.state.isShaking);
  assert(feed.count(MotionEventKind::Tap) == 0);
}

// A slow poll keeps a window of 10 samples (one slot each), as before.
static void testShakeWindowAtSlowPoll() {
  Feed feed;
  MotionActionSample s = still();
  s.periodMs = 1000;
  feed.run(s, 9, 1000);
  MotionActionSample spike = s;
  spike.az = 25.0f;
  feed.run(spike, 1, 1000);
  assert(feed.count(MotionEventKind::Shake) == 1);  // variance about 21 over 10 samples
}

static void testTap() {
  Feed feed;
  feed.run(still(), 5);
  MotionActionSample spike = still();
  spike.az = 35.0f;
  feed.run(spike);
  const MotionEvent* tap = feed.first(MotionEventKind::Tap);
  assert(tap && std::strcmp(tap->subject, "0.50") == 0 && tap->detail[0] == '\0');
  feed.run(spike);  // 20 ms later: inside the 200 ms lockout
  assert(feed.count(MotionEventKind::Tap) == 1 && feed.state.tapCount == 1);
  feed.run(still(), 15);  // 300 ms
  feed.run(spike);
  assert(feed.count(MotionEventKind::Tap) == 2);
}

static void testFreefall() {
  Feed feed;
  feed.run(still(), 5);
  MotionActionSample fall;  // |accel| near zero
  fall.az = 0.5f;
  feed.run(fall, 7, 20);  // 0..120 ms: not yet
  assert(feed.state.isFreefalling && feed.count(MotionEventKind::Freefall) == 0);
  feed.run(fall, 2, 20);  // 140, 160 ms
  const MotionEvent* event = feed.first(MotionEventKind::Freefall);
  assert(event && std::strcmp(event->subject, "160ms") == 0);
  feed.run(fall, 5, 20);
  assert(feed.count(MotionEventKind::Freefall) == 1);  // once per fall
  feed.run(still());
  assert(!feed.state.isFreefalling && feed.state.freefallDurationMs == 0);
}

static void testTilt() {
  Feed feed;
  MotionActionSample s = still();
  s.pitch = 40.0f;
  feed.run(s);
  assert(feed.state.isTilted && feed.state.tiltDirection == 'F' && near(feed.state.tiltAngle, 40.0f));
  s.pitch = 5.0f;
  s.roll = -50.0f;
  feed.run(s);
  assert(feed.state.isTilted && feed.state.tiltDirection == 'L');
  s.roll = 20.0f;
  feed.run(s);
  assert(!feed.state.isTilted && feed.state.tiltDirection == 'N');
}

// At the BNO055's 200 ms poll: three samples in a row.
static void testPoseDebounce() {
  Feed feed;
  feed.run(still(), 3, 200);
  MotionActionSample down = still();
  down.az = -9.81f;
  feed.run(down, 2, 200);
  assert(feed.state.orientation == 'D' && feed.count(MotionEventKind::Orientation) == 0);
  feed.run(still(), 1, 200);  // back before it held: no event
  feed.run(down, 2, 200);
  assert(feed.count(MotionEventKind::Orientation) == 0);
  feed.run(down, 1, 200);  // the third sample in a row
  const MotionEvent* pose = feed.first(MotionEventKind::Orientation);
  assert(pose && std::strcmp(pose->subject, "face-down") == 0 && std::strcmp(pose->detail, "face-up") == 0);
  feed.run(down, 10, 200);
  assert(feed.count(MotionEventKind::Orientation) == 1);
}

// At the 4517's 50 Hz: three samples (60 ms) are not enough; the pose must
// hold for 200 ms, so a brief wobble across a boundary posts nothing.
static void testPoseDebounceAt50Hz() {
  Feed feed;
  feed.run(still(), 5);
  MotionActionSample down = still();
  down.az = -9.81f;
  feed.run(down, 5);  // 0..80 ms
  feed.run(still(), 1);
  feed.run(down, 10);  // 0..180 ms
  assert(feed.count(MotionEventKind::Orientation) == 0);
  feed.run(down, 1);  // 200 ms after it was first seen
  assert(feed.count(MotionEventKind::Orientation) == 1);
  feed.run(down, 50);
  assert(feed.count(MotionEventKind::Orientation) == 1);
}

// The 4517's latched single tap replaces the |accel| rule: a knock the 50 Hz
// read missed still counts, and a spike without the flag does not.
static void testHardwareTap() {
  Feed feed;
  MotionActionSample s = still();
  s.hasHwTap = true;
  feed.run(s, 5);
  s.hwTap = true;  // latched since the last sample; |accel| looks still
  feed.run(s);
  const MotionEvent* tap = feed.first(MotionEventKind::Tap);
  assert(tap && std::strcmp(tap->subject, "1.00") == 0 && feed.state.tapCount == 1);
  feed.run(s);  // 20 ms later: inside the 200 ms lockout
  assert(feed.state.tapCount == 1);
  s.hwTap = false;
  s.az = 40.0f;  // a spike, but the chip says no tap
  feed.run(s, 15);
  assert(feed.state.tapCount == 1);
}

// The chip's latched free fall (it already waited FF_DUR) posts at once.
static void testHardwareFreefall() {
  Feed feed;
  MotionActionSample s = still();
  s.hasHwFreefall = true;
  feed.run(s, 5);
  s.hwFreefall = true;  // a short drop the 50 Hz read caught only as the flag
  feed.run(s);
  const MotionEvent* fall = feed.first(MotionEventKind::Freefall);
  assert(fall && std::strcmp(fall->subject, "150ms") == 0);
  feed.run(s, 3);
  assert(feed.count(MotionEventKind::Freefall) == 1);  // once per fall
  s.hwFreefall = false;
  feed.run(s);
  assert(!feed.state.isFreefalling);
  // Without the flag a near-zero reading still counts as falling (both rules).
  MotionActionSample low = s;
  low.az = 0.5f;
  feed.run(low, 9);
  assert(feed.count(MotionEventKind::Freefall) == 2);
}

static void testSteps() {
  Feed feed;
  feed.run(still(), 3);
  MotionActionSample peak = still(), valley = still();
  peak.az = 14.0f;
  valley.az = 6.0f;
  for (int i = 0; i < 4; ++i) {
    feed.run(peak, 1, 250);
    feed.run(valley, 1, 250);
  }
  assert(feed.state.stepCount == 4 && feed.state.isWalking);
  const MotionEvent* walking = feed.first(MotionEventKind::Walking);
  assert(walking && std::strcmp(walking->subject, "started") == 0);
  feed.run(still(), 15, 200);  // 3 s without a step
  assert(!feed.state.isWalking && feed.count(MotionEventKind::Walking) == 2);

  // One step alone is no walk.
  Feed once;
  once.run(still(), 3, 200);
  once.run(peak, 1, 250);
  once.run(valley, 1, 250);
  once.run(still(), 15, 200);
  assert(once.state.stepCount == 1 && once.count(MotionEventKind::Walking) == 0);
  // Two steps too far apart are none either.
  once.run(peak, 1, 250);
  once.run(valley, 1, 250);
  once.run(still(), 15, 200);
  assert(once.state.stepCount == 2 && once.count(MotionEventKind::Walking) == 0);
}

static void testWithoutEvents() {
  MotionActionState state;
  MotionActionSample spike = still();
  spike.az = 40.0f;
  motionActionsUpdate(state, spike, 1000, nullptr);  // a renderer-free caller: state only
  assert(state.tapCount == 1 && state.tapDetected);
}

// ---- Automation condition variables ---------------------------------------------
// IMU_PITCH / IMU_ROLL / IMU_TILT (degrees), IMU_POSE (the announced pose, upper
// case) and IMU_TAP_AGE (seconds). No value means the condition is false.
static void testConditionVariables() {
  for (const char* name : {"IMU_PITCH", "IMU_ROLL", "IMU_TILT", "IMU_POSE", "IMU_TAP_AGE"}) {
    assert(motionConditionIsVariable(name));
  }
  // No heading variable yet, no lower case (the parser upper-cases), no prefix match.
  for (const char* name : {"IMU_HEADING", "IMU_YAW", "IMU_", "IMU_PITCHX", "imu_pitch", "PITCH", ""}) {
    assert(!motionConditionIsVariable(name));
  }
  assert(!motionConditionIsVariable(nullptr));

  Feed feed;
  MotionActionSample flat = still();
  feed.run(flat, 5);
  MotionConditionValue v;
  const unsigned long now = feed.now;
  // Tilt: the reading's angles, and the larger of the two.
  assert(motionConditionValue("IMU_PITCH", true, -35.5f, 12.0f, feed.state, now, v) && !v.isText && near(v.number, -35.5f));
  assert(motionConditionValue("IMU_ROLL", true, -35.5f, 12.0f, feed.state, now, v) && !v.isText && near(v.number, 12.0f));
  assert(motionConditionValue("IMU_TILT", true, -35.5f, 12.0f, feed.state, now, v) && near(v.number, 35.5f));
  assert(motionConditionValue("IMU_TILT", true, 5.0f, -60.0f, feed.state, now, v) && near(v.number, 60.0f));
  // The pose the detector announced, upper case: the first sample's without an event.
  assert(motionConditionValue("IMU_POSE", true, 0, 0, feed.state, now, v) && v.isText && !std::strcmp(v.text, "FACE-UP"));
  // No reading: nothing has a value, whatever the detector holds.
  for (const char* name : {"IMU_PITCH", "IMU_ROLL", "IMU_TILT", "IMU_POSE", "IMU_TAP_AGE"}) {
    assert(!motionConditionValue(name, false, 10, 10, feed.state, now, v));
  }
  // An unknown IMU_ name has none either.
  assert(!motionConditionValue("IMU_HEADING", true, 0, 0, feed.state, now, v));
  assert(!motionConditionValue(nullptr, true, 0, 0, feed.state, now, v));
  // No tap since the IMU opened: no age (a condition IMU_TAP_AGE<5 stays false).
  assert(!feed.state.tapAgeKnown);
  assert(!motionConditionValue("IMU_TAP_AGE", true, 0, 0, feed.state, now, v));

  // Flipped over: the pose changes once it held (kMotionDebounce*), not on the first sample.
  MotionActionSample down = still();
  down.az = -9.81f;
  feed.run(down, 1);
  assert(motionConditionValue("IMU_POSE", true, 0, 0, feed.state, feed.now, v) && !std::strcmp(v.text, "FACE-UP"));
  feed.run(down, 15);  // 300 ms at 50 Hz
  assert(motionConditionValue("IMU_POSE", true, 0, 0, feed.state, feed.now, v) && !std::strcmp(v.text, "FACE-DOWN"));
  assert(feed.count(MotionEventKind::Orientation) == 1);
  // Every pose name fits the value and is upper case.
  for (char pose : {'F', 'D', 'P', 'U', 'R', 'L'}) {
    MotionActionState state;
    state.posePosted = pose;
    assert(motionConditionValue("IMU_POSE", true, 0, 0, state, 0, v) && v.isText);
    assert(std::strlen(v.text) == std::strlen(motionPoseName(pose)));
    for (const char* c = v.text; *c; ++c) assert(!(*c >= 'a' && *c <= 'z'));
  }
  { MotionActionState state;  // no pose yet (the detector has not seen a sample)
    assert(!motionConditionValue("IMU_POSE", true, 0, 0, state, 0, v)); }

  // A tap (the chip's, as on the 4517): its age in seconds from then on.
  MotionActionSample tap = still();
  tap.hasHwTap = true;
  tap.hwTap = true;
  feed.run(still(), 15);
  const unsigned long tapAt = feed.now;
  feed.run(tap, 1);
  assert(feed.state.tapCount == 1 && feed.state.lastTapMs == tapAt);
  assert(motionConditionValue("IMU_TAP_AGE", true, 0, 0, feed.state, tapAt, v) && near(v.number, 0.0f));
  assert(motionConditionValue("IMU_TAP_AGE", true, 0, 0, feed.state, tapAt + 2500, v) && near(v.number, 2.5f));
  // The device's millisecond clock is 32 bits and wraps after 49.7 days. A
  // tap just before the wrap, asked about just after it, has its real age.
  { MotionActionState state;
    state.tapCount = 1;
    state.tapAgeKnown = true;
    state.lastTapMs = 0xFFFFFFFFul - 999;  // 1 s before the wrap
    assert(motionConditionValue("IMU_TAP_AGE", true, 0, 0, state, 2000, v) && near(v.number, 3.0f));
    // The same on a 32-bit count that has wrapped (the device's unsigned long).
    const uint32_t wrapped = static_cast<uint32_t>(0xFFFFFFFFul - 999) + 3000u;
    assert(wrapped == 2000u);
    assert(motionConditionValue("IMU_TAP_AGE", true, 0, 0, state, wrapped, v) && near(v.number, 3.0f)); }
  // A tap keeps its age for an hour, then has none: 49.7 days later the clock
  // reads the tap's time again, and the old tap must not look fresh.
  { Feed old;
    old.run(still(), 5);
    MotionActionSample knock = still();
    knock.hasHwTap = true;
    knock.hwTap = true;
    const unsigned long tapTime = old.now;
    old.run(knock, 1);
    assert(old.state.tapAgeKnown);
    assert(motionConditionValue("IMU_TAP_AGE", true, 0, 0, old.state, tapTime + kMotionTapAgeCapMs, v) &&
           near(v.number, kMotionTapAgeCapMs / 1000.0f, 0.5f));
    // Past the cap the question has no answer even before the detector runs again...
    assert(!motionConditionValue("IMU_TAP_AGE", true, 0, 0, old.state, tapTime + kMotionTapAgeCapMs + 1, v));
    // ...and one full turn of the 32-bit clock later (age 0 in 32 bits) the
    // detector has long forgotten the tap: it runs on every sample.
    old.now = tapTime + kMotionTapAgeCapMs + 20;
    old.run(still(), 1);
    assert(!old.state.tapAgeKnown && old.state.tapCount == 1);
    const uint32_t oneTurnLater = static_cast<uint32_t>(tapTime);  // tapTime + 2^32 in 32 bits
    assert(!motionConditionValue("IMU_TAP_AGE", true, 0, 0, old.state, oneTurnLater, v));
    // A new tap has an age again.
    old.run(knock, 1);
    assert(old.state.tapAgeKnown && motionConditionValue("IMU_TAP_AGE", true, 0, 0, old.state, old.now, v)); }
  // A detector that never ran again (the IMU task stalled) and a clock that
  // came round: the state still says "known", so the age is taken in 32 bits
  // and is small; the reading's own age gate (motionReadingFresh) is what
  // makes such a condition false.
  assert(motionReadingFresh(1000, 900, 200) && motionReadingFresh(2400, 1000, 200));
  assert(!motionReadingFresh(2401, 1000, 200));                // older than 2 polls + 1 s
  assert(motionReadingFresh(5, 0xFFFFFFF0u, 200));              // across the clock's wrap
  assert(!motionReadingFresh(0x80000000u, 0, 1000));
  assert(motionReadingFresh(3000, 0, 1000) && !motionReadingFresh(3001, 0, 1000));
}

// ---- What the surfaces show -----------------------------------------------------
static void testCompassPointAndGyroOffsetState() {
  // Eight points, each 45 degrees wide and centred on its direction.
  const struct { float heading; const char* point; } cases[] = {
      {0.0f, "N"}, {22.4f, "N"}, {22.6f, "NE"}, {45.0f, "NE"}, {90.0f, "E"}, {135.0f, "SE"}, {180.0f, "S"},
      {225.0f, "SW"}, {270.0f, "W"}, {315.0f, "NW"}, {337.4f, "NW"}, {337.6f, "N"}, {359.9f, "N"},
      {360.0f, "N"}, {-90.0f, "W"}, {450.0f, "E"},
  };
  for (const auto& c : cases) assert(!std::strcmp(motionCompassPoint(c.heading), c.point));
  assert(!std::strcmp(motionCompassPoint(NAN), "?") && !std::strcmp(motionCompassPoint(INFINITY), "?"));

  // Yaw on a small display: a whole number in 0..359, three characters at most.
  const struct { float yaw; int shown; } yaws[] = {
      {0.0f, 0}, {359.4f, 359}, {359.99f, 359}, {360.0f, 0}, {-0.4f, 359}, {-180.0f, 180}, {725.0f, 5},
      {-721.0f, 359}, {180.5f, 180},
  };
  for (const auto& y : yaws) assert(motionYawForDisplay(y.yaw) == y.shown);
  assert(motionYawForDisplay(NAN) == 0 && motionYawForDisplay(-INFINITY) == 0);
  for (float y = -1000.0f; y < 1000.0f; y += 0.37f) assert(motionYawForDisplay(y) >= 0 && motionYawForDisplay(y) <= 359);
  assert(kMotionOledColumns == 21);

  // 'imustatus': a chip that fuses on its own keeps its offset; a backend the
  // layer fuses measures it at each start, and a start on a moving board has none.
  assert(!std::strcmp(motionGyroOffsetState(false, true, false, false), "chip"));
  assert(!std::strcmp(motionGyroOffsetState(false, false, true, true), "chip"));
  assert(!std::strcmp(motionGyroOffsetState(true, false, true, true), "closed"));
  assert(!std::strcmp(motionGyroOffsetState(true, true, false, false), "measuring"));
  assert(!std::strcmp(motionGyroOffsetState(true, true, true, true), "still"));
  assert(!std::strcmp(motionGyroOffsetState(true, true, true, false), "moving"));
  // Each state has its own sentence part, and "moving" says what to do.
  const char* states[] = {"chip", "closed", "measuring", "still", "moving"};
  for (const char* a : states) {
    assert(std::strlen(motionGyroOffsetText(a)) > 0);
    for (const char* b : states) assert(a == b || std::strcmp(motionGyroOffsetText(a), motionGyroOffsetText(b)) != 0);
  }
  assert(std::strstr(motionGyroOffsetText("moving"), "'closeimu' and 'openimu'") != nullptr);
}

int main() {
  testConditionVariables();
  testCompassPointAndGyroOffsetState();
  testOpenAddressIsLatched();
  testOrientationCorrection();
  testStartIsNotAShake();
  testRotationIsRadPerSecond();
  testShake();
  testKnockAt50Hz();
  testShakeAt50Hz();
  testShakeWindowAtSlowPoll();
  testTap();
  testFreefall();
  testTilt();
  testPoseDebounce();
  testPoseDebounceAt50Hz();
  testHardwareTap();
  testHardwareFreefall();
  testSamplingAndPublish();
  testSteps();
  testWithoutEvents();
  std::puts("motion core: open address latched, correction off by default and unclamped, rotation in rad/s, no shake at start, "
            "tap, free fall, tilt, pose debounce (200 ms and 50 Hz), chip tap and free fall, "
            "knock at 50 Hz (no shake, no walk), shake at 50 Hz and at a slow poll, sampling, steps, "
            "automation condition variables, compass points and gyro offset states passed");
  return 0;
}
