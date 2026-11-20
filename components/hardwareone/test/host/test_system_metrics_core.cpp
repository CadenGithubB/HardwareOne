// System metrics sampler core (System_MetricsCore.h): the metric table, the
// 60-sample history ring and its wrap, change and trend, the consumer table
// that sets the interval, and the due/CPU-warm schedule.
#include "System_MetricsCore.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); std::abort(); \
} } while (false)

namespace smc = system_metrics_core;

static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

static void metricTable() {
  static_assert(smc::kMetricCount == 12, "twelve metrics");
  static_assert(smc::kHistoryLen == 60, "60-sample history");
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

  // Wrap: 150 pushes keep the newest 60, in order, across the seam.
  for (int i = 10; i < 150; ++i) smc::historyPush(h, (float)i);
  CHECK(h.count == smc::kHistoryLen);
  CHECK(smc::historyCopy(h, out, 64) == smc::kHistoryLen);
  for (int i = 0; i < smc::kHistoryLen; ++i) CHECK(out[i] == (float)(90 + i));
  CHECK(smc::historyLatest(h, latest) && latest == 149.0f);
  CHECK(smc::historyCopy(h, out, 1) == 1 && out[0] == 149.0f);

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
  CHECK(smc::cpuWarmWanted(s, 12345, iv));
  s = smc::Schedule{10000, true};
  CHECK(!smc::sampleDue(s, 10000 + iv - 1, iv));
  CHECK(smc::sampleDue(s, 10000 + iv, iv));
  CHECK(smc::msUntilDue(s, 10000, iv) == iv);
  CHECK(!smc::cpuWarmWanted(s, 10000 + 1000, iv));            // slow: CPU sampler idles
  CHECK(!smc::cpuWarmWanted(s, 10000 + iv - smc::kCpuWarmMs - 1, iv));
  CHECK(smc::cpuWarmWanted(s, 10000 + iv - smc::kCpuWarmMs, iv));
  // Fast: warm from 2 s after a sample, and the gap since the previous
  // sample's read is under kCpuPokeGapMs, so the CPU sampler never lapses.
  CHECK(!smc::cpuWarmWanted(smc::Schedule{0, true}, 1999, smc::kFastIntervalMs));
  CHECK(smc::cpuWarmWanted(smc::Schedule{0, true}, 2000, smc::kFastIntervalMs));
  static_assert(smc::kFastIntervalMs - smc::kCpuWarmMs < smc::kCpuPokeGapMs,
                "fast interval keeps one warm run");
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

// The tick's schedule as System_Metrics.cpp runs it: poke at most every
// second while warm is wanted, sample when due and settled, and count the
// read at the sample as a poke. Returns the samples taken in `durationMs`;
// *pokeRuns counts warm runs started, *maxLateMs the worst lateness.
static int simulate(uint32_t intervalMs, uint32_t durationMs, uint32_t lapMs, int* pokeRuns,
                    uint32_t* maxLateMs) {
  smc::Schedule s{0, false};
  smc::CpuWarm w{};
  uint32_t lastPoke = 0;
  int samples = 0;
  *pokeRuns = 0;
  *maxLateMs = 0;
  uint32_t dueAt = 0;
  for (uint32_t now = 1; now <= durationMs; now += lapMs) {
    if (smc::cpuWarmWanted(s, now, intervalMs) && (!w.poked || now - lastPoke >= 1000)) {
      const uint32_t before = w.sinceMs;
      const bool had = w.poked;
      smc::cpuPoke(w, now);
      if (!had || w.sinceMs != before) ++*pokeRuns;
      lastPoke = now;
    }
    if (!smc::sampleDue(s, now, intervalMs)) continue;
    if (!smc::cpuSettled(w, now)) continue;
    if (s.sampled && now - dueAt > *maxLateMs) *maxLateMs = now - dueAt;
    s.lastSampleMs = now;
    s.sampled = true;
    dueAt = now + intervalMs;
    ++samples;
    smc::cpuPoke(w, now);
    lastPoke = now;
  }
  return samples;
}

static void scheduleSimulation() {
  int runs = 0;
  uint32_t late = 0;
  // Fast, 5 min at a 10 ms loop: one warm run, a sample every 5 s on time.
  int n = simulate(smc::kFastIntervalMs, 300000, 10, &runs, &late);
  CHECK(runs == 1);
  CHECK(n >= 59 && n <= 60);
  CHECK(late == 0);
  // Slow, 10 min: a new warm run per sample (the CPU sampler idles between),
  // and still no sample waits for it.
  n = simulate(smc::kSlowIntervalMs, 600000, 10, &runs, &late);
  CHECK(n == 10);
  CHECK(runs == n + 1);  // the last warm-up is for the sample just past the end
  CHECK(late == 0);
}

int main() {
  metricTable();
  historyRing();
  changeAndTrend();
  valueHelpers();
  consumers();
  schedule();
  scheduleSimulation();
  std::puts("system_metrics_core_tests: ok");
  return 0;
}
