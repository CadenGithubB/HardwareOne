// System_SdWatchCore.h - when to mount an SD card on its own (UP-06). Pure:
// no Arduino, no ESP-IDF, no allocation; the host test runs it as it is.
//
// On a board with a card-detect switch the firmware used to mount the card at
// boot and on `sdmount` only: a card put in later, or pulled and put back,
// stayed unusable until someone typed the command. This is the policy that
// mounts it. System_VFS.cpp feeds it the switch and the mount state and does
// what it asks (sdWatchTick, on the main loop; the mount itself runs on
// cmd_exec, where `sdmount` runs).
//
// Rules:
//  - The switch is sampled every 500 ms. A sample counts as "present" only if
//    the switch read present at every look since the sample before it (the
//    loop looks on every pass), so a bouncing contact never counts.
//  - Three present samples in a row (the card has been in, undisturbed, for at
//    least 1 s; the mount comes 1 to 2 s after it was put in) while no usable
//    mount exists ask for one mount.
//  - A failed mount is tried again after 5 s, 30 s and 5 min, then no more
//    until the card is taken out and put back, or `sdmount` is run. A card
//    that boot could not mount counts as the first failed attempt, so boot is
//    unchanged and an unreadable card is tried three more times, not forever.
//  - The slot counts as empty only when the switch has read open at every
//    look for 100 ms (two looks at least). A contact that opens for a moment
//    (a knock, a worn device) restarts the one-second insertion count and
//    nothing else: it does not report a removal, end the `sdunmount` hold or
//    start the backoff over. (The driver's own latch is unchanged: a card
//    access that finds the switch open still marks the mount stale, and the
//    watch then reports that and mounts again.)
//  - `sdunmount` holds the watch until `sdmount`, `sdformat`, a restart, or
//    the slot is empty: a card put in after that is a new insertion.
//  - While the card is in use (a recording, a transcription, a registered
//    owner) no mount is started and the backoff does not advance.
//  - A removal is reported once per mount (the caller posts the existing
//    SD-unmounted event), whether the switch or a failed access showed it.
#ifndef SYSTEM_SD_WATCH_CORE_H
#define SYSTEM_SD_WATCH_CORE_H

#include <stdint.h>

namespace sdwatch {

constexpr uint32_t kSampleMs        = 500;
constexpr uint8_t  kPresentSamples  = 3;       // 1 s of an undisturbed switch
constexpr uint32_t kAbsentHoldMs    = 100;     // the switch open this long at every look: the slot is empty
constexpr uint8_t  kAbsentLooks     = 2;       // and at this many looks in a row at least
constexpr uint8_t  kBackoffSteps    = 3;
constexpr uint32_t kBackoffMs[kBackoffSteps] = {5000, 30000, 300000};
constexpr uint32_t kBusyRetryMs     = 2000;    // the card was in use: look again, no backoff step

struct State {
  uint32_t lastSampleMs;
  uint32_t retryAtMs;     // valid while waitRetry
  uint32_t absentSinceMs; // the first of the open looks in a row (valid while absentLooks != 0)
  uint8_t  presentRun;    // present samples in a row
  uint8_t  failures;      // failed mounts since the card last left the slot
  uint8_t  absentLooks;   // looks in a row that read the switch open (saturates)
  bool     sampled;       // the first sample was taken
  bool     sawAbsent;     // the switch read absent at some look since the last sample
  bool     waitRetry;     // no mount before retryAtMs
  bool     userUnmounted; // `sdunmount` holds the watch
  bool     removalNoted;  // this mount's removal was reported
  bool     mountPending;  // a mount was asked for and its result is not in
  bool     gaveUp;        // every backoff step failed
};

struct Input {
  uint32_t nowMs;
  bool     pinPresent;      // the switch reads "card in" right now
  bool     mounted;         // a mount is registered (even a stale one)
  bool     latchedRemoved;  // the driver saw the card leave this mount
};

struct Output {
  bool noteRemoved;   // latch the driver and report the removal (once)
  bool requestMount;  // start one mount
};

// Every pass of the loop: remember a switch that read absent between samples,
// and since when it has read absent without a break.
inline void look(State& s, bool pinPresent, uint32_t nowMs) {
  if (pinPresent) {
    s.absentLooks = 0;
    return;
  }
  s.sawAbsent = true;
  if (s.absentLooks == 0) s.absentSinceMs = nowMs;
  if (s.absentLooks < 255) ++s.absentLooks;
}

// The slot is empty: the switch read open at the last looks without a break,
// for kAbsentHoldMs or longer. One open read, or a short run of them, is a
// contact bounce and not a card that left.
constexpr bool slotEmpty(const State& s, uint32_t nowMs) {
  return s.absentLooks >= kAbsentLooks && static_cast<uint32_t>(nowMs - s.absentSinceMs) >= kAbsentHoldMs;
}

constexpr bool sampleDue(const State& s, uint32_t nowMs) {
  return !s.sampled || static_cast<uint32_t>(nowMs - s.lastSampleMs) >= kSampleMs;
}

inline Output sample(State& s, const Input& in) {
  Output out{false, false};
  const bool first = !s.sampled;
  const bool present = in.pinPresent && !s.sawAbsent;
  const bool empty = !in.pinPresent && slotEmpty(s, in.nowMs);
  s.sampled = true;
  s.sawAbsent = false;
  s.lastSampleMs = in.nowMs;

  // A mount whose card left: report it once, however it was noticed.
  if (in.mounted && (empty || in.latchedRemoved) && !s.removalNoted) {
    s.removalNoted = true;
    out.noteRemoved = true;
  }
  if (!present) {
    // The switch was open at some look: the insertion count starts over.
    s.presentRun = 0;
    if (empty) {
      // The slot is empty: whatever comes next is a new insertion.
      s.failures = 0;
      s.waitRetry = false;
      s.gaveUp = false;
      s.userUnmounted = false;
    }
    return out;
  }
  if (s.presentRun < 255) ++s.presentRun;
  if (in.mounted && !in.latchedRemoved) {
    // A usable mount.
    s.removalNoted = false;
    s.failures = 0;
    s.waitRetry = false;
    s.gaveUp = false;
    return out;
  }
  if (first && !in.mounted) {
    // A card is in and boot did not mount it: that was attempt one.
    s.failures = 1;
    s.waitRetry = true;
    s.retryAtMs = in.nowMs + kBackoffMs[0];
    return out;
  }
  if (s.userUnmounted || s.mountPending || s.gaveUp) return out;
  if (s.presentRun < kPresentSamples) return out;
  if (s.waitRetry && static_cast<int32_t>(in.nowMs - s.retryAtMs) < 0) return out;
  s.mountPending = true;
  out.requestMount = true;
  return out;
}

// The mount that was asked for may still run (asked again where it runs: the
// user may have typed `sdunmount`, or the card may have left, in between).
constexpr bool mountStillWanted(const State& s) {
  return s.mountPending && !s.userUnmounted;
}

// The mount ran.
inline void mountResult(State& s, bool ok, uint32_t nowMs) {
  s.mountPending = false;
  if (ok) {
    s.failures = 0;
    s.waitRetry = false;
    s.gaveUp = false;
    s.removalNoted = false;
    return;
  }
  if (s.failures < 255) ++s.failures;
  if (s.failures > kBackoffSteps) {
    s.gaveUp = true;
    s.waitRetry = false;
    return;
  }
  s.waitRetry = true;
  s.retryAtMs = nowMs + kBackoffMs[s.failures - 1];
}

// The mount did not run (the card is in use, a restart is under way, the job
// could not be queued): look again soon, without a backoff step.
inline void mountSkipped(State& s, uint32_t nowMs) {
  s.mountPending = false;
  if (!s.waitRetry || static_cast<int32_t>(nowMs + kBusyRetryMs - s.retryAtMs) > 0) {
    s.waitRetry = true;
    s.retryAtMs = nowMs + kBusyRetryMs;
  }
}

// `sdunmount` released a mount. Returns true when this mount's removal was
// already reported (the card was pulled first), so the caller does not report
// the unmount a second time. An empty slot has nothing to hold: the command
// only released a stale mount, and the next card is a new insertion.
inline bool userUnmounted(State& s, uint32_t nowMs) {
  const bool alreadyReported = s.removalNoted;
  s.userUnmounted = !slotEmpty(s, nowMs);
  s.removalNoted = false;
  s.mountPending = false;
  return alreadyReported;
}

// `sdmount` or `sdformat` ran: the hold ends and the backoff starts over. A
// failed `sdmount` counts as a failed attempt (the watch tries again after 5 s).
inline void userMounted(State& s, bool ok, uint32_t nowMs) {
  s.userUnmounted = false;
  s.gaveUp = false;
  s.failures = 0;
  s.waitRetry = false;
  mountResult(s, ok, nowMs);
}

// For `sddiag`.
enum class Phase : uint8_t { Watching, Held, Waiting, GaveUp, Mounting };
constexpr Phase phase(const State& s) {
  return s.userUnmounted ? Phase::Held
       : s.mountPending  ? Phase::Mounting
       : s.gaveUp        ? Phase::GaveUp
       : s.waitRetry     ? Phase::Waiting
                         : Phase::Watching;
}

}  // namespace sdwatch

#endif  // SYSTEM_SD_WATCH_CORE_H
