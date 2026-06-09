#pragma once

#include <stdint.h>

// ============================================================================
// Background-poll pause — quiesce periodic sensor / I2C polling during a heavy,
// latency-sensitive, or bus-/radio-contended operation.
// ============================================================================
//
// Any subsystem doing heavy work that contends for a shared I2C bus or the BLE
// radio (BLE connect/discovery, ESP-NOW bursts, file I/O, I2C bus recovery, …)
// can fence off the periodic sensor poll loops while it runs. Lives in its own
// lightweight header so non-I2C callers can use it without the sensor database.
//
// TWO GRANULARITIES:
//   * Blanket  — pollPause()/PollPauseGuard{}  (default): pause EVERY bus. Use
//     when the contended resource is global (the CPU/FS/radio) or unknown.
//   * Per-bus  — pollPause(bus)/PollPauseGuard{bus}: pause only that I2C bus.
//     Use when the contention is bus-local (e.g. the gamepad bus storms during
//     a BLE connect, but the OLED bus is fine — so only the gamepad bus yields
//     and the display keeps rendering). Bus recovery / per-bus scans likewise
//     only need to freeze the bus being worked on.
//
// Semantics:
//   * Blanket and per-bus depths are tracked independently and each nests, so a
//     blanket pause and a per-bus pause compose without clobbering each other —
//     correct across tasks and cores. Always pair pause/resume; prefer the RAII
//     guard, which resumes on every return path.
//   * A bus is "paused" iff a blanket pause is active OR that bus is paused.
//   * Sensor poll loops gate on pollPaused(theirBus) so they only yield for
//     pauses that actually affect their bus.
//
// This pauses only the periodic *sensor poll loops*; it does not abort in-flight
// transactions and does not touch BLE/WiFi.

// Number of independent I2C buses tracked. Mirrors I2CDeviceManager's NUM_BUSES
// (bus 0 = Wire1 / I2C_NUM_1 / gamepad; bus 1 = Wire / I2C_NUM_0 / OLED). Kept
// as its own constant so this module needn't depend on the I2C manager header —
// if NUM_BUSES ever grows, bump this to match.
static constexpr uint8_t POLL_NUM_BUSES = 2;

// Sentinel meaning "every bus" — the blanket pause, and the default.
static constexpr uint8_t POLL_BUS_ALL = 0xFF;

// Coarse mirror: true if ANY pause is active (blanket or any single bus). For
// logging / "is the system busy" gates. Bus-specific poll loops should gate on
// pollPaused(theirBus) instead, so they only yield for pauses affecting them.
extern volatile bool gSensorPollingPaused;

// Pause / resume. bus = POLL_BUS_ALL (default) pauses every bus (blanket);
// bus = 0..POLL_NUM_BUSES-1 pauses only that bus. An out-of-range bus is treated
// as blanket (the safe direction).
void pollPause(uint8_t bus = POLL_BUS_ALL);
void pollResume(uint8_t bus = POLL_BUS_ALL);

// Is polling paused for `bus`? True if a blanket pause is active OR that bus is
// paused. A bus outside [0, POLL_NUM_BUSES) falls back to the coarse
// "anything paused" answer (conservative).
bool pollPaused(uint8_t bus);

// RAII scope guard: pauses on construction, resumes on destruction — covers
// every early-return path automatically. Default scope is all buses; pass a bus
// index to scope it to one bus.
struct PollPauseGuard {
  explicit PollPauseGuard(uint8_t bus = POLL_BUS_ALL) : bus_(bus) { pollPause(bus_); }
  ~PollPauseGuard() { pollResume(bus_); }
  PollPauseGuard(const PollPauseGuard&) = delete;
  PollPauseGuard& operator=(const PollPauseGuard&) = delete;
 private:
  uint8_t bus_;
};
