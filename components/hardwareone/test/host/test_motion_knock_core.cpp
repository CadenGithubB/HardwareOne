// Host test for HAL_Motion_KnockCore.h: knocks found from 416 Hz samples (the
// 4517's FIFO) and from 100 Hz samples (the BNO055 with IMU input), and the
// shared feed's pending taps and clock (HAL_Motion_KnockFeed.cpp uses them).
//   clang++ -std=c++17 -I. -Itest/host -o /tmp/t test/host/test_motion_knock_core.cpp && /tmp/t
#include <assert.h>
#include <math.h>
#include <stdio.h>

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
  // A soft one (300 mg) is recorded, not counted.
  ev = run100(cfg, 600, [](float t, float* mg) { knock100At(t, 200, 300, 40, &mg[2]); });
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

int main() {
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
  puts("motion_knock_core: ok");
  return 0;
}
