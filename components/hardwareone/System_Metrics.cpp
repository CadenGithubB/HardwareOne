// System metrics sampler (System_Metrics.h). Main-loop producer, any-task
// readers: the PSRAM state is written under sMetricsMux and copied out under
// it. The ring, change, consumer and schedule arithmetic is in
// System_MetricsCore.h (host test system_metrics_core_tests).
//
// Banned here (host test system_metrics_source): the C6 memory RPC, any fresh
// filesystem stat or filesystem lock (getStatsSnapshot, isSdAvailable),
// WiFi.RSSI(). Each would block the main loop for an RPC, a LittleFS walk
// (1.3 s on the P4X-EYE) or a long filesystem operation.
#include "System_Metrics.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "System_BuildConfig.h"
#include "System_MemUtil.h"
#include "System_MemoryMonitor.h"
#include "System_TaskUtils.h"
#include "System_VFS.h"
#include "HAL_Radio.h"
#include "BLE_CentralCoordinator.h"
#include "System_ESPNow.h"
#include "G2_Ring.h"

namespace smc = system_metrics_core;

static_assert(smc::kCpuPokeGapMs < TASK_CPU_INTEREST_MS,
              "pokes must come before the task-CPU sampler's interest lapses");

namespace {

// Task-CPU rows copied per sample. The IDLE rows can sit anywhere in the
// table, so this covers the busiest image's task count with room; a table
// longer than this can cut them off, and CPU then reads unavailable.
constexpr size_t kCpuRows = 80;
// How often the warm-up pokes the task-CPU sampler (any closer is wasted).
constexpr uint32_t kCpuPokeEveryMs = 1000;

struct Raw {
  float value;
  float rangeLow;
  float rangeHigh;
  bool available;
};

struct MetricsState {
  smc::History history[smc::kMetricCount];
  SystemMetricReading current[smc::kMetricCount];
  TaskCpuShare cpuRows[kCpuRows];
  smc::Schedule schedule;
  smc::CpuWarm cpuWarm;
  uint32_t lastCpuPokeMs;
  uint32_t generation;
  // Storage figures, re-read at most every kStorageRefreshMs.
  uint32_t storageReadMs;
  bool storageRead;
  Raw sd;
  Raw lfs;
  bool running;  // a run is in progress (some consumer open since the last start)
};

MetricsState* sMetrics = nullptr;  // PSRAM, allocated by the tick, never freed
portMUX_TYPE sMetricsMux = portMUX_INITIALIZER_UNLOCKED;
smc::Consumers sConsumers = {};    // under sMetricsMux
uint32_t sAllocRetryMs = 0;        // main loop only

Raw unavailable() { return Raw{0.0f, 0.0f, 0.0f, false}; }

Raw percentRaw(bool ok, float pct) {
  return ok ? Raw{pct, 0.0f, 100.0f, true} : unavailable();
}

// CPU: per-core load from the IDLE rows of the shared sampler's last interval.
void readCpu(MetricsState& s, Raw& all, Raw& core0, Raw& core1) {
  all = core0 = core1 = unavailable();
  size_t n = 0;
  if (!taskCpuShares(s.cpuRows, kCpuRows, &n, nullptr)) return;
  float load[2] = {0.0f, 0.0f};
  bool have[2] = {false, false};
  for (size_t i = 0; i < n; ++i) {
    if (!s.cpuRows[i].idle) continue;
    const int core = smc::idleCoreIndex(s.cpuRows[i].name);
    if (core < 0 || core > 1) continue;
    load[core] = smc::loadPercentFromIdlePermille(s.cpuRows[i].permille);
    have[core] = true;
  }
  if (have[0]) core0 = Raw{load[0], 0.0f, 100.0f, true};
  if (have[1]) core1 = Raw{load[1], 0.0f, 100.0f, true};
  const int cores = portNUM_PROCESSORS;
  if (cores == 1 && have[0]) {
    all = core0;
  } else if (cores >= 2 && have[0] && have[1]) {
    all = Raw{(load[0] + load[1]) / 2.0f, 0.0f, 100.0f, true};
  }
}

// Storage: the VFS presentation snapshots, cache only (VFS::peekStatsSnapshot
// never takes the filesystem lock or samples). A tier with no snapshot (at
// boot until something reads it, after the G2 Files page or a delete marks it
// stale, after any SD mount change) reads unavailable until another reader
// (the log writer's free-space check, /api/system, the Files page) refills
// it. Every SD mount change drops the SD snapshot, so an SD hit also means a
// mounted card.
void readStorage(MetricsState& s, uint32_t nowMs) {
  if (s.storageRead && (uint32_t)(nowMs - s.storageReadMs) < smc::kStorageRefreshMs) return;
  s.storageRead = true;
  s.storageReadMs = nowMs;
  float pct = 0.0f;
  VFS::CapacitySnapshot snap = {};
  bool ok = VFS::peekStatsSnapshot(VFS::INTERNAL, snap) &&
            smc::usedPercent(snap.totalBytes, snap.freeBytes, pct);
  s.lfs = percentRaw(ok, pct);
  snap = {};
  pct = 0.0f;
  ok = VFS::peekStatsSnapshot(VFS::SDCARD, snap) &&
       smc::usedPercent(snap.totalBytes, snap.freeBytes, pct);
  s.sd = percentRaw(ok, pct);
}

Raw readTemperature() {
  const float c = temperatureRead();
#ifdef CONFIG_IDF_TARGET_ESP32
  // The classic ESP32's undocumented sensor reads raw 128 ((128-32)/1.8 =
  // 53.3 C) when it has nothing: not a temperature.
  if (c > 53.3f && c < 53.4f) return unavailable();
#endif
  if (!smc::temperatureBelievable(c)) return unavailable();
  return Raw{c, 0.0f, 100.0f, true};
}

Raw readLinks() {
#if HW1_RADIO_COMPANION && ENABLE_BLUETOOTH
  const uint8_t limit = bleCentralLinkLimit();
  if (limit == 0) return unavailable();
  uint8_t used = 0;
  static constexpr BleCentralLink kLinks[] = {BleCentralLink::Left, BleCentralLink::Right,
                                              BleCentralLink::Ring, BleCentralLink::Phone};
  for (BleCentralLink link : kLinks) used += bleCentralLinkHeld(link) ? 1 : 0;
  return Raw{(float)used, 0.0f, (float)limit, true};
#else
  return unavailable();  // no C6 companion on this board
#endif
}

Raw readMesh() {
#if ENABLE_ESPNOW
  if (!gEspNow || !gEspNow->initialized) return unavailable();
  int paired = 0, up = 0;
  const int n = gEspNow->deviceCount < 16 ? gEspNow->deviceCount : 16;
  for (int i = 0; i < n; ++i) {
    const uint8_t* mac = gEspNow->devices[i].mac;
    if (isSelfMac(mac)) continue;
    ++paired;
    if (espnowSessionUp(mac)) ++up;
  }
  return Raw{(float)up, 0.0f, (float)(paired > 0 ? paired : 1), true};
#else
  return unavailable();
#endif
}

Raw readRing() {
  G2RingTelemetry t{};
  g2RingGetTelemetry(t);
  if (!t.batteryValid || t.battery > 100) return unavailable();
  return Raw{(float)t.battery, 0.0f, 100.0f, true};
}

void takeSample(MetricsState& s, uint32_t nowMs) {
  Raw raw[smc::kMetricCount];
  readCpu(s, raw[smc::index(SystemMetricId::Cpu)], raw[smc::index(SystemMetricId::Cpu0)],
          raw[smc::index(SystemMetricId::Cpu1)]);

  const size_t internalTotal = hw1InternalTotalBytes();
  float pct = 0.0f;
  bool ok = smc::usedPercent(internalTotal, hw1InternalFreeBytes(), pct);
  raw[smc::index(SystemMetricId::Heap)] = percentRaw(ok, pct);
  size_t largest = 0;
  raw[smc::index(SystemMetricId::HeapBlock)] =
      memoryMonitorInternalLargestBlock(largest) && internalTotal > 0
          ? Raw{(float)largest / 1024.0f, 0.0f, (float)internalTotal / 1024.0f, true}
          : unavailable();
  pct = 0.0f;
  ok = smc::usedPercent(heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
                        heap_caps_get_free_size(MALLOC_CAP_SPIRAM), pct);
  raw[smc::index(SystemMetricId::Psram)] = percentRaw(ok, pct);

  readStorage(s, nowMs);
  raw[smc::index(SystemMetricId::Sd)] = s.sd;
  raw[smc::index(SystemMetricId::Lfs)] = s.lfs;
  raw[smc::index(SystemMetricId::Temp)] = readTemperature();
  raw[smc::index(SystemMetricId::Links)] = readLinks();
  raw[smc::index(SystemMetricId::Mesh)] = readMesh();
  raw[smc::index(SystemMetricId::Ring)] = readRing();

  portENTER_CRITICAL(&sMetricsMux);
  for (size_t i = 0; i < smc::kMetricCount; ++i) {
    SystemMetricReading& r = s.current[i];
    smc::History& h = s.history[i];
    r.sampledAtMs = nowMs;
    r.available = raw[i].available;
    if (raw[i].available) {
      float previous = 0.0f;
      const bool havePrevious = smc::historyLatest(h, previous);
      const smc::Change c =
          smc::changeSince(havePrevious, previous, raw[i].value, smc::kMetrics[i].trendDeadband);
      smc::historyPush(h, raw[i].value);
      r.value = raw[i].value;
      r.delta = c.delta;
      r.trend = c.trend;
      r.rangeLow = raw[i].rangeLow;
      r.rangeHigh = raw[i].rangeHigh;
    } else {
      r.value = 0.0f;
      r.delta = 0.0f;
      r.trend = 0;
      r.rangeLow = smc::kMetrics[i].rangeLow;
      r.rangeHigh = smc::kMetrics[i].rangeHigh;
    }
    r.historyCount = h.count;
  }
  ++s.generation;
  if (s.generation == 0) s.generation = 1;  // 0 means "no sample yet"
  portEXIT_CRITICAL(&sMetricsMux);
}

}  // namespace

void systemMetricsTick() {
  const uint32_t nowMs = millis();
  portENTER_CRITICAL(&sMetricsMux);
  const uint32_t intervalMs = smc::effectiveInterval(sConsumers);
  portEXIT_CRITICAL(&sMetricsMux);

  if (intervalMs == 0) {
    // Nobody is looking: stop. The task-CPU sampler idles on its own once
    // the pokes stop (TASK_CPU_INTEREST_MS).
    if (sMetrics && sMetrics->running) {
      portENTER_CRITICAL(&sMetricsMux);
      sMetrics->running = false;
      portEXIT_CRITICAL(&sMetricsMux);
    }
    return;
  }

  if (!sMetrics) {
    if (sAllocRetryMs != 0 && (uint32_t)(nowMs - sAllocRetryMs) < 10000UL) return;
    sAllocRetryMs = nowMs;
    // PSRAM only: on a board without it the sampler stays off (reads false)
    // rather than taking ~5 KB of internal RAM.
    auto* block = static_cast<MetricsState*>(
        ps_alloc(sizeof(MetricsState), AllocPolicy::RequirePsram, "sysmetrics"));
    if (!block) return;
    memset(block, 0, sizeof(MetricsState));
    portENTER_CRITICAL(&sMetricsMux);
    sMetrics = block;
    portEXIT_CRITICAL(&sMetricsMux);
  }
  MetricsState& s = *sMetrics;

  if (!s.running) {
    // A new run: histories and change baselines start afresh, so a chart never
    // joins samples across a stretch nobody sampled.
    portENTER_CRITICAL(&sMetricsMux);
    for (smc::History& h : s.history) smc::historyClear(h);
    memset(s.current, 0, sizeof(s.current));
    s.generation = 0;
    s.running = true;
    portEXIT_CRITICAL(&sMetricsMux);
    s.schedule = smc::Schedule{0, false};
    s.storageRead = false;
  }

  // Keep the shared 500 ms task-CPU sampler interested only around a sample:
  // within kCpuWarmMs of it (the whole time at the 5 s interval).
  if (smc::cpuWarmWanted(s.schedule, nowMs, intervalMs) &&
      (!s.cpuWarm.poked || (uint32_t)(nowMs - s.lastCpuPokeMs) >= kCpuPokeEveryMs)) {
    (void)taskCpuShares(nullptr, 0, nullptr, nullptr);  // registers interest only
    smc::cpuPoke(s.cpuWarm, nowMs);
    s.lastCpuPokeMs = nowMs;
  }

  if (!smc::sampleDue(s.schedule, nowMs, intervalMs)) return;
  // Wait (at most kCpuSettleMs) until the CPU sampler has a share to serve.
  if (!smc::cpuSettled(s.cpuWarm, nowMs)) return;
  s.schedule.lastSampleMs = nowMs;
  s.schedule.sampled = true;
  takeSample(s, nowMs);
  // The read in takeSample also registered interest.
  smc::cpuPoke(s.cpuWarm, nowMs);
  s.lastCpuPokeMs = nowMs;
}

uint8_t systemMetricsOpen(uint32_t intervalMs) {
  portENTER_CRITICAL(&sMetricsMux);
  const uint8_t handle = smc::consumerOpen(sConsumers, intervalMs);
  portEXIT_CRITICAL(&sMetricsMux);
  return handle;
}

bool systemMetricsSetInterval(uint8_t handle, uint32_t intervalMs) {
  portENTER_CRITICAL(&sMetricsMux);
  const bool ok = smc::consumerSetInterval(sConsumers, handle, intervalMs);
  portEXIT_CRITICAL(&sMetricsMux);
  return ok;
}

void systemMetricsClose(uint8_t handle) {
  portENTER_CRITICAL(&sMetricsMux);
  (void)smc::consumerClose(sConsumers, handle);
  portEXIT_CRITICAL(&sMetricsMux);
}

bool systemMetricsRead(SystemMetricId id, SystemMetricReading& out) {
  const size_t i = smc::index(id);
  if (i >= smc::kMetricCount) return false;
  portENTER_CRITICAL(&sMetricsMux);
  const bool ok = sMetrics && sMetrics->running && sMetrics->generation != 0;
  if (ok) out = sMetrics->current[i];
  portEXIT_CRITICAL(&sMetricsMux);
  return ok;
}

uint8_t systemMetricsHistory(SystemMetricId id, float* out, size_t cap) {
  const size_t i = smc::index(id);
  if (i >= smc::kMetricCount || !out || cap == 0) return 0;
  portENTER_CRITICAL(&sMetricsMux);
  const uint8_t n = (sMetrics && sMetrics->running) ? smc::historyCopy(sMetrics->history[i], out, cap) : 0;
  portEXIT_CRITICAL(&sMetricsMux);
  return n;
}

uint32_t systemMetricsGeneration() {
  portENTER_CRITICAL(&sMetricsMux);
  const uint32_t g = (sMetrics && sMetrics->running) ? sMetrics->generation : 0;
  portEXIT_CRITICAL(&sMetricsMux);
  return g;
}
