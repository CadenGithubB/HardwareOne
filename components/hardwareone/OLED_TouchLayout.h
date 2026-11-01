#pragma once
#include "Input_TouchCore.h"

// UI-owned controls in physical panel space. Drawing and hit-testing share
// these rectangles. The display backend owns the canvas viewport transform.
enum class TouchAction : uint8_t { None, Up, Down, Left, Right, Select, Back, Submit, Delete, Menu, Function };
struct TouchControl { int x, y, w, h; const char* label; TouchAction action; };
static constexpr TouchControl touchControls[] = {
  {40, 850, 200, 76, "BACK", TouchAction::Back},
  {260,850, 200, 76, "UP", TouchAction::Up},
  {480,850, 200, 76, "SELECT", TouchAction::Select},
  {40, 940, 200, 76, "LEFT", TouchAction::Left},
  {260,940, 200, 76, "DOWN", TouchAction::Down},
  {480,940, 200, 76, "RIGHT", TouchAction::Right},
  {40,1030, 200, 76, "X / OK", TouchAction::Submit},
  {260,1030,200, 76, "Y / DEL", TouchAction::Delete},
  {480,1030,200, 76, "MENU", TouchAction::Menu},
  {260,1120,200, 76, "FN", TouchAction::Function},
};
inline TouchAction touchHitTest(int x, int y) {
  for (const auto& c : touchControls)
    if (x >= c.x && x < c.x+c.w && y >= c.y && y < c.y+c.h) return c.action;
  return TouchAction::None;
}
inline bool touchActionIsDirection(TouchAction action) {
  return action == TouchAction::Up || action == TouchAction::Down ||
         action == TouchAction::Left || action == TouchAction::Right;
}
inline bool touchWithinControl(TouchAction action, int x, int y) {
  // Small finger motion at a border should not discard an otherwise valid tap.
  // Capture starts inside the drawn button; adjacent controls remain separate.
  constexpr int slop = 8;
  for (const auto& c : touchControls)
    if (c.action == action)
      return x >= c.x-slop && x < c.x+c.w+slop &&
             y >= c.y-slop && y < c.y+c.h+slop;
  return false;
}
class TouchTap {
 public:
  void reset() { armed_ = TouchAction::None; }
  TouchAction accept(const TouchEvent& e) {
    if (e.phase == TouchPhase::Down) {
      armed_ = touchHitTest(e.x, e.y); id_ = e.id;
      // Directions are reversible navigation. Emit one step immediately,
      // without a second step on release or movement into another button.
      if (touchActionIsDirection(armed_)) {
        const auto action = armed_; reset(); return action;
      }
    } else if (e.phase == TouchPhase::Cancel || e.id != id_ ||
               !touchWithinControl(armed_, e.x, e.y)) reset();
    else if (e.phase == TouchPhase::Up) {
      const auto action = armed_; reset(); return action;
    }
    return TouchAction::None;
  }
 private:
  TouchAction armed_ = TouchAction::None;
  uint8_t id_ = 0;
};
