// System metrics sampler core (System_MetricsCore.h): the metric table, the
// 185-point history ring and its wrap, the fixed 20 s history clock under
// uneven ticks, change and trend, the consumer table that sets the refresh
// interval, the run bookkeeping, and the due/CPU-warm schedule.
#include "System_MetricsCore.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); std::abort(); \
} } while (false)

namespace smc = system_metrics_core;

static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

static void metricTable() {
  static_assert(smc::kMetricCount == 12, "twelve metrics");
  static_assert(smc::kHistoryLen == 185, "185-point history (the glasses' chart)");
  static_assert(smc::kHistoryStepMs == 20000, "a point every 20 s");
  static_assert(smc::kHistoryLen * smc::kHistoryStepMs / 60000 >= 60, "about an hour");
  for (size_t i = 0; i < smc::kMetricCount; ++i) {
    const smc::MetricInfo& m = smc::kMetrics[i];
    CHECK(smc::index(m.id) == i);                 // table order is the id order
    CHECK(std::strlen(m.label) >= 2 && std::strlen(m.label) <= 4);
    CHECK(std::strlen(m.longName) > 0 && std::strlen(m.longName) <= 127);
    CHECK(std::strlen(m.unit) > 0);
    CHECK(m.rangeLow == 0.0f);
    CHECK(m.trendDeadband >= 0.0f);
    for (size_t j = 0; j < i; ++j) CHECK(std::strcmp(m.label, smc::kMetrics[j].label) != 0);
    SystemMetricId back{};
    CHECK(smc::idFromLabel(m.label, back) && back == m.id);
  }
  // Ids are stored in settings: pinned values.
  CHECK(smc::index(SystemMetricId::Cpu) == 0);
  CHECK(smc::index(SystemMetricId::Heap) == 3);
  CHECK(smc::index(SystemMetricId::Psram) == 5);
  CHECK(smc::index(SystemMetricId::Ring) == 11);
  // The owner's first tickers.
  CHECK(std::strcmp(smc::info(SystemMetricId::Cpu)->label, "CPU") == 0);
  CHECK(std::strcmp(smc::info(SystemMetricId::Heap)->label, "HEAP") == 0);
  CHECK(std::strcmp(smc::info(SystemMetricId::Psram)->label, "PSRM") == 0);
  SystemMetricId id{};
  CHECK(smc::idFromLabel("heap", id) && id == SystemMetricId::Heap);
  CHECK(smc::idFromLabel("Psrm", id) && id == SystemMetricId::Psram);
  CHECK(!smc::idFromLabel("PSRAM", id));
  CHECK(!smc::idFromLabel("CP", id));
  CHECK(!smc::idFromLabel("", id));
  CHECK(!smc::idFromLabel(nullptr, id));
  CHECK(smc::info(static_cast<SystemMetricId>(12)) == nullptr);
}

static void historyRing() {
  smc::History h{};
  float out[smc::kHistoryLen + 5];
  float latest = -1.0f;
  CHECK(!smc::historyLatest(h, latest));
  CHECK(smc::historyCopy(h, out, sizeof(out) / sizeof(out[0])) == 0);

  for (int i = 0; i < 10; ++i) smc::historyPush(h, (float)i);
  CHECK(h.count == 10);
  CHECK(smc::historyLatest(h, latest) && latest == 9.0f);
  CHECK(smc::historyCopy(h, out, 64) == 10);
  for (int i = 0; i < 10; ++i) CHECK(out[i] == (float)i);  // oldest first
  CHECK(smc::historyCopy(h, out, 3) == 3);                 // newest three
  CHECK(out[0] == 7.0f && out[1] == 8.0f && out[2] == 9.0f);
  CHECK(smc::historyCopy(h, nullptr, 3) == 0);
  CHECK(smc::historyCopy(h, out, 0) == 0);

  // Wrap: 500 pushes keep the newest 185, in order, across the seam (twice).
  for (int i = 10; i < 500; ++i) smc::historyPush(h, (float)i);
  CHECK(h.count == smc::kHistoryLen);
  CHECK(smc::historyCopy(h, out, smc::kHistoryLen + 5) == smc::kHistoryLen);
  for (int i = 0; i < smc::kHistoryLen; ++i) CHECK(out[i] == (float)(500 - smc::kHistoryLen + i));
  CHECK(smc::historyLatest(h, latest) && latest == 499.0f);
  CHECK(smc::historyCopy(h, out, 60) == 60 && out[0] == 440.0f && out[59] == 499.0f);
  CHECK(smc::historyCopy(h, out, 1) == 1 && out[0] == 499.0f);

  smc::historyClear(h);
  CHECK(h.count == 0 && !smc::historyLatest(h, latest));
}

static void changeAndTrend() {
  smc::Change c = smc::changeSince(false, 50.0f, 70.0f, 0.5f);
  CHECK(c.delta == 0.0f && c.trend == 0);            // no previous: flat
  c = smc::changeSince(true, 50.0f, 70.0f, 0.5f);
  CHECK(near(c.delta, 20.0f) && c.trend == 1);
  c = smc::changeSince(true, 70.0f, 50.0f, 0.5f);
  CHECK(near(c.delta, -20.0f) && c.trend == -1);
  c = smc::changeSince(true, 50.0f, 50.4f, 0.5f);    // inside the dead band
  CHECK(near(c.delta, 0.4f) && c.trend == 0);
  c = smc::changeSince(true, 50.0f, 50.0f, 0.5f);
  CHECK(c.delta == 0.0f && c.trend == 0);
  c = smc::changeSince(true, 2.0f, 3.0f, 0.0f);      // counts: any step is a trend
  CHECK(c.trend == 1);
  c = smc::changeSince(true, 3.0f, 3.0f, 0.0f);
  CHECK(c.trend == 0);
}

static void valueHelpers() {
  float pct = -1.0f;
  CHECK(!smc::usedPercent(0, 0, pct));               // no pool: unavailable
  CHECK(pct == -1.0f);
  CHECK(smc::usedPercent(1000, 250, pct) && near(pct, 75.0f));
  CHECK(smc::usedPercent(1000, 1000, pct) && pct == 0.0f);
  CHECK(smc::usedPercent(1000, 0, pct) && pct == 100.0f);
  CHECK(smc::usedPercent(1000, 5000, pct) && pct == 0.0f);   // free > total clamps
  const uint64_t sd = 64ULL * 1024 * 1024 * 1024;            // 64 GB card
  CHECK(smc::usedPercent(sd, sd / 4, pct) && near(pct, 75.0f));

  CHECK(smc::loadPercentFromIdlePermille(1000) == 0.0f);
  CHECK(smc::loadPercentFromIdlePermille(0) == 100.0f);
  CHECK(near(smc::loadPercentFromIdlePermille(873), 12.7f));
  CHECK(smc::loadPercentFromIdlePermille(1200) == 0.0f);      // clamped

  CHECK(smc::idleCoreIndex("IDLE0") == 0);
  CHECK(smc::idleCoreIndex("IDLE1") == 1);
  CHECK(smc::idleCoreIndex("IDLE") == 0);
  CHECK(smc::idleCoreIndex("IDLEX") == -1);
  CHECK(smc::idleCoreIndex("IDLE10") == -1);
  CHECK(smc::idleCoreIndex("loopTask") == -1);
  CHECK(smc::idleCoreIndex(nullptr) == -1);

  CHECK(smc::temperatureBelievable(42.0f));
  CHECK(smc::temperatureBelievable(-40.0f) && smc::temperatureBelievable(125.0f));
  CHECK(!smc::temperatureBelievable(NAN));
  CHECK(!smc::temperatureBelievable(INFINITY));
  CHECK(!smc::temperatureBelievable(-41.0f));
  CHECK(!smc::temperatureBelievable(200.0f));
}

static void consumers() {
  smc::Consumers c{};
  CHECK(smc::effectiveInterval(c) == 0 && smc::consumerCount(c) == 0);
  const uint8_t slow = smc::consumerOpen(c, smc::kSlowIntervalMs);
  CHECK(slow != 0 && smc::effectiveInterval(c) == smc::kSlowIntervalMs);
  const uint8_t fast = smc::consumerOpen(c, smc::kFastIntervalMs);
  CHECK(fast != 0 && fast != slow);
  CHECK(smc::effectiveInterval(c) == smc::kFastIntervalMs);  // fastest wins
  CHECK(smc::consumerSetInterval(c, fast, smc::kSlowIntervalMs));
  CHECK(smc::effectiveInterval(c) == smc::kSlowIntervalMs);
  CHECK(smc::consumerSetInterval(c, fast, 10));               // clamped up
  CHECK(smc::effectiveInterval(c) == smc::kMinIntervalMs);
  CHECK(smc::consumerSetInterval(c, fast, 0xFFFFFFFFu));      // clamped down
  CHECK(smc::effectiveInterval(c) == smc::kSlowIntervalMs);
  CHECK(smc::consumerClose(c, fast));
  CHECK(!smc::consumerClose(c, fast));                        // already closed
  CHECK(!smc::consumerSetInterval(c, fast, 5000));
  CHECK(!smc::consumerClose(c, 0) && !smc::consumerClose(c, smc::kMaxConsumers + 1));
  CHECK(smc::consumerCount(c) == 1);
  CHECK(smc::consumerClose(c, slow));
  CHECK(smc::effectiveInterval(c) == 0);                      // nobody: sampler idles

  uint8_t handles[smc::kMaxConsumers];
  for (uint8_t i = 0; i < smc::kMaxConsumers; ++i) {
    handles[i] = smc::consumerOpen(c, 30000);
    CHECK(handles[i] == i + 1);
  }
  CHECK(smc::consumerOpen(c, 30000) == 0);                    // full
  CHECK(smc::consumerClose(c, handles[2]));
  CHECK(smc::consumerOpen(c, 7000) == handles[2]);            // slot reused
  CHECK(smc::effectiveInterval(c) == 7000);
}

static void schedule() {
  smc::Schedule s{0, false};
  const uint32_t iv = smc::kSlowIntervalMs;
  CHECK(smc::sampleDue(s, 12345, iv));                        // never sampled
  s = smc::Schedule{10000, true};
  CHECK(!smc::sampleDue(s, 10000 + iv - 1, iv));
  CHECK(smc::sampleDue(s, 10000 + iv, iv));
  CHECK(smc::msUntilDue(s, 10000, iv) == iv);

  // The sampler: the next sample is the sooner of the refresh interval and
  // the next history point.
  smc::Sampler sm{};
  CHECK(smc::cpuWarmWanted(sm, 12345, iv));                   // never sampled
  smc::Plan p = smc::plan(sm, 12345, iv);
  CHECK(p.sample && p.point);                                 // a run starts on a point
  smc::sampleTaken(sm, p, 10000);
  CHECK(smc::msUntilSample(sm, 10000, iv) == smc::kHistoryStepMs);  // slow: the point first
  CHECK(!smc::cpuWarmWanted(sm, 10000 + 1000, iv));           // CPU sampler idles between
  CHECK(!smc::cpuWarmWanted(sm, 10000 + smc::kHistoryStepMs - smc::kCpuWarmMs - 1, iv));
  CHECK(smc::cpuWarmWanted(sm, 10000 + smc::kHistoryStepMs - smc::kCpuWarmMs, iv));
  // Fast: warm from 7 s after a sample, and the gap since the previous
  // sample's read is under kCpuPokeGapMs, so the CPU sampler never lapses.
  CHECK(smc::msUntilSample(sm, 10000, smc::kFastIntervalMs) == smc::kFastIntervalMs);
  CHECK(!smc::cpuWarmWanted(sm, 10000 + 6999, smc::kFastIntervalMs));
  CHECK(smc::cpuWarmWanted(sm, 10000 + 7000, smc::kFastIntervalMs));
  static_assert(smc::kFastIntervalMs - smc::kCpuWarmMs < smc::kCpuPokeGapMs,
                "fast interval keeps one warm run");
  // A refresh-only sample does not move the history grid.
  CHECK(p.refresh);                                           // the first sample is both
  p = smc::plan(sm, 20000, smc::kFastIntervalMs);
  CHECK(p.sample && !p.point && p.refresh);
  smc::sampleTaken(sm, p, 20000);
  CHECK(sm.clock.nextMs == 30000);
  p = smc::plan(sm, 29999, smc::kFastIntervalMs);
  CHECK(!p.sample);
  p = smc::plan(sm, 30000, smc::kFastIntervalMs);
  CHECK(p.sample && p.point && p.refresh);
  smc::sampleTaken(sm, p, 30000);
  CHECK(sm.clock.nextMs == 50000);
  // ... and a history-only sample does not move the refresh schedule: at the
  // 60 s tile the refresh samples stay 60 s apart, between the 20 s points.
  p = smc::plan(sm, 50000, smc::kSlowIntervalMs);
  CHECK(p.sample && p.point && !p.refresh);
  smc::sampleTaken(sm, p, 50000);
  CHECK(sm.refresh.lastSampleMs == 30000 && sm.clock.nextMs == 70000);
  p = smc::plan(sm, 70000, smc::kSlowIntervalMs);
  CHECK(p.point && !p.refresh);
  smc::sampleTaken(sm, p, 70000);
  p = smc::plan(sm, 89999, smc::kSlowIntervalMs);
  CHECK(!p.sample);
  p = smc::plan(sm, 90000, smc::kSlowIntervalMs);
  CHECK(p.point && p.refresh);
  smc::sampleTaken(sm, p, 90000);
  CHECK(sm.refresh.lastSampleMs == 90000 && sm.clock.nextMs == 110000);
  // millis() wrap.
  s = smc::Schedule{0xFFFFF000u, true};
  CHECK(!smc::sampleDue(s, 0x00000F00u, 0x2000));
  CHECK(smc::sampleDue(s, 0x00001000u, 0x2000));

  // CPU warm runs: settled 1.2 s after the first poke of a run; a gap longer
  // than kCpuPokeGapMs starts a new run.
  smc::CpuWarm w{};
  CHECK(!smc::cpuSettled(w, 0));
  smc::cpuPoke(w, 1000);
  CHECK(!smc::cpuSettled(w, 1000 + smc::kCpuSettleMs - 1));
  CHECK(smc::cpuSettled(w, 1000 + smc::kCpuSettleMs));
  smc::cpuPoke(w, 3000);
  smc::cpuPoke(w, 9000);
  CHECK(w.sinceMs == 1000 && smc::cpuSettled(w, 9000));
  CHECK(!smc::cpuSettled(w, 9000 + smc::kCpuPokeGapMs + 1)); // interest lapsed
  smc::cpuPoke(w, 30000);                                    // new run
  CHECK(w.sinceMs == 30000 && !smc::cpuSettled(w, 30500));
  CHECK(smc::cpuSettled(w, 30000 + smc::kCpuSettleMs));
  // Wrap inside a run.
  w = smc::CpuWarm{};
  smc::cpuPoke(w, 0xFFFFFF00u);
  smc::cpuPoke(w, 0x00000300u);
  CHECK(w.sinceMs == 0xFFFFFF00u && smc::cpuSettled(w, 0x00000500u));
}

// ── History clock ───────────────────────────────────────────────────────────

static void historyClock() {
  smc::HistoryClock c{};
  CHECK(smc::pointDue(c, 777) && smc::msUntilPoint(c, 777) == 0);
  smc::pointTaken(c, 1000);                                   // the grid starts here
  CHECK(c.started && c.nextMs == 21000);
  CHECK(!smc::pointDue(c, 20999) && smc::msUntilPoint(c, 20999) == 1);
  CHECK(smc::pointDue(c, 21000));
  smc::pointTaken(c, 21900);                                  // late by 0.9 s
  CHECK(c.nextMs == 41000);                                   // still on the grid
  smc::pointTaken(c, 41000 + 45000);                          // a 45 s stall: 2 steps missed
  CHECK(c.nextMs == 101000);                                  // skipped, not filled, on the grid
  // millis() wrap.
  c = smc::HistoryClock{};
  smc::pointTaken(c, 0xFFFFC000u);
  CHECK(c.nextMs == 0xFFFFC000u + smc::kHistoryStepMs);       // wrapped
  CHECK(!smc::pointDue(c, 0xFFFFFFFFu));
  CHECK(!smc::pointDue(c, c.nextMs - 1) && smc::pointDue(c, c.nextMs));
  CHECK(smc::msUntilPoint(c, 0xFFFFFFFFu) == c.nextMs + 1);
}

static void seriesAndRun() {
  smc::Series s{};
  smc::seriesClear(s);
  // The first sample sets the baseline (no change yet).
  smc::Change c = smc::seriesSample(s, true, 10.0f, true, true, 0.5f);
  CHECK(c.trend == 0 && s.history.count == 1 && s.haveLast && s.last == 10.0f);
  // A history-only sample: pushed, change against the baseline, baseline kept.
  c = smc::seriesSample(s, true, 12.0f, true, false, 0.5f);
  CHECK(near(c.delta, 2.0f) && c.trend == 1 && s.history.count == 2 && s.last == 10.0f);
  c = smc::seriesSample(s, true, 12.0f, true, false, 0.5f);
  CHECK(near(c.delta, 2.0f) && c.trend == 1 && s.history.count == 3);  // still +2, not flat
  // A refresh-only sample: change against the baseline, then it is the
  // baseline; not a history point.
  c = smc::seriesSample(s, true, 12.0f, false, true, 0.5f);
  CHECK(near(c.delta, 2.0f) && c.trend == 1 && s.history.count == 3 && s.last == 12.0f);
  // Unavailable: nothing pushed, baseline kept, flat.
  c = smc::seriesSample(s, false, 0.0f, true, true, 0.5f);
  CHECK(c.delta == 0.0f && c.trend == 0 && s.history.count == 3 && s.last == 12.0f);
  c = smc::seriesSample(s, true, 11.0f, true, true, 0.5f);
  CHECK(near(c.delta, -1.0f) && c.trend == -1 && s.history.count == 4);
  float out[4];
  CHECK(smc::historyCopy(s.history, out, 4) == 4 && out[0] == 10.0f && out[3] == 11.0f);
  smc::seriesClear(s);
  CHECK(s.history.count == 0 && !s.haveLast);
  // A metric first available on a history-only sample: that value is the
  // baseline until the next refresh.
  c = smc::seriesSample(s, true, 5.0f, true, false, 0.5f);
  CHECK(c.trend == 0 && s.haveLast && s.last == 5.0f);
  smc::seriesClear(s);

  // A run lasts while a consumer is open; only a new run starts afresh.
  bool running = false;
  CHECK(smc::runStep(running, false) == smc::RunStep::Idle && !running);
  CHECK(smc::runStep(running, true) == smc::RunStep::Start && running);
  CHECK(smc::runStep(running, true) == smc::RunStep::Continue);
  CHECK(smc::runStep(running, true) == smc::RunStep::Continue);
  CHECK(smc::runStep(running, false) == smc::RunStep::Idle && !running);
  CHECK(smc::runStep(running, true) == smc::RunStep::Start);
}

// The change shown is the change over the last refresh interval, whatever
// the 20 s history samples in between do: the tile at 60 s, the value jumps
// 40 -> 45 at 5 s. Every reading up to the 60 s refresh shows +5 (never a
// flat 0 from an intermediate history sample); after it, 0.
static void changeOverRefreshInterval() {
  smc::Sampler sm{};
  smc::Series series{};
  smc::seriesClear(series);
  smc::Change shown{0.0f, 0};
  int refreshes = 0, points = 0;
  for (uint32_t t = 1; t <= 130001; t += 10) {
    const smc::Plan p = smc::plan(sm, t, smc::kSlowIntervalMs);
    if (p.sample) {
      smc::sampleTaken(sm, p, t);
      shown = smc::seriesSample(series, true, t < 5000 ? 40.0f : 45.0f, p.point, p.refresh, 0.1f);
      refreshes += p.refresh;
      points += p.point;
    }
    if (t >= 5000 && t < 20001) CHECK(shown.trend == 0);         // not sampled yet
    if (t >= 20001 && t < 80001) CHECK(near(shown.delta, 5.0f) && shown.trend == 1);
    if (t >= 80001) CHECK(near(shown.delta, 0.0f) && shown.trend == 0);
  }
  CHECK(refreshes == 3 && points == 7);                          // 1, 60001, 120001; every 20 s
  CHECK(series.history.count == 7);
}

// The tick as System_Metrics.cpp runs it, for one metric: the run, the CPU
// warm-up (poke at most every second while wanted), the plan, the settle
// wait, the sample (a point only on the history grid). Laps are uneven: the
// next lap comes after lapMs(i), and the refresh interval may change.
struct Model {
  smc::Consumers consumers{};
  smc::Sampler sampler{};
  smc::CpuWarm warm{};
  smc::Series series{};
  bool running = false;
  uint32_t lastPoke = 0;
  int samples = 0;
  int points = 0;
  int pokeRuns = 0;
  uint32_t pointAt[4096];
  float nextValue = 0.0f;

  void tick(uint32_t now) {
    const uint32_t refresh = smc::effectiveInterval(consumers);
    const smc::RunStep step = smc::runStep(running, refresh != 0);
    if (step == smc::RunStep::Idle) return;
    if (step == smc::RunStep::Start) {
      smc::seriesClear(series);
      sampler = smc::Sampler{};
    }
    if (smc::cpuWarmWanted(sampler, now, refresh) && (!warm.poked || now - lastPoke >= 1000)) {
      const uint32_t before = warm.sinceMs;
      const bool had = warm.poked;
      smc::cpuPoke(warm, now);
      if (!had || warm.sinceMs != before) ++pokeRuns;
      lastPoke = now;
    }
    const smc::Plan p = smc::plan(sampler, now, refresh);
    if (!p.sample || !smc::cpuSettled(warm, now)) return;
    smc::sampleTaken(sampler, p, now);
    (void)smc::seriesSample(series, true, nextValue, p.point, p.refresh, 0.0f);
    ++samples;
    if (p.point && points < 4096) pointAt[points++] = now;
    nextValue += 1.0f;
    smc::cpuPoke(warm, now);
    lastPoke = now;
  }
};

// Deterministic uneven laps: 1 ms .. 2.5 s.
static uint32_t lap(uint32_t& seed) {
  seed = seed * 1664525u + 1013904223u;
  return 1 + (seed >> 8) % 2500;
}

static void fixedClockUnevenTicks() {
  // Two hours, starting just before millis() wraps, laps 1 ms .. 2.5 s, the
  // refresh interval switching between the tile (60 s) and the page (10 s)
  // every 7 minutes: one point per 20 s step on the first point's grid, each
  // at most one lap plus the CPU settle late, none skipped or doubled.
  for (uint32_t start : {1000u, 0xFFFF0000u}) {
    auto* m = new Model();
    const uint8_t h = smc::consumerOpen(m->consumers, smc::kSlowIntervalMs);
    CHECK(h != 0);
    uint32_t seed = 12345;
    const uint32_t duration = 2u * 3600u * 1000u;
    uint32_t t = 0;
    while (t < duration) {
      const bool fast = (t / 420000u) % 2 == 1;
      smc::consumerSetInterval(m->consumers, h, fast ? smc::kFastIntervalMs : smc::kSlowIntervalMs);
      m->tick(start + t);
      t += lap(seed);
    }
    CHECK(m->points >= 359 && m->points <= 361);              // 7200 s / 20 s
    CHECK(m->samples > m->points);                            // the page refreshes in between
    const uint32_t t0 = m->pointAt[0];
    for (int k = 0; k < m->points; ++k) {
      const uint32_t since = m->pointAt[k] - t0;              // wraps cleanly
      CHECK(since / smc::kHistoryStepMs == (uint32_t)k);      // its own grid step
      CHECK(since % smc::kHistoryStepMs <= 2500 + smc::kCpuSettleMs);
    }
    // The ring holds the newest 185 points, oldest first.
    CHECK(m->series.history.count == smc::kHistoryLen);
    float out[smc::kHistoryLen];
    CHECK(smc::historyCopy(m->series.history, out, smc::kHistoryLen) == smc::kHistoryLen);
    for (int i = 1; i < smc::kHistoryLen; ++i) CHECK(out[i] > out[i - 1]);
    float latest = 0.0f;
    CHECK(smc::historyLatest(m->series.history, latest) && latest == out[smc::kHistoryLen - 1]);
    delete m;
  }

  // Every tick on a 10 ms loop at the fast refresh: still one point per 20 s.
  auto* m = new Model();
  smc::consumerOpen(m->consumers, smc::kFastIntervalMs);
  for (uint32_t t = 1; t <= 600000; t += 10) m->tick(t);
  CHECK(m->points == 30);
  CHECK(m->samples >= 59 && m->samples <= 61);                // every 10 s
  CHECK(m->pokeRuns == 1);                                    // one warm run
  delete m;

  // Slow refresh, 10 min: a new warm run per point (the CPU sampler idles
  // between), the points on time.
  m = new Model();
  smc::consumerOpen(m->consumers, smc::kSlowIntervalMs);
  for (uint32_t t = 1; t <= 600000; t += 10) m->tick(t);
  CHECK(m->points == 30 && m->samples == 30);
  CHECK(m->pokeRuns >= 30);
  for (int k = 1; k < m->points; ++k) CHECK(m->pointAt[k] - m->pointAt[k - 1] == smc::kHistoryStepMs);
  delete m;

  // A main-loop stall of 45 s: the missed steps are skipped and the grid kept.
  m = new Model();
  smc::consumerOpen(m->consumers, smc::kSlowIntervalMs);
  uint32_t t = 1;
  for (; t <= 100000; t += 10) m->tick(t);
  const int before = m->points;
  t += 45000;
  for (; t <= 300000; t += 10) m->tick(t);
  CHECK(m->points == before + (300000 - 100000) / 20000 - 2);
  for (int k = 0; k < m->points; ++k)
    CHECK((m->pointAt[k] - m->pointAt[0]) % smc::kHistoryStepMs <= smc::kCpuSettleMs + 10 ||
          k == before);                                       // the one after the stall is late
  delete m;
}

static void historyKeptWhileAConsumerStays() {
  // The run (and the history) survives as long as one consumer is open,
  // whatever the interval does; closing the last one ends it, and the next
  // open starts afresh.
  auto* m = new Model();
  uint8_t h = smc::consumerOpen(m->consumers, smc::kSlowIntervalMs);
  uint32_t t = 1;
  for (; t <= 200000; t += 10) m->tick(t);
  CHECK(m->series.history.count == 10);
  smc::consumerSetInterval(m->consumers, h, smc::kFastIntervalMs);
  for (; t <= 400000; t += 10) m->tick(t);
  CHECK(m->series.history.count == 20);
  smc::consumerClose(m->consumers, h);
  m->tick(t);
  CHECK(!m->running);
  h = smc::consumerOpen(m->consumers, smc::kSlowIntervalMs);
  m->tick(t + 10);
  CHECK(m->running && m->series.history.count <= 1);         // afresh
  delete m;
}

int main() {
  metricTable();
  historyRing();
  changeAndTrend();
  valueHelpers();
  consumers();
  schedule();
  historyClock();
  seriesAndRun();
  changeOverRefreshInterval();
  fixedClockUnevenTicks();
  historyKeptWhileAConsumerStays();
  std::puts("system_metrics_core_tests: ok");
  return 0;
}
