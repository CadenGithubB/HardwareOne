// Host test for System_RestartQuiesceCore.h: what an intentional restart does
// to the SDMMC host first, with a fake clock. The order matters: the radio is
// silenced at the source before anything waits, the storage lock is bounded,
// and the drain wait counts from the moment the radio went quiet.
#include "../../System_RestartQuiesceCore.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using hw1::restartq::QuiescePolicy;
using hw1::restartq::QuiesceResult;
using hw1::restartq::quiesceSdmmcForRestart;

namespace {

struct Bench {
  uint32_t now = 0;
  uint32_t lockTakesMs = 0;  // how long the filesystem lock holder keeps it
  bool lockFree = true;      // false: the holder never lets go (the timeout runs out)
  uint32_t lockTimeoutSeen = 0;
  std::vector<std::string> steps;
  std::vector<uint32_t> sleeps;

  QuiesceResult run(const QuiescePolicy& policy = QuiescePolicy()) {
    return quiesceSdmmcForRestart(
        [this] { steps.push_back("radio"); },
        [this](uint32_t timeoutMs) {
          lockTimeoutSeen = timeoutMs;
          steps.push_back("lock");
          if (!lockFree) {
            now += timeoutMs;
            return false;
          }
          now += lockTakesMs;
          return true;
        },
        [this] { return now; },
        [this](uint32_t ms) {
          steps.push_back("sleep");
          sleeps.push_back(ms);
          now += ms;
        },
        policy);
  }
};

void radio_is_silenced_before_any_wait_and_drained_for_the_sdmmc_timeout() {
  Bench b;
  b.now = 5000;
  const QuiesceResult r = b.run();
  assert((b.steps == std::vector<std::string>{"radio", "lock", "sleep"}));
  assert(r.storageLocked);
  assert(b.lockTimeoutSeen == 2000);
  // The drain covers IDF's 1 s SDMMC transfer timeout: a transfer the C6 hold
  // cut off has ended, with its DMA stopped by the driver, before the reset.
  assert(QuiescePolicy().drainMs > 1000);
  assert(b.sleeps.size() == 1 && b.sleeps[0] == QuiescePolicy().drainMs);
  assert(r.drainedMs == QuiescePolicy().drainMs);
}

void a_slow_lock_holder_counts_toward_the_drain() {
  Bench b;
  b.lockTakesMs = 300;
  const QuiesceResult r = b.run();
  assert(r.storageLocked);
  assert(b.sleeps.size() == 1 && b.sleeps[0] == QuiescePolicy().drainMs - 300);
  assert(r.drainedMs == QuiescePolicy().drainMs);
}

void a_stuck_lock_holder_does_not_stop_the_restart() {
  Bench b;
  b.lockFree = false;  // e.g. a writer blocked on the dead link
  const QuiesceResult r = b.run();
  assert(!r.storageLocked);
  // The 2 s lock timeout already covers the drain: no extra sleep.
  assert(b.sleeps.empty());
  assert(r.drainedMs == 2000);
}

void no_radio_no_drain() {
  Bench b;
  QuiescePolicy p;
  p.drainMs = 0;  // SD card only: the lock alone idles it
  const QuiesceResult r = b.run(p);
  assert((b.steps == std::vector<std::string>{"radio", "lock"}));
  assert(r.storageLocked && r.drainedMs == 0);
}

void survives_a_clock_wrap() {
  Bench b;
  b.now = 0xFFFFFF00u;  // 256 ms before millis() wraps
  b.lockTakesMs = 100;
  const QuiesceResult r = b.run();
  assert(b.sleeps.size() == 1 && b.sleeps[0] == QuiescePolicy().drainMs - 100);
  assert(r.drainedMs == QuiescePolicy().drainMs);
}

}  // namespace

int main() {
  radio_is_silenced_before_any_wait_and_drained_for_the_sdmmc_timeout();
  a_slow_lock_holder_counts_toward_the_drain();
  a_stuck_lock_holder_does_not_stop_the_restart();
  no_radio_no_drain();
  survives_a_clock_wrap();
  puts("restart quiesce core tests passed");
  return 0;
}
