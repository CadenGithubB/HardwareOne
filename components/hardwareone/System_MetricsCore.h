#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Dependency-free core of the system metrics sampler (System_Metrics.cpp):
// the metric table, the 60-sample history ring, change-since-last-sample and
// trend, the consumer table that sets the sample interval, and the due/warm
// schedule. No Arduino, ESP-IDF or FreeRTOS types, so the host test
// (test_system_metrics_core.cpp) runs the same arithmetic the device runs.

// Stable ids: consumers (the G2 stock feed) store them in settings, so append
// only and never renumber.
enum class SystemMetricId : uint8_t {
  Cpu = 0,     // CPU load, mean of the cores
  Cpu0 = 1,    // CPU load, core 0
  Cpu1 = 2,    // CPU load, core 1 (unavailable on single-core chips)
  Heap = 3,    // internal RAM used
  HeapBlock = 4,  // internal RAM largest free block
  Psram = 5,   // PSRAM used
  Sd = 6,      // SD card used
  Lfs = 7,     // LittleFS used
  Temp = 8,    // chip temperature
  Links = 9,   // C6 companion BLE links in use (range high = the limit)
  Mesh = 10,   // ESP-NOW peers with an active session (range high = paired)
  Ring = 11,   // R1 ring battery (cached telemetry)
};

namespace system_metrics_core {

inline constexpr size_t kMetricCount = 12;
inline constexpr uint8_t kHistoryLen = 60;

// Sample intervals a consumer may ask for. Fast: the expanded stock page is
// open; slow: the tile only. Requests are clamped to [kMinIntervalMs,
// kMaxIntervalMs]; the sampler runs at the fastest open consumer's interval.
inline constexpr uint32_t kFastIntervalMs = 5000;
inline constexpr uint32_t kSlowIntervalMs = 60000;
inline constexpr uint32_t kMinIntervalMs = 1000;
inline constexpr uint32_t kMaxIntervalMs = 600000;
inline constexpr uint8_t kMaxConsumers = 4;

// The shared task-CPU sampler needs about a second of interest before its
// first valid share; the metrics sampler starts asking this long before a
// sample is due, so a slow (60 s) consumer does not keep it running all the
// time.
inline constexpr uint32_t kCpuWarmMs = 3000;
// Storage figures change slowly and their snapshot read can, on a cold cache,
// take the filesystem lock: re-read them at most this often.
inline constexpr uint32_t kStorageRefreshMs = 60000;

struct MetricInfo {
  SystemMetricId id;
  const char* label;     // <= 4 chars (the glasses' row-1 code)
  const char* longName;  // row-2 name
  const char* unit;
  float rangeLow;        // chart range; the sampler overrides it per reading
  float rangeHigh;       // for the count and size metrics (rangeHigh 0 here)
  float trendDeadband;   // |change| at or below this reads as flat
};

inline constexpr MetricInfo kMetrics[kMetricCount] = {
    {SystemMetricId::Cpu, "CPU", "CPU load", "%", 0.0f, 100.0f, 0.5f},
    {SystemMetricId::Cpu0, "CPU0", "CPU core 0 load", "%", 0.0f, 100.0f, 0.5f},
    {SystemMetricId::Cpu1, "CPU1", "CPU core 1 load", "%", 0.0f, 100.0f, 0.5f},
    {SystemMetricId::Heap, "HEAP", "Internal RAM used", "%", 0.0f, 100.0f, 0.1f},
    {SystemMetricId::HeapBlock, "HBLK", "Largest RAM block", "KB", 0.0f, 0.0f, 0.5f},
    {SystemMetricId::Psram, "PSRM", "PSRAM used", "%", 0.0f, 100.0f, 0.1f},
    {SystemMetricId::Sd, "SD", "SD card used", "%", 0.0f, 100.0f, 0.01f},
    {SystemMetricId::Lfs, "LFS", "Flash storage used", "%", 0.0f, 100.0f, 0.01f},
    {SystemMetricId::Temp, "TEMP", "Chip temperature", "C", 0.0f, 100.0f, 0.5f},
    {SystemMetricId::Links, "LINK", "BLE links in use", "links", 0.0f, 0.0f, 0.0f},
    {SystemMetricId::Mesh, "MESH", "ESP-NOW peers up", "peers", 0.0f, 0.0f, 0.0f},
    {SystemMetricId::Ring, "RING", "R1 ring battery", "%", 0.0f, 100.0f, 0.0f},
};

inline constexpr size_t index(SystemMetricId id) { return static_cast<size_t>(id); }

inline const MetricInfo* info(SystemMetricId id) {
  const size_t i = index(id);
  return i < kMetricCount ? &kMetrics[i] : nullptr;
}

// Label lookup, case-insensitive ("cpu", "Heap"). False for an unknown label.
inline bool idFromLabel(const char* label, SystemMetricId& out) {
  if (!label || !*label) return false;
  for (const MetricInfo& m : kMetrics) {
    const char* a = m.label;
    const char* b = label;
    while (*a && *b) {
      char ca = *a, cb = *b;
      if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
      if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
      if (ca != cb) break;
      ++a;
      ++b;
    }
    if (!*a && !*b) {
      out = m.id;
      return true;
    }
  }
  return false;
}

// ── History ring ────────────────────────────────────────────────────────────
// The last kHistoryLen available samples of one metric. A sample the sampler
// could not read is not pushed (no fake fill-ins), so a metric's history can
// hold fewer samples than another's.
struct History {
  float v[kHistoryLen];
  uint8_t head;   // next write slot
  uint8_t count;  // samples held, <= kHistoryLen
};

inline void historyClear(History& h) {
  h.head = 0;
  h.count = 0;
}

inline void historyPush(History& h, float x) {
  h.v[h.head] = x;
  h.head = (uint8_t)((h.head + 1) % kHistoryLen);
  if (h.count < kHistoryLen) ++h.count;
}

inline bool historyLatest(const History& h, float& out) {
  if (h.count == 0) return false;
  out = h.v[(h.head + kHistoryLen - 1) % kHistoryLen];
  return true;
}

// Copies the newest min(cap, count) samples, oldest first. Returns how many.
inline uint8_t historyCopy(const History& h, float* out, size_t cap) {
  if (!out || cap == 0 || h.count == 0) return 0;
  const uint8_t n = (uint8_t)(cap < h.count ? cap : h.count);
  const uint8_t start = (uint8_t)((h.head + kHistoryLen - n) % kHistoryLen);
  for (uint8_t i = 0; i < n; ++i) out[i] = h.v[(start + i) % kHistoryLen];
  return n;
}

// ── Change since the previous sample ───────────────────────────────────────
// delta is in the metric's own unit (percentage points for the % metrics).
// trend: +1 up, -1 down, 0 flat (|delta| <= deadband, or no previous sample).
struct Change {
  float delta;
  int8_t trend;
};

inline Change changeSince(bool havePrevious, float previous, float current, float deadband) {
  if (!havePrevious) return Change{0.0f, 0};
  const float d = current - previous;
  if (d > deadband) return Change{d, 1};
  if (d < -deadband) return Change{d, -1};
  return Change{d, 0};
}

// ── Value helpers ───────────────────────────────────────────────────────────
// Used share of a pool in percent, clamped to [0, 100]. False when the total
// is zero (no such pool): the metric is then unavailable, never 0 %.
inline bool usedPercent(uint64_t totalBytes, uint64_t freeBytes, float& out) {
  if (totalBytes == 0) return false;
  const uint64_t freeClamped = freeBytes > totalBytes ? totalBytes : freeBytes;
  out = (float)((double)(totalBytes - freeClamped) * 100.0 / (double)totalBytes);
  return true;
}

// Core load from the IDLE task's share of that core (permille of one core).
inline float loadPercentFromIdlePermille(uint16_t idlePermille) {
  const uint16_t idle = idlePermille > 1000 ? 1000 : idlePermille;
  return (float)(1000 - idle) / 10.0f;
}

// Core of an idle task by name: "IDLE0" -> 0, "IDLE1" -> 1, "IDLE" (single
// core) -> 0; -1 for any other task.
inline int idleCoreIndex(const char* name) {
  if (!name || strncmp(name, "IDLE", 4) != 0) return -1;
  const char c = name[4];
  if (c == '\0') return 0;
  if (c >= '0' && c <= '9' && name[5] == '\0') return c - '0';
  return -1;
}

// A chip temperature is believable (finite, -40..125 °C, the parts' rated range).
inline bool temperatureBelievable(float c) {
  return isfinite(c) && c >= -40.0f && c <= 125.0f;
}

// ── Consumers ───────────────────────────────────────────────────────────────
// Each open consumer holds one slot with the interval it wants; the sampler
// (and, through it, the shared 500 ms task-CPU sampler) runs only while at
// least one slot is open. Handles are 1..kMaxConsumers; 0 means none.
struct Consumers {
  uint32_t intervalMs[kMaxConsumers];  // 0 = free slot
};

inline uint32_t clampInterval(uint32_t ms) {
  if (ms < kMinIntervalMs) return kMinIntervalMs;
  if (ms > kMaxIntervalMs) return kMaxIntervalMs;
  return ms;
}

inline uint8_t consumerOpen(Consumers& c, uint32_t intervalMs) {
  for (uint8_t i = 0; i < kMaxConsumers; ++i) {
    if (c.intervalMs[i] == 0) {
      c.intervalMs[i] = clampInterval(intervalMs);
      return (uint8_t)(i + 1);
    }
  }
  return 0;
}

inline bool consumerSetInterval(Consumers& c, uint8_t handle, uint32_t intervalMs) {
  if (handle == 0 || handle > kMaxConsumers || c.intervalMs[handle - 1] == 0) return false;
  c.intervalMs[handle - 1] = clampInterval(intervalMs);
  return true;
}

inline bool consumerClose(Consumers& c, uint8_t handle) {
  if (handle == 0 || handle > kMaxConsumers || c.intervalMs[handle - 1] == 0) return false;
  c.intervalMs[handle - 1] = 0;
  return true;
}

inline uint8_t consumerCount(const Consumers& c) {
  uint8_t n = 0;
  for (uint32_t ms : c.intervalMs) n += ms != 0;
  return n;
}

// The fastest open consumer's interval; 0 when nobody is open.
inline uint32_t effectiveInterval(const Consumers& c) {
  uint32_t best = 0;
  for (uint32_t ms : c.intervalMs) {
    if (ms != 0 && (best == 0 || ms < best)) best = ms;
  }
  return best;
}

// ── Schedule ────────────────────────────────────────────────────────────────
// millis() may wrap; every comparison is unsigned subtraction.
struct Schedule {
  uint32_t lastSampleMs;
  bool sampled;  // lastSampleMs is meaningful
};

inline bool sampleDue(const Schedule& s, uint32_t nowMs, uint32_t intervalMs) {
  return !s.sampled || (uint32_t)(nowMs - s.lastSampleMs) >= intervalMs;
}

inline uint32_t msUntilDue(const Schedule& s, uint32_t nowMs, uint32_t intervalMs) {
  if (sampleDue(s, nowMs, intervalMs)) return 0;
  return intervalMs - (uint32_t)(nowMs - s.lastSampleMs);
}

// True while the task-CPU sampler should be kept interested: within
// kCpuWarmMs of the next sample (always, at the fast interval).
inline bool cpuWarmWanted(const Schedule& s, uint32_t nowMs, uint32_t intervalMs) {
  return msUntilDue(s, nowMs, intervalMs) <= kCpuWarmMs;
}

// Whether the task-CPU sampler has been interested long enough to serve a
// share. Its interest lapses TASK_CPU_INTEREST_MS (15 s) after the last read;
// kCpuPokeGapMs stays under that, so pokes closer together than it count as
// one run. A run is settled kCpuSettleMs after its first poke (two 500 ms
// snapshots plus slack). A due sample waits for that, so the first sample after
// a start or an interval change carries a real CPU figure.
inline constexpr uint32_t kCpuPokeGapMs = 10000;
inline constexpr uint32_t kCpuSettleMs = 1200;

struct CpuWarm {
  uint32_t sinceMs;     // first poke of the current run
  uint32_t lastPokeMs;  // newest poke
  bool poked;           // the fields above are meaningful
};

inline void cpuPoke(CpuWarm& w, uint32_t nowMs) {
  if (!w.poked || (uint32_t)(nowMs - w.lastPokeMs) > kCpuPokeGapMs) w.sinceMs = nowMs;
  w.lastPokeMs = nowMs;
  w.poked = true;
}

inline bool cpuSettled(const CpuWarm& w, uint32_t nowMs) {
  return w.poked && (uint32_t)(nowMs - w.lastPokeMs) <= kCpuPokeGapMs &&
         (uint32_t)(nowMs - w.sinceMs) >= kCpuSettleMs;
}

}  // namespace system_metrics_core
