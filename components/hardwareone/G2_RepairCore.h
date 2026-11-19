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
// A try reconnects by the missing temple's saved address with a direct connect
// (kDirectConnectTimeoutMs); it scans only when that side has no saved
// address, and that scan gives way to a waiting ring or phone setup.
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
// A try deferred for another client's setup (ring, phone) runs again after
// this, without using up a try.
constexpr uint32_t kDeferMs = 2000;

// --- Arm liveness on stock G2 ---------------------------------------------
// RIGHT is the master temple: heartbeat replies, events and EvenHub replies
// come from it. LEFT sends only mic audio and acks for traffic written to it,
// so its E0 silence is normal and never counts toward pluginDead. LEFT health
// is the link (no real disconnect) plus the TX-wedge rule (five failed writes).
// RIGHT silence still counts: on stock the mic stops when its plugin task dies.
constexpr bool rendererSilenceCounts(char side) { return side == 'R'; }

// --- Reconnect path -------------------------------------------------------
// Bluedroid cancels a direct connect after CONFIG_BT_BLE_ESTAB_LINK_CONN_TOUT
// (30 s on our builds). The repair's own bound stays under it, so our wait,
// not the host's timer, ends a try for an absent temple.
constexpr uint32_t kHostEstablishTimeoutMs = 30000;
constexpr uint32_t kDirectConnectTimeoutMs = 25000;
static_assert(kDirectConnectTimeoutMs < kHostEstablishTimeoutMs,
              "the repair's direct connect must end before Bluedroid's own timer");
// A direct connect that fails this fast was refused, not missed: the address
// is busy (the controller may still hold the link). A miss takes the full bound.
constexpr uint32_t kDirectBusyMs = 3000;
// The fallback scan: interval 100 ms, window 30 ms (about 30% duty), in the
// milliseconds BLEScan takes, and in 0.625 ms units for the coex record.
constexpr uint16_t kScanIntervalMs = 100;
constexpr uint16_t kScanWindowMs = 30;
constexpr uint16_t kScanIntervalUnits = 160;
constexpr uint16_t kScanWindowUnits = 48;
constexpr uint8_t kScanSeconds = 5;

// HCI 0x02 (Unknown Connection Identifier) on a drop: the host released a link
// the controller still owns (a failed connect's handle-0 completion, see
// docs). It is not a normal disconnect, and the arm does not advertise.
constexpr uint16_t kHostDesyncReason = 0x02;
constexpr bool isHostDesyncReason(uint16_t reason) { return reason == kHostDesyncReason; }

enum class TryPath : uint8_t {
  Direct,   // connect to the saved address, no scan
  Scan,     // no saved address: a 30% duty scan, then connect to what it found
  Defer,    // no saved address and another client's setup is waiting: run it first
  BackOff,  // no saved address, but the drop was a host desync: no scan this try
};
// One try's path. hostDesync is consumed by the try that reads it.
constexpr TryPath tryPath(bool savedAddress, bool hostDesync, bool otherSetupWaiting) {
  return savedAddress ? TryPath::Direct
       : hostDesync ? TryPath::BackOff
       : otherSetupWaiting ? TryPath::Defer
       : TryPath::Scan;
}
// A failed direct connect that ended before kDirectBusyMs: the address is busy.
constexpr bool directRefusedBusy(bool ok, uint32_t elapsedMs) {
  return !ok && elapsedMs < kDirectBusyMs;
}
// The found-but-refused streak (escalates to a host recycle): a scan-found
// temple whose open failed, or a direct connect refused at once. A direct
// connect that ran its full bound only says the temple is away.
constexpr bool countsTowardOpenFailStreak(bool direct, uint32_t elapsedMs) {
  return !direct || elapsedMs < kDirectBusyMs;
}
// An OPEN wait that our own bound ended before the host's establishment timer
// is an ordinary miss (the unregister cancels the pending open); only a wait
// longer than the host's timer means the OPEN event was lost. Takes the
// caller's connect bound (0 = unknown, read as a lost OPEN), not the elapsed
// time, which also counts the APP_REGISTER wait (up to 10 s).
constexpr bool openTimeoutWasOwnBound(uint32_t connectBoundMs) {
  return connectBoundMs != 0 && connectBoundMs < kHostEstablishTimeoutMs;
}
// The saved address type, or, when it was not saved, a guess from the address:
// top two bits 0b11 is a random static address.
constexpr uint8_t savedAddressType(bool known, uint8_t type, unsigned msb) {
  return known ? type : ((msb & 0xC0u) == 0xC0u ? uint8_t(1) : uint8_t(0));
}

enum class Phase : uint8_t { Off, Fast, Slow, Held };

struct Schedule {
  bool active = false;        // exactly one temple up: an episode is running
  bool queued = false;        // a try is on the setup worker
  bool held = false;          // the last queue attempt was refused by the user's intent
  bool slowNotice = false;    // the fast tries just ran out; the caller reports it once
  bool restartNotice = false; // a wear edge or gesture restarted the fast tries; report once
  bool gestureRestarted = false; // a gesture restarted the fast tries this episode
  bool deferNext = false;     // the running try gave way to another setup (deferred())
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
  void unclaim() { queued = deferNext = false; }
  void refused(bool byIntent, uint32_t now) {
    if (queued) return;  // another caller's try won the claim and is running
    held = byIntent;
    nextMs = now + kRequeueMs;
  }
  // Each heartbeat: whether the user's intent or the saved target holds the
  // repair now, also while a try runs (that try then ends at its intent fence).
  void observeIntent(bool heldNow) { held = heldNow; }

  // The running try gives way to another client's setup: its finish does not
  // use up a try and asks again after kDeferMs.
  void deferred() {
    if (queued) deferNext = true;
  }
  // A try ended. half: exactly one temple is still up.
  void finished(bool ok, bool half, uint32_t now) {
    queued = false;
    const bool gaveWay = deferNext;
    deferNext = false;
    if (ok || !half) {
      reset();
      return;
    }
    if (gaveWay) {
      nextMs = now + kDeferMs;
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
