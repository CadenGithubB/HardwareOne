// OLED_ValueScroll.h - the coalescing rule of a value-scroll row (OLED
// background commands, step 2, 2026-11-22): FM tune and the LED brightness
// nudge.
//
// A scroll step must not wait for the device and must not be dropped. The
// screens compute the next value from what they show, and the device's value
// only changes once a command has run, so the row keeps its own wanted value:
// a step moves it at once (and the row shows it), and only the newest value is
// sent, when none of the row's commands is in flight and the shared OLED
// command slot is free. A step while one runs replaces the waiting value; it
// is never refused with "Busy". When the last command ends and the device
// shows the wanted value (or kSettleMs later, or at once after a failure,
// whose refusal toast was already shown), the row follows the device again.
//
// Dependency-free (host-tested by test_oled_value_scroll.cpp); the OLED glue
// (OLED_Utils.cpp oledValueScrollStep/oledValueScrollShown) runs it on the
// display task only. All fields start at zero, so a row can live in
// EXT_RAM_BSS_ATTR storage.
#pragma once

#include <stdint.h>

struct OledValueScroll {
  // After the last command succeeded, how long the row keeps showing its own
  // value while the device has not reported it yet (a tuner that settles a
  // little later).
  static constexpr uint32_t kSettleMs = 1500;

  bool leading;     // the row shows `wanted`, not the device's value
  bool waiting;     // `wanted` has not been sent yet
  bool inFlight;    // one of this row's commands runs
  int32_t wanted;
  uint32_t endedAtMs;  // when the last command ended (settle window)

  // The value the row shows, and the base of the next step.
  int32_t shown(int32_t device) const { return leading ? wanted : device; }

  // A scroll step to `value` (already clamped by the screen).
  void want(int32_t value) {
    leading = true;
    waiting = true;
    wanted = value;
  }

  // The value to send now, if any: only when one waits, none of this row's
  // commands runs and the shared slot is free. Marks it in flight.
  bool takeSend(bool slotBusy, int32_t& out) {
    if (!waiting || inFlight || slotBusy) return false;
    waiting = false;
    inFlight = true;
    out = wanted;
    return true;
  }

  // The row's command ended (ok false: refused, full queue, timeout, replaced
  // session, or the start itself failed). A newer value that waits is still
  // sent; otherwise a failure gives the row back to the device at once.
  void finished(bool ok, uint32_t nowMs) {
    inFlight = false;
    endedAtMs = nowMs;
    if (!waiting && !ok) leading = false;
  }

  // Each look at the row: once nothing waits or runs, the row follows the
  // device again when the device shows the wanted value or kSettleMs passed.
  // True while the row still leads (the screen keeps redrawing).
  bool follow(int32_t device, uint32_t nowMs) {
    if (!leading) return false;
    if (waiting || inFlight) return true;
    if (device == wanted || (uint32_t)(nowMs - endedAtMs) >= kSettleMs) leading = false;
    return leading;
  }
};
