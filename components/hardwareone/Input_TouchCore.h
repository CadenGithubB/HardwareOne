#pragma once
#include <stdint.h>
#include <stddef.h>

enum class TouchPhase : uint8_t { Down, Move, Up, Cancel };
struct TouchEvent {
  TouchPhase phase = TouchPhase::Cancel;
  uint8_t id = 0;
  uint16_t x = 0, y = 0;
  uint32_t timestamp = 0, sequence = 0;
};
struct TouchSnapshot {
  bool valid = false, down = false;
  TouchEvent contact{};
};

// Single producer, one UI consumer; HAL owns synchronization. Overflow cancels
// the entire gesture, rather than dropping an Up and leaving a stuck contact.
class TouchQueue {
 public:
  TouchSnapshot snapshot{};
  void cancel(uint32_t now) {
    count_ = 0;
    TouchEvent e = snapshot.contact;
    e.phase = TouchPhase::Cancel; e.timestamp = now; e.sequence = ++sequence_;
    events_[count_++] = e;
    snapshot = {false, false, e};
    blocked_ = true;
  }
  void discard(uint32_t now) {
    const bool held = blocked_ || snapshot.down;
    cancel(now);
    blocked_ = held;
    snapshot.valid = !held;
  }
  void sample(uint8_t count, uint8_t id, uint16_t x, uint16_t y, uint32_t now) {
    if (count > 1) { cancel(now); return; }
    if (blocked_) {
      if (!count) { blocked_ = false; snapshot.valid = true; }
      return;
    }
    TouchEvent e = snapshot.contact;
    e.timestamp = now;
    if (!count) {
      snapshot.valid = true;
      if (!snapshot.down) return;
      e.phase = TouchPhase::Up;
    } else {
      if (snapshot.down && id != e.id) { cancel(now); return; }
      if (snapshot.down && x == e.x && y == e.y) return;
      e.phase = snapshot.down ? TouchPhase::Move : TouchPhase::Down;
      e.id = id; e.x = x; e.y = y;
    }
    e.sequence = ++sequence_;
    snapshot = {true, count == 1, e};
    if (count_ == capacity) { cancel(now); return; }
    events_[count_++] = e;
  }
  bool pending() const { return count_ != 0; }
  bool pop(TouchEvent& out) {
    if (!count_) return false;
    out = events_[0];
    for (size_t i = 1; i < count_; ++i) events_[i-1] = events_[i];
    --count_; return true;
  }
 private:
  static constexpr size_t capacity = 16;
  TouchEvent events_[capacity]{};
  size_t count_ = 0;
  uint32_t sequence_ = 0;
  bool blocked_ = true; // A held finger at start is not a new press.
};
