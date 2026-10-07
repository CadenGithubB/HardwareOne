// G2_HoldGesture.h — dependency-free long-press ("hold") vocabulary, press/
// release pairing, firmware profiles, route policy and dispatcher bookkeeping
// for the G2 Even Hub page events LONG_PRESS(9) / LONG_PRESS_RELEASE(10).
//
// Kept free of Arduino, ESP-IDF and FreeRTOS so the host tests
// (test/host/test_g2_hold_gesture.cpp) compile this exact header. The owner
// (G2_Glasses.cpp) serialises every G2HoldPairing call under its own portMUX,
// supplies the clock, and performs whatever the verdict or route asks for;
// nothing here does I/O, allocates or blocks.
//
// Firmware facts this encodes:
//   * 9/10 reach only the foreground Even Hub page (a HW1 page), from the
//     master (right) arm. 2.2.9.22 (static analysis) sends eventSource 0, so
//     ring and temple holds look alike; 2.3.2.14 (bench, 2026-10-05) sends
//     1 = right temple, 2 = ring, 3 = left temple, one Sys copy per edge.
//   * 10 follows only a 9 the same page saw; a Hub DISPLAY_EXIT drops it, so
//     an open press must also close on exits, the end of its widget
//     lifecycle (terminal loss, BLE disconnect, ...) or a timeout.
//   * A firmware overlay (OS menu, Even AI, a notification) covering the page
//     also ends an open press (R11). HW1's overlay model, from hardware:
//     FG_ENTER(4) raises it and FG_EXIT(5) dismisses it.
//   * The pairing never compares presentation epochs: a page swap mid-hold
//     keeps the firmware's press flag, and the release must still pair. The
//     swap's own Shutdown echo (reason-1 SYSTEM_EXIT) closes nothing.
//   * HW1 only consumes 9/10 delivered to its own page. It never writes the
//     glasses' long-press table, and never emulates, answers or suppresses a
//     native QuickNote/shortcut binding (2.3.2-era firmware may fire one).
#ifndef G2_HOLD_GESTURE_H
#define G2_HOLD_GESTURE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Phase values equal the wire eventType; Abort is HW1's implicit release.
enum class G2HoldPhase  : uint8_t { Abort = 0, Press = 9, Release = 10 };
// = SDK EventSourceType. Unknown on 2.2.9.22; filled on 2.3.2.14.
enum class G2HoldSource : uint8_t { Unknown = 0, RightTemple = 1, Ring = 2, LeftTemple = 3 };
// DevEvent sub-message field that carried the copy; Sim = `g2hold sim`.
enum class G2HoldVia    : uint8_t { List = 1, Text = 2, Sys = 3, Sim = 4 };
enum class G2HoldVerdict: uint8_t { Press, Release, DupPress, DupRelease, OrphanRelease,
                                    PressAfterLostRelease, Aborted, NothingOpen };

// Why a hold copy never reached pairing or a consumer, or why an open press
// was closed without a release. Recorded next to the verdict; never text.
enum class G2HoldReason : uint8_t {
  None = 0,
  // Route gates (g2HoldRouteDecide order) plus the pre-pairing freshness gate.
  Unprofiled, Overlay, NoPage, Stale, Keyboard, Camera, ImageProbe,
  SwapInFlight, ActionOff, Source,
  // Dispatch outcomes: the global consumer declined the press; the page that
  // consumed a press was gone at its release; the dispatcher queue was full.
  Refused, PageGone, QueueFull,
  // Implicit releases (R10 / R14). Overlay (above) also closes an open press:
  // the firmware covered the page (FG_ENTER, or a SyncInfo-only rise).
  // Lifecycle = any other widget lifecycle boundary (BLE disconnect, closeg2,
  // topology change, a fresh Blocks start).
  // LostRelease = a later press (or press delivery) found this one still open.
  FgExit, AbnormalExit, SystemExit, Terminal, Lifecycle, Expired,
  LostRelease, Sim,
};

// Who consumed a delivered press. Its release or abort goes to that same
// consumer, never to whatever is current when the finger lifts.
enum class G2HoldConsumer : uint8_t { None = 0, Page = 1, Global = 2 };

// A second copy of the same edge (mirrored arm, or the Sys + Text + List
// copies of one hold) lands inside this window; a real re-press cannot,
// because the firmware's hold threshold alone is 500-800 ms.
constexpr uint32_t kG2HoldDupWindowMs = 400;

struct G2HoldPairing {             // caller serialises (portMUX in G2_Glasses.cpp)
  bool     open = false;
  uint32_t pressId = 0;            // the open press, or the latest once closed
  uint32_t pressAtMs = 0;
  uint8_t  source = 0;             // G2HoldSource of the open/latest press
  char     side = 0;               // arm that delivered the admitted press
  uint32_t lastReleaseAtMs = 0;
  uint32_t lastPressId = 0;        // most recently closed press (any reason)
  bool     lastClosedByRelease = false;  // gates DupRelease on lastReleaseAtMs

  // Press while open inside the window is a duplicate (a nonzero src upgrades
  // an unknown source). Later than that, the release was lost: the old press
  // closes (lastPressId) and a new one opens.
  G2HoldVerdict onPress(char pressSide, uint8_t src, uint32_t nowMs) {
    if (open) {
      if ((uint32_t)(nowMs - pressAtMs) < kG2HoldDupWindowMs) {
        if (src != 0 && source == 0) source = src;
        return G2HoldVerdict::DupPress;
      }
      lastPressId = pressId;
      lastClosedByRelease = false;
      begin(pressSide, src, nowMs);
      return G2HoldVerdict::PressAfterLostRelease;
    }
    begin(pressSide, src, nowMs);
    return G2HoldVerdict::Press;
  }

  // A release is honoured only for the open press, from either arm. With
  // nothing open, a copy inside the window of the last release is its
  // duplicate; anything else is an orphan (its press was dropped or aborted).
  G2HoldVerdict onRelease(char releaseSide, uint8_t src, uint32_t nowMs,
                          uint32_t* heldMs) {
    (void)releaseSide;
    if (heldMs) *heldMs = 0;
    if (open) {
      if (src != 0 && source == 0) source = src;
      if (heldMs) *heldMs = openAgeMs(nowMs);
      open = false;
      lastPressId = pressId;
      lastReleaseAtMs = nowMs;
      lastClosedByRelease = true;
      return G2HoldVerdict::Release;
    }
    if (lastClosedByRelease &&
        (uint32_t)(nowMs - lastReleaseAtMs) < kG2HoldDupWindowMs) {
      return G2HoldVerdict::DupRelease;
    }
    return G2HoldVerdict::OrphanRelease;
  }

  // Implicit release. openMs (optional) receives how long the press was open.
  G2HoldVerdict abort(uint32_t nowMs, uint32_t* openMs = nullptr) {
    if (openMs) *openMs = 0;
    if (!open) return G2HoldVerdict::NothingOpen;
    if (openMs) *openMs = openAgeMs(nowMs);
    open = false;
    lastPressId = pressId;
    lastClosedByRelease = false;
    return G2HoldVerdict::Aborted;
  }

  bool expired(uint32_t nowMs, uint32_t maxOpenMs) const {
    return open && openAgeMs(nowMs) >= maxOpenMs;
  }

  // How long the press has been open. A clock read taken just before another
  // task admitted the press (nowMs "earlier" than pressAtMs) is age 0, never a
  // wrapped ~49-day age; real ages stay correct across the millis() wrap.
  uint32_t openAgeMs(uint32_t nowMs) const {
    const int32_t age = (int32_t)(nowMs - pressAtMs);
    return age > 0 ? (uint32_t)age : 0u;
  }

 private:
  void begin(char pressSide, uint8_t src, uint32_t nowMs) {
    pressId = pressId + 1u;
    if (pressId == 0) pressId = 1;   // zero stays "no press"
    pressAtMs = nowMs;
    source = src;
    side = pressSide;
    open = true;
  }
};

inline bool g2HoldIsEvent(uint32_t etype) { return etype == 9 || etype == 10; }

constexpr size_t kG2HoldVerdictCount = (size_t)G2HoldVerdict::NothingOpen + 1;
constexpr size_t kG2HoldReasonCount = (size_t)G2HoldReason::Sim + 1;

// Name lookups are tables, not switches: the firmware formats them from
// several call sites, and a table stays one copy at any optimisation level.
//
// Event subject vocabulary: "unknown","right","ring","left". Out-of-range
// sources from a future firmware stay "unknown" rather than inventing a name.
inline const char* g2HoldSourceName(uint8_t src) {
  static const char* const kNames[] = {"unknown", "right", "ring", "left"};
  return kNames[src < 4 ? src : 0];
}

inline const char* g2HoldPhaseName(uint8_t phase) {
  return phase == 9 ? "press" : phase == 10 ? "release"
       : phase == 0 ? "abort" : "?";
}

inline const char* g2HoldViaName(uint8_t via) {
  static const char* const kNames[] = {"-", "list", "text", "sys", "sim"};
  return kNames[via < 5 ? via : 0];
}

inline const char* g2HoldVerdictName(G2HoldVerdict v) {
  static const char* const kNames[] = {
    "Press", "Release", "DupPress", "DupRelease", "OrphanRelease",
    "PressAfterLostRelease", "Aborted", "NothingOpen",
  };
  static_assert(sizeof(kNames) / sizeof(kNames[0]) == kG2HoldVerdictCount,
                "one name per G2HoldVerdict");
  return (size_t)v < kG2HoldVerdictCount ? kNames[(size_t)v] : "?";
}

inline const char* g2HoldReasonName(G2HoldReason r) {
  static const char* const kNames[] = {
    "", "unprofiled", "overlay", "no-page", "stale", "keyboard", "camera",
    "image-probe", "swap-in-flight", "action-off", "source",
    "refused", "page-gone", "queue-full",
    "fg-exit", "abnormal-exit", "system-exit", "terminal", "lifecycle",
    "expired", "lost-release", "sim",
  };
  static_assert(sizeof(kNames) / sizeof(kNames[0]) == kG2HoldReasonCount,
                "one name per G2HoldReason");
  return (size_t)r < kG2HoldReasonCount ? kNames[(size_t)r] : "?";
}

// ---------------------------------------------------------------------------
// Firmware profiles (R19): hold actions run only on an exact, profiled pair.
// ---------------------------------------------------------------------------
// Both arms must report exactly the profile's version. Anything else, a
// mixed pair, or a version not known yet is unprofiled: holds are decoded,
// recorded and posted as events, but nothing is dispatched (unless the admin
// testing override g2holdunprofiled is on). Add a row only after the hold
// probe ran on that exact pair.
struct G2HoldProfile {
  const char* firmware;        // exact version string on both arms
  bool        sourceReliable;  // 9/10 carry eventSource (ring vs temple)
};

inline const G2HoldProfile* g2HoldProfileFor(const char* left,
                                             const char* right) {
  static constexpr G2HoldProfile kProfiles[] = {
    {"2.2.9.22", false},  // static analysis (spec E-1..E-4): src always 0
    {"2.3.2.14", true},   // bench 2026-10-05: R arm only, src 1/2/3
  };
  if (!left || !right || !left[0]) return nullptr;
  for (const G2HoldProfile& p : kProfiles) {
    if (strcmp(left, p.firmware) == 0 && strcmp(right, p.firmware) == 0) {
      return &p;
    }
  }
  return nullptr;
}

// g2holdsource: 0 = ring only, 1 = ring + temples. Only a profile whose
// firmware fills eventSource can tell them apart; on any other firmware
// (src 0 on 2.2.9.22, or an admin-allowed unprofiled pair) every source is
// accepted, since the ring would otherwise never qualify.
constexpr uint8_t kG2HoldSourceRingOnly = 0;
constexpr uint8_t kG2HoldSourceAny = 1;

inline bool g2HoldSourceAllowed(uint8_t src, bool sourceReliable,
                                uint8_t policy) {
  return !sourceReliable || policy != kG2HoldSourceRingOnly ||
      src == (uint8_t)G2HoldSource::Ring;
}

// ---------------------------------------------------------------------------
// Route policy
// ---------------------------------------------------------------------------
enum class G2HoldRoute : uint8_t { Drop, Page, Global };

// Everything a press needs to be routed, sampled by the caller. action is
// the g2holdaction setting (0 = Off, nonzero = the global consumer).
struct G2HoldGateInputs {
  bool    trusted = false;          // exact firmware profile (R19)
  bool    allowUnprofiled = false;  // admin probing override (R19)
  bool    hijackActive = false;     // a HW1 page owns the lens (R6)
  bool    presentationCurrent = false;  // press epoch == live epoch (R6)
  bool    overlay = false;          // firmware overlay or its post-exit grace (R11)
  bool    keyboard = false;         // text entry owns input (R11)
  bool    camStream = false;        // camera stream page (R11)
  bool    imageProbe = false;       // image-probe hold (R11)
  bool    swapInFlight = false;     // page swap in flight (R11)
  bool    pageHasHandler = false;   // current page registered a hold handler
  bool    sourceAllowed = false;    // g2HoldSourceAllowed() for this press
  uint8_t action = 0;
};

// Pure press routing. The first failing gate names the drop; a page that
// handles holds wins over the global action (pages apply the source policy
// themselves); action 0 drops "action-off", then the source policy drops
// "source". dropReason (optional) is "" unless the route is Drop; reasonOut
// likewise.
inline G2HoldRoute g2HoldRouteDecide(const G2HoldGateInputs& in,
                                     const char** dropReason,
                                     G2HoldReason* reasonOut = nullptr) {
  G2HoldReason reason = G2HoldReason::None;
  G2HoldRoute route = G2HoldRoute::Drop;
  if (!in.trusted && !in.allowUnprofiled) reason = G2HoldReason::Unprofiled;
  else if (in.overlay)                    reason = G2HoldReason::Overlay;
  else if (!in.hijackActive)              reason = G2HoldReason::NoPage;
  else if (!in.presentationCurrent)       reason = G2HoldReason::Stale;
  else if (in.keyboard)                   reason = G2HoldReason::Keyboard;
  else if (in.camStream)                  reason = G2HoldReason::Camera;
  else if (in.imageProbe)                 reason = G2HoldReason::ImageProbe;
  else if (in.swapInFlight)               reason = G2HoldReason::SwapInFlight;
  else if (in.pageHasHandler)             route = G2HoldRoute::Page;
  else if (in.action == 0)                reason = G2HoldReason::ActionOff;
  else if (!in.sourceAllowed)             reason = G2HoldReason::Source;
  else                                    route = G2HoldRoute::Global;
  if (dropReason) *dropReason = g2HoldReasonName(reason);
  if (reasonOut) *reasonOut = reason;
  return route;
}

// ---------------------------------------------------------------------------
// Dispatcher (g2_tap_disp) side
// ---------------------------------------------------------------------------
// One queued hold job: the tap-dispatch payload (16 B, inside its 36 B union).
// phase 9 / 10 / 0 (abort). reason = the ingest gate that dropped a press
// (None = route it), or why an abort happened.
struct G2HoldJob {
  uint8_t  phase;
  uint8_t  source;
  uint8_t  via;
  uint8_t  reason;
  char     side;
  uint32_t pressId;
  uint32_t heldMs;                 // release: held time; abort: time open
};

// The consumer of one delivered press.
struct G2HoldOwner {
  uint32_t       pressId = 0;
  G2HoldConsumer consumer = G2HoldConsumer::None;
  uint8_t        page = 0;         // G2HijackPage, when consumer == Page
  uint8_t        source = 0;       // G2HoldSource the consumer was given
};

// Which consumer owns which press, so a release or abort reaches exactly the
// consumer of its own press. Used only by the dispatcher task, so it needs
// no lock. Jobs from one producer task stay in order, but an abort raised by
// another task (main-loop expiry, terminal loss, `g2hold sim`) can overtake a
// press still being queued: closedId remembers the newest ended press, so a
// press that arrives after its own end is never delivered. Press ids grow
// monotonically (wrap-safe compare).
struct G2HoldDelivery {
  G2HoldOwner owner;               // consumer of the newest delivered press
  uint32_t    seenId = 0;          // newest press that reached beginPress
  uint32_t    closedId = 0;        // newest press whose end was processed

  // A press arrives. Returns false if it already ended (its own end, or a
  // newer press, got here first: only one press is ever open), in which case
  // nothing is delivered. Otherwise *superseded receives (and this forgets)
  // the owner of an older press whose end never arrived; the caller aborts it.
  bool beginPress(uint32_t id, G2HoldOwner* superseded) {
    *superseded = G2HoldOwner();
    if (seenId != 0 && !newer(id, seenId)) return false;
    seenId = id;
    if (owner.consumer != G2HoldConsumer::None) {
      *superseded = owner;
      owner = G2HoldOwner();
    }
    return closedId == 0 || newer(id, closedId);
  }

  void consume(uint32_t id, G2HoldConsumer consumer, uint8_t page,
               uint8_t source) {
    owner.pressId = id;
    owner.consumer = consumer;
    owner.page = page;
    owner.source = source;
  }

  // The release or abort of press id. Returns that press's owner (consumer
  // None if nobody consumed it) and forgets it. *seen (optional) is true if
  // the press itself reached the dispatcher, i.e. its end is worth an event.
  G2HoldOwner end(uint32_t id, bool* seen = nullptr) {
    if (seen) *seen = seenId == id;
    if (closedId == 0 || newer(id, closedId)) closedId = id;
    G2HoldOwner ended;
    if (owner.consumer != G2HoldConsumer::None && owner.pressId == id) {
      ended = owner;
      owner = G2HoldOwner();
    }
    return ended;
  }

  static bool newer(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }
};

// Event rate limit: at most two hold events (a press and its release) in any
// 1 s window. Dispatcher task only.
struct G2HoldEventRate {
  uint32_t at[2] = {0, 0};         // the two most recent admitted posts
  uint8_t  next = 0;               // slot of the older of the two once filled
  uint8_t  filled = 0;

  bool admit(uint32_t nowMs) {
    if (filled == 2 && (uint32_t)(nowMs - at[next]) < 1000u) return false;
    at[next] = nowMs;
    next = (uint8_t)(next ^ 1u);
    if (filled < 2) filled++;
    return true;
  }
};

// Page hold handlers keyed by page id (not registry slot). Written at
// startup, read by the dispatcher; pointer-sized atomics keep a re-register
// during a later init coherent. Out-of-range ids are refused, never wrapped.
template <typename Fn, size_t N>
struct G2HoldPageTable {
  Fn fns[N] = {};

  bool set(uint32_t page, Fn fn) {
    if (page >= N) return false;
    __atomic_store_n(&fns[page], fn, __ATOMIC_RELEASE);
    return true;
  }
  Fn get(uint32_t page) const {
    return page < N ? __atomic_load_n(&fns[page], __ATOMIC_ACQUIRE) : nullptr;
  }
};

#endif  // G2_HOLD_GESTURE_H
