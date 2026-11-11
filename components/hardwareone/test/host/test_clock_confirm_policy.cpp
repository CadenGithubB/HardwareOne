// Exercises the production confirmed-clock policy (System_ClockConfirmPolicy.h) on a
// fake wall clock and monotonic timer: no Arduino/IDF replacements here.
#include "System_ClockConfirmPolicy.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

static unsigned checks = 0;
#define CHECK(condition) \
  do { ++checks; if (!(condition)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); std::exit(1); } } while (0)

using ClockConfirm::Verdict;

static constexpr int64_t kSecond = 1000000;
static constexpr int64_t kHour = 3600 * kSecond;
static constexpr int64_t kDay = 24 * kHour;
static constexpr int64_t kRealStart = 1794000000LL * kSecond;  // November 2026

// A device: the monotonic timer runs from 0 at boot; the wall clock is the
// timer plus an offset that each step replaces.
struct Device {
  ClockConfirm::State state;
  int64_t monoUs = 0;
  int64_t offsetUs = 0;  // wall = offset + mono; 0 = clock never set

  int64_t wall() const { return offsetUs + monoUs; }
  void run(int64_t us) { monoUs += us; }
  // The clock is stepped (lwIP or settimeofday) before the step is reported.
  void stepOnly(int64_t toWallUs) { offsetUs = toWallUs - monoUs; }
  Verdict report(bool fromNetwork, bool reference) {
    return ClockConfirm::onStep(state, fromNetwork, reference, wall(), monoUs);
  }
  Verdict ntp(int64_t toWallUs) { stepOnly(toWallUs); return report(true, true); }
  Verdict local(int64_t toWallUs) { stepOnly(toWallUs); return report(false, true); }
  Verdict ring(int64_t toWallUs) { stepOnly(toWallUs); return report(false, false); }
  int64_t confirmed() const { return ClockConfirm::nowUs(state, wall(), monoUs); }
  bool held() const { return ClockConfirm::disagrees(state, wall(), monoUs); }
  // The real time, for a device whose boot happened at kRealStart.
  int64_t real() const { return kRealStart + monoUs; }
};

static void unsetClockPassesThrough() {
  Device device;
  device.run(5 * kSecond);
  CHECK(device.confirmed() == device.wall() && !device.held());
}

static void firstNetworkSyncIsAccepted() {
  Device device;
  device.run(10 * kSecond);
  CHECK(device.ntp(device.real()) == Verdict::Accepted);
  CHECK(device.confirmed() == device.real() && !device.held());
  // Hourly corrections of a few seconds, and one of just under a day.
  for (int hour = 0; hour < 48; ++hour) {
    device.run(kHour);
    CHECK(device.ntp(device.real() + (hour % 2 ? 3 : -3) * kSecond) == Verdict::Accepted);
    CHECK(device.confirmed() == device.wall());
  }
  CHECK(device.ntp(device.wall() + kDay - kSecond) == Verdict::Accepted && !device.held());
}

static void forgedJumpIsHeldInBothDirections() {
  for (int64_t jump : {400 * kDay, -400 * kDay, kDay + kSecond, -(kDay + kSecond)}) {
    Device device;
    device.run(10 * kSecond);
    CHECK(device.ntp(device.real()) == Verdict::Accepted);
    device.run(kHour);
    CHECK(device.ntp(device.real() + jump) == Verdict::HoldStarted);
    CHECK(device.held() && device.wall() == device.real() + jump);
    CHECK(device.confirmed() == device.real());  // the ring's clock does not move
    device.run(3 * kDay);                        // and keeps running at the real rate
    CHECK(device.confirmed() == device.real() && device.held());
    // The forger keeps answering: still held, reported once.
    CHECK(device.ntp(device.real() + jump) == Verdict::HoldContinues);
    CHECK(device.ntp(device.real() + jump + 50 * kDay) == Verdict::HoldContinues);
    CHECK(device.confirmed() == device.real());
  }
}

static void theWindowBeforeTheStepIsReportedIsCovered() {
  Device device;
  device.run(10 * kSecond);
  CHECK(device.local(device.real()) == Verdict::Accepted);
  device.run(kHour);
  device.stepOnly(device.real() + 900 * kDay);  // lwIP has stepped; the main loop has not drained
  CHECK(device.held() && device.confirmed() == device.real());
  CHECK(device.report(true, true) == Verdict::HoldStarted);
}

static void aTrueReplyEndsTheHold() {
  Device device;
  device.run(10 * kSecond);
  CHECK(device.ntp(device.real()) == Verdict::Accepted);
  device.run(kHour);
  CHECK(device.ntp(device.real() + 30 * kDay) == Verdict::HoldStarted);
  device.run(kHour);
  CHECK(device.ntp(device.real() + 2 * kSecond) == Verdict::HoldReleased);
  CHECK(!device.held() && device.confirmed() == device.wall());
  device.run(kHour);
  CHECK(device.ntp(device.real()) == Verdict::Accepted);
}

static void aLocalSourceEndsTheHoldAndIsNeverHeld() {
  Device device;
  device.run(10 * kSecond);
  CHECK(device.local(device.real() - 10 * kDay) == Verdict::Accepted);  // RTC, ten days slow
  device.run(kHour);
  // The true network time is more than a day from the reference: held.
  CHECK(device.ntp(device.real()) == Verdict::HoldStarted);
  CHECK(device.confirmed() == device.real() - 10 * kDay);
  // `timeset` (or the RTC, or the CM5) settles it, whatever the size of the step.
  CHECK(device.local(device.real()) == Verdict::HoldReleased);
  CHECK(!device.held() && device.confirmed() == device.real());
  CHECK(device.local(device.real() + 700 * kDay) == Verdict::Accepted && !device.held());
  CHECK(device.confirmed() == device.wall());
}

static void aRingAdoptedClockIsNoReference() {
  Device device;
  device.run(10 * kSecond);
  CHECK(device.ring(device.real() - 5 * kDay) == Verdict::Accepted);  // the ring's last measurement time
  device.run(kHour);
  // The first network sync after it is taken, however far it moves the clock.
  CHECK(device.ntp(device.real()) == Verdict::Accepted && !device.held());
  CHECK(device.confirmed() == device.real());
  device.run(kHour);
  CHECK(device.ntp(device.real() + 30 * kDay) == Verdict::HoldStarted);
}

static void aCarriedClockIsNoReference() {
  // After a soft restart the clock is valid but nothing has stepped it this
  // boot: the first network sync is taken.
  Device device;
  device.offsetUs = kRealStart + 300 * kDay;  // a wrong clock carried over
  device.run(10 * kSecond);
  CHECK(!device.held() && device.confirmed() == device.wall());
  CHECK(device.ntp(device.real()) == Verdict::Accepted && device.confirmed() == device.real());
}

static void anInvalidClockFromALocalSourceIsNoReference() {
  // `timeset 1999-…` leaves the clock unsynced: reference = false.
  Device device;
  device.run(10 * kSecond);
  device.stepOnly(900000000LL * kSecond);
  CHECK(device.report(false, false) == Verdict::Accepted);
  device.run(kHour);
  CHECK(device.ntp(device.real()) == Verdict::Accepted && !device.held());
}

int main() {
  unsetClockPassesThrough();
  firstNetworkSyncIsAccepted();
  forgedJumpIsHeldInBothDirections();
  theWindowBeforeTheStepIsReportedIsCovered();
  aTrueReplyEndsTheHold();
  aLocalSourceEndsTheHoldAndIsNeverHeld();
  aRingAdoptedClockIsNoReference();
  aCarriedClockIsNoReference();
  anInvalidClockFromALocalSourceIsNoReference();
  std::printf("clock confirm policy: %u checks passed\n", checks);
  return 0;
}
