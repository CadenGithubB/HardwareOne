#ifndef SYSTEM_SENSOR_RESTART_CORE_H
#define SYSTEM_SENSOR_RESTART_CORE_H

// Restart policy for a sensor that switched itself off after bus errors.
//
// Pure policy: no clock, no hardware, no settings, no events. The sensor queue
// worker (System_I2C.cpp) owns one State per device type, feeds it the time and
// what it observed, and acts on the answers. Times are millis() values;
// elapsed time is taken by unsigned subtraction, so the 49-day rollover of
// millis() does not disturb a wait in progress.
//
// One episode: the sensor auto-disables -> wait -> try to start it -> either it
// runs cleanly for kCleanRunMs (episode over, the ladder is back at its first
// rung) or it fails again and the next wait is one rung longer. The last rung
// repeats; the policy never gives up on its own. A manual close, a manual
// open, or the sensor's enable/auto-start being switched off ends the episode.

#include <stdint.h>

namespace sensor_restart {

// Wait before each try: 30 s, 2 min, 10 min, then every 30 min.
constexpr uint32_t kRungDelayMs[] = { 30000u, 120000u, 600000u, 1800000u };
constexpr uint8_t kRungCount = sizeof(kRungDelayMs) / sizeof(kRungDelayMs[0]);

// A restarted sensor that runs this long without auto-disabling again has
// recovered: the next failure starts at the first rung.
constexpr uint32_t kCleanRunMs = 300000u;

// A try that found the bus busy is repeated on the next ticks; this many in a
// row count as one used try.
constexpr uint8_t kBusyTries = 5;

enum class Phase : uint8_t {
  Idle,      // nothing to do (zero, so zero-filled storage starts here)
  Waiting,   // auto-disabled; the next try is due delayForRung(rung) after sinceMs
  Starting,  // a start was queued and has not settled yet
  Proving,   // restarted; watching for a clean run
};

// 8 bytes. Plain data, valid when zero-filled.
struct State {
  Phase phase;
  uint8_t rung;        // index into kRungDelayMs for the current/next wait
  bool failureShown;   // this episode already reported one failed start
  uint8_t busyTries;   // tries in a row that found the bus busy
  uint32_t sinceMs;    // when the current phase began
};
static_assert(sizeof(State) == 8, "restart state is sized for a per-device PSRAM array");

inline uint32_t elapsedMs(uint32_t nowMs, uint32_t sinceMs) { return nowMs - sinceMs; }

inline uint32_t delayForRung(uint8_t rung) {
  return kRungDelayMs[rung < kRungCount ? rung : (uint8_t)(kRungCount - 1)];
}

// A try is waiting or queued (the sensor is expected back without the user).
inline bool pending(const State& s) {
  return s.phase == Phase::Waiting || s.phase == Phase::Starting;
}

inline void cancel(State& s) { s = State{}; }

inline void nextRung(State& s) {
  if (s.rung < kRungCount - 1) ++s.rung;
}

// The sensor ended by auto-disable. True when a wait began (the caller logs
// it); false for a repeat report while a try is already waiting or queued.
inline bool noteAutoDisabled(State& s, uint32_t nowMs) {
  switch (s.phase) {
    case Phase::Idle:
      s = State{};
      break;
    case Phase::Proving:
      nextRung(s);  // failed again before the clean run: wait longer this time
      break;
    case Phase::Waiting:
    case Phase::Starting:
      return false;
  }
  s.phase = Phase::Waiting;
  s.busyTries = 0;
  s.sinceMs = nowMs;
  return true;
}

enum class Tick : uint8_t {
  None,
  GateClosed,  // enable or auto-start was switched off: episode ended
  Attempt,     // the wait is over: the caller tries a start and reports back
  Stopped,     // the queued start was taken by a stop: episode ended
  RunProven,   // clean run complete: episode ended
};

// Periodic step. `gateOpen`: the sensor's enable and auto-start are both on.
// `startPending`: a start for it is queued or not settled. `running`: its
// running flag is set.
inline Tick tick(State& s, uint32_t nowMs, bool gateOpen, bool startPending, bool running) {
  switch (s.phase) {
    case Phase::Idle:
      return Tick::None;
    case Phase::Waiting:
      if (!gateOpen) { cancel(s); return Tick::GateClosed; }
      return elapsedMs(nowMs, s.sinceMs) >= delayForRung(s.rung) ? Tick::Attempt : Tick::None;
    case Phase::Starting:
      if (startPending) return Tick::None;
      // Settled without a report: it either runs, or a stop removed it.
      if (running) { s.phase = Phase::Proving; s.sinceMs = nowMs; return Tick::None; }
      cancel(s);
      return Tick::Stopped;
    case Phase::Proving:
      if (elapsedMs(nowMs, s.sinceMs) < kCleanRunMs) return Tick::None;
      cancel(s);
      return Tick::RunProven;
  }
  return Tick::None;
}

// Outcomes of the try that Tick::Attempt asked for.

// The start was queued.
inline void attemptQueued(State& s, uint32_t nowMs) {
  s.phase = Phase::Starting;
  s.busyTries = 0;
  s.sinceMs = nowMs;
}

// The device did not answer (or the start was refused for good): this rung is
// used, the next wait is one rung longer.
inline void attemptRefused(State& s, uint32_t nowMs) {
  nextRung(s);
  s.phase = Phase::Waiting;
  s.busyTries = 0;
  s.sinceMs = nowMs;
}

// The bus was busy, so the device was not asked. The wait stays due and the
// next tick tries again; kBusyTries in a row count as one used try. True when
// that happened (the caller logs it like a refusal).
inline bool attemptBusBusy(State& s, uint32_t nowMs) {
  if (++s.busyTries < kBusyTries) return false;
  attemptRefused(s, nowMs);
  return true;
}

// Someone else started it in the meantime: watch it like our own restart.
inline void attemptFoundRunning(State& s, uint32_t nowMs) {
  s.phase = Phase::Proving;
  s.busyTries = 0;
  s.sinceMs = nowMs;
}

// (A try that could not be made yet - cleanup still pending, queue busy -
// changes nothing: the state stays Waiting and the next tick asks again.)

// The queue worker settled a start for this sensor. Returns true when the
// worker should publish the outcome as usual (not our start, a success, or the
// first failure of the episode) and false when a repeated failure should only
// be logged.
inline bool startSettled(State& s, bool started, uint32_t nowMs) {
  if (s.phase != Phase::Starting) return true;
  s.sinceMs = nowMs;
  if (started) {
    s.phase = Phase::Proving;
    return true;
  }
  nextRung(s);
  s.phase = Phase::Waiting;
  const bool show = !s.failureShown;
  s.failureShown = true;
  return show;
}

}  // namespace sensor_restart

#endif  // SYSTEM_SENSOR_RESTART_CORE_H
