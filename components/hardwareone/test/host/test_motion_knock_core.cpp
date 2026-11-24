// Host test for HAL_Motion_KnockCore.h: knocks found from 416 Hz samples (the
// 4517's FIFO) and from 100 Hz samples (the BNO055 with IMU input), the
// shared feed's pending taps and clock (HAL_Motion_KnockFeed.cpp uses them),
// and side or face by taught surfaces (imutapteach) or the ratio rule.
//   clang++ -std=c++17 -I. -Itest/host -o /tmp/t test/host/test_motion_knock_core.cpp && /tmp/t
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "HAL_Motion_KnockCore.h"

using knock::Config;
using knock::Detector;
using knock::Event;
using knock::Verdict;

namespace {

constexpr uint32_t kStepUs = 2404;  // 416 Hz

// A signal: gravity on z plus whatever `extra` adds at time t (ms).
template <typename F>
std::vector<Event> run(const Config& cfg, uint32_t totalMs, F extra) {
  Detector d;
  std::vector<Event> out;
  for (uint32_t t = 1000; t < 1000 + totalMs * 1000u; t += kStepUs) {
    float mg[3] = {0, 0, 1000};
    extra(t / 1000.0f, mg);
    Event ev;
    if (d.add(cfg, mg, t, ev)) out.push_back(ev);
  }
  return out;
}

// A knock: a decaying oscillation (about 67 Hz, well inside the 416 Hz
// samples) starting at t0 (ms), peak amplitude mg, about lenMs long.
void knockAt(float t, float t0, float amp, float lenMs, float* mg) {
  if (t < t0 || t > t0 + lenMs) return;
  const float x = t - t0;
  mg[2] += amp * expf(-x / (lenMs / 2)) * cosf(x * 2.0f * 3.14159f / 15.0f);
}

void testSingleTap() {
  Config cfg;
  auto ev = run(cfg, 500, [](float t, float* mg) { knockAt(t, 100, 900, 25, mg); });
  assert(ev.size() == 1);
  assert(ev[0].verdict == Verdict::Tap);
  assert(ev[0].peakMg > 500 && ev[0].peakMg < 1000);  // sampled at 416 Hz, the top is often between samples
  assert(ev[0].durMs <= 25);
  assert(ev[0].gapMs == 0xFFFF);
  // The knock was on z: its direction is +Z or -Z and z dominates.
  char dir[3];
  uint8_t share = 0;
  knock::knockDirection(ev[0], dir, share);
  assert(dir[1] == 'Z' && share > 90);
  assert(!ev[0].side);  // a Z knock is a face knock (side axis Y)
  // The same knock on Y is a side knock; with Y under sidePct of Z, the face.
  auto evY = run(cfg, 500, [](float t, float* mg) {
    float z[3] = {0, 0, 0};
    knockAt(t, 100, 3000, 25, z);
    mg[1] += z[2];
  });
  assert(evY.size() == 1 && evY[0].side);
  // A soft side knock (Y 1.2 g, Z 0.4 g) is still the side.
  evY = run(cfg, 500, [](float t, float* mg) {
    float k[3] = {0, 0, 0};
    knockAt(t, 100, 1500, 25, k);
    mg[1] += k[2];
    mg[2] += k[2] / 3;
  });
  assert(evY.size() == 1 && evY[0].side);
  // Y at a fifth of Z is the face.
  evY = run(cfg, 500, [](float t, float* mg) {
    float k[3] = {0, 0, 0};
    knockAt(t, 100, 3000, 25, k);
    mg[2] += k[2];
    mg[1] += k[2] / 5;
  });
  assert(evY.size() == 1 && !evY[0].side);
}

void testFastQuadAllCount() {
  // Four knocks 110 ms apart, the later ones softer, as in a quick run.
  Config cfg;
  auto ev = run(cfg, 800, [](float t, float* mg) {
    knockAt(t, 100, 900, 25, mg);
    knockAt(t, 210, 850, 25, mg);
    knockAt(t, 320, 800, 25, mg);
    knockAt(t, 430, 900, 25, mg);
  });
  int taps = 0;
  for (auto& e : ev) taps += e.verdict == Verdict::Tap;
  assert(taps == 4);
  assert(ev.size() == 4);
  assert(ev[1].gapMs >= 105 && ev[1].gapMs <= 115);
}

void testSoftKnockRecorded() {
  Config cfg;
  auto ev = run(cfg, 400, [](float t, float* mg) { knockAt(t, 100, 600, 25, mg); });
  assert(ev.size() == 1 && ev[0].verdict == Verdict::Soft);
  assert(ev[0].peakMg > 250 && ev[0].peakMg < 500);
}

void testRingingAfterTap() {
  // A second bump 50 ms after a tap is its ringing.
  Config cfg;
  auto ev = run(cfg, 400, [](float t, float* mg) {
    knockAt(t, 100, 900, 25, mg);
    knockAt(t, 150, 700, 15, mg);
  });
  assert(ev.size() == 2 && ev[0].verdict == Verdict::Tap && ev[1].verdict == Verdict::Ring);
  cfg.ringMs = 10;  // a short ring window lets it count
  ev = run(cfg, 400, [](float t, float* mg) {
    knockAt(t, 100, 900, 25, mg);
    knockAt(t, 150, 700, 15, mg);
  });
  assert(ev.size() == 2 && ev[1].verdict == Verdict::Tap);
}

void testHandlingIsLongAndTiltIgnored() {
  Config cfg;
  // A 200 ms swing of 800 mg: long, not a tap.
  auto ev = run(cfg, 600, [](float t, float* mg) {
    if (t > 100 && t < 300) mg[0] += 800;
  });
  assert(!ev.empty() && ev[0].verdict == Verdict::Long && ev[0].durMs >= 150);
  // A soft bump right after a long knock is its ringing, not a soft knock.
  ev = run(cfg, 800, [](float t, float* mg) {
    if (t > 100 && t < 300) mg[0] += 800;
    knockAt(t, 360, 600, 15, mg);
  });
  assert(ev.size() == 2 && ev[1].verdict == Verdict::Ring);
  // A slow tilt (gravity moving to x over 1 s) is no knock.
  ev = run(cfg, 2000, [](float t, float* mg) {
    const float a = t < 1000 ? 0 : (t > 2000 ? 1.5708f : (t - 1000) / 1000 * 1.5708f);
    mg[0] = 1000 * sinf(a);
    mg[2] = 1000 * cosf(a);
  });
  assert(ev.empty());
}

void testTrace() {
  knock::Trace tr;
  Event e;
  for (int i = 0; i < 30; ++i) {
    e.ms = (uint32_t)i;
    e.verdict = (i % 3 == 0) ? Verdict::Soft : Verdict::Tap;
    e.peakMg = (uint16_t)(100 + i);
    tr.push(e);
  }
  assert(tr.count == knock::kTraceLen && tr.seq == 30);
  assert(tr.at(0).ms == 30 - knock::kTraceLen && tr.at(knock::kTraceLen - 1).ms == 29);
  assert(tr.counts[(int)Verdict::Soft] == 10 && tr.counts[(int)Verdict::Tap] == 20);
  assert(tr.softMaxMg == 127);
}

// ---- 100 Hz (the BNO055) ---------------------------------------------------------
constexpr uint32_t kStep100Us = 10000;

Config cfg100() {
  Config c;
  c.thresholdMg = knock::kKnockThresholdMg100Hz;
  c.sampleHz = 100;
  return c;
}

// A knock as the BNO055 sees it (62.5 Hz filtered, read at 100 Hz): a
// decaying swing of about 40 ms period, so one sample under the start level
// sits between two above it (x = 10 ms) while it rings.
void knock100At(float t, float t0, float amp, float lenMs, float* axis) {
  if (t < t0 || t > t0 + lenMs) return;
  const float x = t - t0;
  *axis += amp * expf(-x / (lenMs / 2)) * cosf(x * 2.0f * 3.14159f / 40.0f);
}

// The feed at 100 Hz, as the BNO055 backend runs it: each sample stamped by
// the forward-only clock from its read time (with a little jitter).
template <typename F>
std::vector<Event> run100(const Config& cfg, uint32_t totalMs, F extra, knock::Feed* feedOut = nullptr) {
  knock::Feed feed;
  std::vector<Event> out;
  uint32_t i = 0;
  for (uint32_t t = 0; t < totalMs * 1000u; t += kStep100Us, ++i) {
    float mg[3] = {0, 0, 1000};
    extra(t / 1000.0f, mg);
    const uint32_t readUs = 0xFFF00000u + t + (i % 3) * 400;  // reads land up to 0.8 ms late; the clock wraps
    Event ev;
    if (feed.add(cfg, mg, feed.clock.at(readUs), ev)) out.push_back(ev);
  }
  if (feedOut) *feedOut = feed;
  return out;
}

void testQuietTimePerRate() {
  assert(knock::knockQuietUs(416) == knock::kKnockQuietUs);  // the 4517 keeps its 8 ms
  assert(knock::knockQuietUs(100) == 20000);                 // two 10 ms periods
  assert(knock::knockQuietUs(0) == knock::kKnockQuietUs);
}

void testSingleTap100() {
  const Config cfg = cfg100();
  // A face knock (Z), 600 mg as the 100 Hz stream sees it: one tap, not split
  // by the sample under the start level while it rings.
  auto ev = run100(cfg, 600, [](float t, float* mg) { knock100At(t, 200, 600, 40, &mg[2]); });
  assert(ev.size() == 1);
  assert(ev[0].verdict == Verdict::Tap);
  assert(ev[0].peakMg >= 550 && ev[0].peakMg <= 650);
  assert(ev[0].durMs <= 30);
  assert(!ev[0].side);
  // With the 416 Hz quiet time (8 ms) the same samples would end the knock at
  // the first low sample and count its swing back as a second knock.
  Config fast = cfg;
  fast.sampleHz = 416;
  auto split = run100(fast, 600, [](float t, float* mg) { knock100At(t, 200, 600, 40, &mg[2]); });
  assert(split.size() == 2);
  // A soft one (220 mg, under the 300 mg default) is recorded, not counted.
  ev = run100(cfg, 600, [](float t, float* mg) { knock100At(t, 200, 220, 40, &mg[2]); });
  assert(ev.size() == 1 && ev[0].verdict == Verdict::Soft);
}

void testFourTapRun100() {
  const Config cfg = cfg100();
  knock::Feed feed;
  // Four side knocks (Y) about 250 ms apart: four taps, each its own knock,
  // and all four wait as side taps.
  auto ev = run100(cfg, 1500, [](float t, float* mg) {
    for (int k = 0; k < 4; ++k) knock100At(t, 200 + 250.0f * k, 700, 40, &mg[1]);
  }, &feed);
  assert(ev.size() == 4);
  for (const Event& e : ev) {
    assert(e.verdict == Verdict::Tap);
    assert(e.side);
  }
  assert(ev[1].gapMs >= 240 && ev[1].gapMs <= 260);
  assert(feed.pending.count == 4);
  bool side = false;
  for (int k = 0; k < 4; ++k) assert(feed.pending.pop(side) && side);
  assert(!feed.pending.pop(side));
}

void testRinging100() {
  const Config cfg = cfg100();
  // A tap, then the board swinging back 50 ms after it ended: ring, not a tap.
  auto ev = run100(cfg, 800, [](float t, float* mg) {
    knock100At(t, 200, 700, 40, &mg[2]);
    knock100At(t, 290, 600, 20, &mg[2]);
  });
  assert(ev.size() == 2);
  assert(ev[0].verdict == Verdict::Tap);
  assert(ev[1].verdict == Verdict::Ring);
  // The same second knock 200 ms later is a tap of its own.
  ev = run100(cfg, 800, [](float t, float* mg) {
    knock100At(t, 200, 700, 40, &mg[2]);
    knock100At(t, 450, 600, 20, &mg[2]);
  });
  assert(ev.size() == 2 && ev[1].verdict == Verdict::Tap);
}

void testSideFace100() {
  const Config cfg = cfg100();
  // Side: mostly Y with some Z. Face: mostly Z with a little Y.
  auto ev = run100(cfg, 600, [](float t, float* mg) {
    float k = 0;
    knock100At(t, 200, 800, 40, &k);
    mg[1] += k;
    mg[2] += k / 3;
  });
  assert(ev.size() == 1 && ev[0].verdict == Verdict::Tap && ev[0].side);
  ev = run100(cfg, 600, [](float t, float* mg) {
    float k = 0;
    knock100At(t, 200, 800, 40, &k);
    mg[2] += k;
    mg[1] += k / 5;
  });
  assert(ev.size() == 1 && ev[0].verdict == Verdict::Tap && !ev[0].side);
  // Another side axis: X.
  Config cx = cfg;
  cx.sideAxis = 0;
  ev = run100(cx, 600, [](float t, float* mg) { knock100At(t, 200, 800, 40, &mg[0]); });
  assert(ev.size() == 1 && ev[0].side);
}

// ---- The shared feed's parts ---------------------------------------------------
void testPendingTaps() {
  knock::PendingTaps p;
  bool side = true;
  assert(!p.pop(side));
  p.push(true);
  p.push(false);
  p.push(true);
  assert(p.count == 3);
  assert(p.pop(side) && side);
  assert(p.pop(side) && !side);
  assert(p.pop(side) && side);
  assert(!p.pop(side));
  // At most kPendingMax wait; the oldest keep their kinds.
  for (int i = 0; i < 12; ++i) p.push(i % 2 == 0);
  assert(p.count == knock::kPendingMax);
  for (int i = 0; i < knock::kPendingMax; ++i) assert(p.pop(side) && side == (i % 2 == 0));
  p.push(true);
  p.clear();
  assert(p.count == 0 && !p.pop(side));
  // Each tap keeps its peak jump (the shake configuration teaches from it),
  // oldest first; a tap pushed without one has zero.
  const int16_t a[3] = {10, -900, 300}, b[3] = {-5, 80, 1700};
  p.push(true, a);
  p.push(false, b);
  p.push(true);
  int16_t v[3] = {1, 1, 1};
  assert(p.pop(side, v) && side && v[0] == 10 && v[1] == -900 && v[2] == 300);
  assert(p.pop(side, v) && !side && v[0] == -5 && v[1] == 80 && v[2] == 1700);
  assert(p.pop(side, v) && side && v[0] == 0 && v[1] == 0 && v[2] == 0);
  // The feed hands a tap's peak jump on with it.
  knock::Feed feed;
  Config cfg = cfg100();
  float mg[3] = {0, 0, 1000};
  Event ev;
  uint32_t us = 0;
  bool got = false;
  for (int i = 0; i < 200 && !got; ++i, us += 10000) {
    mg[1] = (i == 60) ? 900.0f : 0.0f;
    got = feed.add(cfg, mg, us, ev) && ev.verdict == Verdict::Tap;
  }
  assert(got && feed.pending.count == 1);
  assert(feed.pending.pop(side, v) && v[0] == ev.peakVec[0] && v[1] == ev.peakVec[1] && v[2] == ev.peakVec[2]);
  assert(v[1] > 500);
}

void testSampleClock() {
  knock::SampleClock c;
  // One read per sample: its time, never going back.
  assert(c.at(1000) == 1000);
  assert(c.at(11000) == 11000);
  assert(c.at(10500) == 11001);  // a read stamped earlier than the last: just after it
  assert(c.at(11001) == 11002);
  knock::SampleClock w;  // across the wrap of the microsecond clock
  assert(w.at(0xFFFFE000u) == 0xFFFFE000u);
  assert(w.at(0x00000800u) == 0x00000800u);
  assert(w.at(0xFFFFF000u) == 0x00000801u);  // earlier than the last: just after it
  // FIFO batches: counted on from the last batch unless far behind.
  knock::SampleClock f;
  f.sync(5000);
  assert(f.take(2404) == 5000 && f.take(2404) == 7404);
  f.sync(9000);  // close to where it counts from: keeps counting
  assert(f.take(2404) == 9808);
  f.sync(60000);  // 50 ms ahead of it: starts from the estimate
  assert(f.take(2404) == 60000);
  f.reset();
  f.sync(100);
  assert(f.take(2404) == 100);
}

void testFeedRestartKeepsTaps() {
  const Config cfg = cfg100();
  knock::Feed feed;
  auto ev = run100(cfg, 600, [](float t, float* mg) { knock100At(t, 200, 700, 40, &mg[1]); }, &feed);
  assert(ev.size() == 1 && feed.pending.count == 1);
  feed.restart();  // samples lost: baseline and clock over, the tap still goes out
  assert(feed.pending.count == 1 && !feed.clock.have && !feed.detector.inKnock());
  feed.reset();
  assert(feed.pending.count == 0);
}

}  // namespace

// A baseline taken from a wrong first reading (zeros, or a pose changed while
// starting) must not leave the detector inside a knock for good.
static void testStuckBaselineRecovers() {
  Config cfg;
  Detector d;
  Event ev;
  float zero[3] = {0, 0, 0};
  assert(!d.add(cfg, zero, 1000, ev));  // ignored: no measurement
  float still[3] = {0, 0, 1000};
  // A baseline far from the still pose: force one by feeding an offset first.
  float off[3] = {1000, 0, 0};
  d.add(cfg, off, 2000, ev);
  bool sawLong = false;
  uint32_t t = 2000;
  for (int i = 0; i < 1000; ++i) {
    t += 2404;
    if (d.add(cfg, still, t, ev) && ev.verdict == knock::Verdict::Long) sawLong = true;
  }
  assert(sawLong && !d.inKnock() && d.lastDevMg() < 50);
  // And a real tap after it still counts.
  std::vector<Event> evs;
  for (uint32_t k = 0; k < 200; ++k) {
    t += 2404;
    float mg[3] = {0, 0, 1000};
    knockAt(k * 2.404f, 100, 900, 25, mg);
    if (d.add(cfg, mg, t, ev)) evs.push_back(ev);
  }
  assert(!evs.empty() && evs.back().verdict == knock::Verdict::Tap);
}

// Zeros are not a measurement, but they must not hold a knock open past 1 s,
// and they must not become the baseline the next real reading is judged from.
static void testOpenKnockEndsOnZeros() {
  Config cfg;
  Detector d;
  Event ev;
  float still[3] = {0, 0, 1000};
  assert(!d.add(cfg, still, 0, ev));
  float hit[3] = {0, 2000, 1000};
  assert(!d.add(cfg, hit, 10000, ev) && d.inKnock());
  float zero[3] = {0, 0, 0};
  assert(!d.add(cfg, zero, 10000 + 500000, ev) && d.inKnock());
  assert(d.add(cfg, zero, 10000 + 1000000, ev));
  assert(ev.verdict == knock::Verdict::Long && !d.inKnock());
  assert(!d.add(cfg, still, 10000 + 1100000, ev) && !d.inKnock());
  assert(!d.add(cfg, still, 10000 + 1200000, ev) && d.lastDevMg() < 50);
}

// ---- Taught surfaces (imutapteach) ------------------------------------------------
using knock::Surface;
using knock::Surfaces;
using knock::Teacher;
using knock::TeachState;

struct Vec { int16_t v[3]; };

// A tap event with this peak jump.
Event tapWith(const Vec& p, Verdict verdict = Verdict::Tap) {
  Event e;
  e.verdict = verdict;
  for (int i = 0; i < 3; ++i) e.peakVec[i] = p.v[i];
  return e;
}

// A teach step's knocks (fewer than five leave it recording; the model is
// built from what it holds either way).
Teacher stepWith(const std::vector<Vec>& knocks, bool side) {
  Teacher t;
  t.start(side, 1000);
  for (const Vec& k : knocks) t.add(tapWith(k));
  assert(t.got.count == (knocks.size() < knock::kTeachKnocks ? knocks.size() : knock::kTeachKnocks));
  return t;
}

// Surfaces taught from these side and face knocks.
Surfaces teachBoth(const std::vector<Vec>& side, const std::vector<Vec>& face, uint16_t* faceAgree = nullptr) {
  uint16_t agree = 0;
  Surfaces s = knock::withTaught(Surfaces{}, stepWith(side, true), agree);
  s = knock::withTaught(s, stepWith(face, false), agree);
  if (faceAgree) *faceAgree = agree;
  assert(s.taught());
  return s;
}

bool sortsSide(const Surfaces& s, const Vec& k) {
  Config cfg;
  cfg.surfaces = s;
  Event e;
  knock::classifySurface(cfg, k.v, e);
  assert(e.taught);
  return e.side;
}

Vec flipped(const Vec& k) { return {{int16_t(-k.v[0]), int16_t(-k.v[1]), int16_t(-k.v[2])}}; }

void testCos() {
  const int16_t z[3] = {0, 0, 0}, a[3] = {0, 0, 700}, b[3] = {0, 0, -1300}, c[3] = {500, 0, 0};
  int16_t u[3];
  assert(!knock::unitMilli(z, u));
  assert(knock::unitMilli(b, u) && u[0] == 0 && u[1] == 0 && u[2] == -1000);
  assert(knock::cosMilli(a, b) == -1000 && knock::absCosMilli(a, b) == 1000);
  assert(knock::cosMilli(a, c) == 0 && knock::cosMilli(a, z) == 0);
  const int16_t d[3] = {707, 707, 0};
  assert(knock::cosMilli(c, d) >= 706 && knock::cosMilli(c, d) <= 708);  // 45 degrees
}

void testTeacher() {
  Teacher t;
  assert(t.state == TeachState::Idle);
  assert(!t.add(tapWith({{0, 900, 0}})));  // not recording
  t.start(true, 5000);
  assert(t.state == TeachState::Recording && t.side && t.msLeft(5000) == knock::kTeachTimeoutMs);
  // Only taps count, and a zero jump teaches nothing.
  assert(!t.add(tapWith({{0, 900, 0}}, Verdict::Soft)));
  assert(!t.add(tapWith({{0, 900, 0}}, Verdict::Ring)));
  assert(!t.add(tapWith({{0, 0, 0}})));
  for (int k = 0; k < 4; ++k) assert(t.add(tapWith({{0, int16_t(-900 - k), 100}})));
  assert(t.state == TeachState::Recording && t.got.count == 4);
  // Side or face verdict does not matter: any tap is an example.
  Event e = tapWith({{0, 0, 2000}});
  e.side = false;
  assert(t.add(e) && t.state == TeachState::Done && t.got.count == 5);
  assert(t.got.ex[0][1] < -990 && t.got.ex[4][2] == 1000);
  assert(!t.add(tapWith({{0, 900, 0}})) && t.got.count == 5);  // full
  assert(!t.expire(5000 + knock::kTeachTimeoutMs * 2));          // done is done
  // The time-out: 30 s without five taps, across the clock's wrap.
  t.start(false, 0xFFFFF000u);
  t.add(tapWith({{0, 0, 900}}));
  assert(!t.expire(0xFFFFF000u + knock::kTeachTimeoutMs - 1));
  assert(t.msLeft(0xFFFFF000u + 1000) == knock::kTeachTimeoutMs - 1000);
  assert(t.expire(0xFFFFF000u + knock::kTeachTimeoutMs) && t.state == TeachState::TimedOut);
  assert(!t.expire(0xFFFFF000u + knock::kTeachTimeoutMs + 5) && t.msLeft(0) == 0);
  assert(!t.add(tapWith({{0, 0, 900}})));
  t.cancel();
  assert(t.state == TeachState::Idle && t.got.count == 0);
}

void testUntaughtKeepsTheRatioRule() {
  Config cfg;  // side axis Y, 60 %
  const Vec side = {{100, -900, 300}}, face = {{100, 200, 1200}};
  Event e;
  knock::classifySurface(cfg, side.v, e);
  assert(e.side && !e.taught && e.cosSide == 0 && e.cosFace == 0);
  knock::classifySurface(cfg, face.v, e);
  assert(!e.side && !e.taught);
  // One surface taught is not enough: still the ratio rule, even where the
  // taught face (along -Y) would have said otherwise.
  uint16_t agree = 0;
  cfg.surfaces = knock::withTaught(Surfaces{}, stepWith({{{0, -1000, 0}}, {{0, -900, 50}}, {{0, -1000, 0}}}, false), agree);
  assert(cfg.surfaces.haveFace && !cfg.surfaces.haveSide && !cfg.surfaces.taught());
  knock::classifySurface(cfg, side.v, e);
  assert(e.side && !e.taught);
  assert(knock::ratioSide(cfg, side.v) && !knock::ratioSide(cfg, face.v));
}

// The owner's pod, BNO055 glued down (bench 2026-11-23, x,y,z mg at the peak).
// Side: five single side knocks plus one more (the first is soft). Face: five
// single face knocks, always +Z with Y about -0.45 of Z.
const std::vector<Vec> kPodSide = {{{-54, -242, 83}}, {{40, -639, -194}}, {{57, 362, -485}},
                                   {{-49, 311, -465}}, {{53, 629, 303}}};
const Vec kPodSideMore = {{1, 529, -362}};
const std::vector<Vec> kPodFace = {{{19, -1214, 2243}}, {{-378, -1023, 1750}}, {{-240, -476, 2127}},
                                   {{432, -1062, 1893}}, {{361, -1238, 2552}}};

void testPodSurfaces() {
  uint16_t agree = 0;
  const Surfaces s = teachBoth(kPodSide, kPodFace, &agree);
  assert(agree > 950);  // the face knocks agree in sign
  for (const Vec& k : kPodSide) assert(sortsSide(s, k));
  assert(sortsSide(s, kPodSideMore));
  for (const Vec& k : kPodFace) assert(!sortsSide(s, k));
  // A face knock read the other way round is no face knock. (A side knock
  // read the other way round can land on the face's direction itself:
  // (-57,-362,+485) would be the face, as on any rule that keeps the face's
  // sign.)
  for (const Vec& k : kPodFace) assert(sortsSide(s, flipped(k)));
  // Why the face keeps its sign: this side knock points against the face
  // (signed cos about -0.98) and would read as the face with |cos|.
  const Vec against = kPodSide[2];
  assert(knock::cosMilli(against.v, s.face) < -950);
  assert(knock::absCosMilli(against.v, s.face) > knock::absCosMilli(against.v, s.side));
  Config cfg;
  cfg.surfaces = s;
  Event e;
  knock::classifySurface(cfg, against.v, e);
  assert(e.side && e.cosFace < -950 && e.cosSide > 600);
  knock::classifySurface(cfg, kPodFace[0].v, e);
  assert(!e.side && e.cosFace > 950 && e.cosSide < 800);
  // Each knock left out of its own surface's teaching still sorts right.
  for (size_t out = 0; out < kPodFace.size(); ++out) {
    std::vector<Vec> rest;
    for (size_t i = 0; i < kPodFace.size(); ++i)
      if (i != out) rest.push_back(kPodFace[i]);
    assert(!sortsSide(teachBoth(kPodSide, rest), kPodFace[out]));
  }
  for (size_t out = 0; out < kPodSide.size(); ++out) {
    std::vector<Vec> rest;
    for (size_t i = 0; i < kPodSide.size(); ++i)
      if (i != out) rest.push_back(kPodSide[i]);
    assert(sortsSide(teachBoth(rest, kPodFace), kPodSide[out]));
  }
  // The teach step's report: none of the taught knocks sorts the other way,
  // the surfaces are well apart.
  assert(knock::teachMisfits(stepWith(kPodSide, true), s) == 0);
  assert(knock::teachMisfits(stepWith(kPodFace, false), s) == 0);
  const knock::TeachReport r = knock::teachReport(s);
  assert(!r.close && r.sepDeg >= 40);
}

// The meeting sign (4517 at 416 Hz): side knocks along Y (either sign), face
// knocks along -Z.
const std::vector<Vec> kSignSide = {{{40, -1300, 200}}, {{-60, 1500, -150}}, {{90, -1100, 300}}, {{0, 1250, 100}},
                                    {{-120, -1400, -250}}};
const std::vector<Vec> kSignFace = {{{80, 150, -1600}}, {{-100, -60, -1400}}, {{30, 200, -1800}}, {{60, -90, -1500}},
                                    {{-40, 120, -1700}}};

void testSignSurfaces() {
  const Surfaces s = teachBoth(kSignSide, kSignFace);
  for (const Vec& k : kSignSide) assert(sortsSide(s, k));
  for (const Vec& k : kSignFace) assert(!sortsSide(s, k));
  for (const Vec& k : std::vector<Vec>{{{100, -1200, 250}}, {{-30, 1700, 0}}, {{250, 900, -300}}})
    assert(sortsSide(s, k));
  for (const Vec& k : std::vector<Vec>{{{100, 250, -1200}}, {{-30, 0, -1700}}, {{250, -300, -900}}})
    assert(!sortsSide(s, k));
  const knock::TeachReport r = knock::teachReport(s);
  assert(!r.close && r.sepDeg > 75);
  // Through the detector at 100 Hz: the knock's peak jump is sorted by the
  // taught surfaces, with the margin on the event. Gravity is +Z here, so the
  // knock's first swing goes -Z.
  Config c100 = cfg100();
  c100.surfaces = s;
  auto ev = run100(c100, 600, [](float t, float* mg) {
    float k = 0;
    knock100At(t, 200, 800, 40, &k);
    mg[2] -= k;
    mg[1] += k / 5;
  });
  assert(ev.size() == 1 && ev[0].verdict == Verdict::Tap && !ev[0].side && ev[0].taught);
  assert(ev[0].cosFace > 900 && ev[0].cosSide < 500);
  ev = run100(c100, 600, [](float t, float* mg) {
    float k = 0;
    knock100At(t, 200, 800, 40, &k);
    mg[1] -= k;
    mg[2] += k / 4;
  });
  assert(ev.size() == 1 && ev[0].side && ev[0].taught && ev[0].cosFace < static_cast<int16_t>(ev[0].cosSide));
}

void testTeachWarnings() {
  // Face knocks that disagree in sign: a short mean, warned about.
  uint16_t agree = 0;
  Surfaces s = teachBoth(kSignSide, {{{0, 0, 1500}}, {{0, 0, -1500}}, {{50, 0, 1400}}, {{0, 30, -1600}}, {{0, 0, 1500}}},
                         &agree);
  assert(agree < knock::kTeachAgreeMin);
  // Side and face along nearly the same line: too close to tell apart.
  s = teachBoth({{{0, 1000, 200}}, {{0, 1000, 150}}, {{0, -1000, -250}}, {{50, 1000, 200}}, {{0, 1000, 200}}},
                {{{0, 1000, 500}}, {{0, 1000, 450}}, {{0, 1000, 550}}, {{-50, 1000, 500}}, {{0, 1000, 500}}});
  knock::TeachReport r = knock::teachReport(s);
  assert(r.close && r.sepDeg < knock::kTeachCloseDeg && r.sepDeg > 5);
  // A teach step whose knocks the result sorts the other way is counted.
  const Teacher mixed = stepWith({{{0, 1000, 500}}, {{0, 1000, 200}}, {{0, 1000, 450}}, {{0, 1000, 550}}, {{0, 1000, 500}}},
                                 false);
  assert(knock::teachMisfits(mixed, s) >= 1);
  // Far apart: no warning. One surface only: no report.
  s = teachBoth(kSignSide, kSignFace);
  assert(!knock::teachReport(s).close);
  s.haveFace = false;
  r = knock::teachReport(s);
  assert(!r.close && r.sepDeg == 0 && knock::teachMisfits(mixed, s) == 0);
}

// imutapteach and the shake configuration judge what was taught alike
// (judgeTaught): the surfaces to save and the first warning, in order.
void testJudgeTaught() {
  using knock::TeachVerdict;
  // Both steps at once (the configuration), well apart: Ok, nothing misfits.
  const Teacher side = stepWith(kSignSide, true), face = stepWith(kSignFace, false);
  knock::TeachJudgement j = knock::judgeTaught(Surfaces{}, &side, &face);
  assert(j.verdict == TeachVerdict::Ok && j.surfaces.taught() && j.knocks == 10 && j.misfits == 0);
  assert(j.sepDeg == knock::teachReport(teachBoth(kSignSide, kSignFace)).sepDeg && j.sepDeg > 75);
  // The same surfaces as two imutapteach steps in a row.
  knock::TeachJudgement one = knock::judgeTaught(Surfaces{}, &side, nullptr);
  assert(one.verdict == TeachVerdict::NeedOther && one.surfaces.haveSide && !one.surfaces.haveFace && one.knocks == 5);
  knock::TeachJudgement two = knock::judgeTaught(one.surfaces, nullptr, &face);
  assert(two.verdict == TeachVerdict::Ok && two.sepDeg == j.sepDeg);
  for (int i = 0; i < 3; ++i) assert(two.surfaces.side[i] == j.surfaces.side[i] && two.surfaces.face[i] == j.surfaces.face[i]);
  // Both steps replace what was taught before.
  Surfaces old = teachBoth(kPodSide, kPodFace);
  j = knock::judgeTaught(old, &side, &face);
  for (int i = 0; i < 3; ++i) assert(j.surfaces.side[i] == two.surfaces.side[i] && j.surfaces.face[i] == two.surfaces.face[i]);
  // Face knocks of opposite signs come first.
  const Teacher mixedFace =
      stepWith({{{0, 0, 1500}}, {{0, 0, -1500}}, {{50, 0, 1400}}, {{0, 30, -1600}}, {{0, 0, 1500}}}, false);
  j = knock::judgeTaught(Surfaces{}, &side, &mixedFace);
  assert(j.verdict == TeachVerdict::FaceDisagrees && j.faceAgree < knock::kTeachAgreeMin);
  // Too close to tell apart.
  const Teacher nearSide =
      stepWith({{{0, 1000, 200}}, {{0, 1000, 150}}, {{0, -1000, -250}}, {{50, 1000, 200}}, {{0, 1000, 200}}}, true);
  const Teacher nearFace =
      stepWith({{{0, 1000, 500}}, {{0, 1000, 450}}, {{0, 1000, 550}}, {{-50, 1000, 500}}, {{0, 1000, 500}}}, false);
  j = knock::judgeTaught(Surfaces{}, &nearSide, &nearFace);
  assert(j.verdict == TeachVerdict::Close && j.sepDeg < knock::kTeachCloseDeg);
  // Apart, but a knock of a step sorts as the other surface.
  const Teacher oddFace =
      stepWith({{{80, 150, -1600}}, {{-100, -60, -1400}}, {{30, 200, -1800}}, {{60, -1500, -300}}, {{-40, 120, -1700}}},
               false);
  j = knock::judgeTaught(Surfaces{}, &side, &oddFace);
  assert(j.verdict == TeachVerdict::Misfits && j.misfits >= 1 && j.sepDeg >= knock::kTeachCloseDeg);
}

void testDirectionText() {
  const Surfaces s = teachBoth(kPodSide, kPodFace);
  char text[knock::kTaughtTextMax];
  knock::formatDirection(true, s.face, text, sizeof(text));
  bool have = false;
  int16_t back[3];
  assert(knock::parseDirection(text, have, back) && have);
  for (int i = 0; i < 3; ++i) assert(abs(back[i] - s.face[i]) <= 1);  // re-normalised
  // The longest text fits.
  const int16_t big[3] = {-1000, -1000, -1000};
  knock::formatDirection(true, big, text, sizeof(text));
  assert(strcmp(text, "-1000,-1000,-1000") == 0);
  // Untaught is "", spaces are fine, junk forgets the direction.
  knock::formatDirection(false, big, text, sizeof(text));
  assert(text[0] == 0);
  assert(knock::parseDirection("", have, back) && !have);
  assert(knock::parseDirection("  ", have, back) && !have);
  assert(knock::parseDirection(nullptr, have, back) && !have);
  assert(knock::parseDirection(" 0, -1000 ,0", have, back) && have && back[1] == -1000);
  assert(knock::parseDirection("3,0,4", have, back) && have && back[0] == 600 && back[2] == 800);
  for (const char* bad : {"1,2", "1,2,3;", "a,b,c", "0,0,0", "1001,0,0", "1,2,3,4", "1.5,2,3", "1,,3", "1,2,3;4,5,6"}) {
    have = true;
    assert(!knock::parseDirection(bad, have, back) && !have);
  }
}

int main() {
  testStuckBaselineRecovers();
  testOpenKnockEndsOnZeros();
  testQuietTimePerRate();
  testSingleTap100();
  testFourTapRun100();
  testRinging100();
  testSideFace100();
  testPendingTaps();
  testSampleClock();
  testFeedRestartKeepsTaps();
  testSingleTap();
  testFastQuadAllCount();
  testSoftKnockRecorded();
  testRingingAfterTap();
  testHandlingIsLongAndTiltIgnored();
  testTrace();
  testCos();
  testTeacher();
  testJudgeTaught();
  testUntaughtKeepsTheRatioRule();
  testPodSurfaces();
  testSignSurfaces();
  testTeachWarnings();
  testDirectionText();
  puts("motion_knock_core: ok");
  return 0;
}
