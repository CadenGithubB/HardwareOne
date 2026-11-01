#pragma once
#include "Input_TouchCore.h"
#include "HAL_Display_WavesharePixels.h"

// UI-owned controls in the selected surface orientation. Drawing and hit-testing
// share these rectangles; the HAL geometry maps to native panel/touch space.
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
static constexpr TouchControl landscapeTouchControls[] = {
  {1040, 20, 216, 72, "UP", TouchAction::Up},
  {1040,108, 104, 72, "LEFT", TouchAction::Left},
  {1152,108, 104, 72, "RIGHT", TouchAction::Right},
  {1040,196, 216, 72, "DOWN", TouchAction::Down},
  {1040,284, 216, 72, "SELECT", TouchAction::Select},
  {1040,372, 104, 72, "BACK", TouchAction::Back},
  {1152,372, 104, 72, "MENU", TouchAction::Menu},
  {1040,460, 104, 72, "X/OK", TouchAction::Submit},
  {1152,460, 104, 72, "Y/DEL", TouchAction::Delete},
  {1040,548, 216, 72, "FN", TouchAction::Function},
};
constexpr size_t touchControlCount = sizeof(touchControls)/sizeof(touchControls[0]);
static_assert(sizeof(landscapeTouchControls)/sizeof(landscapeTouchControls[0]) == touchControlCount);
inline const TouchControl* touchControlsFor(bool landscape) {
  return landscape ? landscapeTouchControls : touchControls;
}
inline TouchAction touchHitTest(int x, int y, bool landscape = false) {
  for (size_t i = 0; i < touchControlCount; ++i) {
    const auto& c = touchControlsFor(landscape)[i];
    if (x >= c.x && x < c.x+c.w && y >= c.y && y < c.y+c.h) return c.action;
  }
  return TouchAction::None;
}
inline bool touchActionIsDirection(TouchAction action) {
  return action == TouchAction::Up || action == TouchAction::Down ||
         action == TouchAction::Left || action == TouchAction::Right;
}
inline bool touchWithinControl(TouchAction action, int x, int y, bool landscape = false) {
  // Small finger motion at a border should not discard an otherwise valid tap.
  // Capture starts inside the drawn button; adjacent controls remain separate.
  constexpr int slop = 8;
  for (size_t i = 0; i < touchControlCount; ++i) {
    const auto& c = touchControlsFor(landscape)[i];
    if (c.action == action)
      return x >= c.x-slop && x < c.x+c.w+slop &&
             y >= c.y-slop && y < c.y+c.h+slop;
  }
  return false;
}
class TouchTap {
 public:
  void reset() { armed_ = TouchAction::None; }
  TouchAction accept(TouchEvent e, bool landscape = false) {
    if (landscape != landscape_) { reset(); landscape_ = landscape; }
    const auto point = waveshare_display::fromPanel(e.x, e.y, landscape);
    e.x = point.x; e.y = point.y;
    if (e.phase == TouchPhase::Down) {
      armed_ = touchHitTest(e.x, e.y, landscape); id_ = e.id;
      // Directions are reversible navigation. Emit one step immediately,
      // without a second step on release or movement into another button.
      if (touchActionIsDirection(armed_)) {
        const auto action = armed_; reset(); return action;
      }
    } else if (e.phase == TouchPhase::Cancel || e.id != id_ ||
               !touchWithinControl(armed_, e.x, e.y, landscape)) reset();
    else if (e.phase == TouchPhase::Up) {
      const auto action = armed_; reset(); return action;
    }
    return TouchAction::None;
  }
 private:
  TouchAction armed_ = TouchAction::None;
  uint8_t id_ = 0;
  bool landscape_ = false;
};
