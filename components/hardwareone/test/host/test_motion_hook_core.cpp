// The motion layer's per-sample hook and holder list (HAL_MotionHookCore.h):
// one slot, a call per sample with its ctx, an unregister that waits for a
// call in progress, register and unregister one at a time (two unregisters of
// one registration, the hook removing itself while its owner does), all run
// with real threads; the hook sample's fields (the raw tap has no lockout,
// unlike the imu_tap event, and is dropped after a gap), the stopped sample
// and who holds the IMU open.
//
// Consumers in the threaded cases are never freed and carry a unique id: a
// call that reaches a consumer after its unregister returned, or with another
// consumer's ctx, is seen as such (and not hidden by a reused address).
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "HAL_MotionCore.h"
#include "HAL_MotionHookCore.h"

static bool near(float a, float b, float tolerance = 0.001f) { return std::fabs(a - b) <= tolerance; }
static void noWait() { assert(false && "nothing to wait for here"); }
static void yieldStep() { std::this_thread::yield(); }

// A consumer: counts its calls and keeps the last sample. alive goes false
// when the consumer has torn itself down; a call after that is the fault the
// unregister rule exists to prevent. id and its inverse catch a ctx that is
// not a consumer's (or not this one's).
struct Consumer {
  explicit Consumer(uint32_t which = 0) : id(which), check(~which) {}
  const uint32_t id;
  const uint32_t check;
  std::atomic<bool> alive{true};
  std::atomic<uint32_t> calls{0};
  std::atomic<uint32_t> lastMs{0};
  std::atomic<uint32_t> stops{0};
};
static void consumeAs(const MotionHookSample& sample, void* ctx, uint32_t fnId) {
  Consumer* c = static_cast<Consumer*>(ctx);
  assert(c->check == ~c->id);          // a consumer, whole
  assert(c->id % 3 == fnId);           // called through the function it registered with
  assert(c->alive.load());             // and not after its unregister returned
  c->lastMs.store(sample.ms);
  if (sample.stopped) c->stops.fetch_add(1);
  c->calls.fetch_add(1);
}
// Three distinct functions: a consumer registers with the one its id selects,
// so a function paired with another registration's ctx is caught.
static void consume0(const MotionHookSample& s, void* ctx) { consumeAs(s, ctx, 0); }
static void consume1(const MotionHookSample& s, void* ctx) { consumeAs(s, ctx, 1); }
static void consume2(const MotionHookSample& s, void* ctx) { consumeAs(s, ctx, 2); }
static MotionSampleHookFn fnFor(const Consumer& c) {
  static const MotionSampleHookFn fns[3] = {consume0, consume1, consume2};
  return fns[c.id % 3];
}
static void otherHook(const MotionHookSample&, void*) {}

static MotionHookSample sampleAt(uint32_t ms) {
  MotionHookSample s;
  s.ms = ms;
  return s;
}

// ---- One slot ---------------------------------------------------------------------
static void testRegisterAndCall() {
  MotionHookSlot slot;
  Consumer a(0), b(1);
  // Nothing registered: the call does nothing.
  assert(!motionHookRegistered(slot) && !motionHookCall(slot, sampleAt(1)));
  assert(!motionHookRegister(slot, nullptr, &a, noWait));  // no function
  assert(motionHookRegister(slot, consume0, &a, noWait) && motionHookRegistered(slot));
  // One slot: a second consumer is refused, also with the same function.
  assert(!motionHookRegister(slot, consume1, &b, noWait) && !motionHookRegister(slot, otherHook, &b, noWait));
  assert(!motionHookRegister(slot, consume0, &a, noWait));
  // Each call reaches the consumer once, with its ctx and the sample.
  assert(motionHookCall(slot, sampleAt(20)) && motionHookCall(slot, sampleAt(40)));
  assert(a.calls == 2 && a.lastMs == 40 && b.calls == 0);
  // Only the consumer that has the slot can give it back.
  assert(!motionHookUnregister(slot, consume0, &b, false, noWait));
  assert(!motionHookUnregister(slot, otherHook, &a, false, noWait));
  assert(!motionHookUnregister(slot, nullptr, &a, false, noWait));
  assert(motionHookRegistered(slot) && motionHookCall(slot, sampleAt(60)) && a.calls == 3);
  assert(motionHookUnregister(slot, consume0, &a, false, noWait));
  // After it: no call, a second unregister is refused, and the slot is free.
  assert(!motionHookRegistered(slot) && !motionHookCall(slot, sampleAt(80)) && a.calls == 3);
  assert(!motionHookUnregister(slot, consume0, &a, false, noWait));
  assert(motionHookRegister(slot, consume1, &b, noWait) && motionHookCall(slot, sampleAt(100)) && b.calls == 1 &&
         a.calls == 3);
  assert(motionHookUnregister(slot, consume1, &b, false, noWait));
  assert(slot.inCall.load() == 0 && !slot.claimed.load() && !slot.changing.load() && slot.ctx.load() == nullptr);
  // A null ctx is a ctx like any other.
  assert(motionHookRegister(slot, otherHook, nullptr, noWait) && motionHookCall(slot, sampleAt(1)));
  assert(motionHookUnregister(slot, otherHook, nullptr, false, noWait));
  // The last call of a run.
  const MotionHookSample last = motionHookStoppedSample(4242);
  assert(last.stopped && last.ms == 4242 && !last.tapLatched && !last.isShaking && !last.hasGravity && !last.hasHwTap);
  assert(!sampleAt(1).stopped);
  Consumer c(2);
  assert(motionHookRegister(slot, consume2, &c, noWait) && motionHookCall(slot, last) && c.stops == 1 && c.calls == 1);
  // The registration stays across a stop: the next run's samples arrive.
  assert(motionHookCall(slot, sampleAt(5000)) && c.calls == 2 && c.stops == 1);
  assert(motionHookUnregister(slot, consume2, &c, false, noWait));
}

// ---- Unregister waits for a call in progress ------------------------------------------
struct Blocking {
  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};
  std::atomic<bool> finished{false};
  std::atomic<bool> alive{true};
};
static void blockingHook(const MotionHookSample&, void* ctx) {
  Blocking* b = static_cast<Blocking*>(ctx);
  b->entered.store(true);
  while (!b->release.load()) std::this_thread::yield();
  assert(b->alive.load());  // the consumer is still there while its call runs
  b->finished.store(true);
}

static void testUnregisterWaitsForTheCall() {
  MotionHookSlot slot;
  Blocking state;
  assert(motionHookRegister(slot, blockingHook, &state, noWait));
  std::thread imuTask([&] { motionHookCall(slot, sampleAt(1)); });
  while (!state.entered.load()) std::this_thread::yield();
  // The call is in progress. Unregister must not return before it ends.
  std::atomic<bool> unregistered{false};
  std::atomic<uint32_t> waits{0};
  std::thread consumer([&] {
    assert(motionHookUnregister(slot, blockingHook, &state, false, [&] {
      waits.fetch_add(1);
      std::this_thread::yield();
    }));
    assert(state.finished.load());  // the call ended first
    state.alive.store(false);       // now the consumer may tear down
    unregistered.store(true);
  });
  while (waits.load() < 50) std::this_thread::yield();
  assert(!unregistered.load() && !state.finished.load());
  // While it waits, no new call starts.
  assert(!motionHookCall(slot, sampleAt(2)));
  state.release.store(true);
  imuTask.join();
  consumer.join();
  assert(unregistered.load() && !motionHookRegistered(slot) && slot.inCall.load() == 0 && !slot.changing.load());
  Consumer other(0);
  assert(motionHookRegister(slot, consume0, &other, noWait) && motionHookCall(slot, sampleAt(3)) && other.calls == 1);
  assert(motionHookUnregister(slot, consume0, &other, false, noWait));
  // How long is too long to wait without a word.
  assert(!motionHookWaitTooLong(0) && !motionHookWaitTooLong(kMotionHookWaitWarnMs - 1) &&
         motionHookWaitTooLong(kMotionHookWaitWarnMs) && motionHookWaitTooLong(60000));
}

// Two tasks unregister the same registration while its call is in progress.
// One removes it (true), the other is told it is not its to remove (false);
// neither returns before the call has ended, so either may free the ctx.
static void testTwoUnregisterers() {
  for (int round = 0; round < 50; ++round) {
    MotionHookSlot slot;
    Blocking state;
    assert(motionHookRegister(slot, blockingHook, &state, noWait));
    std::thread imuTask([&] { motionHookCall(slot, sampleAt(1)); });
    while (!state.entered.load()) std::this_thread::yield();
    std::atomic<int> removed{0}, returned{0};
    auto unregisterer = [&] {
      const bool mine = motionHookUnregister(slot, blockingHook, &state, false, yieldStep);
      assert(state.finished.load());  // winner or not: the call has ended
      if (mine) removed.fetch_add(1);
      returned.fetch_add(1);
    };
    std::thread first(unregisterer), second(unregisterer);
    for (int i = 0; i < 2000; ++i) std::this_thread::yield();
    assert(returned.load() == 0);  // both wait while the call runs
    state.release.store(true);
    imuTask.join();
    first.join();
    second.join();
    assert(removed.load() == 1 && returned.load() == 2);
    assert(!motionHookRegistered(slot) && !slot.claimed.load() && !slot.changing.load() && slot.inCall.load() == 0);
  }
}

// The loser of two unregisters cannot remove a newer registration of the
// same function and ctx made in between (the stale compare an unserialised
// version had): the second unregister of a pair finds either the old
// registration gone (false) or, when the consumer registered again, its own
// new one, which is then its to remove. The slot never ends up claimed by
// nobody or free with a function in it.
static void testReregistrationBetweenUnregisters() {
  MotionHookSlot slot;
  Consumer a(0);
  for (int round = 0; round < 2000; ++round) {
    assert(motionHookRegister(slot, consume0, &a, yieldStep));
    std::atomic<int> removed{0};
    std::thread one([&] { removed += motionHookUnregister(slot, consume0, &a, false, yieldStep) ? 1 : 0; });
    std::thread two([&] { removed += motionHookUnregister(slot, consume0, &a, false, yieldStep) ? 1 : 0; });
    one.join();
    two.join();
    assert(removed.load() == 1);  // one registration, removed once
    assert(!slot.claimed.load() && slot.fn.load() == nullptr && slot.ctx.load() == nullptr && !slot.changing.load());
  }
}

// The hook gives the slot back from inside its own call (on the IMU task):
// its own call is not waited for, and no further call reaches it.
struct SelfRemoving {
  MotionHookSlot* slot = nullptr;
  std::atomic<uint32_t> calls{0};
  std::atomic<int> removed{-1};
  std::atomic<bool> entered{false};
  std::atomic<bool> go{true};
  std::atomic<bool> alive{true};
};
static void selfRemovingHook(const MotionHookSample&, void* ctx) {
  SelfRemoving* s = static_cast<SelfRemoving*>(ctx);
  assert(s->alive.load());
  s->calls.fetch_add(1);
  s->entered.store(true);
  while (!s->go.load()) std::this_thread::yield();
  s->removed.store(motionHookUnregister(*s->slot, selfRemovingHook, s, /*fromHook=*/true, yieldStep) ? 1 : 0);
  assert(s->alive.load());  // still inside the call: the owner has not torn down
}
static void testUnregisterFromTheHook() {
  MotionHookSlot slot;
  SelfRemoving self;
  self.slot = &slot;
  assert(motionHookRegister(slot, selfRemovingHook, &self, noWait));
  assert(motionHookCall(slot, sampleAt(1)) && self.calls == 1 && self.removed == 1);
  assert(!motionHookCall(slot, sampleAt(2)) && self.calls == 1 && slot.inCall.load() == 0 && !slot.changing.load());
  Consumer next(0);
  assert(motionHookRegister(slot, consume0, &next, noWait));
  assert(motionHookUnregister(slot, consume0, &next, false, noWait));
}

// The hook removes itself while its owner (another task) removes it too.
// Exactly one of them removes the registration, nobody deadlocks (the owner
// waits for the call, the call must not wait for the owner), and the owner
// returns only after the call has ended, whichever of the two won.
static void testSelfUnregisterAgainstOwner() {
  int selfWon = 0, ownerWon = 0;
  for (int round = 0; round < 400; ++round) {
    MotionHookSlot slot;
    auto self = std::make_unique<SelfRemoving>();
    self->slot = &slot;
    self->go.store(false);
    assert(motionHookRegister(slot, selfRemovingHook, self.get(), noWait));
    std::atomic<bool> callDone{false};
    std::thread imuTask([&] {
      motionHookCall(slot, sampleAt(1));
      callDone.store(true);
    });
    while (!self->entered.load()) std::this_thread::yield();
    std::atomic<int> ownerRemoved{-1};
    std::thread owner([&] {
      if (round % 2) std::this_thread::yield();
      ownerRemoved.store(motionHookUnregister(slot, selfRemovingHook, self.get(), false, yieldStep) ? 1 : 0);
      assert(callDone.load() || slot.inCall.load() == 0);  // the call has ended
      self->alive.store(false);                             // the owner tears down
    });
    if (round % 3 == 0) std::this_thread::yield();
    self->go.store(true);
    imuTask.join();
    owner.join();
    assert(self->calls == 1);
    assert(self->removed.load() + ownerRemoved.load() == 1);  // exactly one removed it
    selfWon += self->removed.load();
    ownerWon += ownerRemoved.load();
    assert(!motionHookRegistered(slot) && !slot.claimed.load() && !slot.changing.load() && slot.inCall.load() == 0);
  }
  std::printf("  self against owner: the hook removed itself %d times, the owner %d times\n", selfWon, ownerWon);
}

// ---- Register and unregister against a running IMU task ----------------------------------
// The IMU task calls at full speed while consumers come and go on two other
// tasks. A consumer marks itself dead the moment its unregister returns and
// is never freed: a late call, a call through another registration's
// function, or a call with a torn ctx fails in the hook.
static void testRace() {
  MotionHookSlot slot;
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> delivered{0};
  std::thread imuTask([&] {
    uint32_t ms = 0;
    while (!stop.load()) {
      if (motionHookCall(slot, sampleAt(ms += 20))) delivered.fetch_add(1);
    }
  });
  std::atomic<uint32_t> nextId{0};
  std::atomic<uint64_t> registrations{0};
  auto churn = [&](std::vector<std::unique_ptr<Consumer>>& keep) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    uint64_t mine = 0;
    while (std::chrono::steady_clock::now() < until || mine < 500) {
      keep.push_back(std::make_unique<Consumer>(nextId.fetch_add(1)));
      Consumer* c = keep.back().get();
      // The other churner may have the slot: then this consumer never registers.
      if (!motionHookRegister(slot, fnFor(*c), c, yieldStep)) {
        assert(c->calls.load() == 0);
        std::this_thread::yield();
        continue;
      }
      if (mine % 3 == 0) std::this_thread::yield();
      assert(motionHookUnregister(slot, fnFor(*c), c, false, yieldStep));
      const uint32_t calls = c->calls.load();
      c->alive.store(false);  // any call from here on is a use after the teardown
      std::this_thread::yield();
      assert(c->calls.load() == calls);  // none arrived after unregister returned
      ++mine;
    }
    registrations.fetch_add(mine);
  };
  std::vector<std::unique_ptr<Consumer>> keepA, keepB;  // never freed while the test runs
  std::thread a([&] { churn(keepA); }), b([&] { churn(keepB); });
  a.join();
  b.join();
  // Every consumer got only its own calls: one that never registered got none.
  // A consumer that stays gets every sample from then on.
  Consumer stay(nextId.fetch_add(1));
  assert(motionHookRegister(slot, fnFor(stay), &stay, yieldStep));
  while (stay.calls.load() < 1000) std::this_thread::yield();
  stop.store(true);
  imuTask.join();
  assert(motionHookUnregister(slot, fnFor(stay), &stay, false, noWait));
  assert(slot.inCall.load() == 0 && !slot.changing.load() && delivered.load() >= 1000);
  std::printf("  race: %llu registrations by two tasks, %llu samples delivered\n",
              (unsigned long long)registrations.load(), (unsigned long long)delivered.load());
}

// ---- The sample --------------------------------------------------------------------
static void testSampleFields() {
  const float accel[3] = {0.5f, -1.0f, 9.7f};
  const float gravity[3] = {0.0f, 0.6f, 0.8f};
  MotionHookSample s = motionHookSample(123456, -12.5f, 30.0f, accel, gravity, /*hasHwTap=*/true, /*hwTap=*/false,
                                        /*afterGap=*/false, /*isShaking=*/true, kMotionTapAccel);
  assert(s.ms == 123456 && near(s.pitchDeg, -12.5f) && near(s.rollDeg, 30.0f) && !s.stopped);
  assert(near(s.accel[0], 0.5f) && near(s.accel[1], -1.0f) && near(s.accel[2], 9.7f));
  assert(s.hasGravity && near(s.gravity[0], 0.0f) && near(s.gravity[1], 0.6f) && near(s.gravity[2], 0.8f));
  assert(s.hasHwTap && !s.tapLatched && s.isShaking);
  // Gravity is a unit vector whatever the backend's rounding.
  const float scaled[3] = {0.0f, 0.66f, 0.88f};
  s = motionHookSample(1, 0, 0, accel, scaled, true, false, false, false, kMotionTapAccel);
  assert(s.hasGravity && near(sqrtf(s.gravity[0] * s.gravity[0] + s.gravity[1] * s.gravity[1] + s.gravity[2] * s.gravity[2]), 1.0f));
  assert(near(s.gravity[1], 0.6f) && near(s.gravity[2], 0.8f));
  // A backend without one (the BNO055 today), or a vector that is no direction.
  s = motionHookSample(1, 0, 0, accel, nullptr, false, false, false, false, kMotionTapAccel);
  assert(!s.hasGravity && s.gravity[0] == 0 && s.gravity[1] == 0 && s.gravity[2] == 0 && !s.hasHwTap);
  const float none[3] = {0, 0, 0};
  assert(!motionHookSample(1, 0, 0, accel, none, true, false, false, false, kMotionTapAccel).hasGravity);
  const float bad[3] = {NAN, 0, 1};
  assert(!motionHookSample(1, 0, 0, accel, bad, true, false, false, false, kMotionTapAccel).hasGravity);
  // A tap from the knock detector carries its kind and peak jump; a sample
  // without a tap, or a stale latch after a gap, carries neither.
  const int16_t jump[3] = {-54, -242, 83};
  s = motionHookSample(1, 0, 0, accel, gravity, true, true, false, false, kMotionTapAccel, kMotionTapKindSide, jump);
  assert(s.tapLatched && s.tapKind == kMotionTapKindSide && s.tapVec[0] == -54 && s.tapVec[1] == -242 && s.tapVec[2] == 83);
  s = motionHookSample(1, 0, 0, accel, gravity, true, false, false, false, kMotionTapAccel, kMotionTapKindSide, jump);
  assert(!s.tapLatched && s.tapKind == kMotionTapKindUnknown && s.tapVec[0] == 0 && s.tapVec[1] == 0 && s.tapVec[2] == 0);
  s = motionHookSample(1, 0, 0, accel, gravity, true, true, true, false, kMotionTapAccel, kMotionTapKindFace, jump);
  assert(!s.tapLatched && s.tapVec[1] == 0);
}

// The raw tap has no lockout. Knocks 60 ms apart: the hook sees each, while
// the action detector (the imu_tap event) counts one per 200 ms.
static void testTapLatchedHasNoLockout() {
  MotionActionState detector;
  MotionActionSample still;
  still.az = 9.81f;
  still.hasHwTap = true;
  MotionActionSample knock = still;
  knock.hwTap = true;
  const float accel[3] = {0, 0, 9.81f};
  unsigned long now = 50000;
  int hookTaps = 0, events = 0;
  MotionHookFeed feed;
  // 50 Hz; the chip latched a tap in samples 3, 6 and 9 (60 ms apart), then at 23 (after the lockout).
  for (int i = 0; i < 30; ++i, now += 20) {
    const bool chipTap = i == 3 || i == 6 || i == 9 || i == 23;
    MotionEvents out;
    motionActionsUpdate(detector, chipTap ? knock : still, now, &out);
    for (uint8_t k = 0; k < out.count; ++k) events += out.items[k].kind == MotionEventKind::Tap;
    const bool afterGap = motionHookAfterGap(feed, static_cast<uint32_t>(now), 20);
    assert(afterGap == (i == 0));  // only the run's first sample follows a gap
    const MotionHookSample s = motionHookSample(static_cast<uint32_t>(now), 0, 0, accel, nullptr, true, chipTap,
                                                afterGap, detector.isShaking, kMotionTapAccel);
    hookTaps += s.tapLatched;
    assert(s.tapLatched == chipTap);  // exactly the samples the chip latched one in
  }
  assert(hookTaps == 4 && events == 2 && detector.tapCount == 2);

  // A chip without a tap engine (the BNO055): the raw tap is this sample's
  // spike above the detector's threshold, again without the lockout.
  const float spike[3] = {0, 0, kMotionTapAccel + 5.0f};
  const float edge[3] = {0, 0, kMotionTapAccel};
  assert(motionRawTap(false, false, false, spike, kMotionTapAccel) && !motionRawTap(false, false, false, accel, kMotionTapAccel));
  assert(!motionRawTap(false, false, false, edge, kMotionTapAccel));
  // ...and the chip's flag is ignored there; with a tap engine the spike is.
  assert(!motionRawTap(false, true, false, accel, kMotionTapAccel) && !motionRawTap(true, false, false, spike, kMotionTapAccel));
  assert(motionRawTap(true, true, false, accel, kMotionTapAccel));
  MotionHookSample s = motionHookSample(1, 0, 0, spike, nullptr, false, false, false, false, kMotionTapAccel);
  assert(s.tapLatched && !s.hasHwTap);
  s = motionHookSample(2, 0, 0, spike, nullptr, false, false, false, false, kMotionTapAccel);
  assert(s.tapLatched);  // the next sample too: no lockout
}

// After a gap the chip's latch may hold a tap as old as the gap: the first
// sample after one carries none. A spike in the sample itself is not stale.
static void testGapDropsTheLatchedTap() {
  const float accel[3] = {0, 0, 9.81f};
  const float spike[3] = {0, 0, kMotionTapAccel + 5.0f};
  MotionHookFeed feed;
  // The first sample after the IMU opened (the 4517's gyro-offset second):
  // a tap latched before it is dropped.
  assert(motionHookAfterGap(feed, 1000, 20));
  assert(!motionHookSample(1000, 0, 0, accel, nullptr, true, true, true, false, kMotionTapAccel).tapLatched);
  // Samples one period apart, or a tick or two late: no gap.
  assert(!motionHookAfterGap(feed, 1020, 20) && !motionHookAfterGap(feed, 1043, 20) && !motionHookAfterGap(feed, 1061, 20));
  assert(motionHookSample(1061, 0, 0, accel, nullptr, true, true, false, false, kMotionTapAccel).tapLatched);
  // A few failed reads (a busy bus): up to kMotionHookGapMs is no gap...
  assert(!motionHookAfterGap(feed, 1061 + kMotionHookGapMs, 20));
  // ...one millisecond more is (a paused bus, a long stall).
  assert(motionHookAfterGap(feed, 1061 + kMotionHookGapMs + kMotionHookGapMs + 1, 20));
  assert(!motionHookSample(0, 0, 0, accel, nullptr, true, true, true, false, kMotionTapAccel).tapLatched);
  // The sample after that is normal again.
  assert(!motionHookAfterGap(feed, 1061 + 2 * kMotionHookGapMs + 21, 20));
  // A slow reader (the BNO055 at its 200 ms poll): a gap is more than three periods.
  MotionHookFeed slow;
  assert(motionHookAfterGap(slow, 5000, 200) && !motionHookAfterGap(slow, 5200, 200) && !motionHookAfterGap(slow, 5800, 200));
  assert(motionHookAfterGap(slow, 6401, 200));
  // Without a tap engine the tap is the sample's own spike: not stale, kept.
  assert(motionHookSample(0, 0, 0, spike, nullptr, false, false, true, false, kMotionTapAccel).tapLatched);
  // A software-detector tap (the BNO055, or the LSM6 FIFO) was computed from
  // this sample. A gap keeps it; a chip latch on the same sample is dropped.
  assert(!motionRawTap(true, true, true, accel, kMotionTapAccel));
  assert(motionRawTap(true, true, true, accel, kMotionTapAccel, true));
  assert(motionHookSample(0, 0, 0, accel, nullptr, true, true, true, false, kMotionTapAccel, kMotionTapKindSide,
                          nullptr, true)
             .tapLatched);
  // Across the 32-bit clock's wrap a period is still a period.
  MotionHookFeed wrap;
  assert(motionHookAfterGap(wrap, 0xFFFFFFF0u, 20) && !motionHookAfterGap(wrap, 0x00000004u, 20));
  // A new run starts after a gap again.
  feed = MotionHookFeed{};
  assert(motionHookAfterGap(feed, 9000, 20));
}

// ---- Holders --------------------------------------------------------------------------
static void testHolders() {
  MotionHolders holders;
  char text[160] = "untouched";
  // Nobody holds the IMU: closeimu goes ahead.
  assert(motionHoldRefusal(holders, text, sizeof(text)) == nullptr && std::string(text) == "untouched");
  assert(!motionHoldRemove(holders, "input"));
  assert(!motionHoldAdd(holders, nullptr) && !motionHoldAdd(holders, "") && holders.count == 0);
  assert(motionHoldAdd(holders, "input") && holders.count == 1);
  // A holder holds once.
  assert(!motionHoldAdd(holders, "input") && holders.count == 1);
  { const std::string same = "input"; assert(!motionHoldAdd(holders, same.c_str())); }  // by name, not by pointer
  assert(std::string(motionHoldRefusal(holders, text, sizeof(text))) ==
         "Error: The IMU is in use by input and stays open for it. Stop it first, then run 'closeimu'.");
  assert(motionHoldAdd(holders, "gestures") && holders.count == 2);
  assert(std::string(motionHoldRefusal(holders, text, sizeof(text))) ==
         "Error: The IMU is in use by input, gestures and stays open for them. Stop them first, then run 'closeimu'.");
  char names[32];
  assert(motionHolderNames(holders, names, sizeof(names)) == 15 && std::string(names) == "input, gestures");
  // Releasing one leaves the other; a name that does not hold releases nothing.
  assert(!motionHoldRemove(holders, "other") && !motionHoldRemove(holders, nullptr) && holders.count == 2);
  assert(motionHoldRemove(holders, "input") && holders.count == 1 && std::string(holders.who[0]) == "gestures");
  assert(motionHoldRefusal(holders, text, sizeof(text)) != nullptr);
  assert(motionHoldRemove(holders, "gestures") && holders.count == 0 && holders.who[0] == nullptr);
  assert(motionHoldRefusal(holders, text, sizeof(text)) == nullptr);
  assert(!motionHoldRemove(holders, "gestures"));
  // The list is bounded.
  const char* many[] = {"a", "b", "c", "d", "e"};
  for (int i = 0; i < 4; ++i) assert(motionHoldAdd(holders, many[i]));
  assert(!motionHoldAdd(holders, many[4]) && holders.count == kMotionMaxHolders);
  assert(motionHolderNames(holders, names, sizeof(names)) == 10 && std::string(names) == "a, b, c, d");
  // A small buffer gets whole names only.
  char small[5];
  assert(motionHolderNames(holders, small, sizeof(small)) == 4 && std::string(small) == "a, b");
  assert(motionHolderNames(holders, nullptr, 8) == 0 && motionHoldRefusal(holders, nullptr, 8) == nullptr);
}

int main() {
  testRegisterAndCall();
  testUnregisterWaitsForTheCall();
  testTwoUnregisterers();
  testReregistrationBetweenUnregisters();
  testUnregisterFromTheHook();
  testSelfUnregisterAgainstOwner();
  testRace();
  testSampleFields();
  testTapLatchedHasNoLockout();
  testGapDropsTheLatchedTap();
  testHolders();
  std::puts("motion hook core: one slot, calls with ctx, the stopped call, unregister waits for a call in progress, "
            "two unregisterers, the hook against its owner, the register/unregister race (threads), sample fields, "
            "raw tap without lockout, no stale tap after a gap, holders passed");
  return 0;
}
