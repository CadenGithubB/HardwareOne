// Policy of the automatic sensor restart (System_SensorRestartCore.h): the
// wait ladder, its reset after a clean run, the ways an episode ends, and the
// one-failure-report rule. The production header is compiled as it ships.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

#include "System_SensorRestartCore.h"

using namespace sensor_restart;

static State zeroed() { State s; std::memset(&s, 0, sizeof(s)); return s; }

// The state lives in zero-filled PSRAM (.ext_ram.bss): zero must mean idle.
static void storageCases() {
  static_assert(std::is_trivially_copyable<State>::value, "state is plain data");
  static_assert(sizeof(State) == 8, "one device costs 8 bytes");
  State s = zeroed();
  assert(s.phase == Phase::Idle && s.rung == 0 && !s.failureShown && !pending(s));
  assert(tick(s, 123456, true, false, false) == Tick::None && s.phase == Phase::Idle);
  assert(startSettled(s, false, 1) && s.phase == Phase::Idle);  // Not our start: announce as usual.
  assert(startSettled(s, true, 1) && s.phase == Phase::Idle);
}

static void ladderCases() {
  assert(kRungCount == 4);
  assert(delayForRung(0) == 30000 && delayForRung(1) == 120000);
  assert(delayForRung(2) == 600000 && delayForRung(3) == 1800000);
  assert(delayForRung(4) == 1800000 && delayForRung(255) == 1800000);  // The last rung repeats.
  assert(kCleanRunMs == 300000);

  // A device that never answers: 30 s, 2 min, 10 min, then every 30 min, for ever.
  State s = zeroed();
  uint32_t now = 1000;
  assert(noteAutoDisabled(s, now) && s.phase == Phase::Waiting && pending(s) && s.rung == 0);
  const uint32_t waits[] = {30000, 120000, 600000, 1800000, 1800000, 1800000, 1800000};
  for (unsigned i = 0; i < sizeof(waits) / sizeof(waits[0]); ++i) {
    assert(tick(s, now + waits[i] - 1, true, false, false) == Tick::None);
    now += waits[i];
    assert(tick(s, now, true, false, false) == Tick::Attempt);
    assert(tick(s, now + 5, true, false, false) == Tick::Attempt);  // Still due until an outcome is reported.
    attemptRefused(s, now);
    assert(s.phase == Phase::Waiting && pending(s));
    assert(s.rung == (i + 1 < 3 ? i + 1 : 3));
  }
}

static void recoveryCases() {
  // Restarted and clean for five minutes: the episode is over, the ladder is reset.
  State s = zeroed();
  assert(noteAutoDisabled(s, 0));
  assert(tick(s, 30000, true, false, false) == Tick::Attempt);
  attemptQueued(s, 30000);
  assert(s.phase == Phase::Starting && pending(s));
  assert(tick(s, 30500, true, true, false) == Tick::None);  // Queued, not settled.
  assert(tick(s, 9999999, true, true, false) == Tick::None);
  assert(startSettled(s, true, 31000) && s.phase == Phase::Proving && !pending(s));
  assert(tick(s, 31000 + kCleanRunMs - 1, true, false, true) == Tick::None);
  assert(tick(s, 31000 + kCleanRunMs, true, false, true) == Tick::RunProven);
  assert(s.phase == Phase::Idle && s.rung == 0 && !s.failureShown);
  assert(noteAutoDisabled(s, 400000) && s.rung == 0);

  // Failing again before the clean run climbs one rung each time and stops at the last.
  s = zeroed();
  uint32_t now = 0;
  assert(noteAutoDisabled(s, now));
  for (unsigned cycle = 0; cycle < 6; ++cycle) {
    const uint8_t rung = s.rung;
    assert(rung == (cycle < 3 ? cycle : 3));
    now += delayForRung(rung);
    assert(tick(s, now, true, false, false) == Tick::Attempt);
    attemptQueued(s, now);
    assert(startSettled(s, true, now));
    now += kCleanRunMs - 1;  // Just short of a clean run.
    assert(tick(s, now, true, false, true) == Tick::None);
    assert(noteAutoDisabled(s, now) && s.phase == Phase::Waiting);
  }

  // A repeat report while a try waits or is queued changes nothing.
  s = zeroed();
  assert(noteAutoDisabled(s, 0));
  assert(!noteAutoDisabled(s, 10000) && s.sinceMs == 0 && s.rung == 0);
  assert(tick(s, 30000, true, false, false) == Tick::Attempt);
  attemptQueued(s, 30000);
  assert(!noteAutoDisabled(s, 30001) && s.phase == Phase::Starting);

  // Started by someone else while the try was due: watched like our own.
  s = zeroed();
  assert(noteAutoDisabled(s, 0));
  attemptFoundRunning(s, 30000);
  assert(s.phase == Phase::Proving && !pending(s));
  assert(tick(s, 30000 + kCleanRunMs, true, false, true) == Tick::RunProven);
}

static void failureReportCases() {
  // The device answers but its init fails: only the first failure of an
  // episode is announced (the start-failed toast); later ones are log lines.
  State s = zeroed();
  uint32_t now = 0;
  assert(noteAutoDisabled(s, now));
  for (unsigned i = 0; i < 6; ++i) {
    now += delayForRung(s.rung);
    assert(tick(s, now, true, false, false) == Tick::Attempt);
    attemptQueued(s, now);
    const bool announced = startSettled(s, false, now);
    assert(announced == (i == 0));
    assert(s.phase == Phase::Waiting && s.rung == (i + 1 < 3 ? i + 1 : 3));
  }
  // A success is always announced, and a later failure in the same episode
  // (failed again before the clean run) stays quiet.
  now += delayForRung(s.rung);
  assert(tick(s, now, true, false, false) == Tick::Attempt);
  attemptQueued(s, now);
  assert(startSettled(s, true, now) && s.phase == Phase::Proving);
  assert(noteAutoDisabled(s, now + 1000));
  now += 1000 + delayForRung(s.rung);
  assert(tick(s, now, true, false, false) == Tick::Attempt);
  attemptQueued(s, now);
  assert(!startSettled(s, false, now));
  // A new episode reports its first failure again.
  cancel(s);
  assert(noteAutoDisabled(s, now));
  attemptQueued(s, now);
  assert(startSettled(s, false, now));
}

static void endingCases() {
  // Enable or auto-start switched off while a try waits.
  State s = zeroed();
  assert(noteAutoDisabled(s, 0));
  assert(tick(s, 1000, false, false, false) == Tick::GateClosed && s.phase == Phase::Idle);
  assert(tick(s, 9999999, true, false, false) == Tick::None);  // Switching it back on does not revive it.

  // A manual close or open (the caller cancels) in every phase.
  for (int phase = 0; phase < 3; ++phase) {
    s = zeroed();
    assert(noteAutoDisabled(s, 0));
    if (phase >= 1) attemptQueued(s, 30000);
    if (phase >= 2) assert(startSettled(s, true, 31000));
    s.failureShown = true;
    cancel(s);
    assert(s.phase == Phase::Idle && s.rung == 0 && !s.failureShown && !pending(s));
    assert(tick(s, 9999999, true, false, true) == Tick::None);
  }

  // A queued start that a stop removed before the worker settled it.
  s = zeroed();
  assert(noteAutoDisabled(s, 0));
  attemptQueued(s, 30000);
  assert(tick(s, 31000, true, false, false) == Tick::Stopped && s.phase == Phase::Idle);
  // ... or that settled without a report while the sensor runs.
  s = zeroed();
  assert(noteAutoDisabled(s, 0));
  attemptQueued(s, 30000);
  assert(tick(s, 31000, true, false, true) == Tick::None && s.phase == Phase::Proving && s.sinceMs == 31000);
  // The gate is not looked at once the start is queued or the sensor runs.
  s = zeroed();
  assert(noteAutoDisabled(s, 0));
  attemptQueued(s, 30000);
  assert(tick(s, 31000, false, true, false) == Tick::None && s.phase == Phase::Starting);
  assert(startSettled(s, true, 32000));
  assert(tick(s, 33000, false, false, true) == Tick::None && s.phase == Phase::Proving);
}

static void busyCases() {
  // A busy bus is not a used try until it happens kBusyTries times in a row.
  State s = zeroed();
  assert(noteAutoDisabled(s, 0));
  for (unsigned i = 1; i < kBusyTries; ++i) {
    assert(tick(s, 30000 + i * 1000, true, false, false) == Tick::Attempt);
    assert(!attemptBusBusy(s, 30000 + i * 1000) && s.rung == 0 && s.sinceMs == 0);
  }
  assert(attemptBusBusy(s, 40000) && s.rung == 1 && s.sinceMs == 40000 && s.busyTries == 0);
  // The count starts again after any other outcome.
  s = zeroed();
  assert(noteAutoDisabled(s, 0));
  assert(!attemptBusBusy(s, 30000) && !attemptBusBusy(s, 31000));
  attemptRefused(s, 32000);
  for (unsigned i = 1; i < kBusyTries; ++i) assert(!attemptBusBusy(s, 200000));
  assert(s.rung == 1);
}

static void rolloverCases() {
  // millis() wraps after 49.7 days; a wait or a clean run across the wrap is
  // measured correctly.
  assert(elapsedMs(5, 0xFFFFFFF0u) == 21);
  State s = zeroed();
  const uint32_t start = 0xFFFFFFFFu - 10000u;
  assert(noteAutoDisabled(s, start));
  assert(tick(s, start + 29999u, true, false, false) == Tick::None);   // Already wrapped.
  assert(tick(s, start + 30000u, true, false, false) == Tick::Attempt);
  attemptQueued(s, start + 30000u);
  assert(startSettled(s, true, start + 30000u));
  assert(tick(s, start + 30000u + kCleanRunMs - 1, true, false, true) == Tick::None);
  assert(tick(s, start + 30000u + kCleanRunMs, true, false, true) == Tick::RunProven);
  s = zeroed();
  const uint32_t late = 0xFFFFFFFFu - 100u;
  assert(noteAutoDisabled(s, 100));
  attemptRefused(s, 100); attemptRefused(s, 100); attemptRefused(s, late - 1700000u);
  assert(s.rung == 3);
  assert(tick(s, late, true, false, false) == Tick::None);
  assert(tick(s, late + 100001u, true, false, false) == Tick::Attempt);  // 1 800 001 ms, across the wrap.
}

int main() {
  storageCases(); ladderCases(); recoveryCases(); failureReportCases();
  endingCases(); busyCases(); rolloverCases();
  std::puts("Sensor restart policy passed (ladder, clean-run reset, cancel, one failure report, busy bus, rollover)");
}
