#pragma once
// When the backend tick tries the HTTP default model again (llmBackendTick).
// Pure: no Arduino or IDF includes, so host harnesses include it as is.
//
// A level rule, not a one-shot latch: while the conditions hold (LLM on,
// autostart on, an http: default, nothing selected, no user unload this boot)
// the tick tries at next(), 5 s after a failure and doubling to 60 s, and at
// once again when the request path comes up (a rising edge, sampled per tick).
// A success resets the delay, so a later loss of the selection retries at once.
#include <cstdint>
namespace llmautostart {
constexpr uint32_t kFirstDelayMs = 5000, kMaxDelayMs = 60000;
class Retry {
 public:
  // Sample the request path every tick: a rising edge makes the next try due now.
  void notePath(bool up) {
    if (up && !pathUp_) { scheduled_ = false; delay_ = kFirstDelayMs; }
    pathUp_ = up;
  }
  bool due(uint32_t now) const { return !scheduled_ || int32_t(now - next_) >= 0; }
  void result(bool ok, uint32_t now) {
    if (ok) { scheduled_ = false; delay_ = kFirstDelayMs; return; }
    scheduled_ = true; next_ = now + delay_;
    delay_ = delay_ >= kMaxDelayMs / 2 ? kMaxDelayMs : delay_ * 2;
  }
  uint32_t delayMs() const { return delay_; }
 private:
  uint32_t next_ = 0, delay_ = kFirstDelayMs;
  bool scheduled_ = false, pathUp_ = false;
};
}  // namespace llmautostart
