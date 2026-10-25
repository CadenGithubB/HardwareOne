#pragma once
// The G2 one-temple repair schedule. While exactly one temple is up, the
// glasses setup job retries the missing side: seven fast tries 5, 10, 30, 60,
// 120, 240 and 300 s apart (about 13 minutes), then one try every 10 minutes
// for as long as the pair stays half-connected. The surviving temple being put
// back on (the wear edge) or a lens gesture sends a slow repair back to the
// first fast try; gestures do so at most once per 30 minutes, so steady lens
// use cannot keep the radio scanning. Both temples up or both down ends the
// episode, except while a try runs: the repair's own connect shows both
// temples up until its session is promoted or refused, so the try's result
// decides. The user's intent (a disconnect, owner authority, the surviving
// temple's saved address) is observed each heartbeat and shown as held.
// No OS dependencies: the caller holds its own lock around every call and
// passes the clock, so the host test runs this file as is.
#include <cstddef>
#include <cstdint>

namespace g2repair {
constexpr uint32_t kFastBackoffMs[] = {5000, 10000, 30000, 60000, 120000, 240000, 300000};
constexpr uint8_t kFastTries = sizeof(kFastBackoffMs) / sizeof(kFastBackoffMs[0]);
constexpr uint32_t kSlowRetryMs = 600000;
// A try that could not be queued (worker busy, or held) is asked again after this.
constexpr uint32_t kRequeueMs = 1000;
constexpr uint32_t kGestureRestartCooldownMs = 30u * 60u * 1000u;
static_assert(kFastTries < 10, "describe() prints one digit");

enum class Phase : uint8_t { Off, Fast, Slow, Held };

struct Schedule {
  bool active = false;        // exactly one temple up: an episode is running
  bool queued = false;        // a try is on the setup worker
  bool held = false;          // the last queue attempt was refused by the user's intent
  bool slowNotice = false;    // the fast tries just ran out; the caller reports it once
  bool restartNotice = false; // a wear edge or gesture restarted the fast tries; report once
  bool gestureRestarted = false; // a gesture restarted the fast tries this episode
  uint8_t failed = 0;         // failed tries this episode, saturating at kFastTries (slow)
  uint32_t nextMs = 0;        // when the next try is due, while active
  uint32_t gestureRestartMs = 0; // when, for the cooldown

  void reset() { *this = Schedule{}; }
  // The episode ended (both temples up or down, or the client is off). While
  // a try is on the worker its result decides instead (finished()).
  void end() {
    if (!queued) reset();
  }
  // A new episode (or g2recover) starting at fast try 1 after delayMs. A try
  // already on the worker stays queued; its result counts as that try.
  void restart(uint32_t now, uint32_t delayMs) {
    active = true;
    held = slowNotice = restartNotice = false;
    failed = 0;
    nextMs = now + delayMs;
  }
  Phase phase() const {
    if (!active) return Phase::Off;
    if (held) return Phase::Held;
    return failed < kFastTries ? Phase::Fast : Phase::Slow;
  }
  // The next (or running) fast try, 1..kFastTries; 0 when not in the fast phase.
  uint8_t fastTry() const { return active && failed < kFastTries ? uint8_t(failed + 1) : 0; }

  // Each heartbeat while exactly one temple is up. True when a try is due.
  bool due(uint32_t now) {
    if (queued) return false;
    if (!active) {
      restart(now, kFastBackoffMs[0]);
      return false;
    }
    return int32_t(now - nextMs) >= 0;
  }
  // Queue admission: claim() before the try goes on the worker (false while
  // one is already there), unclaim() if it then could not. A refused attempt
  // is reported with refused(): busy (false) or held by the user's intent (true).
  bool claim() {
    if (queued) return false;
    queued = true;
    return true;
  }
  void unclaim() { queued = false; }
  void refused(bool byIntent, uint32_t now) {
    if (queued) return;  // another caller's try won the claim and is running
    held = byIntent;
    nextMs = now + kRequeueMs;
  }
  // Each heartbeat: whether the user's intent or the saved target holds the
  // repair now, also while a try runs (that try then ends at its intent fence).
  void observeIntent(bool heldNow) { held = heldNow; }

  // A try ended. half: exactly one temple is still up.
  void finished(bool ok, bool half, uint32_t now) {
    queued = false;
    if (ok || !half) {
      reset();
      return;
    }
    if (failed < kFastTries) {
      ++failed;
      if (failed == kFastTries) slowNotice = true;
    }
    nextMs = now + (failed < kFastTries ? kFastBackoffMs[failed] : kSlowRetryMs);
  }

  // The surviving temple was put back on (gesture false), or the lens saw a
  // gesture. A slow repair goes back to fast try 1; a running slow try counts
  // as that try. Gestures restart at most once per kGestureRestartCooldownMs
  // in an episode. True when it restarted.
  bool nudge(uint32_t now, bool gesture) {
    if (!active || held || failed < kFastTries) return false;
    if (gesture) {
      if (gestureRestarted && uint32_t(now - gestureRestartMs) < kGestureRestartCooldownMs) return false;
      gestureRestarted = true;
      gestureRestartMs = now;
    }
    failed = 0;
    slowNotice = false;
    restartNotice = true;
    if (!queued) nextMs = now + kFastBackoffMs[0];
    return true;
  }
};

// "none", "held", "slow" or "n/7" for status lines.
inline const char* describe(const Schedule& s, char (&buf)[8]) {
  switch (s.phase()) {
    case Phase::Off: return "none";
    case Phase::Held: return "held";
    case Phase::Slow: return "slow";
    case Phase::Fast: break;
  }
  buf[0] = char('0' + s.fastTry());
  buf[1] = '/';
  buf[2] = char('0' + kFastTries);
  buf[3] = '\0';
  return buf;
}
}  // namespace g2repair
