// Host test for HAL_Motion_KnockCore.h: knocks found from 416 Hz samples.
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

}  // namespace

int main() {
  testSingleTap();
  testFastQuadAllCount();
  testSoftKnockRecorded();
  testRingingAfterTap();
  testHandlingIsLongAndTiltIgnored();
  testTrace();
  puts("motion_knock_core: ok");
  return 0;
}
