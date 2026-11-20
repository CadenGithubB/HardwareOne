#pragma once

#include <stddef.h>
#include <stdint.h>

#include "System_MetricsCore.h"

// System metrics sampler: one snapshot and a 60-sample history per metric,
// for the glasses' stock widget feed (stock-feed plan, phase 3) and any other
// reader. systemMetricsTick() runs on the main loop next to
// periodicMemorySample(); it does nothing until a consumer opens it.
//
// Every figure comes from a cached or O(1) read: the task-CPU sampler's shares,
// heap_caps free/total counters, the Tier 1 memory watch's largest block
// (memoryMonitorInternalLargestBlock), the VFS capacity snapshots (cache only,
// VFS::peekStatsSnapshot), the chip temperature register, the BLE central link
// table, the ESP-NOW session table and the R1 telemetry cache. Never the C6
// memory RPC, a fresh filesystem stat or WiFi.RSSI() (host test
// system_metrics_source pins it). A metric that cannot be read is marked
// unavailable, never filled in. Left out on purpose: Wi-Fi and phone RSSI, log
// sizes, the P4X-EYE battery, the G2 battery (needs a sid 0x09 request).
//
// State: the snapshot, histories and the CPU scratch rows live in one PSRAM
// block allocated on first use and kept (no churn); internal RAM holds a
// pointer, a spinlock and the consumer table. No task of its own.

struct SystemMetricReading {
  float value;        // in the metric's unit (System_MetricsCore.h kMetrics)
  float delta;        // value minus the previous available sample, 0 if none
  float rangeLow;     // chart range for this reading
  float rangeHigh;    //   (limit / paired count / total KB for the count metrics)
  uint32_t sampledAtMs;  // millis() of the sample this reading came from
  uint8_t historyCount;  // samples in this metric's history, <= 60
  int8_t trend;       // +1 up, -1 down, 0 flat or no previous sample
  bool available;     // false: the value could not be read this sample
};

// Main loop only.
void systemMetricsTick();

// Consumers (any task). Open while something displays the metrics; the
// sampler, and through it the 500 ms task-CPU sampler, runs only while at
// least one consumer is open, at the fastest open consumer's interval
// (system_metrics_core::kFastIntervalMs 5 s, kSlowIntervalMs 60 s; clamped to
// 1 s..10 min). Returns a handle 1..4, or 0 when every slot is taken.
// Reopening after everyone closed starts the histories afresh.
uint8_t systemMetricsOpen(uint32_t intervalMs);
bool systemMetricsSetInterval(uint8_t handle, uint32_t intervalMs);
void systemMetricsClose(uint8_t handle);

// Readers (any task). systemMetricsRead is false until the first sample of
// the current run (or with no PSRAM); out.available then says whether this
// metric had a value. systemMetricsHistory copies the newest min(cap, count)
// samples, oldest first, and returns how many. systemMetricsGeneration counts
// samples taken (0 = none yet); a feed sends when it changes.
bool systemMetricsRead(SystemMetricId id, SystemMetricReading& out);
uint8_t systemMetricsHistory(SystemMetricId id, float* out, size_t cap);
uint32_t systemMetricsGeneration();
