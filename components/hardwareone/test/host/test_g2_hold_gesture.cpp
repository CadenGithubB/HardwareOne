// Host-side tests for the exact production G2 hold (long-press) pairing,
// firmware profiles, source and route policy, and dispatcher bookkeeping in
// G2_HoldGesture.h.
#include <cstdio>
#include <cstring>

#include "../../G2_HoldGesture.h"

static int failures = 0;

#define CHECK(condition, message)                                            \
  do {                                                                       \
    if (!(condition)) {                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, message); \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static bool same(const char* a, const char* b) {
  return a && b && std::strcmp(a, b) == 0;
}

static void test_press_then_release() {
  G2HoldPairing p;
  uint32_t held = 99;
  CHECK(p.onPress('R', 0, 1000) == G2HoldVerdict::Press,
        "a first press must be admitted");
  CHECK(p.open && p.pressId == 1 && p.side == 'R' && p.source == 0,
        "the admitted press must be open with id 1 on its arm");
  CHECK(p.onRelease('R', 0, 3000, &held) == G2HoldVerdict::Release,
        "the release must pair with the open press");
  CHECK(held == 2000, "held time is release minus press");
  CHECK(!p.open && p.lastPressId == 1, "the released press must close");
  CHECK(p.onPress('R', 0, 9000) == G2HoldVerdict::Press && p.pressId == 2,
        "the next hold must get a fresh id");
}

static void test_mirrored_arms_are_duplicates() {
  G2HoldPairing p;
  uint32_t held = 0;
  CHECK(p.onPress('R', 0, 1000) == G2HoldVerdict::Press, "R press");
  CHECK(p.onPress('L', 0, 1120) == G2HoldVerdict::DupPress,
        "an L mirror inside 400 ms must be a duplicate press");
  CHECK(p.open && p.pressId == 1 && p.side == 'R',
        "a duplicate must not replace the admitted press");
  CHECK(p.onRelease('R', 0, 2500, &held) == G2HoldVerdict::Release &&
            held == 1500,
        "R release pairs");
  CHECK(p.onRelease('L', 0, 2610, &held) == G2HoldVerdict::DupRelease,
        "an L mirror release inside 400 ms must be a duplicate release");
  CHECK(held == 0, "a duplicate release carries no held time");
  CHECK(p.onRelease('L', 0, 2900, &held) == G2HoldVerdict::OrphanRelease,
        "a release outside the window with nothing open is an orphan");
}

static void test_release_from_other_arm_pairs() {
  G2HoldPairing p;
  uint32_t held = 0;
  CHECK(p.onPress('R', 0, 0) == G2HoldVerdict::Press, "R press at t=0");
  CHECK(p.onRelease('L', 0, 800, &held) == G2HoldVerdict::Release &&
            held == 800,
        "either arm's release closes the single open press");
}

static void test_sys_and_text_copies() {
  G2HoldPairing p;
  uint32_t held = 0;
  CHECK(p.onPress('R', 0, 5000) == G2HoldVerdict::Press, "Sys copy");
  CHECK(p.onPress('R', 0, 5001) == G2HoldVerdict::DupPress,
        "a same-arm Text copy of the press is a duplicate");
  CHECK(p.onPress('R', 0, 5002) == G2HoldVerdict::DupPress,
        "a same-arm List copy of the press is a duplicate");
  CHECK(p.onRelease('R', 0, 6000, &held) == G2HoldVerdict::Release, "Sys");
  CHECK(p.onRelease('R', 0, 6001, &held) == G2HoldVerdict::DupRelease,
        "a Text copy of the release is a duplicate");
}

static void test_orphan_release() {
  G2HoldPairing p;
  uint32_t held = 7;
  CHECK(p.onRelease('R', 0, 50, &held) == G2HoldVerdict::OrphanRelease,
        "a release with nothing ever opened is an orphan, even near t=0");
  CHECK(held == 0 && !p.open && p.lastPressId == 0,
        "an orphan must not change the pairing");
  CHECK(p.onRelease('R', 0, 60, nullptr) == G2HoldVerdict::OrphanRelease,
        "heldMs is optional");
}

static void test_lost_release() {
  G2HoldPairing p;
  CHECK(p.onPress('R', 0, 1000) == G2HoldVerdict::Press, "first press");
  CHECK(p.onPress('R', 0, 1399) == G2HoldVerdict::DupPress,
        "399 ms is still inside the duplicate window");
  CHECK(p.onPress('R', 0, 3000) == G2HoldVerdict::PressAfterLostRelease,
        "a press 2 s after an unreleased press means the release was lost");
  CHECK(p.open && p.pressId == 2 && p.lastPressId == 1 && p.pressAtMs == 3000,
        "the old press closes and the new one opens");
  uint32_t held = 0;
  CHECK(p.onRelease('R', 0, 3500, &held) == G2HoldVerdict::Release &&
            held == 500 && p.lastPressId == 2,
        "the release pairs with the new press");
}

static void test_window_boundary() {
  G2HoldPairing p;
  CHECK(p.onPress('R', 0, 1000) == G2HoldVerdict::Press, "press");
  CHECK(p.onPress('L', 0, 1400) == G2HoldVerdict::PressAfterLostRelease,
        "exactly 400 ms is outside the duplicate window");
  uint32_t held = 0;
  CHECK(p.onRelease('R', 0, 2000, &held) == G2HoldVerdict::Release, "rel");
  CHECK(p.onRelease('R', 0, 2400, &held) == G2HoldVerdict::OrphanRelease,
        "exactly 400 ms after a release is no longer its duplicate");
}

static void test_source_upgrade() {
  G2HoldPairing p;
  CHECK(p.onPress('R', 0, 100) == G2HoldVerdict::Press && p.source == 0,
        "press with unknown source");
  CHECK(p.onPress('R', 2, 150) == G2HoldVerdict::DupPress && p.source == 2,
        "a duplicate with a nonzero src upgrades the unknown source");
  CHECK(p.onPress('L', 3, 200) == G2HoldVerdict::DupPress && p.source == 2,
        "a known source is never overwritten by a later copy");
  CHECK(p.onPress('R', 1, 900) == G2HoldVerdict::PressAfterLostRelease &&
            p.source == 1,
        "a new press records its own source");
  uint32_t held = 0;
  G2HoldPairing q;
  CHECK(q.onPress('R', 0, 0) == G2HoldVerdict::Press, "q press");
  CHECK(q.onRelease('R', 2, 700, &held) == G2HoldVerdict::Release &&
            q.source == 2,
        "a release with a nonzero src upgrades an unknown source");
  CHECK(q.onPress('R', 0, 5000) == G2HoldVerdict::Press && q.source == 0,
        "the next press does not inherit the previous source");
}

static void test_expiry() {
  G2HoldPairing p;
  CHECK(!p.expired(1000000, 135000), "nothing open never expires");
  CHECK(p.onPress('R', 0, 1000) == G2HoldVerdict::Press, "press");
  CHECK(!p.expired(1000 + 134999, 135000), "just below the limit");
  CHECK(p.expired(1000 + 135000, 135000), "at the limit");
  // millis() wrap: press shortly before the 32-bit wrap.
  G2HoldPairing w;
  CHECK(w.onPress('R', 0, 0xFFFFFF00u) == G2HoldVerdict::Press, "wrap press");
  CHECK(!w.expired(0x00000010u, 135000), "unsigned age survives the wrap");
  CHECK(w.onPress('R', 0, 0x00000020u) == G2HoldVerdict::DupPress,
        "the duplicate window survives the wrap");
  CHECK(w.expired(0xFFFFFF00u + 135000u, 135000),
        "an open press still expires across the wrap");
}

// The main loop reads its clock, then another task admits a press before the
// loop's check runs: the press looks younger than "now". It is age 0, never
// a wrapped ~49-day age that would expire or abort it on the spot.
static void test_press_admitted_after_clock_read() {
  G2HoldPairing p;
  CHECK(p.onPress('R', 0, 50001) == G2HoldVerdict::Press, "racing press");
  CHECK(p.openAgeMs(50000) == 0 && p.openAgeMs(50001) == 0 &&
            p.openAgeMs(50301) == 300,
        "an age read before the press is clamped to 0");
  CHECK(!p.expired(50000, 135000),
        "a press admitted after the clock read must not expire");
  uint32_t openMs = 99;
  CHECK(p.abort(50000, &openMs) == G2HoldVerdict::Aborted && openMs == 0,
        "an intended abort that raced the press records 0 ms, not a wrap");
  CHECK(p.onPress('R', 0, 0x00000005u) == G2HoldVerdict::Press, "wrap press");
  CHECK(!p.expired(0xFFFFFFF0u, 135000) && p.openAgeMs(0xFFFFFFF0u) == 0,
        "a stale read from before the wrap is still age 0");
  uint32_t held = 99;
  CHECK(p.onRelease('R', 0, 0x00000001u, &held) == G2HoldVerdict::Release &&
            held == 0,
        "a release can never report a wrapped held time");
}

static void test_abort() {
  G2HoldPairing p;
  uint32_t openMs = 5;
  CHECK(p.abort(10, &openMs) == G2HoldVerdict::NothingOpen && openMs == 0,
        "aborting with nothing open reports NothingOpen");
  CHECK(p.onPress('R', 0, 1000) == G2HoldVerdict::Press, "press");
  CHECK(p.abort(1700, &openMs) == G2HoldVerdict::Aborted && openMs == 700,
        "aborting an open press reports Aborted and its open time");
  CHECK(!p.open && p.lastPressId == 1, "the aborted press closes");
  CHECK(p.abort(1800) == G2HoldVerdict::NothingOpen,
        "a second abort is a no-op");
  uint32_t held = 3;
  CHECK(p.onRelease('R', 0, 1750, &held) == G2HoldVerdict::OrphanRelease &&
            held == 0,
        "a release after an abort is an orphan, never a duplicate");
  CHECK(p.onPress('R', 0, 1760) == G2HoldVerdict::Press && p.pressId == 2,
        "a press after an abort opens a fresh hold");
}

static void test_pairing_ignores_presentation() {
  // There is no epoch input at all: a page swap mid-hold (which advances the
  // presentation epoch) cannot un-pair a release. Encode the expectation.
  G2HoldPairing p;
  uint32_t held = 0;
  CHECK(p.onPress('R', 0, 0) == G2HoldVerdict::Press, "press on page A");
  CHECK(p.onRelease('R', 0, 4000, &held) == G2HoldVerdict::Release,
        "release after a swap to page B still pairs");
}

static void test_names() {
  CHECK(g2HoldIsEvent(9) && g2HoldIsEvent(10), "9/10 are hold events");
  CHECK(!g2HoldIsEvent(0) && !g2HoldIsEvent(3) && !g2HoldIsEvent(8) &&
            !g2HoldIsEvent(11) && !g2HoldIsEvent(0x109),
        "nothing else is a hold event");
  CHECK(same(g2HoldSourceName(0), "unknown") &&
            same(g2HoldSourceName(1), "right") &&
            same(g2HoldSourceName(2), "ring") &&
            same(g2HoldSourceName(3), "left") &&
            same(g2HoldSourceName(4), "unknown") &&
            same(g2HoldSourceName(255), "unknown"),
        "source names match the event subject vocabulary");
  CHECK(same(g2HoldPhaseName(9), "press") &&
            same(g2HoldPhaseName(10), "release") &&
            same(g2HoldPhaseName(0), "abort"),
        "phase names");
  CHECK(same(g2HoldViaName(1), "list") && same(g2HoldViaName(2), "text") &&
            same(g2HoldViaName(3), "sys") && same(g2HoldViaName(4), "sim") &&
            same(g2HoldViaName(0), "-") && same(g2HoldViaName(5), "-") &&
            same(g2HoldViaName(255), "-"),
        "via names");
  CHECK((uint8_t)G2HoldPhase::Press == 9 &&
            (uint8_t)G2HoldPhase::Release == 10 &&
            (uint8_t)G2HoldPhase::Abort == 0,
        "phase values equal the wire eventType");
  CHECK((uint8_t)G2HoldVia::List == 1 && (uint8_t)G2HoldVia::Text == 2 &&
            (uint8_t)G2HoldVia::Sys == 3 && (uint8_t)G2HoldVia::Sim == 4,
        "via values equal the DevEvent sub-message field; sim follows");
  for (int v = 0; v <= (int)G2HoldVerdict::NothingOpen; ++v) {
    const char* name = g2HoldVerdictName((G2HoldVerdict)v);
    CHECK(name && name[0] && !same(name, "?"), "every verdict is named");
  }
  for (size_t r = 1; r < kG2HoldReasonCount; ++r) {
    const char* name = g2HoldReasonName((G2HoldReason)r);
    CHECK(name && name[0] && !same(name, "?") && std::strlen(name) <= 24,
          "every reason has a short name");
    for (size_t q = 1; q < r; ++q) {
      CHECK(!same(name, g2HoldReasonName((G2HoldReason)q)),
            "reason names are unique");
    }
  }
  CHECK(same(g2HoldReasonName((G2HoldReason)kG2HoldReasonCount), "?"),
        "an out-of-range reason is '?'");
  CHECK(same(g2HoldReasonName(G2HoldReason::None), ""),
        "no reason is the empty string");
  CHECK(same(g2HoldReasonName(G2HoldReason::Terminal), "terminal") &&
            same(g2HoldReasonName(G2HoldReason::Lifecycle), "lifecycle") &&
            same(g2HoldReasonName(G2HoldReason::Expired), "expired") &&
            same(g2HoldReasonName(G2HoldReason::LostRelease), "lost-release") &&
            same(g2HoldReasonName(G2HoldReason::Sim), "sim"),
        "implicit-release reason names");
  CHECK(same(g2HoldReasonName(G2HoldReason::Source), "source") &&
            same(g2HoldReasonName(G2HoldReason::Refused), "refused") &&
            same(g2HoldReasonName(G2HoldReason::PageGone), "page-gone") &&
            same(g2HoldReasonName(G2HoldReason::QueueFull), "queue-full"),
        "dispatch reason names");
}

static G2HoldGateInputs allOpen(uint8_t action) {
  G2HoldGateInputs in;
  in.trusted = true;
  in.hijackActive = true;
  in.presentationCurrent = true;
  in.sourceAllowed = true;
  in.action = action;
  return in;
}

static void expectDrop(const G2HoldGateInputs& in, const char* reason,
                       G2HoldReason code, const char* message) {
  const char* why = nullptr;
  G2HoldReason got = G2HoldReason::None;
  const G2HoldRoute route = g2HoldRouteDecide(in, &why, &got);
  CHECK(route == G2HoldRoute::Drop, message);
  CHECK(same(why, reason), message);
  CHECK(got == code, message);
}

static void test_route_truth_table() {
  const char* why = "unset";
  G2HoldReason got = G2HoldReason::Expired;

  // Happy paths: Page before Global; action selects Global; Off drops.
  G2HoldGateInputs in = allOpen(1);
  CHECK(g2HoldRouteDecide(in, &why, &got) == G2HoldRoute::Global &&
            same(why, "") && got == G2HoldReason::None,
        "trusted, live page, action on → Global");
  in.pageHasHandler = true;
  CHECK(g2HoldRouteDecide(in, &why) == G2HoldRoute::Page && same(why, ""),
        "a page hold handler wins over the global action");
  in = allOpen(0);
  in.pageHasHandler = true;
  CHECK(g2HoldRouteDecide(in, &why) == G2HoldRoute::Page,
        "a page hold handler still consumes with action off");
  expectDrop(allOpen(0), "action-off", G2HoldReason::ActionOff,
             "action off with no page handler → action-off");
  CHECK(g2HoldRouteDecide(allOpen(7), nullptr) == G2HoldRoute::Global,
        "any nonzero action selects the global consumer; reason optional");

  // Source policy: gates only the global action; a page gets every source.
  in = allOpen(1);
  in.sourceAllowed = false;
  expectDrop(in, "source", G2HoldReason::Source,
             "a disallowed source drops 'source' before the global action");
  in.pageHasHandler = true;
  CHECK(g2HoldRouteDecide(in, &why) == G2HoldRoute::Page,
        "a page handler still gets a press the source policy would drop");
  in = allOpen(0);
  in.sourceAllowed = false;
  expectDrop(in, "action-off", G2HoldReason::ActionOff,
             "action off names action-off, not source");

  // Profile gate (R19).
  in = allOpen(1);
  in.trusted = false;
  expectDrop(in, "unprofiled", G2HoldReason::Unprofiled,
             "untrusted firmware drops 'unprofiled'");
  in.allowUnprofiled = true;
  CHECK(g2HoldRouteDecide(in, &why) == G2HoldRoute::Global,
        "allowUnprofiled admits an untrusted pair");
  in = allOpen(1);
  in.allowUnprofiled = true;
  CHECK(g2HoldRouteDecide(in, &why) == G2HoldRoute::Global,
        "allowUnprofiled is harmless on a trusted pair");

  // Each R6 / R11 gate alone.
  struct Row {
    bool G2HoldGateInputs::*field;
    bool value;
    const char* reason;
    G2HoldReason code;
  };
  const Row rows[] = {
    {&G2HoldGateInputs::overlay, true, "overlay", G2HoldReason::Overlay},
    {&G2HoldGateInputs::hijackActive, false, "no-page", G2HoldReason::NoPage},
    {&G2HoldGateInputs::presentationCurrent, false, "stale",
     G2HoldReason::Stale},
    {&G2HoldGateInputs::keyboard, true, "keyboard", G2HoldReason::Keyboard},
    {&G2HoldGateInputs::camStream, true, "camera", G2HoldReason::Camera},
    {&G2HoldGateInputs::imageProbe, true, "image-probe",
     G2HoldReason::ImageProbe},
    {&G2HoldGateInputs::swapInFlight, true, "swap-in-flight",
     G2HoldReason::SwapInFlight},
  };
  for (const Row& row : rows) {
    for (int handler = 0; handler <= 1; ++handler) {
      for (int action = 0; action <= 1; ++action) {
        G2HoldGateInputs r = allOpen((uint8_t)action);
        r.pageHasHandler = handler != 0;
        r.*(row.field) = row.value;
        expectDrop(r, row.reason, row.code,
                   "each gate drops regardless of handler and action");
      }
    }
  }

  // Precedence: the first failing gate names the drop, in the documented
  // order unprofiled → overlay → no-page → stale → keyboard → camera →
  // image-probe → swap-in-flight → (page) → action-off.
  for (size_t first = 0; first < sizeof(rows) / sizeof(rows[0]); ++first) {
    G2HoldGateInputs r = allOpen(0);
    for (size_t k = first; k < sizeof(rows) / sizeof(rows[0]); ++k) {
      r.*(rows[k].field) = rows[k].value;
    }
    expectDrop(r, rows[first].reason, rows[first].code,
               "the earliest failing gate wins");
    r.trusted = false;
    expectDrop(r, "unprofiled", G2HoldReason::Unprofiled,
               "the profile gate precedes every other gate");
  }

  // Exhaustive: every combination of the eleven booleans and both actions
  // routes the same as an independent reference.
  for (uint32_t bits = 0; bits < (1u << 11); ++bits) {
    for (uint8_t action = 0; action <= 1; ++action) {
      G2HoldGateInputs r;
      r.trusted = bits & 1u;
      r.allowUnprofiled = bits & 2u;
      r.hijackActive = bits & 4u;
      r.presentationCurrent = bits & 8u;
      r.overlay = bits & 16u;
      r.keyboard = bits & 32u;
      r.camStream = bits & 64u;
      r.imageProbe = bits & 128u;
      r.swapInFlight = bits & 256u;
      r.pageHasHandler = bits & 512u;
      r.sourceAllowed = bits & 1024u;
      r.action = action;
      const bool blocked = (!r.trusted && !r.allowUnprofiled) || r.overlay ||
          !r.hijackActive || !r.presentationCurrent || r.keyboard ||
          r.camStream || r.imageProbe || r.swapInFlight;
      const G2HoldRoute expect = blocked ? G2HoldRoute::Drop
          : r.pageHasHandler ? G2HoldRoute::Page
          : (action && r.sourceAllowed) ? G2HoldRoute::Global
          : G2HoldRoute::Drop;
      const char* reason = nullptr;
      const G2HoldRoute route = g2HoldRouteDecide(r, &reason);
      CHECK(route == expect, "exhaustive route matches the reference");
      CHECK(reason && ((route == G2HoldRoute::Drop) == (reason[0] != '\0')),
            "a reason is named exactly when the route is Drop");
    }
  }
}

static void test_profiles() {
  const G2HoldProfile* p = g2HoldProfileFor("2.2.9.22", "2.2.9.22");
  CHECK(p && same(p->firmware, "2.2.9.22") && !p->sourceReliable,
        "2.2.9.22 is profiled; its holds carry no source");
  p = g2HoldProfileFor("2.3.2.14", "2.3.2.14");
  CHECK(p && same(p->firmware, "2.3.2.14") && p->sourceReliable,
        "2.3.2.14 is profiled (bench-verified) and reports the source");
  CHECK(!g2HoldProfileFor("2.2.9.22", "2.3.2.14") &&
            !g2HoldProfileFor("2.3.2.14", "2.2.9.22"),
        "a mixed pair is unprofiled");
  CHECK(!g2HoldProfileFor("2.2.10.10", "2.2.10.10") &&
            !g2HoldProfileFor("2.3.0.24", "2.3.0.24") &&
            !g2HoldProfileFor("2.2.6.10", "2.2.6.10"),
        "candidate and older firmware stay unprofiled until probed");
  CHECK(!g2HoldProfileFor("2.2.9.221", "2.2.9.221") &&
            !g2HoldProfileFor("2.2.9.2", "2.2.9.2") &&
            !g2HoldProfileFor("2.3.2.14 ", "2.3.2.14") &&
            !g2HoldProfileFor("v2.3.2.14", "v2.3.2.14"),
        "matching is exact: no prefix, suffix or padding");
  CHECK(!g2HoldProfileFor("", "") && !g2HoldProfileFor("", "2.3.2.14") &&
            !g2HoldProfileFor("2.3.2.14", "") &&
            !g2HoldProfileFor(nullptr, "2.3.2.14") &&
            !g2HoldProfileFor("2.3.2.14", nullptr),
        "an unknown version fails closed");
}

static void test_source_policy() {
  for (uint8_t src = 0; src < 6; ++src) {
    for (uint8_t policy = 0; policy <= 1; ++policy) {
      CHECK(g2HoldSourceAllowed(src, false, policy),
            "without a reliable source every source is admitted");
      const bool expect = policy == kG2HoldSourceAny ||
          src == (uint8_t)G2HoldSource::Ring;
      CHECK(g2HoldSourceAllowed(src, true, policy) == expect,
            "reliable source: ring only, unless temples are allowed");
    }
  }
  CHECK(!g2HoldSourceAllowed((uint8_t)G2HoldSource::Unknown, true,
                             kG2HoldSourceRingOnly),
        "an unknown source on a reliable profile is not the ring");
  CHECK(g2HoldSourceAllowed((uint8_t)G2HoldSource::LeftTemple, true,
                            kG2HoldSourceAny) &&
            g2HoldSourceAllowed((uint8_t)G2HoldSource::RightTemple, true,
                                kG2HoldSourceAny),
        "ring + temples admits both temples");
  CHECK(!g2HoldSourceAllowed((uint8_t)G2HoldSource::LeftTemple, true, 0) &&
            !g2HoldSourceAllowed((uint8_t)G2HoldSource::RightTemple, true, 0),
        "ring only refuses both temples");
}

static void test_job_layout() {
  static_assert(sizeof(G2HoldJob) == 16, "hold job is 16 B");
  static_assert(sizeof(G2HoldJob) <= 36,
                "hold job fits the tap-dispatch payload union");
  static_assert(alignof(G2HoldJob) <= 4, "hold job keeps the entry alignment");
  G2HoldJob job = {};
  CHECK(job.phase == 0 && job.pressId == 0 && job.heldMs == 0,
        "a value-initialised job is all zero");
}

static G2HoldOwner owned(const G2HoldDelivery& d) { return d.owner; }

static void test_delivery_owner() {
  G2HoldDelivery d;
  G2HoldOwner gone;
  bool seen = false;
  CHECK(d.beginPress(1, &gone) && gone.consumer == G2HoldConsumer::None,
        "a first press is live");
  d.consume(1, G2HoldConsumer::Page, 24, 2);
  CHECK(owned(d).pressId == 1 && owned(d).consumer == G2HoldConsumer::Page &&
            owned(d).page == 24 && owned(d).source == 2,
        "the consumer is recorded with its page and source");
  G2HoldOwner end = d.end(1, &seen);
  CHECK(seen && end.consumer == G2HoldConsumer::Page && end.page == 24 &&
            end.pressId == 1,
        "the release reaches the consumer of its press");
  CHECK(owned(d).consumer == G2HoldConsumer::None,
        "the owner is forgotten at the end");
  end = d.end(1, &seen);
  CHECK(end.consumer == G2HoldConsumer::None,
        "a second end of the same press reaches nobody");

  // A dropped press: seen, but no consumer; its end is absorbed.
  CHECK(d.beginPress(2, &gone), "press 2 live");
  end = d.end(2, &seen);
  CHECK(seen && end.consumer == G2HoldConsumer::None,
        "an ignored press's release is absorbed but still seen");

  // The end of a press the dispatcher never saw (queue full): not seen.
  end = d.end(3, &seen);
  CHECK(!seen && end.consumer == G2HoldConsumer::None,
        "an end without its press is unseen and reaches nobody");
  CHECK(!d.beginPress(3, &gone),
        "a press arriving after its own end is never delivered");

  // Global consumer, then a lost release superseded by the next press.
  CHECK(d.beginPress(4, &gone), "press 4 live");
  d.consume(4, G2HoldConsumer::Global, 0, 2);
  CHECK(d.beginPress(5, &gone) && gone.consumer == G2HoldConsumer::Global &&
            gone.pressId == 4,
        "a newer press hands back an owner whose end never came");
  CHECK(owned(d).consumer == G2HoldConsumer::None,
        "the superseded owner is forgotten");
  end = d.end(4, &seen);
  CHECK(!seen && end.consumer == G2HoldConsumer::None,
        "a late end of a superseded press reaches nobody");

  // An end for an older press leaves the current owner alone.
  d.consume(5, G2HoldConsumer::Page, 7, 1);
  end = d.end(4, &seen);
  CHECK(end.consumer == G2HoldConsumer::None &&
            owned(d).consumer == G2HoldConsumer::Page,
        "an older press's end never steals the current owner");
  end = d.end(5, &seen);
  CHECK(seen && end.consumer == G2HoldConsumer::Page && end.page == 7,
        "the current press still ends normally");

  // Cross-task reorder: abort of press 6 overtakes press 6 itself.
  end = d.end(6, &seen);
  CHECK(!seen && end.consumer == G2HoldConsumer::None, "early abort");
  CHECK(!d.beginPress(6, &gone), "the overtaken press is dropped");
  CHECK(d.beginPress(7, &gone), "the next press is live again");

  // A press older than one already delivered was closed long ago.
  CHECK(!d.beginPress(5, &gone),
        "a stale older press is never delivered after a newer one");
}

static void test_delivery_wrap() {
  G2HoldDelivery d;
  G2HoldOwner gone;
  bool seen = false;
  CHECK(d.beginPress(0xFFFFFFFEu, &gone), "near-wrap press");
  d.consume(0xFFFFFFFEu, G2HoldConsumer::Global, 0, 0);
  CHECK(d.end(0xFFFFFFFEu, &seen).consumer == G2HoldConsumer::Global && seen,
        "near-wrap end");
  CHECK(d.beginPress(0xFFFFFFFFu, &gone), "last id before the wrap");
  (void)d.end(0xFFFFFFFFu, &seen);
  // G2HoldPairing skips 0 on wrap; ids continue at 1.
  CHECK(d.beginPress(1, &gone),
        "an id after the wrap is newer, not stale");
  CHECK(!d.beginPress(0xFFFFFFFFu, &gone),
        "the pre-wrap id is older than the post-wrap one");
}

static void test_event_rate() {
  G2HoldEventRate r;
  CHECK(r.admit(1000) && r.admit(1000), "two events at once are admitted");
  CHECK(!r.admit(1500) && !r.admit(1999),
        "a third inside the same second is limited");
  CHECK(r.admit(2000), "one second after the oldest, one more fits");
  CHECK(r.admit(2500), "the window slides: only 2000 is within 1 s of 2500");
  CHECK(!r.admit(2600) && !r.admit(2999),
        "2000 and 2500 fill the window until 3000");
  CHECK(r.admit(3000) && !r.admit(3001), "one slot per freed second");
  G2HoldEventRate w;
  CHECK(w.admit(0xFFFFFF00u) && w.admit(0xFFFFFF10u), "pre-wrap pair");
  CHECK(!w.admit(0x00000010u), "the window survives the millis() wrap");
  CHECK(w.admit(0xFFFFFF00u + 1000u), "and reopens across it");
}

static bool pageHoldA(uint8_t, uint8_t) { return true; }
static bool pageHoldB(uint8_t, uint8_t) { return false; }

static void test_page_table() {
  typedef bool (*Fn)(uint8_t, uint8_t);
  G2HoldPageTable<Fn, 32> t;
  CHECK(t.get(0) == nullptr && t.get(24) == nullptr && t.get(31) == nullptr,
        "an empty table has no handlers");
  CHECK(t.set(24, pageHoldA) && t.get(24) == pageHoldA,
        "a page id beyond the 23-slot page registry is still a valid key");
  CHECK(t.get(23) == nullptr && t.get(25) == nullptr,
        "lookup is by exact page id");
  CHECK(t.set(24, pageHoldB) && t.get(24) == pageHoldB,
        "re-registration replaces the handler");
  CHECK(t.set(0, pageHoldA) && t.set(31, pageHoldA) && t.get(31) == pageHoldA,
        "both ends of the id range register");
  CHECK(!t.set(32, pageHoldA) && !t.set(0xFFFFFFFFu, pageHoldA),
        "out-of-range ids are refused, never wrapped");
  CHECK(t.get(32) == nullptr && t.get(0x100u + 24u) == nullptr,
        "out-of-range lookups find nothing");
  CHECK(t.set(24, nullptr) && t.get(24) == nullptr,
        "nullptr unregisters");
  const Fn fn = t.get(0);
  CHECK(fn && fn(9, 2), "a looked-up handler is callable");
}

int main() {
  test_press_then_release();
  test_mirrored_arms_are_duplicates();
  test_release_from_other_arm_pairs();
  test_sys_and_text_copies();
  test_orphan_release();
  test_lost_release();
  test_window_boundary();
  test_source_upgrade();
  test_expiry();
  test_press_admitted_after_clock_read();
  test_abort();
  test_pairing_ignores_presentation();
  test_names();
  test_route_truth_table();
  test_profiles();
  test_source_policy();
  test_job_layout();
  test_delivery_owner();
  test_delivery_wrap();
  test_event_rate();
  test_page_table();
  if (failures) {
    std::fprintf(stderr, "%d G2 hold gesture test(s) failed\n", failures);
    return 1;
  }
  std::puts("G2 hold gesture tests passed");
  return 0;
}
