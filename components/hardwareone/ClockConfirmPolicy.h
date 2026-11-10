#pragma once
#include <stdint.h>

// ============================================================================
// ClockConfirmPolicy — the clock the R1 ring is given (readiness G-79)
// ============================================================================
// Pure decision code: no IDF or Arduino types, so the host suite runs it
// (test/host/test_clock_confirm_policy.cpp). System_Clock.cpp feeds it the
// wall clock and the monotonic timer.
//
// SNTP replies are not authenticated, so one forged reply can move the device
// clock by years. The ring stores its daily pages under the clock it is
// given, and keeps 30 days: every day stored under a far-future date pushes a
// real day out. The ring is therefore given the *confirmed* clock:
//
//   - Every step of the clock by a local source (RTC, `timeset`, CM5 carrier)
//     and every network step that agrees with the clock already running
//     becomes the reference: wall clock minus monotonic time.
//   - A network step that moves a clock set earlier this boot by more than
//     kUnconfirmedJumpUs is not taken as the reference. While the wall clock
//     and the reference disagree by more than that, the confirmed clock is
//     the reference carried forward by the monotonic timer.
//   - The hold ends with a step from a local source, a network step back to
//     within the bound of the reference, or a restart.
//
// The comparison is between the wall clock and the reference, not a flag set
// when a step is reported, so the time between lwIP stepping the clock and
// the main loop reporting the step is covered too.
//
// Not covered: the first network sync after a start with no clock (nothing to
// compare it with), and a forged clock that is in force at a restart.

namespace ClockConfirm {

constexpr int64_t kUnconfirmedJumpUs = 24LL * 60 * 60 * 1000000;

struct State {
  int64_t refUs = 0;     // confirmed epoch-us minus monotonic-us; meaningful when seeded
  bool seeded = false;   // the reference came from a source that may vouch for the clock
  bool holding = false;  // the last network step was refused as the reference
};

enum class Verdict : uint8_t { Accepted, HoldStarted, HoldContinues, HoldReleased };

inline bool disagrees(const State& state, int64_t wallUs, int64_t monoUs) {
  if (!state.seeded) return false;
  const int64_t difference = wallUs - (state.refUs + monoUs);
  return difference > kUnconfirmedJumpUs || difference < -kUnconfirmedJumpUs;
}

// Call after every step of the clock. `fromNetwork`: the step came from an
// SNTP reply. `reference`: the source may vouch for the clock from now on
// (a valid clock from the RTC, SNTP, `timeset` or the CM5; not a clock
// adopted from the ring, which would make the ring its own witness).
inline Verdict onStep(State& state, bool fromNetwork, bool reference, int64_t wallUs, int64_t monoUs) {
  if (fromNetwork && disagrees(state, wallUs, monoUs)) {
    const bool already = state.holding;
    state.holding = true;
    return already ? Verdict::HoldContinues : Verdict::HoldStarted;
  }
  const bool released = state.holding;
  state.refUs = wallUs - monoUs;
  state.seeded = reference;
  state.holding = false;
  return released ? Verdict::HoldReleased : Verdict::Accepted;
}

// The confirmed clock in epoch-us.
inline int64_t nowUs(const State& state, int64_t wallUs, int64_t monoUs) {
  return disagrees(state, wallUs, monoUs) ? state.refUs + monoUs : wallUs;
}

}  // namespace ClockConfirm
