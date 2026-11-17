// Host harness for the OLED timed dirty deadline (OLED_Utils.cpp).
//
// test_millis_deadlines.py inserts the production definition of
// oledDirtyUntilMs, oledMarkDirtyUntil and oledIsDirty, and puts the component
// directory on the include path for System_Deadline.h. Everything else that
// oledIsDirty asks (the dirty sequence, input activity, sensor status, the
// pairing ribbon, toasts) is a quiet double, so the only reason for "dirty"
// here is the timed deadline. The clock is a settable 32-bit millis().
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

#include "System_Deadline.h"

static uint32_t gNowMs = 0;
static uint32_t millis() { return gNowMs; }

static bool oledToastActive() { return false; }
static bool oledConfirmIsActive() { return false; }
static void oledFeedbackSetPaused(bool) {}
static void oledTextFieldSetPaused(bool) {}
static bool oledCliViewerNeedsFrame() { return false; }
static bool oledTextFieldNeedsFrame() { return false; }
static bool inputActivitySequence(uint32_t&, int) { return false; }
static std::atomic<uint32_t> oledDirtySeq{1};
static uint32_t oledConsumedDirtySeq = 1;
static uint32_t oledLastRenderedGamepadSeq = 0;
static unsigned long oledLastRenderedSensorSeq = 0;
static unsigned long gSensorStatusSeq = 0;

// INSERT_STATE

// INSERT_PRODUCTION

static int failures = 0;
static int checks = 0;
static void check(bool ok, const std::string& what) {
  ++checks;
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}
static bool dirtyAt(uint32_t nowMs) {
  gNowMs = nowMs;
  return oledIsDirty();
}
static void markAt(uint32_t nowMs, uint32_t delayMs) {
  gNowMs = nowMs;
  oledMarkDirtyUntil(millis() + delayMs);  // the form every caller uses
}
static void reset() {
  oledDirtyUntilMs = 0;
}

int main() {
  constexpr uint32_t kDay = 24u * 60u * 60u * 1000u;

  // Nothing marked: not dirty at any uptime.
  reset();
  for (uint32_t nowMs : {0u, 1000u, 20u * kDay, 0x80000000u, 0x80000001u, 45u * kDay, 0xFFFFFFFFu}) {
    check(!dirtyAt(nowMs), "no mark: not dirty at " + std::to_string(nowMs) + " ms");
  }

  // One mark: dirty until it is reached.
  reset();
  markAt(10000, 3000);
  check(dirtyAt(10000) && dirtyAt(12999), "a mark keeps the frame dirty until it is reached");
  check(!dirtyAt(13000) && !dirtyAt(13001), "the frame is clean once the mark is reached");

  // A later mark extends the time, an earlier one does not shorten it.
  reset();
  markAt(10000, 5000);
  markAt(10100, 1000);
  check(dirtyAt(14999), "a shorter mark does not shorten a longer one");
  check(!dirtyAt(15000), "the longer mark still ends on time");
  reset();
  markAt(10000, 1000);
  markAt(10100, 5000);
  check(dirtyAt(15099) && !dirtyAt(15100), "a longer mark extends a shorter one");

  // The fault: a mark made on day 40, then the millis() wrap. The old running
  // maximum stayed above millis() for 40 more days and oledIsDirty() was true
  // on every 125 ms check.
  reset();
  markAt(40u * kDay, 3000);
  check(dirtyAt(40u * kDay + 2999) && !dirtyAt(40u * kDay + 3000), "day 40: the mark runs its 3 s");
  check(!dirtyAt(0xFFFFFFFFu), "not dirty just before the wrap");
  for (uint32_t nowMs : {0u, 125u, 60000u, kDay, 20u * kDay, 39u * kDay}) {
    check(!dirtyAt(nowMs), "after the wrap an old mark does not keep the frame dirty (" +
                               std::to_string(nowMs) + " ms)");
  }

  // Marks made after the wrap are smaller numbers than the old one. They were dropped.
  markAt(5000, 3000);
  check(dirtyAt(5000) && dirtyAt(7999), "a mark made after the wrap keeps the frame dirty");
  check(!dirtyAt(8000), "and ends on time");

  // A mark that crosses the wrap: set 1 s before it, due 2 s after it.
  reset();
  markAt(0xFFFFFC18u, 3000);
  check(dirtyAt(0xFFFFFC18u) && dirtyAt(0xFFFFFFFFu), "a mark across the wrap is dirty before the wrap");
  check(dirtyAt(0u) && dirtyAt(1999u), "and after the wrap, until it is reached");
  check(!dirtyAt(2000u), "and ends on time after the wrap");

  // A passed deadline is put back to "none", so it cannot read as ahead again
  // once millis() has run on by 24.9 days.
  reset();
  markAt(10000, 3000);
  check(!dirtyAt(13500), "the mark has passed");
  check(oledDirtyUntilMs == 0u, "a passed deadline is cleared");
  for (uint32_t later : {25u * kDay, 30u * kDay, 49u * kDay}) {
    check(!dirtyAt(13000u + later), "a passed mark is not dirty again " + std::to_string(later / kDay) +
                                        " days later");
  }

  // A mark replaces a passed deadline that was not looked at yet.
  reset();
  markAt(10000, 3000);
  markAt(20000, 1000);  // 13000 has passed and is smaller than 21000 either way
  check(dirtyAt(20999) && !dirtyAt(21000), "a new mark replaces a passed one");
  reset();
  markAt(0xFFFFF000u, 1000);  // passes before the wrap
  markAt(500u, 1000);         // after the wrap: a smaller number than the stored one
  check(dirtyAt(1499u) && !dirtyAt(1500u), "a new mark after the wrap replaces a passed one from before it");

  // A request that is already reached, or 0, keeps nothing alive.
  reset();
  gNowMs = 50000;
  oledMarkDirtyUntil(50000);
  oledMarkDirtyUntil(49000);
  oledMarkDirtyUntil(0);
  check(!dirtyAt(50000) && oledDirtyUntilMs == 0u, "a deadline that is already reached is not stored");

  if (failures != 0) {
    std::fprintf(stderr, "%d of %d OLED dirty deadline checks failed\n", failures, checks);
    return 1;
  }
  std::printf("OLED dirty deadline: %d checks passed\n", checks);
  return 0;
}
