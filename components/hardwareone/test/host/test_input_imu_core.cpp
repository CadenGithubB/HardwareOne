// The IMU as an input device (Input_IMUCore.h): a run of taps becomes one
// button after the tap window (side: 1 = A, 2 = B, 3 = Y, 4 = X; face: 2 =
// START, 3 = SELECT; other counts nothing), a sustained shake starts the
// configuration (still for 3 s: the centre; then 5 side and 5 face knocks;
// nothing changes unless every stage is done; a shake cancels it) and cancels
// taps, a lean from the reference pose is the joystick (one axis, hysteresis,
// a dwell), the reference can be any pose, and a gap drops what was being
// counted.
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
  uint8_t kind = kTapSide;   // where taps hit
  int16_t vec[3] = {0, 900, 100};  // the knock's peak jump (mg) taps carry
  int calEvents[16] = {0};   // count per calEvent
  uint8_t lastCountdown = 0;
  uint8_t lastEndStage = kStageNone;  // calStage of the last kCalTimedOut / kCalShakeCancelled / kCalCancelled
  uint8_t lastEndKnocks = 0;
  int learnEvents = 0;       // any teach-step event

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
    s.tapKind = kind;
    if (tap)
      for (int i = 0; i < 3; ++i) s.tapVec[i] = vec[i];
    last = engine.update(s, cfg);
    pressedSince |= last.pressed;
    if (last.recentred) ++recentres;
    if (last.calEvent) ++calEvents[last.calEvent];
    if (last.calEvent == kCalCountdown) lastCountdown = last.calSecondsLeft;
    if (last.calEvent == kCalTimedOut || last.calEvent == kCalShakeCancelled || last.calEvent == kCalCancelled) {
      lastEndStage = last.calStage;
      lastEndKnocks = last.calKnocks;
    }
    if (last.learnEvent) ++learnEvents;
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
  // A sustained shake (no taps), 800 ms.
  void shake() {
    for (int i = 0; i < 40; ++i) step(false, (i % 2) ? 14.0f : -14.0f);
  }
  // Steps until `pred` holds (at most `maxMs`); false if it never did.
  template <typename Pred>
  bool until(Pred pred, uint32_t maxMs) {
    for (uint32_t t = 0; t < maxMs; t += 20) {
      if (pred()) return true;
      step();
    }
    return pred();
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
  // Five and more on the side: nothing, and no re-centre.
  for (int count = 5; count <= 8; ++count) {
    Rig r;
    r.start();
    r.taps(count);
    r.run(1500);
    assert(r.take() == 0 && r.recentres == 1);
  }
  assert(buttonForTaps(0, false) == 0 && buttonForTaps(5, false) == 0 && buttonForTaps(16, false) == 0);
  assert(buttonForTaps(1, true) == 0 && buttonForTaps(2, true) == kButtonStart &&
         buttonForTaps(3, true) == kButtonSelect && buttonForTaps(4, true) == 0);
}

static void testFaceTaps() {
  const uint8_t expect[] = {0, 0, kButtonStart, kButtonSelect, 0, 0};
  for (int count = 1; count <= 5; ++count) {
    Rig r;
    r.kind = kTapFace;
    r.start();
    r.taps(count);
    r.run(600);
    assert(r.take() == expect[count]);
    if (!expect[count]) assert(r.engine.lastDrop() == kDropNoButton && r.engine.lastDropFace());
  }
  // A chip that cannot tell counts as side.
  Rig unknown;
  unknown.kind = kTapUnknown;
  unknown.start();
  unknown.taps(2);
  unknown.run(600);
  assert(unknown.take() == kButtonB);
}

static void testKindChangeEndsTheRun() {
  // One side tap, then two face taps right after: A at once, then START.
  Rig r;
  r.start();
  r.tap();
  r.kind = kTapFace;
  r.step(true, 15.0f);
  assert(r.last.pressed == kButtonA);  // the side run ended with the face tap
  r.run(140);
  r.tap();
  r.run(600);
  assert(r.take() == (kButtonA | kButtonStart));
  // Two face taps then a side tap: START, then the side run of one: A.
  Rig f;
  f.kind = kTapFace;
  f.start();
  f.taps(2);
  f.kind = kTapSide;
  f.tap();
  f.run(600);
  assert(f.take() == (kButtonStart | kButtonA));
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
  // A shake presses nothing; when it ends a calibration starts: the device is
  // set down, held still 3 s, and that pose becomes the centre.
  Rig r;
  r.start();
  for (int i = 0; i < 40; ++i) r.step(i % 5 == 0, (i % 2) ? 14.0f : -14.0f);
  assert(r.engine.shaking());
  assert(r.take() == 0);
  r.run(600);  // the shake winds down
  assert(!r.engine.shaking() && r.engine.calibrating());
  assert(r.calEvents[kCalStarted] == 1);
  // Set down standing on its edge (+Y up), still.
  r.pose(0, 1, 0);
  r.run(2000);
  assert(r.engine.calibrating() && r.calEvents[kCalCountdown] >= 2 && r.calEvents[kCalDone] == 0);
  // Taps and the joystick are off meanwhile.
  r.tap();
  r.run(400);
  assert(r.take() == 0 && r.last.joyX == kJoyCentre && r.last.joyY == kJoyCentre);
  // The tap broke the stillness: the countdown started over (moved).
  assert(r.calEvents[kCalMoved] >= 1);
  r.run(3600);
  assert(!r.engine.calibrating() && r.calEvents[kCalDone] == 1);
  assert(std::fabs(r.engine.reference()[1] - 1.0f) < 0.01f);
  assert(r.lastCountdown == 1);
  // Taps work again.
  r.tap();
  r.run(400);
  assert(r.take() == kButtonA);

  // Never still: cancelled after the timeout, the old centre kept.
  Rig busy;
  busy.start();
  for (int i = 0; i < 40; ++i) busy.step(false, (i % 2) ? 14.0f : -14.0f);
  busy.run(600);
  assert(busy.engine.calibrating());
  for (int i = 0; i < 1100; ++i) busy.step(false, (i % 4 == 0) ? 4.0f : 0.0f);  // fidgeting, not a shake
  assert(!busy.engine.calibrating() && busy.calEvents[kCalCancelled] == 1 && busy.calEvents[kCalDone] == 0);
  assert(std::fabs(busy.engine.reference()[2] - 1.0f) < 0.01f);

  // With shakeCalibrate off a shake does nothing (and still cancels taps).
  Rig off;
  off.cfg.shakeCalibrate = false;
  off.start();
  off.taps(2);
  for (int i = 0; i < 40; ++i) off.step(false, (i % 2) ? 14.0f : -14.0f);
  off.run(1500);
  assert(off.take() == 0 && !off.engine.calibrating() && off.calEvents[kCalStarted] == 0);
  assert(off.engine.lastDrop() == kDropShake && off.engine.lastDropTaps() == 2 && !off.engine.lastDropFace());

  // Five hard taps are not a shake (and on the side, five is nothing).
  Rig taps;
  taps.start();
  taps.taps(5, 140);
  assert(!taps.engine.shaking());
  taps.run(1000);
  assert(taps.take() == 0 && taps.recentres == 1 && !taps.engine.calibrating());
}

// ---- The shake configuration with the knock stages ---------------------------
// Knocks as the knock detector sees them: side knocks along Y (either sign),
// face ("top") knocks along +Z (the sign's and the pod's shapes).
static const int16_t kSideKnocks[5][3] = {{40, -1300, 200}, {-60, 1500, -150}, {90, -1100, 300}, {0, 1250, 100},
                                          {-120, -1400, -250}};
static const int16_t kFaceKnocks[5][3] = {{80, 150, 1600}, {-100, -60, 1400}, {30, 200, 1800}, {60, -90, 1500},
                                          {-40, 120, 1700}};

// One knock as it rings: the tap flag on a strong sample, two more strong
// samples, then quiet until `gapMs` after it.
static void knockOn(Rig& r, const int16_t v[3], uint8_t kind, uint32_t gapMs = 400) {
  for (int i = 0; i < 3; ++i) r.vec[i] = v[i];
  r.kind = kind;
  r.step(true, 15.0f);
  r.step(false, -12.0f);
  r.step(false, 9.0f);
  r.run(gapMs - 60);
}

// Started lying flat, then shaken and set down standing on its edge (+Y up):
// stops at the start of the side stage (the centre taken, not yet in use).
static void toSideStage(Rig& r) {
  r.cfg.teachKnocks = true;
  r.start();
  r.shake();
  assert(r.until([&] { return r.calEvents[kCalStarted] == 1; }, 1000));
  r.pose(0, 1, 0);
  assert(r.until([&] { return r.engine.calStage() == kStageSide; }, 4000));
  assert(r.calEvents[kCalSideStart] == 1 && r.calEvents[kCalDone] == 0);
}

static void toFaceStage(Rig& r) {
  toSideStage(r);
  for (int k = 0; k < 5; ++k) knockOn(r, kSideKnocks[k], k % 2 ? kTapFace : kTapSide);
  assert(r.engine.calStage() == kStageFace && r.calEvents[kCalFaceStart] == 1);
}

// The old centre (flat) is still in use and nothing was taught.
static void assertUnchanged(Rig& r) {
  assert(!r.engine.calibrating() && r.engine.calStage() == kStageNone);
  assert(std::fabs(r.engine.reference()[2] - 1.0f) < 0.01f && r.recentres == 1);
  assert(r.calEvents[kCalDone] == 0);
  assert(r.engine.sideTeach().state != knock::TeachState::Done && r.engine.faceTeach().state != knock::TeachState::Done);
}

// Taps are buttons again (set back down flat so the joystick is quiet).
static void assertTapsWork(Rig& r) {
  r.pose(0, 0, 1);
  r.run(2600);
  r.take();
  r.kind = kTapSide;
  r.taps(2);
  r.run(600);
  assert(r.take() == kButtonB);
  r.kind = kTapFace;
  r.taps(3);
  r.run(600);
  assert(r.take() == kButtonSelect);
}

static void testConfigurationHappyPath() {
  Rig r;
  toSideStage(r);
  assert(r.take() == 0);
  // The centre is not in use yet: the old one (flat) still is.
  assert(std::fabs(r.engine.reference()[2] - 1.0f) < 0.01f && r.recentres == 1);
  assert(r.last.joyX == kJoyCentre && r.last.joyY == kJoyCentre);
  // Five side knocks, counted; they press nothing whatever the detector called
  // them (some read as face here).
  for (int k = 0; k < 5; ++k) {
    knockOn(r, kSideKnocks[k], k % 2 ? kTapFace : kTapSide);
    if (k < 4) assert(r.engine.calStage() == kStageSide && r.engine.calKnocks() == k + 1);
  }
  assert(r.calEvents[kCalSideKnock] == 4 && r.calEvents[kCalFaceStart] == 1);
  assert(r.engine.calStage() == kStageFace && r.engine.calKnocks() == 0);
  assert(r.engine.sideTeach().state == knock::TeachState::Done);
  // Five top (face) knocks; on the fifth the configuration is done.
  for (int k = 0; k < 5; ++k) {
    knockOn(r, kFaceKnocks[k], k == 2 ? kTapSide : kTapFace);
    if (k < 4) assert(r.engine.calStage() == kStageFace && r.engine.calKnocks() == k + 1);
  }
  assert(r.calEvents[kCalFaceKnock] == 4 && r.calEvents[kCalDone] == 1);
  assert(r.take() == 0);  // ten knocks, no button
  assert(!r.engine.calibrating() && r.engine.calStage() == kStageNone);
  // Now the new centre (standing on its edge) is in use...
  assert(std::fabs(r.engine.reference()[1] - 1.0f) < 0.01f && r.recentres == 2);
  // ...and both steps are there for the caller to save, judged as imutapteach does.
  assert(r.engine.sideTeach().state == knock::TeachState::Done && r.engine.sideTeach().side);
  assert(r.engine.faceTeach().state == knock::TeachState::Done && !r.engine.faceTeach().side);
  const knock::TeachJudgement j =
      knock::judgeTaught(knock::Surfaces{}, &r.engine.sideTeach(), &r.engine.faceTeach());
  assert(j.verdict == knock::TeachVerdict::Ok && j.knocks == 10 && j.sepDeg > 75);
  // No mount teach step is offered, and nothing else was ended or timed out.
  assert(r.learnEvents == 0 && r.calEvents[kCalTimedOut] == 0 && r.calEvents[kCalShakeCancelled] == 0);
  // Standing on its edge, taps map normally again and the joystick follows the new centre.
  r.run(400);
  r.take();
  r.kind = kTapSide;
  r.taps(1);
  r.run(600);
  assert(r.take() == kButtonA);
  r.kind = kTapFace;
  r.taps(2);
  r.run(600);
  assert(r.take() == kButtonStart);
  r.pose(0, std::cos(20 * kDeg), std::sin(20 * kDeg));
  r.run(200);
  assert(r.last.joyX != kJoyCentre || r.last.joyY != kJoyCentre);
  // A tap without a peak jump (a chip that cannot tell) teaches nothing: the
  // stage waits for a real knock.
  Rig blind;
  toSideStage(blind);
  const int16_t none[3] = {0, 0, 0};
  knockOn(blind, none, kTapSide);
  assert(blind.engine.calKnocks() == 0 && blind.engine.calStage() == kStageSide);
}

static void testConfigurationTimeouts() {
  // Settle: never still within 20 s.
  Rig settle;
  settle.cfg.teachKnocks = true;
  settle.start();
  settle.shake();
  settle.run(600);
  for (int i = 0; i < 1100; ++i) settle.step(false, (i % 4 == 0) ? 4.0f : 0.0f);
  assert(settle.calEvents[kCalCancelled] == 1 && settle.lastEndStage == kStageSettle);
  assertUnchanged(settle);
  assertTapsWork(settle);
  // Side: three knocks, then nothing for 30 s.
  Rig side;
  toSideStage(side);
  for (int k = 0; k < 3; ++k) knockOn(side, kSideKnocks[k], kTapSide);
  assert(side.until([&] { return side.calEvents[kCalTimedOut] == 1; }, knock::kTeachTimeoutMs));
  assert(side.lastEndStage == kStageSide && side.lastEndKnocks == 3);
  assert(side.take() == 0);
  assertUnchanged(side);
  assertTapsWork(side);
  // Face: all five side knocks, two face knocks, then nothing.
  Rig face;
  toFaceStage(face);
  for (int k = 0; k < 2; ++k) knockOn(face, kFaceKnocks[k], kTapFace);
  assert(face.until([&] { return face.calEvents[kCalTimedOut] == 1; }, knock::kTeachTimeoutMs));
  assert(face.lastEndStage == kStageFace && face.lastEndKnocks == 2);
  assert(face.take() == 0);
  assertUnchanged(face);
  assertTapsWork(face);
  // The time-out is the knock stage's own: knocks keep a stage alive no longer.
  Rig slow;
  toSideStage(slow);
  const uint32_t began = slow.ms;
  for (int k = 0; k < 4; ++k) knockOn(slow, kSideKnocks[k], kTapSide, 7000);
  assert(slow.until([&] { return slow.calEvents[kCalTimedOut] == 1; }, knock::kTeachTimeoutMs));
  assert(slow.ms - began <= knock::kTeachTimeoutMs + 100 && slow.lastEndKnocks == 4);
}

// A shake in `r` (in some stage of the configuration): it is cancelled with
// nothing changed, and neither that shake nor one soon after starts another;
// one after a quiet kCalRearmMs does.
static void assertShakeCancels(Rig& r, uint8_t stage) {
  assert(r.engine.calStage() == stage);
  const int started = r.calEvents[kCalStarted];
  r.shake();
  assert(r.calEvents[kCalShakeCancelled] == 1 && r.lastEndStage == stage);
  r.run(1000);  // the shake ends
  assert(!r.engine.shaking() && r.calEvents[kCalStarted] == started && r.engine.calBlocked());
  assertUnchanged(r);
  assert(r.take() == 0);
  // Another shake 1 s after: still nothing (and it pushes the quiet period on).
  r.shake();
  r.run(1000);
  assert(r.calEvents[kCalStarted] == started && !r.engine.calibrating());
  r.run(1500);  // 2.5 s quiet after it
  assert(!r.engine.calBlocked());
  // Taps work meanwhile and after.
  assertTapsWork(r);
  // Now a shake starts a fresh configuration.
  r.shake();
  assert(r.until([&] { return r.calEvents[kCalStarted] == started + 1; }, 1000));
  assert(r.engine.calStage() == kStageSettle && r.engine.calKnocks() == 0);
}

static void testConfigurationShakeCancels() {
  // Settle.
  Rig settle;
  settle.cfg.teachKnocks = true;
  settle.start();
  settle.shake();
  assert(settle.until([&] { return settle.engine.calStage() == kStageSettle && !settle.engine.shaking(); }, 1000));
  settle.run(1000);
  assertShakeCancels(settle, kStageSettle);
  // Centre: the shake begins as the centre is being taken.
  Rig centre;
  centre.cfg.teachKnocks = true;
  centre.start();
  centre.shake();
  centre.pose(0, 1, 0);
  assert(centre.until([&] { return centre.engine.calStage() == kStageCentre; }, 5000));
  assertShakeCancels(centre, kStageCentre);
  assert(!centre.engine.capturing());
  // Side, after two knocks.
  Rig side;
  toSideStage(side);
  knockOn(side, kSideKnocks[0], kTapSide);
  knockOn(side, kSideKnocks[1], kTapSide);
  assertShakeCancels(side, kStageSide);
  // Face, after three face knocks.
  Rig face;
  toFaceStage(face);
  for (int k = 0; k < 3; ++k) knockOn(face, kFaceKnocks[k], kTapFace);
  assertShakeCancels(face, kStageFace);
  // Without the knock stages (a feed without peak jumps) the same rule holds.
  Rig tilt;
  tilt.start();
  tilt.shake();
  tilt.run(1000);
  assert(tilt.engine.calStage() == kStageSettle);
  assertShakeCancels(tilt, kStageSettle);
}

// A gap in the middle of the shake that cancelled a configuration is not the
// end of that shake's quiet period: the rearm starts at the gap's sample, so
// the rest of the same motion cannot start another configuration.
static void testGapDuringCancellingShakeRearms() {
  Rig r;
  r.cfg.teachKnocks = true;
  r.start();
  r.shake();
  assert(r.until([&] { return r.engine.calStage() == kStageSettle && !r.engine.shaking(); }, 1000));
  const int started = r.calEvents[kCalStarted];
  for (int i = 0; i < 130; ++i) r.step(false, (i % 2) ? 14.0f : -14.0f);  // past the 2 s rearm
  assert(r.engine.shaking() && r.calEvents[kCalShakeCancelled] == 1 && r.engine.calBlocked());
  r.ms += 400;
  r.step(false, 14.0f);
  assert(r.engine.calBlocked() && r.calEvents[kCalStarted] == started);
  for (int i = 0; i < 40; ++i) r.step(false, (i % 2) ? 14.0f : -14.0f);
  r.run(1000);
  assert(r.calEvents[kCalStarted] == started && r.engine.calBlocked());
}

// A gap while waiting to hold still is not time held still, however long it is.
static void testGapDuringSettleIsNotStill() {
  Rig r;
  r.cfg.teachKnocks = true;
  r.start();
  r.shake();
  assert(r.until([&] { return r.engine.calStage() == kStageSettle && !r.engine.shaking(); }, 1000));
  r.run(200);
  r.ms += 4000;
  r.step();
  assert(r.engine.calStage() == kStageSettle && r.calEvents[kCalSideStart] == 0);
  assert(r.until([&] { return r.engine.calStage() == kStageCentre; }, 4000));
}

static void testConfigurationKnocksPressNothing() {
  // Every count of side or face knocks, quick or slow, in either stage: no
  // button, no drop counted as a run, no joystick.
  Rig r;
  toSideStage(r);
  const uint32_t drops = r.engine.drops();
  for (int k = 0; k < 5; ++k) {
    knockOn(r, kSideKnocks[k], kTapSide, k < 2 ? 160 : 700);
    assert(r.last.pressed == 0 && r.last.held == 0 && r.last.joyX == kJoyCentre && r.last.joyY == kJoyCentre);
  }
  for (int k = 0; k < 5; ++k) {
    knockOn(r, kFaceKnocks[k], kTapFace, k < 3 ? 160 : 700);
    assert(r.last.pressed == 0 && r.last.held == 0);
  }
  assert(r.take() == 0 && r.calEvents[kCalDone] == 1 && r.engine.drops() == drops);
  // The knock that finishes the configuration is no tap either, even sorted
  // to the side (where one tap would be A).
  Rig last;
  toFaceStage(last);
  for (int k = 0; k < 4; ++k) knockOn(last, kFaceKnocks[k], kTapFace);
  knockOn(last, kFaceKnocks[4], kTapSide, 1000);
  assert(last.calEvents[kCalDone] == 1 && last.take() == 0 && last.engine.tapCount() == 0);
  // Five ringing knocks in quick succession are not a shake.
  Rig quick;
  toSideStage(quick);
  for (int k = 0; k < 5; ++k) knockOn(quick, kSideKnocks[k], kTapSide, 200);
  assert(quick.calEvents[kCalShakeCancelled] == 0 && quick.engine.calStage() == kStageFace);
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

// The mount (imuinputmount): mirrored first, then turned the way oledrotation
// turns the screen. The 8 mounts give the 8 cases the old swap and inversion
// flags gave.
static void testTiltSettings() {
  struct Case { uint8_t turns; bool mirror; float rightDeg, upDeg; int wantX, wantY; };
  // Leaning the device's right (+X end drops) shows as...
  const Case cases[] = {
      {0, false, 20, 0, +1, 0},  {1, false, 20, 0, 0, +1}, {2, false, 20, 0, -1, 0},
      {3, false, 20, 0, 0, -1},  {0, true, 20, 0, -1, 0},  {1, true, 20, 0, 0, -1},
      {2, true, 20, 0, +1, 0},   {3, true, 20, 0, 0, +1},  {0, false, 0, 20, 0, +1},
      {1, false, 0, 20, -1, 0},  {0, true, 0, 20, 0, +1},  {3, true, 0, 20, +1, 0},
  };
  for (const Case& c : cases) {
    Rig r;
    r.cfg.mountTurns = c.turns;
    r.cfg.mountMirror = c.mirror;
    r.start();
    r.flatLean(c.rightDeg, c.upDeg);
    r.run(200);
    const int dx = r.last.joyX > kJoyCentre ? 1 : r.last.joyX < kJoyCentre ? -1 : 0;
    const int dy = r.last.joyY > kJoyCentre ? 1 : r.last.joyY < kJoyCentre ? -1 : 0;
    assert(dx == c.wantX && dy == c.wantY);
  }
}

// The teach step: the first lean held past the engage angle, in the device's
// own frame, is reported; the mount it gives shows that lean as up on the
// screen for each of the 8 mounts and each screen rotation. No lean: it ends
// quietly after the timeout and reports nothing.
static void testLearnMount() {
  using input_rotation::Dir;
  using input_rotation::Mount;
  const float leans[4][2] = {{0, 20}, {20, 0}, {0, -20}, {-20, 0}};  // device up, right, down, left
  static const int kCodes[] = {0, 1, 90, 91, 180, 181, 270, 271};
  for (int code : kCodes) {
    for (int screen = 0; screen < 4; ++screen) {
      // A device mounted `code` relative to the panel: its own frame is the
      // straight frame un-turned and un-mirrored. "The top of the screen" in
      // the device's frame is the direction that maps to up.
      const Mount truth = input_rotation::mountFromCode(code);
      int topDir = -1;
      for (int d = 0; d < 4; ++d) {
        float x = leans[d][0], y = leans[d][1];
        input_rotation::orientVector(truth, screen, x, y);
        if (x == 0 && y > 0) topDir = d;
      }
      assert(topDir >= 0);
      Rig r;
      r.start();
      r.engine.startLearn(r.ms);
      r.run(200);
      assert(r.engine.learning() && r.last.joyX == kJoyCentre && r.last.joyY == kJoyCentre);
      r.flatLean(leans[topDir][0], leans[topDir][1]);
      uint8_t learned = 255;
      for (int i = 0; i < 20 && learned == 255; ++i) {
        r.step();
        if (r.last.learnEvent == kLearnDone) learned = r.last.learnDir;
      }
      assert(learned == topDir && !r.engine.learning());
      const int turns = input_rotation::learnQuarterTurns(static_cast<Dir>(learned), truth.mirror, screen);
      assert(input_rotation::mountCode(turns * 90, truth.mirror) == code);
    }
  }
  // No lean: a quiet timeout, nothing learned, and the joystick works again.
  Rig idle;
  idle.start();
  idle.engine.startLearn(idle.ms);
  bool timedOut = false, done = false;
  for (uint32_t t = 0; t < kLearnTimeoutMs + 200; t += 20) {
    idle.step();
    timedOut |= idle.last.learnEvent == kLearnTimedOut;
    done |= idle.last.learnEvent == kLearnDone;
  }
  assert(timedOut && !done && !idle.engine.learning());
  idle.flatLean(20, 0);
  idle.run(200);
  assert(idle.last.joyX > kJoyCentre);
  // A shake configuration no longer offers the step: afterwards the joystick
  // works at once and face taps are buttons again.
  Rig cal;
  cal.start();
  cal.shake();
  assert(cal.until([&] { return cal.calEvents[kCalDone] == 1; }, 5000));
  cal.run(kLearnTimeoutMs);
  assert(cal.learnEvents == 0 && !cal.engine.learning());
  cal.take();
  cal.kind = kTapFace;
  cal.taps(2);
  cal.run(600);
  assert(cal.take() == kButtonStart && !cal.engine.learning() && cal.learnEvents == 0);
  // imuinputlearn still starts it.
  Rig face;
  face.start();
  face.engine.startLearn(face.ms);
  face.run(100);
  assert(face.engine.learning());
  // A shake ends a running step, and says so.
  bool cancelled = false;
  for (int i = 0; i < 40; ++i) {
    face.step(false, (i % 2) ? 14.0f : -14.0f);
    cancelled |= face.last.learnEvent == kLearnCancelled;
  }
  assert(cancelled && !face.engine.learning());
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
  // Standing on its edge: +Y up. A re-centre makes that the centre.
  Rig r;
  r.start();
  r.pose(0, 1, 0);
  r.run(200);
  assert(r.last.joyX == kJoyCentre && r.last.joyY == kJoyCentre);  // 90 degrees away: out of range
  r.engine.recentre();
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

// ---- A 100 Hz feed through the decimator (the BNO055 with IMU input) -------------
static void testDecimationFactor() {
  assert(decimationFor(0) == 1);   // sampled every imuDevicePollMs
  assert(decimationFor(20) == 1);  // the 4517
  assert(decimationFor(50) == 1);
  assert(decimationFor(10) == 2);  // the BNO055
  assert(decimationFor(5) == 4);
  assert(decimationFor(1) == 8);   // capped
}

static void testDecimatorPassThrough() {
  // Factor 1: every sample goes to the engine unchanged, taps and kinds too.
  Decimator d;
  for (uint32_t i = 0; i < 50; ++i) {
    Sample s;
    s.ms = 1000 + 20 * i;
    s.up[0] = 0.1f * (float)i;
    s.accel[2] = (float)i;
    s.tap = i % 3 == 0;
    s.tapKind = s.tap ? (uint8_t)(i % 2 ? kTapFace : kTapSide) : kTapUnknown;
    Sample out;
    assert(d.add(s, 1, out));
    assert(out.ms == s.ms && out.up[0] == s.up[0] && out.accel[2] == s.accel[2]);
    assert(out.tap == s.tap && out.tapKind == s.tapKind);
  }
  assert(d.waiting() == 0);
}

static void testDecimatorKeepsEveryTap() {
  Decimator d;
  Sample out;
  uint32_t ms = 1000;
  auto feed = [&](bool tap, uint8_t kind, bool& forwarded) {
    Sample s;
    s.ms = ms;
    s.tap = tap;
    s.tapKind = kind;
    ms += 10;
    forwarded = d.add(s, 2, out);
  };
  bool fwd = false;
  // Every second sample goes on: the first is held, the second forwarded.
  feed(false, 0, fwd);
  assert(!fwd);
  feed(false, 0, fwd);
  assert(fwd && !out.tap && out.ms == 1010);
  // A tap on the held sample goes out with the next one, with its kind.
  feed(true, kTapFace, fwd);
  assert(!fwd && d.waiting() == 1);
  feed(false, 0, fwd);
  assert(fwd && out.tap && out.tapKind == kTapFace && d.waiting() == 0);
  // ...and its peak jump; a sample without a tap carries none.
  {
    Sample s;
    s.ms = ms;
    s.tap = true;
    s.tapKind = kTapSide;
    s.tapVec[0] = 12;
    s.tapVec[1] = -900;
    s.tapVec[2] = 345;
    ms += 10;
    assert(!d.add(s, 2, out));
    Sample q;
    q.ms = ms;
    q.tapVec[1] = 77;  // not a tap: ignored
    ms += 10;
    assert(d.add(q, 2, out) && out.tap && out.tapKind == kTapSide);
    assert(out.tapVec[0] == 12 && out.tapVec[1] == -900 && out.tapVec[2] == 345);
    feed(false, 0, fwd);
    feed(false, 0, fwd);
    assert(fwd && !out.tap && out.tapVec[0] == 0 && out.tapVec[1] == 0 && out.tapVec[2] == 0);
  }
  // Taps on both samples of one engine period: one now, the other at the next
  // update, oldest first.
  feed(true, kTapSide, fwd);
  feed(true, kTapFace, fwd);
  assert(fwd && out.tap && out.tapKind == kTapSide && d.waiting() == 1);
  feed(false, 0, fwd);
  assert(!fwd);
  feed(false, 0, fwd);
  assert(fwd && out.tap && out.tapKind == kTapFace && d.waiting() == 0);
  feed(false, 0, fwd);
  feed(false, 0, fwd);
  assert(fwd && !out.tap);
  // A gap drops what waits (the engine drops its run as well).
  feed(true, kTapSide, fwd);
  assert(d.waiting() == 1);
  ms += kGapMs + 50;
  feed(false, 0, fwd);
  assert(d.waiting() == 0);
}

// The engine fed at 100 Hz through the decimator: the same buttons, and the
// sample-counted windows keep their times.
struct Rig100 {
  Engine engine;
  Decimator dec;
  Config cfg;
  uint32_t ms = 1000;
  float up[3] = {0, 0, 1};
  uint8_t pressedSince = 0;
  int recentres = 0;
  int calEvents[16] = {0};
  int updates = 0;
  Output last;
  void step(bool tap = false, uint8_t kind = kTapSide, float extraAccel = 0.0f) {
    Sample s;
    s.ms = ms;
    for (int i = 0; i < 3; ++i) {
      s.up[i] = up[i];
      s.accel[i] = kGravity * up[i];
    }
    s.accel[0] += extraAccel;
    s.tap = tap;
    s.tapKind = kind;
    Sample in;
    if (dec.add(s, decimationFor(10), in)) {
      last = engine.update(in, cfg);
      ++updates;
      pressedSince |= last.pressed;
      if (last.recentred) ++recentres;
      if (last.calEvent) ++calEvents[last.calEvent];
    }
    ms += 10;
  }
  void run(uint32_t forMs) {
    for (uint32_t t = 0; t < forMs; t += 10) step();
  }
  // One knock: the detector's tap on one sample (odd or even), strong.
  void taps(int count, uint32_t gapMs, uint8_t kind, bool odd) {
    for (int i = 0; i < count; ++i) {
      if (odd) step();
      step(true, kind, 15.0f);
      run(gapMs - (odd ? 20 : 10));
    }
  }
  uint8_t take() {
    const uint8_t p = pressedSince;
    pressedSince = 0;
    return p;
  }
};

static void testEngineAt100Hz() {
  const uint8_t side[] = {0, kButtonA, kButtonB, kButtonY, kButtonX};
  for (int odd = 0; odd < 2; ++odd) {
    for (int count = 1; count <= 4; ++count) {
      Rig100 r;
      r.run(400);
      assert(r.engine.hasReference() && r.recentres == 1);
      r.take();
      r.taps(count, 160, kTapSide, odd != 0);
      r.run(600);
      assert(r.take() == side[count]);
    }
    // The 4-tap run about 250 ms apart is still X with a wider window.
    Rig100 x;
    x.cfg.tapWindowMs = 450;
    x.run(400);
    x.take();
    x.taps(4, 250, kTapSide, odd != 0);
    x.run(800);
    assert(x.take() == kButtonX);
    // Face: 2 START, 3 SELECT.
    Rig100 f;
    f.run(400);
    f.take();
    f.taps(2, 160, kTapFace, odd != 0);
    f.run(600);
    assert(f.take() == kButtonStart);
    f.taps(3, 160, kTapFace, odd != 0);
    f.run(600);
    assert(f.take() == kButtonSelect);
  }
  // Two taps 10 ms apart (each a knock the detector told apart): both count.
  Rig100 two;
  two.cfg.tapMinGapMs = 0;
  two.run(400);
  two.take();
  two.step(true, kTapSide, 15.0f);
  two.step(true, kTapSide, 15.0f);
  two.run(600);
  assert(two.take() == kButtonB);
  // The engine is updated every 20 ms: 1 s of samples is 50 updates.
  Rig100 rate;
  rate.run(1000);
  assert(rate.updates == 50);
}

static void testWindowsKeepTheirTimesAt100Hz() {
  // A lean engages after the dwell: three engine updates, 60 ms, not 30.
  Rig100 r;
  r.run(400);
  const float d = 25.0f * kDeg;
  r.up[0] = -std::sin(d);
  r.up[1] = 0;
  r.up[2] = std::cos(d);
  int engagedAfterMs = -1;
  for (int t = 0; t < 200 && engagedAfterMs < 0; t += 10) {
    r.step();
    if (r.last.joyX != kJoyCentre) engagedAfterMs = t;
  }
  assert(engagedAfterMs >= 40 && engagedAfterMs <= 60);
  // A shake: strong more than half of 500 ms starts the calibration; 200 ms
  // of it is not enough (20 samples at 100 Hz, but only 10 engine updates).
  Rig100 s;
  s.run(400);
  for (int t = 0; t < 200; t += 10) s.step(false, kTapSide, (t / 10) % 2 ? 20.0f : -20.0f);
  s.run(600);
  assert(s.calEvents[kCalStarted] == 0);
  for (int t = 0; t < 1200; t += 10) s.step(false, kTapSide, (t / 10) % 2 ? 20.0f : -20.0f);
  s.run(800);  // the shake ends once at most 2 of the last 25 updates were strong
  assert(s.calEvents[kCalStarted] == 1);
  // Still for 3 s: done, the centre taken from 10 calm updates (200 ms).
  s.run(3000);
  assert(s.calEvents[kCalDone] == 1);
}

int main() {
  testDecimationFactor();
  testDecimatorPassThrough();
  testDecimatorKeepsEveryTap();
  testEngineAt100Hz();
  testWindowsKeepTheirTimesAt100Hz();
  testRingingKnocksStillCount();
  testTapCounts();
  testFaceTaps();
  testKindChangeEndsTheRun();
  testTapTiming();
  testShake();
  testConfigurationHappyPath();
  testConfigurationTimeouts();
  testConfigurationShakeCancels();
  testGapDuringCancellingShakeRearms();
  testGapDuringSettleIsNotStill();
  testConfigurationKnocksPressNothing();
  testTapsWaitForCalm();
  testTiltDirections();
  testTiltThresholds();
  testTiltSettings();
  testLearnMount();
  testTapDoesNotMoveTheJoystick();
  testRecentreAnyPose();
  testCentreWaitsForCalm();
  testGap();
  testLeanAxes();
  std::puts("input_imu_core: ok");
  return 0;
}
