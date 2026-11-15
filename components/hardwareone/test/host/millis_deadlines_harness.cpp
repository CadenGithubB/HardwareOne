// Host harness for the millis() deadline owner, System_Deadline.h.
//
// The production header is included as it is (test_millis_deadlines.py puts
// the component directory on the include path), with the ESP-NOW header that
// forwards to it. uint32_t is used throughout: unsigned long is 64 bits here.
#include <cstdint>
#include <cstdio>
#include <string>

#include "System_Deadline.h"
#include "System_ESPNow_Deadline.h"

static int failures = 0;
static int checks = 0;
static void check(bool ok, const std::string& what) {
  ++checks;
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

int main() {
  constexpr uint32_t kDay = 24u * 60u * 60u * 1000u;

  // A deadline that was never set is not pending at any uptime. The bare
  // signed difference read it as pending from day 24.9 to day 49.7.
  for (uint32_t nowMs : {0u, 1u, 5000u, 20u * kDay, 0x7FFFFFFFu, 0x80000000u, 0x80000001u,
                         30u * kDay, 45u * kDay, 0xFFFFFFFFu}) {
    check(!deadlinePending(0, nowMs), "no deadline is not pending at " + std::to_string(nowMs) + " ms");
  }

  // An ordinary deadline: pending until it is reached, then passed.
  for (uint32_t startMs : {0u, 1000u, 20u * kDay, 0x7FFFF000u, 0x80000000u, 40u * kDay}) {
    const uint32_t until = deadlineAfter(startMs, 2500);
    const std::string at = " (set at " + std::to_string(startMs) + " ms)";
    check(until != 0, "a set deadline is not the 'none' value" + at);
    check(deadlinePending(until, startMs), "pending when set" + at);
    check(deadlinePending(until, startMs + 2499), "pending 1 ms before it is reached" + at);
    check(!deadlinePending(until, startMs + 2500), "passed when it is reached" + at);
    check(!deadlinePending(until, startMs + 2501), "passed after it" + at);
    check(!deadlinePending(until, startMs + 10u * kDay), "still passed ten days later" + at);
  }

  // Set shortly before the wrap, due after it: `millis() < until` called this
  // passed at once; `millis() >= until` called it pending.
  {
    const uint32_t startMs = 0xFFFFFC00u;         // 1024 ms before the wrap
    const uint32_t until = deadlineAfter(startMs, 3000);
    check(until == 3000u - 1024u, "the deadline lies after the wrap");
    check(deadlinePending(until, startMs), "pending when set before the wrap");
    check(deadlinePending(until, 0xFFFFFFFFu), "pending at the last millisecond before the wrap");
    check(deadlinePending(until, 0u), "pending at the wrap");
    check(deadlinePending(until, until - 1), "pending 1 ms before it is reached, after the wrap");
    check(!deadlinePending(until, until), "passed when it is reached, after the wrap");
  }

  // Left over from before the wrap: a raw comparison keeps it pending for as
  // long as the device had been up when it was set.
  {
    const uint32_t until = deadlineAfter(40u * kDay, 3000);
    check(!deadlinePending(until, 0xFFFFFFFFu), "an old deadline is passed just before the wrap");
    check(!deadlinePending(until, 0u), "an old deadline is passed at the wrap");
    check(!deadlinePending(until, 5u * kDay), "an old deadline is passed five days after the wrap");
  }

  // A deadline never lands on 0, the value for "none".
  check(deadlineAfter(0xFFFFFFFFu, 1) == 1u, "a deadline that would be 0 is stored as 1");
  check(deadlineAfter(0, 0) == 1u, "delay 0 at time 0 is stored as 1");
  check(deadlinePending(deadlineAfter(0xFFFFFFFFu, 1), 0xFFFFFFFFu), "that deadline is pending when set");
  check(deadlineAfter(123, 0) == 123u && !deadlinePending(123, 123), "delay 0 is due at once");

  // A delay beyond 24.8 days is capped: far ahead, not already passed.
  {
    const uint32_t until = deadlineAfter(1000, 0xF0000000u);
    check(until == 1000u + 0x7FFFFFFFu, "an oversized delay is capped at INT32_MAX ms");
    check(deadlinePending(until, 1000) && deadlinePending(until, 1000u + 20u * kDay),
          "a capped deadline reads as ahead");
  }

  // The ESP-NOW names answer through the same owner.
  for (uint32_t nowMs : {0u, 7u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFF0u}) {
    for (uint32_t delayMs : {0u, 1u, 5000u, 0x7FFFFFFFu, 0xFFFFFFFFu}) {
      const uint32_t until = deadlineAfter(nowMs, delayMs);
      check(espnowDeadlineAfter(nowMs, delayMs) == until, "espnowDeadlineAfter forwards");
      check(espnowDeadlinePending(until, nowMs) == deadlinePending(until, nowMs) &&
            espnowDeadlinePending(until, nowMs + delayMs) == deadlinePending(until, nowMs + delayMs) &&
            !espnowDeadlinePending(0, nowMs), "espnowDeadlinePending forwards");
    }
  }

  if (failures != 0) {
    std::fprintf(stderr, "%d of %d deadline checks failed\n", failures, checks);
    return 1;
  }
  std::printf("millis() deadlines: %d checks passed\n", checks);
  return 0;
}
