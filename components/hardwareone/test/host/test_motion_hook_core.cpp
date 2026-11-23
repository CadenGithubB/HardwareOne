// The motion layer's per-sample hook and holder list (HAL_MotionHookCore.h):
// one slot, a call per sample with its ctx, an unregister that waits for a
// call in progress (run with real threads), the hook sample's fields (the raw
// tap has no lockout, unlike the imu_tap event) and who holds the IMU open.
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "HAL_MotionCore.h"
#include "HAL_MotionHookCore.h"

static bool near(float a, float b, float tolerance = 0.001f) { return std::fabs(a - b) <= tolerance; }
static void noWait() { assert(false && "no call is in progress: nothing to wait for"); }

// A consumer: counts its calls and keeps the last sample. alive goes false
// when the consumer has torn itself down; a call after that is the fault the
// unregister rule exists to prevent.
struct Consumer {
  std::atomic<bool> alive{true};
  std::atomic<uint32_t> calls{0};
  MotionHookSample last;
};
static void consume(const MotionHookSample& sample, void* ctx) {
  Consumer* c = static_cast<Consumer*>(ctx);
  assert(c->alive.load());
  c->last = sample;
  c->calls.fetch_add(1);
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
  Consumer a, b;
  // Nothing registered: the call does nothing.
  assert(!motionHookRegistered(slot) && !motionHookCall(slot, sampleAt(1)));
  assert(!motionHookRegister(slot, nullptr, &a));  // no function
  assert(motionHookRegister(slot, consume, &a) && motionHookRegistered(slot));
  // One slot: a second consumer is refused, also with the same function.
  assert(!motionHookRegister(slot, consume, &b) && !motionHookRegister(slot, otherHook, &b));
  assert(!motionHookRegister(slot, consume, &a));
  // Each call reaches the consumer once, with its ctx and the sample.
  assert(motionHookCall(slot, sampleAt(20)) && motionHookCall(slot, sampleAt(40)));
  assert(a.calls == 2 && a.last.ms == 40 && b.calls == 0);
  // Only the consumer that has the slot can give it back.
  assert(!motionHookUnregister(slot, consume, &b, false, noWait));
  assert(!motionHookUnregister(slot, otherHook, &a, false, noWait));
  assert(!motionHookUnregister(slot, nullptr, &a, false, noWait));
  assert(motionHookRegistered(slot) && motionHookCall(slot, sampleAt(60)) && a.calls == 3);
  assert(motionHookUnregister(slot, consume, &a, false, noWait));
  // After it: no call, a second unregister is refused, and the slot is free.
  assert(!motionHookRegistered(slot) && !motionHookCall(slot, sampleAt(80)) && a.calls == 3);
  assert(!motionHookUnregister(slot, consume, &a, false, noWait));
  assert(motionHookRegister(slot, consume, &b) && motionHookCall(slot, sampleAt(100)) && b.calls == 1 && a.calls == 3);
  assert(motionHookUnregister(slot, consume, &b, false, noWait));
  assert(slot.inCall.load() == 0 && !slot.claimed.load() && slot.ctx.load() == nullptr);
  // A null ctx is a ctx like any other.
  assert(motionHookRegister(slot, otherHook, nullptr) && motionHookCall(slot, sampleAt(1)));
  assert(motionHookUnregister(slot, otherHook, nullptr, false, noWait));
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
  assert(motionHookRegister(slot, blockingHook, &state));
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
  // While it waits, no new call starts, and nobody else gets the slot.
  Consumer other;
  assert(!motionHookCall(slot, sampleAt(2)));
  assert(!motionHookRegister(slot, consume, &other));
  state.release.store(true);
  imuTask.join();
  consumer.join();
  assert(unregistered.load() && !motionHookRegistered(slot) && slot.inCall.load() == 0);
  assert(motionHookRegister(slot, consume, &other) && motionHookCall(slot, sampleAt(3)) && other.calls == 1);
  assert(motionHookUnregister(slot, consume, &other, false, noWait));
}

// The hook gives the slot back from inside its own call (on the IMU task):
// its own call is not waited for, and no further call reaches it.
struct SelfRemoving {
  MotionHookSlot* slot = nullptr;
  uint32_t calls = 0;
  bool removed = false;
};
static void selfRemovingHook(const MotionHookSample&, void* ctx) {
  SelfRemoving* s = static_cast<SelfRemoving*>(ctx);
  ++s->calls;
  s->removed = motionHookUnregister(*s->slot, selfRemovingHook, s, /*fromHook=*/true, noWait);
}
static void testUnregisterFromTheHook() {
  MotionHookSlot slot;
  SelfRemoving self;
  self.slot = &slot;
  assert(motionHookRegister(slot, selfRemovingHook, &self));
  assert(motionHookCall(slot, sampleAt(1)) && self.calls == 1 && self.removed);
  assert(!motionHookCall(slot, sampleAt(2)) && self.calls == 1 && slot.inCall.load() == 0);
  Consumer next;
  assert(motionHookRegister(slot, consume, &next));
  assert(motionHookUnregister(slot, consume, &next, false, noWait));
}

// ---- Register and unregister against a running IMU task ----------------------------------
// The IMU task calls at full speed while consumers come and go. A consumer
// marks itself dead the moment its unregister returns; a call into a dead
// consumer, or a call that sees another consumer's ctx, fails the test.
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
  uint64_t registrations = 0;
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
  while (std::chrono::steady_clock::now() < until || registrations < 2000) {
    Consumer* c = new Consumer;
    assert(motionHookRegister(slot, consume, c));
    if (registrations % 3 == 0) std::this_thread::yield();
    assert(motionHookUnregister(slot, consume, c, false, [] { std::this_thread::yield(); }));
    c->alive.store(false);  // any call from here on is a use after the teardown
    const uint32_t calls = c->calls.load();
    std::this_thread::yield();
    assert(c->calls.load() == calls);  // no call arrived after unregister returned
    delete c;
    ++registrations;
  }
  // A consumer that stays gets every sample from then on.
  Consumer stay;
  assert(motionHookRegister(slot, consume, &stay));
  while (stay.calls.load() < 1000) std::this_thread::yield();
  stop.store(true);
  imuTask.join();
  assert(motionHookUnregister(slot, consume, &stay, false, noWait));
  assert(slot.inCall.load() == 0 && delivered.load() >= 1000);
  std::printf("  race: %llu registrations, %llu samples delivered\n", (unsigned long long)registrations,
              (unsigned long long)delivered.load());
}

// ---- The sample --------------------------------------------------------------------
static void testSampleFields() {
  const float accel[3] = {0.5f, -1.0f, 9.7f};
  const float gravity[3] = {0.0f, 0.6f, 0.8f};
  MotionHookSample s = motionHookSample(123456, -12.5f, 30.0f, accel, gravity, /*hasHwTap=*/true, /*hwTap=*/false,
                                        /*isShaking=*/true, kMotionTapAccel);
  assert(s.ms == 123456 && near(s.pitchDeg, -12.5f) && near(s.rollDeg, 30.0f));
  assert(near(s.accel[0], 0.5f) && near(s.accel[1], -1.0f) && near(s.accel[2], 9.7f));
  assert(s.hasGravity && near(s.gravity[0], 0.0f) && near(s.gravity[1], 0.6f) && near(s.gravity[2], 0.8f));
  assert(s.hasHwTap && !s.tapLatched && s.isShaking);
  // Gravity is a unit vector whatever the backend's rounding.
  const float scaled[3] = {0.0f, 0.66f, 0.88f};
  s = motionHookSample(1, 0, 0, accel, scaled, true, false, false, kMotionTapAccel);
  assert(s.hasGravity && near(sqrtf(s.gravity[0] * s.gravity[0] + s.gravity[1] * s.gravity[1] + s.gravity[2] * s.gravity[2]), 1.0f));
  assert(near(s.gravity[1], 0.6f) && near(s.gravity[2], 0.8f));
  // A backend without one (the BNO055 today), or a vector that is no direction.
  s = motionHookSample(1, 0, 0, accel, nullptr, false, false, false, kMotionTapAccel);
  assert(!s.hasGravity && s.gravity[0] == 0 && s.gravity[1] == 0 && s.gravity[2] == 0 && !s.hasHwTap);
  const float none[3] = {0, 0, 0};
  assert(!motionHookSample(1, 0, 0, accel, none, true, false, false, kMotionTapAccel).hasGravity);
  const float bad[3] = {NAN, 0, 1};
  assert(!motionHookSample(1, 0, 0, accel, bad, true, false, false, kMotionTapAccel).hasGravity);
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
  // 50 Hz; the chip latched a tap in samples 0, 3 and 6 (60 ms apart), then at 20 (after the lockout).
  for (int i = 0; i < 30; ++i, now += 20) {
    const bool chipTap = i == 0 || i == 3 || i == 6 || i == 20;
    MotionEvents out;
    motionActionsUpdate(detector, chipTap ? knock : still, now, &out);
    for (uint8_t k = 0; k < out.count; ++k) events += out.items[k].kind == MotionEventKind::Tap;
    const MotionHookSample s = motionHookSample(static_cast<uint32_t>(now), 0, 0, accel, nullptr, true, chipTap,
                                                detector.isShaking, kMotionTapAccel);
    hookTaps += s.tapLatched;
    assert(s.tapLatched == chipTap);  // exactly the samples the chip latched one in
  }
  assert(hookTaps == 4 && events == 2 && detector.tapCount == 2);

  // A chip without a tap engine (the BNO055): the raw tap is this sample's
  // spike above the detector's threshold, again without the lockout.
  const float spike[3] = {0, 0, kMotionTapAccel + 5.0f};
  const float edge[3] = {0, 0, kMotionTapAccel};
  assert(motionRawTap(false, false, spike, kMotionTapAccel) && !motionRawTap(false, false, accel, kMotionTapAccel));
  assert(!motionRawTap(false, false, edge, kMotionTapAccel));
  // ...and the chip's flag is ignored there; with a tap engine the spike is.
  assert(!motionRawTap(false, true, accel, kMotionTapAccel) && !motionRawTap(true, false, spike, kMotionTapAccel));
  assert(motionRawTap(true, true, accel, kMotionTapAccel));
  MotionHookSample s = motionHookSample(1, 0, 0, spike, nullptr, false, false, false, kMotionTapAccel);
  assert(s.tapLatched && !s.hasHwTap);
  s = motionHookSample(2, 0, 0, spike, nullptr, false, false, false, kMotionTapAccel);
  assert(s.tapLatched);  // the next sample too: no lockout
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
  testUnregisterFromTheHook();
  testRace();
  testSampleFields();
  testTapLatchedHasNoLockout();
  testHolders();
  std::puts("motion hook core: one slot, calls with ctx, unregister waits for a call in progress (threads), "
            "unregister from the hook, the register/unregister race, sample fields, raw tap without lockout, holders passed");
  return 0;
}
