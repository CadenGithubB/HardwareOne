#pragma once

#include <stdint.h>

// Hardware-independent rotary decoding and button gestures. Kept separate from
// GPIO/FreeRTOS so bounce, missed edges and time rollover can be exercised on a
// host. Native action bits deliberately follow HAL_Input's logical button order.
namespace rotary {
enum Action : uint32_t {
  Select = 1u << 0,
  Back = 1u << 1,
  Extra = 1u << 2,
  Delete = 1u << 3,
  Function = 1u << 5,
};

class Quadrature {
 public:
  void reset(uint8_t ab) { previous_ = ab & 3u; partial_ = 0; }

  // Both phases changing together is a missed/invalid transition; discard the
  // partial detent rather than invent a direction. Ordinary contact bounce
  // traverses the same edge backwards and cancels itself.
  __attribute__((always_inline)) inline int update(uint8_t ab, uint8_t steps = 4) {
    ab &= 3u;
    const uint8_t changed = previous_ ^ ab;
    if (!changed) return 0;
    if (changed == 3u) {
      previous_ = ab;
      partial_ = 0;
      return 0;
    }
    partial_ += ((previous_ >> 1) ^ (ab & 1u)) ? 1 : -1;
    previous_ = ab;
    if (partial_ >= steps) { partial_ = 0; return 1; }
    if (partial_ <= -static_cast<int>(steps)) { partial_ = 0; return -1; }
    return 0;
  }

 private:
  uint8_t previous_ = 0;
  int8_t partial_ = 0;
};

struct Events {
  int detents = 0;
  uint32_t actions = 0;
};

class Gestures {
 public:
  static constexpr uint32_t DebounceMs = 20;
  static constexpr uint32_t HoldMs = 700;
  static constexpr uint32_t DoubleClickMs = 250;

  void reset(bool pressed, uint32_t now) {
    raw_ = stable_ = pressed;
    rawSince_ = pressedAt_ = now;
    held_ = turned_ = clickPending_ = secondClick_ = false;
    suppress_ = pressed;  // A switch held during startup must first be released.
  }

  Events update(bool pressed, uint32_t now, int detents) {
    Events out;
    if (pressed != raw_) { raw_ = pressed; rawSince_ = now; }
    if (stable_ != raw_ && uint32_t(now - rawSince_) >= DebounceMs) {
      stable_ = raw_;
      if (stable_) {
        pressedAt_ = now;
        held_ = turned_ = false;
        secondClick_ = clickPending_ && uint32_t(rawSince_ - releasedAt_) <= DoubleClickMs;
        if (secondClick_) clickPending_ = false;
      } else if (suppress_) {
        suppress_ = false;
      } else if (!held_ && !turned_) {
        if (secondClick_) out.actions |= Function;
        else { clickPending_ = true; releasedAt_ = now; }
        secondClick_ = false;
      }
    }

    if (detents) {
      if (stable_ && !suppress_) {
        // Holding the wheel while turning provides the secondary UI actions
        // needed by text entry (X=submit, Y=delete), without selecting on release.
        turned_ = true;
        clickPending_ = secondClick_ = false;
        if (!held_) out.actions |= detents > 0 ? Extra : Delete;
      } else if (!suppress_) {
        out.detents = detents;
      }
    }
    if (stable_ && !suppress_ && !held_ && !turned_ && uint32_t(now - pressedAt_) >= HoldMs) {
      held_ = true;
      clickPending_ = secondClick_ = false;
      out.actions |= Back;
    }
    const bool secondPressDebouncing = raw_ && uint32_t(rawSince_ - releasedAt_) <= DoubleClickMs;
    if (clickPending_ && !secondPressDebouncing && uint32_t(now - releasedAt_) > DoubleClickMs) {
      clickPending_ = false;
      out.actions |= Select;
    }
    return out;
  }

  bool pressed() const { return stable_; }

 private:
  bool raw_ = false, stable_ = false, suppress_ = false;
  bool held_ = false, turned_ = false, clickPending_ = false, secondClick_ = false;
  uint32_t rawSince_ = 0, pressedAt_ = 0, releasedAt_ = 0;
};
}  // namespace rotary
