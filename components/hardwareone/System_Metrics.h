#pragma once

#include <stddef.h>
#include <stdint.h>

#include "System_MetricsCore.h"

// System metrics sampler: one snapshot and a 185-point history per metric
// (one point per history step on the sampler's own clock: 10, 20 or 60 s,
// default 20 s = about an hour; systemMetricsSetHistoryStep), for the
// glasses' stock widget feed (stock-feed plan, phase 3) and any other reader.
// systemMetricsTick() runs on the main loop next to periodicMemorySample(); it
// does nothing until a consumer opens it.
//
// Every figure comes from a cached or O(1) read: the task-CPU sampler's shares,
// heap_caps free/total counters, the Tier 1 memory watch's largest block
// (memoryMonitorInternalLargestBlock), the VFS capacity snapshots (cache only,
// VFS::peekStatsSnapshot), the chip temperature register, the CPU clock
// registers (getCpuFrequencyMhz), the BLE central link table, the ESP-NOW
// session table and the R1 telemetry cache. Never the C6 memory RPC, a fresh
// filesystem stat or WiFi.RSSI() (host test system_metrics_source pins it). A metric that cannot be read is marked
// unavailable, never filled in. Left out on purpose: Wi-Fi and phone RSSI, log
// sizes, the P4X-EYE battery, the G2 battery (needs a sid 0x09 request).
//
// State: the snapshot, histories and the CPU scratch rows live in one PSRAM
// block (about 11.5 KB) allocated on first use and kept (no churn); internal
// RAM holds a pointer, a spinlock and the consumer table. No task of its own.
// Nothing is persisted: a reboot starts with empty histories.

struct SystemMetricReading {
  float value;        // in the metric's unit (System_MetricsCore.h kMetrics)
  float delta;        // value minus the value at the last refresh sample (the
                      //   change over the last refresh interval), 0 if none
  float rangeLow;     // chart range for this reading
  float rangeHigh;    //   (limit / paired count / total KB for the count metrics)
  uint32_t sampledAtMs;  // millis() of the sample this reading came from
  uint8_t historyCount;  // points in this metric's history, <= 185
  int8_t trend;       // +1 up, -1 down, 0 flat or no previous sample
  bool available;     // false: the value could not be read this sample
};

// Main loop only.
void systemMetricsTick();

// Consumers (any task). Open while something displays the metrics, or wants
// their history kept; the sampler, and through it the 500 ms task-CPU
// sampler, runs only while at least one consumer is open. The value shown is
// refreshed at the fastest open consumer's interval
// (system_metrics_core::kFastIntervalMs 10 s, kSlowIntervalMs 60 s; clamped
// to 1 s..10 min); the history takes one point per history step whatever
// the intervals. Returns a handle 1..4, or 0 when
// every slot is taken. Reopening after everyone closed starts the histories
// afresh; as long as one consumer stays open they are kept.
uint8_t systemMetricsOpen(uint32_t intervalMs);
bool systemMetricsSetInterval(uint8_t handle, uint32_t intervalMs);
void systemMetricsClose(uint8_t handle);

// Readers (any task). systemMetricsRead is false until the first sample of
// the current run (or with no PSRAM); out.available then says whether this
// metric had a value. systemMetricsHistory copies the newest min(cap, count)
// history points, oldest first, and returns how many. systemMetricsGeneration counts
// samples taken (0 = none yet). systemMetricsPointGeneration counts the
// history points of the current run (one per history step taken, 0 = none
// yet; a step change starts it over),
// so a chart feed can resend when its line has moved although the shown
// values did not.
bool systemMetricsRead(SystemMetricId id, SystemMetricReading& out);
uint8_t systemMetricsHistory(SystemMetricId id, float* out, size_t cap);
uint32_t systemMetricsGeneration();
uint32_t systemMetricsPointGeneration();

// History step (any task): one of system_metrics_core::kHistoryStepChoicesMs
// (10, 20 or 60 s; the default before any call is kHistoryStepMs, 20 s).
// Returns true when it changed the step. A change during a run drops every
// metric's history at once (the shown values and change baselines stay) and
// the next sample starts a new grid at the new step, so a chart never mixes
// two spacings; with no run it applies to the next one. A step not in the
// list is refused (false, nothing changes).
bool systemMetricsSetHistoryStep(uint32_t stepMs);
