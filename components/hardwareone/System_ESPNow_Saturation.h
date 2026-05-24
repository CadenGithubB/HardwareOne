#pragma once

// ESP-NOW Link Saturation — rolling 30s sampler of derived link-pressure
// signals on top of the cumulative counters in EspNowState::routerMetrics and
// the existing stream/pending-frame queues. Tick once per second from the
// periodic ESP-NOW task; report via the `espnowsaturation` CLI command.
//
// Intentionally honest: there is no fictitious "% of bandwidth" here — ESP-NOW
// shares airtime with WiFi and other RF activity, so the radio's effective
// ceiling is not constant or measurable from this device. Instead we surface
// indicators that DO have calibrated ceilings (stream queue depth, pending
// frames, rekey progress, send-failure ratio) and absolute rates (frames/sec,
// ACK RTT). The single most actionable signal for stress testing is the
// streamDroppedCount delta over the window: non-zero means the receive ring
// overflowed during the test, i.e. we definitively saturated something.
//
// Memory cost: ~1.1 KiB static DRAM for the 30-sample ring. No PSRAM.

#include "System_BuildConfig.h"
#include <stdint.h>

#if ENABLE_ESPNOW

// Called once per second from the periodic ESP-NOW task tick (any cadence is
// fine — internal clock divides to 1 Hz). Snapshots cumulative counters into
// the next ring slot and rolls window aggregates.
void espnowSaturationTick();

// Called from the broadcast-tracker completion path (and any other code path
// that observes a measured ACK round-trip in ms). Accumulates into the current
// 1-second bucket; the tick averages and rolls them into the ring.
void espnowSaturationNoteAckRtt(uint32_t rttMs);

// Prints a human-readable report via BROADCAST_PRINTF. Safe to call from the
// CLI thread (atomically reads uint32_t counters; never writes).
void espnowSaturationReport();

// Clears the ring + the in-progress accumulators. Useful before a stress test
// so window peaks/avgs reflect only the test, not boot warmup.
void espnowSaturationReset();

#else  // !ENABLE_ESPNOW

inline void espnowSaturationTick() {}
inline void espnowSaturationNoteAckRtt(uint32_t) {}
inline void espnowSaturationReport() {}
inline void espnowSaturationReset() {}

#endif
