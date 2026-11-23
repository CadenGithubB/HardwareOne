// The IMU as an input device (Input_IMUCore.h): a run of taps becomes one
// button after the tap window (1 = A, 2 = B, 3 = Y, 4 = X, 5 = re-centre,
// more = nothing), a sustained shake is START and cancels taps, a lean from
// the reference pose is the joystick (one axis, hysteresis, a dwell), the
// reference can be any pose, and a gap drops what was being counted.
#include <cassert>
#include <cmath>
#include <cstdio>

#include "Input_IMUCore.h"

using namespace imuinput;

static constexpr float kDeg = 1.0f / kRadToDeg;

// A device fed one sample every 20 ms.
struct Rig {
  Engine engine;
  Config cfg;
  uint32_t ms = 1000;
  float up[3] = {0, 0, 1};
  Output last;
  uint8_t pressedSince = 0;  // every press since the last clear
  int recentres = 0;

  void pose(float x, float y, float z) {
    const float n = std::sqrt(x * x + y * y + z * z);
    up[0] = x / n;
    up[1] = y / n;
    up[2] = z / n;
  }
  // Lying flat, leaned: right = the +X end drops, upward = the +Y side rises.
  void flatLean(float rightDeg, float upDeg) {
    pose(-std::sin(rightDeg * kDeg), std::sin(upDeg * kDeg), std::cos(std::hypot(rightDeg, upDeg) * kDeg));
  }
  Output step(bool tap = false, float extraAccel = 0.0f) {
    Sample s;
    s.ms = ms;
    for (int i = 0; i < 3; ++i) {
      s.up[i] = up[i];
      s.accel[i] = kGravity * up[i];
    }
    s.accel[0] += extraAccel;
    s.tap = tap;
    last = engine.update(s, cfg);
    pressedSince |= last.pressed;
    if (last.recentred) ++recentres;
    ms += 20;
    return last;
  }
  void run(uint32_t forMs) {
    for (uint32_t t = 0; t < forMs; t += 20) step();
  }
  // One tap (a strong sample, as the knock is) then quiet until `gapMs` later.
  void tap(uint32_t gapMs = 160) {
    step(true, 15.0f);
    run(gapMs - 20);
  }
  void taps(int count, uint32_t gapMs = 160) {
    for (int i = 0; i < count; ++i) tap(gapMs);
  }
  uint8_t take() {
    const uint8_t p = pressedSince;
    pressedSince = 0;
    return p;
  }
  // Started and centred, lying flat.
  void start() {
    run(400);
    assert(engine.hasReference() && !engine.capturing());
    assert(recentres == 1);
    take();
  }
};

static void testTapCounts() {
  const uint8_t expect[] = {0, kButtonA, kButtonB, kButtonY, kButtonX};
  for (int count = 1; count <= 4; ++count) {
    Rig r;
    r.start();
    r.taps(count);
    assert(r.take() == 0);  // not yet: another tap may follow
    r.run(400);
    assert(r.take() == expect[count]);
    r.run(1000);
    assert(r.take() == 0);  // once
  }
  // Six and more: nothing, and no re-centre.
  for (int count = 6; count <= 8; ++count) {
    Rig r;
    r.start();
    r.taps(count);
    r.run(1500);
    assert(r.take() == 0 && r.recentres == 1);
  }
  assert(buttonForTaps(0) == 0 && buttonForTaps(5) == kTapsRecentre && buttonForTaps(16) == 0);
}

static void testTapTiming() {
  // The button arrives the tap window after the last tap, not before.
  Rig r;
  r.start();
  r.step(true, 15.0f);  // the tap, at T
  for (int i = 0; i < 17; ++i) {  // T+20 .. T+340
    r.step();
    assert(r.last.pressed == 0);
  }
  r.step();  // T+360 > 350
  assert(r.last.pressed == kButtonA && r.last.held == kButtonA && r.last.activity);
  // Held for about 100 ms, then released.
  r.run(80);
  assert(r.last.held == kButtonA);
  r.run(40);
  assert(r.last.held == 0);

  // Two taps slower than the window are two A presses.
  Rig slow;
  slow.start();
  slow.tap(600);
  assert(slow.take() == kButtonA);
  slow.tap(600);
  assert(slow.take() == kButtonA);

  // A second flag 40 ms after the first is the same knock.
  Rig bounce;
  bounce.start();
  bounce.step(true, 15.0f);
  bounce.step();
  bounce.step(true);
  bounce.run(600);
  assert(bounce.take() == kButtonA);

  // The window is the setting.
  Rig wide;
  wide.cfg.tapWindowMs = 600;
  wide.start();
  wide.tap(500);
  wide.tap(500);
  wide.run(700);
  assert(wide.take() == kButtonB);
}

static void testShake() {
  // A sustained shake: START once, and the taps the chip reports in it are not buttons.
  Rig r;
  r.start();
  for (int i = 0; i < 40; ++i) r.step(i % 5 == 0, (i % 2) ? 14.0f : -14.0f);
  assert(r.engine.shaking());
  assert(r.take() == kButtonStart);
  r.run(1500);
  assert(!r.engine.shaking());
  assert(r.take() == 0);
  // And taps work again afterwards.
  r.tap();
  r.run(400);
  assert(r.take() == kButtonA);

  // A second shake is a second START.
  for (int i = 0; i < 40; ++i) r.step(false, (i % 2) ? 14.0f : -14.0f);
  r.run(1500);
  assert(r.take() == kButtonStart);

  // With shakeStart off a shake presses nothing (and still cancels taps).
  Rig off;
  off.cfg.shakeStart = false;
  off.start();
  off.taps(2);
  for (int i = 0; i < 40; ++i) off.step(false, (i % 2) ? 14.0f : -14.0f);
  off.run(1500);
  assert(off.take() == 0);

  // Five hard taps are not a shake.
  Rig taps;
  taps.start();
  taps.taps(5, 140);
  assert(!taps.engine.shaking());
  taps.run(1000);
  assert(taps.take() == 0 && taps.recentres == 2);  // the re-centre, no START
}

// A real knock rings: the tap flag plus a few strong samples. Four firm taps
// in quick succession still make X (bench 2026-11-23: the run was dropped as
// "never calm" because the knocks themselves counted as movement).
static void testRingingKnocksStillCount() {
  Rig r;
  r.start();
  for (int i = 0; i < 4; ++i) {
    r.step(true, 15.0f);
    r.step(false, 14.0f);
    r.step(false, 12.0f);
    r.run(120);
  }
  r.run(1200);
  assert(r.take() == kButtonX);
}

static void testTapsWaitForCalm() {
  // A run of taps that ends while the device is being moved about waits, and
  // is dropped when it never gets calm.
  Rig r;
  r.start();
  r.step(true, 15.0f);
  for (int i = 0; i < 100; ++i) r.step(false, (i % 3 == 0) ? 14.0f : 0.0f);  // a third of the samples strong
  assert(!r.engine.shaking());
  assert(r.take() == 0);
  r.run(1000);
  assert(r.take() == 0);  // too old by now
}

static void testTiltDirections() {
  Rig r;
  r.start();
  assert(r.last.joyX == kJoyCentre && r.last.joyY == kJoyCentre);

  r.flatLean(20, 0);
  r.run(200);
  assert(r.last.joyX > kJoyCentre + kJoyDeadZone && r.last.joyY == kJoyCentre && r.last.activity);
  r.flatLean(0, 0);
  r.run(200);
  assert(r.last.joyX == kJoyCentre && r.last.joyY == kJoyCentre && !r.last.activity);

  r.flatLean(-20, 0);
  r.run(200);
  assert(r.last.joyX < kJoyCentre - kJoyDeadZone && r.last.joyY == kJoyCentre);
  r.flatLean(0, 0);
  r.run(200);

  r.flatLean(0, 20);
  r.run(200);
  assert(r.last.joyY > kJoyCentre + kJoyDeadZone && r.last.joyX == kJoyCentre);  // above centre is up
  r.flatLean(0, 0);
  r.run(200);

  r.flatLean(0, -20);
  r.run(200);
  assert(r.last.joyY < kJoyCentre - kJoyDeadZone && r.last.joyX == kJoyCentre);
  assert(r.take() == 0);  // tilting presses nothing
}

static void testTiltThresholds() {
  Rig r;
  r.start();
  // Under the engage angle: centred.
  r.flatLean(13, 0);
  r.run(400);
  assert(r.last.joyX == kJoyCentre);
  // Past it: deflected only after the dwell (three samples).
  r.flatLean(16, 0);
  r.step();
  r.step();
  assert(r.last.joyX == kJoyCentre);
  r.step();
  assert(r.last.joyX == kJoyCentre + kJoyMinDeflection + 17);  // (16 - 15) / 15 of the way to full
  // Between release and engage it stays deflected (hysteresis), at the minimum.
  r.flatLean(12, 0);
  r.run(200);
  assert(r.last.joyX == kJoyCentre + kJoyMinDeflection);
  // Under release: centred.
  r.flatLean(10, 0);
  r.step();
  assert(r.last.joyX == kJoyCentre);
  // Full lean and beyond: full deflection.
  r.flatLean(30, 0);
  r.run(200);
  assert(r.last.joyX == kJoyCentre + kJoyMaxDeflection);
  r.flatLean(45, 0);
  r.run(200);
  assert(r.last.joyX == kJoyCentre + kJoyMaxDeflection);
  // Past 60 degrees the device is in another pose: centred.
  r.flatLean(70, 0);
  r.run(200);
  assert(r.last.joyX == kJoyCentre && r.last.joyY == kJoyCentre);

  // One axis at a time: the engaged one stays while the other grows.
  Rig d;
  d.start();
  d.flatLean(20, 0);
  d.run(200);
  d.flatLean(20, 25);
  d.run(200);
  assert(d.last.joyX > kJoyCentre && d.last.joyY == kJoyCentre);
  // Released, the larger lean takes over.
  d.flatLean(5, 25);
  d.run(200);
  assert(d.last.joyX == kJoyCentre && d.last.joyY > kJoyCentre);
}

static void testTiltSettings() {
  Rig r;
  r.cfg.invertX = true;
  r.start();
  r.flatLean(20, 0);
  r.run(200);
  assert(r.last.joyX < kJoyCentre);

  Rig y;
  y.cfg.invertY = true;
  y.start();
  y.flatLean(0, 20);
  y.run(200);
  assert(y.last.joyY < kJoyCentre);

  Rig s;
  s.cfg.swapXY = true;
  s.start();
  s.flatLean(20, 0);
  s.run(200);
  assert(s.last.joyX == kJoyCentre && s.last.joyY > kJoyCentre);
}

static void testTapDoesNotMoveTheJoystick() {
  // The knock rocks the device for a few samples: no deflection from it.
  Rig r;
  r.start();
  r.step(true, 15.0f);
  r.flatLean(25, 0);  // rocked hard, for 100 ms
  r.run(100);
  assert(r.last.joyX == kJoyCentre);
  r.flatLean(0, 0);
  r.run(600);
  assert(r.last.joyX == kJoyCentre);
  assert(r.take() == kButtonA);

  // A tap while deflected keeps the deflection and still presses.
  Rig held;
  held.start();
  held.flatLean(20, 0);
  held.run(200);
  held.step(true, 15.0f);
  held.run(600);
  assert(held.last.joyX > kJoyCentre);
  assert(held.take() == kButtonA);
}

static void testRecentreAnyPose() {
  // Standing on its edge: +Y up. Five taps make that the centre.
  Rig r;
  r.start();
  r.pose(0, 1, 0);
  r.run(200);
  assert(r.last.joyX == kJoyCentre && r.last.joyY == kJoyCentre);  // 90 degrees away: out of range
  r.taps(5);
  r.run(800);
  assert(r.take() == 0 && r.recentres == 2);
  assert(r.last.joyX == kJoyCentre && r.last.joyY == kJoyCentre);
  assert(std::fabs(r.engine.reference()[1] - 1.0f) < 0.01f);
  // The +X end drops: right, as when lying flat.
  r.pose(-std::sin(20 * kDeg), std::cos(20 * kDeg), 0);
  r.run(200);
  assert(r.last.joyX > kJoyCentre && r.last.joyY == kJoyCentre);
  r.pose(0, 1, 0);
  r.run(200);
  // Turned about +X the same way as "up" when flat (flat: +Y rises; from
  // here the same turn brings -Z up).
  r.pose(0, std::cos(20 * kDeg), -std::sin(20 * kDeg));
  r.run(200);
  assert(r.last.joyY > kJoyCentre && r.last.joyX == kJoyCentre);
  // Turning about the vertical changes nothing (up stays where it is).
  r.pose(0, 1, 0);
  r.run(200);
  assert(r.last.joyX == kJoyCentre && r.last.joyY == kJoyCentre);

  // On its side, X vertical: Y takes X's place and it still works.
  Rig side;
  side.start();
  side.pose(1, 0, 0);
  side.run(100);
  side.engine.recentre();
  side.run(400);
  assert(side.recentres == 2);
  side.pose(std::cos(20 * kDeg), -std::sin(20 * kDeg), 0);
  side.run(200);
  assert(side.last.joyX > kJoyCentre && side.last.joyY == kJoyCentre);

  // A re-centre while leaning ends the scroll: the lean is the new centre.
  Rig lean;
  lean.start();
  lean.flatLean(25, 0);
  lean.run(200);
  assert(lean.last.joyX > kJoyCentre);
  lean.engine.recentre();
  lean.step();
  assert(lean.last.joyX == kJoyCentre);
  lean.run(400);
  assert(lean.last.joyX == kJoyCentre && lean.recentres == 2);
  lean.flatLean(0, 0);  // back to flat is now a lean to the left
  lean.run(200);
  assert(lean.last.joyX < kJoyCentre);
}

static void testCentreWaitsForCalm() {
  // The reference is not taken from samples of a device in motion.
  Rig r;
  for (int i = 0; i <= 30; ++i) r.step(false, (i % 3 == 0) ? 14.0f : 0.0f);  // the last one strong
  assert(!r.engine.hasReference() && r.recentres == 0);
  r.run(180);  // nine calm samples
  assert(!r.engine.hasReference());
  r.step();    // the tenth
  assert(r.engine.hasReference() && r.recentres == 1);
  assert(r.take() == 0);

  // Nor during a shake: it is taken once the shake is over.
  Rig shaken;
  for (int i = 0; i < 30; ++i) shaken.step(false, (i % 2) ? 14.0f : -14.0f);
  assert(shaken.engine.shaking() && !shaken.engine.hasReference());
  shaken.run(1000);
  assert(!shaken.engine.shaking() && shaken.engine.hasReference() && shaken.recentres == 1);
}

static void testGap() {
  // A gap in the samples (the bus paused, the IMU reopened): the run of taps
  // and the deflection are dropped, the reference pose is kept.
  Rig r;
  r.start();
  r.taps(2);
  r.flatLean(20, 0);
  r.run(100);
  assert(r.last.joyX > kJoyCentre);
  r.ms += 2000;
  r.step();
  assert(r.last.joyX == kJoyCentre && r.last.held == 0);  // the dwell starts again
  r.run(600);
  assert(r.take() == 0);  // the two taps are gone
  assert(r.recentres == 1 && r.last.joyX > kJoyCentre);  // same centre, and the lean is read again

  // The millisecond counter wrapping is not a gap.
  Rig wrap;
  wrap.ms = 0xFFFFFFFFu - 500;
  wrap.start();
  wrap.tap();
  wrap.run(600);
  assert(wrap.take() == kButtonA);
}

static void testLeanAxes() {
  float e1[3], e2[3];
  const float flat[3] = {0, 0, 1};
  assert(leanAxes(flat, e1, e2));
  assert(std::fabs(e1[0] - 1) < 1e-5f && std::fabs(e2[1] - 1) < 1e-5f);
  const float none[3] = {0, 0, 0};
  assert(!leanAxes(none, e1, e2));
  // Any pose: unit axes, square to up and to each other.
  const float poses[][3] = {{0, 1, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 0, -1}, {0.6f, 0.0f, 0.8f}, {0.5f, 0.5f, 0.7071f}};
  for (const auto& p : poses) {
    assert(leanAxes(p, e1, e2));
    assert(std::fabs(dot3(e1, e1) - 1) < 1e-4f && std::fabs(dot3(e2, e2) - 1) < 1e-3f);
    assert(std::fabs(dot3(e1, p)) < 1e-3f && std::fabs(dot3(e2, p)) < 1e-3f && std::fabs(dot3(e1, e2)) < 1e-4f);
  }
}

int main() {
  testRingingKnocksStillCount();
  testTapCounts();
  testTapTiming();
  testShake();
  testTapsWaitForCalm();
  testTiltDirections();
  testTiltThresholds();
  testTiltSettings();
  testTapDoesNotMoveTheJoystick();
  testRecentreAnyPose();
  testCentreWaitsForCalm();
  testGap();
  testLeanAxes();
  std::puts("input_imu_core: ok");
  return 0;
}
