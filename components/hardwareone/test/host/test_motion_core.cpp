// The motion layer's pure rules (HAL_MotionCore.h) on synthetic samples: the
// orientation correction (off by default, no pitch clamp) and the action
// detector (rotation in rad/s, a start that is not a shake, tap, free fall,
// tilt, steps and the pose debounce).
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

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
  // Still again: the shake ends 500 ms after the last shaky sample.
  feed.run(still(), 40);
  assert(!feed.state.isShaking && feed.state.shakeIntensity == 0.0f);
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

static void testPoseDebounce() {
  Feed feed;
  feed.run(still(), 3);
  MotionActionSample down = still();
  down.az = -9.81f;
  feed.run(down, 2);
  assert(feed.state.orientation == 'D' && feed.count(MotionEventKind::Orientation) == 0);
  feed.run(still());  // back before it held: no event
  feed.run(down, 2);
  assert(feed.count(MotionEventKind::Orientation) == 0);
  feed.run(down);  // the third sample in a row
  const MotionEvent* pose = feed.first(MotionEventKind::Orientation);
  assert(pose && std::strcmp(pose->subject, "face-down") == 0 && std::strcmp(pose->detail, "face-up") == 0);
  feed.run(down, 10);
  assert(feed.count(MotionEventKind::Orientation) == 1);
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
}

static void testWithoutEvents() {
  MotionActionState state;
  MotionActionSample spike = still();
  spike.az = 40.0f;
  motionActionsUpdate(state, spike, 1000, nullptr);  // a renderer-free caller: state only
  assert(state.tapCount == 1 && state.tapDetected);
}

int main() {
  testOrientationCorrection();
  testStartIsNotAShake();
  testRotationIsRadPerSecond();
  testShake();
  testTap();
  testFreefall();
  testTilt();
  testPoseDebounce();
  testSteps();
  testWithoutEvents();
  std::puts("motion core: correction off by default and unclamped, rotation in rad/s, no shake at start, "
            "tap, free fall, tilt, pose debounce and steps passed");
  return 0;
}
