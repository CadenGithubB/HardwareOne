#pragma once

#include <stdint.h>

// A momentary GPIO button with independent debounce state. The input task uses
// the held level for the shared cache and latches only new press edges for UI
// consumers, so a short press survives a slower display update.
namespace gpioinput {
class Button {
 public:
  static constexpr uint32_t DebounceMs = 20;

  void reset(bool pressed, uint32_t now) {
    raw_ = stable_ = pressed;
    rawSince_ = now;
    suppress_ = pressed;  // Opening input while held must not trigger an action.
  }

  bool update(bool pressed, uint32_t now) {
    if (pressed != raw_) { raw_ = pressed; rawSince_ = now; }
    if (stable_ == raw_ || uint32_t(now - rawSince_) < DebounceMs) return false;
    stable_ = raw_;
    if (!stable_) suppress_ = false;
    return stable_ && !suppress_;
  }

  bool pressed() const { return stable_ && !suppress_; }

 private:
  bool raw_ = false, stable_ = false, suppress_ = false;
  uint32_t rawSince_ = 0;
};
}  // namespace gpioinput
