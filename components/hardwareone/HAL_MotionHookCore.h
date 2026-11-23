// HAL_MotionHookCore.h - the motion layer's per-sample hook and its holder
// list: the pure parts (HAL_Motion.cpp owns the one slot and calls it).
//
// The hook is how a consumer that needs every sample (the IMU input device:
// taps as buttons, tilt as a joystick) gets them, instead of the reading the
// cache publishes every imuDevicePollMs. One slot: one consumer at a time.
//
//   - The IMU task calls the hook once per sample it has a reading for (50 Hz
//     on the Adafruit 4517, every imuDevicePollMs on the BNO055), after the
//     action detector has seen the sample, with no lock held.
//   - Unregistering is safe while the IMU task runs: it returns only when no
//     call is in progress and none can start, so the consumer may free what
//     ctx points at as soon as it returns.
//   - The hook runs on the IMU task: it must be short, must not block and
//     must not start or stop the IMU.
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

// One sample as the hook gets it.
struct MotionHookSample {
  uint32_t ms = 0;             // millis() when the sample was read
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
  // the chip latched it, so a knock between two samples is not lost; without,
  // it is this sample's acceleration spike.
  bool tapLatched = false;
  bool isShaking = false;      // the action detector's state after this sample
  // What the build's chip provides.
  bool hasHwTap = false;       // taps are detected by the chip (latched between samples)
  bool hasGravity = false;     // gravity is filled
};

using MotionSampleHookFn = void (*)(const MotionHookSample& sample, void* ctx);

// The slot. claimed: a consumer has it (from register until its unregister
// returns). fn is what the IMU task calls; inCall counts calls in progress.
struct MotionHookSlot {
  std::atomic<bool> claimed{false};
  std::atomic<MotionSampleHookFn> fn{nullptr};
  std::atomic<void*> ctx{nullptr};
  std::atomic<uint32_t> inCall{0};
};

// Take the slot. False: no function, or another consumer has it.
inline bool motionHookRegister(MotionHookSlot& slot, MotionSampleHookFn fn, void* ctx) {
  if (!fn) return false;
  bool expected = false;
  if (!slot.claimed.compare_exchange_strong(expected, true)) return false;
  slot.ctx.store(ctx);
  slot.fn.store(fn);  // published last: a call sees the function only with its ctx
  return true;
}

// Give the slot back; only the consumer that has it can (the same fn and
// ctx). False: it is not this consumer's, and nothing changed.
// No call starts after the function is cleared, and the return waits for a
// call in progress to end: waitStep() is called while one is (the firmware
// sleeps a tick in it). fromHook: the hook is unregistering itself, on the
// IMU task; the call in progress is its own and is not waited for.
template <typename WaitStep>
inline bool motionHookUnregister(MotionHookSlot& slot, MotionSampleHookFn fn, void* ctx, bool fromHook,
                                 WaitStep&& waitStep) {
  if (!fn || !slot.claimed.load() || slot.fn.load() != fn || slot.ctx.load() != ctx) return false;
  // Only one unregister of this registration goes on: the one that clears fn.
  MotionSampleHookFn expected = fn;
  if (!slot.fn.compare_exchange_strong(expected, nullptr)) return false;
  // A caller counts itself in before it reads fn, so from here either it
  // read the cleared function, or inCall shows it.
  if (!fromHook) {
    while (slot.inCall.load() != 0) waitStep();
  }
  slot.ctx.store(nullptr);
  slot.claimed.store(false);
  return true;
}

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

// ---- The sample ---------------------------------------------------------------
// The raw tap of one sample: the chip's latched flag when the chip detects
// taps, else an acceleration spike above the detector's threshold
// (tapAccelThreshold: kMotionTapAccel, m/s^2). No lockout.
inline bool motionRawTap(bool hasHwTap, bool hwTap, const float accel[3], float tapAccelThreshold) {
  if (hasHwTap) return hwTap;
  return sqrtf(accel[0] * accel[0] + accel[1] * accel[1] + accel[2] * accel[2]) > tapAccelThreshold;
}

// Fill a hook sample. pitch and roll are the backend's (before the
// orientation correction); gravity may be null when the backend has none.
inline MotionHookSample motionHookSample(uint32_t ms, float pitchDeg, float rollDeg, const float accel[3],
                                         const float* gravity, bool hasHwTap, bool hwTap, bool isShaking,
                                         float tapAccelThreshold) {
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
  out.tapLatched = motionRawTap(hasHwTap, hwTap, accel, tapAccelThreshold);
  out.isShaking = isShaking;
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
