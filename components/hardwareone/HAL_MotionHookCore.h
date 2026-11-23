// HAL_MotionHookCore.h - the motion layer's per-sample hook and its holder
// list: the pure parts (HAL_Motion.cpp owns the one slot and calls it).
//
// The hook is how a consumer that needs every sample (the IMU input device:
// taps as buttons, tilt as a joystick) gets them, instead of the reading the
// cache publishes every imuDevicePollMs. One slot: one consumer at a time.
//
// The contract, for whoever registers a hook:
//   - When: the IMU task calls it once per sample that has a reading (50 Hz
//     on the Adafruit 4517, every imuDevicePollMs on the BNO055), after the
//     action detector has seen the sample, with no lock held.
//   - Where: on the IMU task. Its stack is 4 KB and the task's own frames are
//     on it: keep the hook's use under about 512 bytes (no large locals, no
//     formatting of floats, no logging of more than a short line). It must
//     not block (no bus access, no waiting on a queue or mutex, no delay) and
//     must not open or close the IMU: every sample waits for it.
//   - Gaps: there are stretches without calls and nothing announces them: a
//     paused bus, a read that failed (the bus was busy), the first second
//     after the IMU opens (the 4517 measures its gyro offset). The sample's
//     `ms` is when it was read: a consumer that needs continuity compares it
//     with the previous one and treats a jump as a gap (drop what it was
//     tracking). After a gap of more than kMotionHookGapMs the chip's latched
//     tap is dropped from the first sample (it may be as old as the gap).
//   - The end: when the IMU closes ('closeimu', auto-disable after repeated
//     bus failures, a start that failed) the hook gets one last call with
//     `stopped` true; its other fields are not a reading. Calls begin again
//     when the IMU is opened again; the registration stays. Only a start
//     whose task could not even be created ends without that call.
//   - Unregister returns only when no call is in progress and none can
//     start, so what ctx points at may be freed as soon as it returns (a call
//     that hangs keeps it waiting: see "must not block"). The hook may
//     unregister itself from inside its call.
//
// A holder is a consumer that needs the IMU to stay open. While one holds it,
// 'closeimu' refuses and names the holder. Holding does not start the IMU: a
// holder starts it through the normal queued start ('openimu').
//
// Pure header: <atomic> and the C library only; the host test runs it with
// real threads (test_motion_hook_core.cpp).
#pragma once

#include <atomic>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Where a tap hit, when the detector can tell (MotionHookSample::tapKind).
constexpr uint8_t kMotionTapKindUnknown = 0;
constexpr uint8_t kMotionTapKindSide = 1;
constexpr uint8_t kMotionTapKindFace = 2;

// One sample as the hook gets it.
struct MotionHookSample {
  uint32_t ms = 0;             // millis() right after the sample was read from the chip
  // Tilt in degrees, in the fusion's convention (HAL_Motion_Fusion.h: pitch
  // positive when the board's +X end rises, roll positive when its +Y edge
  // rises), BEFORE the user's orientation correction (imuorientationmode and
  // the offsets): a consumer applies its own mapping.
  float pitchDeg = 0.0f, rollDeg = 0.0f;
  float accel[3] = {0, 0, 0};  // m/s^2, sensor frame
  // Which way is up, as a unit vector in the sensor frame (what the
  // accelerometer reads at rest, divided by g: lying flat face up it is
  // (0, 0, 1)). Only with hasGravity; else (0, 0, 0).
  float gravity[3] = {0, 0, 0};
  // At least one single tap since the previous sample. Raw: no lockout
  // between taps (the imu_tap event keeps its own 200 ms one). With hasHwTap
  // the chip latched it, so a knock between two samples is not lost; the
  // first sample after a gap never carries one (the latch may be stale).
  // Without hasHwTap it is this sample's acceleration spike.
  bool tapLatched = false;
  uint8_t tapKind = kMotionTapKindUnknown;  // with tapLatched: side, face, or unknown
  bool isShaking = false;      // the action detector's state after this sample
  // What the build's chip provides.
  bool hasHwTap = false;       // taps are detected by the chip (latched between samples)
  bool hasGravity = false;     // gravity is filled
  // The IMU closed: the last call until it is opened again. Only `ms` is
  // meaningful in such a call.
  bool stopped = false;
};

using MotionSampleHookFn = void (*)(const MotionHookSample& sample, void* ctx);

// The slot. fn is what the IMU task calls (with ctx); inCall counts calls in
// progress. claimed: a consumer has the slot, from its register until its
// unregister is done. changing: a register or an unregister is at work (one
// at a time; the IMU task's call never takes it, so the call stays lock-free).
struct MotionHookSlot {
  std::atomic<bool> claimed{false};
  std::atomic<bool> changing{false};
  std::atomic<MotionSampleHookFn> fn{nullptr};
  std::atomic<void*> ctx{nullptr};
  std::atomic<uint32_t> inCall{0};
};

// Take the slot. False: no function, or another consumer has it. waitStep()
// is called while another register or unregister is at work (the firmware
// sleeps a tick in it).
template <typename WaitStep>
inline bool motionHookRegister(MotionHookSlot& slot, MotionSampleHookFn fn, void* ctx, WaitStep&& waitStep) {
  if (!fn) return false;
  while (slot.changing.exchange(true)) waitStep();
  const bool free = !slot.claimed.load();
  if (free) {
    slot.claimed.store(true);
    slot.ctx.store(ctx);
    slot.fn.store(fn);  // published last: a call sees the function only with its ctx
  }
  slot.changing.store(false);
  return free;
}

// Give the slot back; only the consumer that has it can (the same fn and
// ctx). True: this call removed the registration. False: it is not this
// consumer's (never registered, already removed, or being removed by another
// caller), and this call changed nothing.
//
// Either way, when it returns (not fromHook) no call into fn with this ctx is
// in progress: a caller counts itself in before it reads the function, so
// once the function is cleared the wait for inCall to reach zero covers every
// call that could still be running. waitStep() is called while it waits (for
// a call in progress, or for another register/unregister).
//
// fromHook: the hook is unregistering itself, on the IMU task. Its own call
// is in progress and is not waited for; no later call reaches it. If another
// task is unregistering the same registration at that moment, that one
// removes it and this call answers false.
template <typename WaitStep>
inline bool motionHookUnregister(MotionHookSlot& slot, MotionSampleHookFn fn, void* ctx, bool fromHook,
                                 WaitStep&& waitStep) {
  if (!fn) return false;
  // One register or unregister at a time. The hook itself must not wait for a
  // remover of its own registration (that one is waiting for this call to end).
  while (slot.changing.exchange(true)) {
    if (fromHook && slot.fn.load() != fn) return false;
    waitStep();
  }
  const bool mine = slot.claimed.load() && slot.fn.load() == fn && slot.ctx.load() == ctx;
  if (mine) slot.fn.store(nullptr);  // from here no call starts
  if (mine && fromHook) {
    slot.ctx.store(nullptr);
    slot.claimed.store(false);
    slot.changing.store(false);
    return true;
  }
  if (!mine) slot.changing.store(false);
  // A call in progress ends first: this registration's when it is ours, and
  // when it is not, any that was running when we were asked (it may be the
  // one that removed the registration from inside the hook).
  if (!fromHook) {
    while (slot.inCall.load() != 0) waitStep();
  }
  if (!mine) return false;
  slot.ctx.store(nullptr);
  slot.claimed.store(false);
  slot.changing.store(false);
  return true;
}

// An unregister that has waited this long for a call to end says so once (a
// hook that blocks breaks its contract; the wait itself goes on, because
// returning would let the consumer free what the call still uses).
constexpr uint32_t kMotionHookWaitWarnMs = 500;
inline bool motionHookWaitTooLong(uint32_t waitedMs) { return waitedMs >= kMotionHookWaitWarnMs; }

// The IMU task's call, once per sample. True when a hook ran.
inline bool motionHookCall(MotionHookSlot& slot, const MotionHookSample& sample) {
  slot.inCall.fetch_add(1);
  const MotionSampleHookFn fn = slot.fn.load();
  const bool called = fn != nullptr;
  if (called) fn(sample, slot.ctx.load());
  slot.inCall.fetch_sub(1);
  return called;
}

inline bool motionHookRegistered(const MotionHookSlot& slot) { return slot.fn.load() != nullptr; }

// ---- Gaps ---------------------------------------------------------------------------
// The samples the hook gets are not always one period apart (see the
// contract above). After a gap the chip's tap latch may hold a tap as old as
// the gap, so the first sample after one carries none.
constexpr uint32_t kMotionHookGapMs = 100;
struct MotionHookFeed {
  bool have = false;    // a sample was delivered since the IMU opened
  uint32_t lastMs = 0;  // when it was read
};
// Note the sample read at `ms` (periodMs: the time between samples) and say
// whether it follows a gap: the first one after the IMU opened, or one later
// than the larger of kMotionHookGapMs and three periods after the previous.
inline bool motionHookAfterGap(MotionHookFeed& feed, uint32_t ms, uint32_t periodMs) {
  const uint32_t limit = 3 * periodMs > kMotionHookGapMs ? 3 * periodMs : kMotionHookGapMs;
  const bool gap = !feed.have || static_cast<uint32_t>(ms - feed.lastMs) > limit;
  feed.have = true;
  feed.lastMs = ms;
  return gap;
}

// ---- The sample ---------------------------------------------------------------
// The raw tap of one sample: the chip's latched flag when the chip detects
// taps (not on the first sample after a gap: the latch may be stale), else an
// acceleration spike above the detector's threshold (tapAccelThreshold:
// kMotionTapAccel, m/s^2). No lockout.
inline bool motionRawTap(bool hasHwTap, bool hwTap, bool afterGap, const float accel[3], float tapAccelThreshold) {
  if (hasHwTap) return hwTap && !afterGap;
  return sqrtf(accel[0] * accel[0] + accel[1] * accel[1] + accel[2] * accel[2]) > tapAccelThreshold;
}

// Fill a hook sample. pitch and roll are the backend's (before the
// orientation correction); gravity may be null when the backend has none;
// afterGap is motionHookAfterGap's answer for this sample.
inline MotionHookSample motionHookSample(uint32_t ms, float pitchDeg, float rollDeg, const float accel[3],
                                         const float* gravity, bool hasHwTap, bool hwTap, bool afterGap,
                                         bool isShaking, float tapAccelThreshold,
                                         uint8_t hwTapKind = kMotionTapKindUnknown) {
  MotionHookSample out;
  out.ms = ms;
  out.pitchDeg = pitchDeg;
  out.rollDeg = rollDeg;
  for (int i = 0; i < 3; ++i) out.accel[i] = accel[i];
  out.hasGravity = false;
  if (gravity) {
    const float n = sqrtf(gravity[0] * gravity[0] + gravity[1] * gravity[1] + gravity[2] * gravity[2]);
    if (n > 0.5f && n < 2.0f) {  // a direction: normalised here, whatever the backend's rounding
      for (int i = 0; i < 3; ++i) out.gravity[i] = gravity[i] / n;
      out.hasGravity = true;
    }
  }
  out.hasHwTap = hasHwTap;
  out.tapLatched = motionRawTap(hasHwTap, hwTap, afterGap, accel, tapAccelThreshold);
  out.tapKind = (out.tapLatched && hasHwTap) ? hwTapKind : kMotionTapKindUnknown;
  out.isShaking = isShaking;
  return out;
}

// The last call of a run: the IMU closed at `ms`.
inline MotionHookSample motionHookStoppedSample(uint32_t ms) {
  MotionHookSample out;
  out.ms = ms;
  out.stopped = true;
  return out;
}

// ---- Holders --------------------------------------------------------------------
// Who needs the IMU to stay open, by name (a string literal: the pointer is
// kept). The caller serialises access (HAL_Motion.cpp: a critical section).
constexpr uint8_t kMotionMaxHolders = 4;
struct MotionHolders {
  const char* who[kMotionMaxHolders] = {nullptr, nullptr, nullptr, nullptr};
  uint8_t count = 0;
};

inline int motionHolderIndex(const MotionHolders& holders, const char* who) {
  if (!who || !who[0]) return -1;
  for (uint8_t i = 0; i < holders.count; ++i) {
    if (holders.who[i] && strcmp(holders.who[i], who) == 0) return i;
  }
  return -1;
}

// Add a holder. False: no name, it holds already (a holder holds once), or
// the list is full.
inline bool motionHoldAdd(MotionHolders& holders, const char* who) {
  if (!who || !who[0] || motionHolderIndex(holders, who) >= 0 || holders.count >= kMotionMaxHolders) return false;
  holders.who[holders.count++] = who;
  return true;
}

// Remove a holder. False: it was not holding.
inline bool motionHoldRemove(MotionHolders& holders, const char* who) {
  const int at = motionHolderIndex(holders, who);
  if (at < 0) return false;
  for (uint8_t i = static_cast<uint8_t>(at); i + 1 < holders.count; ++i) holders.who[i] = holders.who[i + 1];
  holders.who[--holders.count] = nullptr;
  return true;
}

// The holders' names, "input" or "input, gestures"; "" when none.
inline size_t motionHolderNames(const MotionHolders& holders, char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  size_t pos = 0;
  for (uint8_t i = 0; i < holders.count; ++i) {
    const int n = snprintf(out + pos, cap - pos, "%s%s", i ? ", " : "", holders.who[i] ? holders.who[i] : "?");
    if (n < 0 || static_cast<size_t>(n) >= cap - pos) break;
    pos += static_cast<size_t>(n);
  }
  return pos;
}

// 'closeimu' while the IMU is held: the reason, or nullptr when nobody holds
// it (out is then untouched).
inline const char* motionHoldRefusal(const MotionHolders& holders, char* out, size_t cap) {
  if (holders.count == 0 || !out || cap == 0) return nullptr;
  char names[64];
  motionHolderNames(holders, names, sizeof(names));
  snprintf(out, cap, "Error: The IMU is in use by %s and stays open for %s. Stop %s first, then run 'closeimu'.",
           names, holders.count == 1 ? "it" : "them", holders.count == 1 ? "it" : "them");
  return out;
}
