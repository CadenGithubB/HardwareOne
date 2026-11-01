#include "OLED_Touch.h"
#include "HAL_Input.h"
#if ENABLE_GT911_TOUCH && ENABLE_OLED_DISPLAY
#include "OLED_TouchLayout.h"
#include "System_SetupWizard.h"
namespace {
TouchTap tap;
uint32_t context = UINT32_MAX;
TouchNavigation pending;
TouchNavigation next() {
  TouchNavigation n;
  TouchEvent e;
  while (inputTouchConsume(e)) {
    const auto action = tap.accept(e);
    switch (action) {
      case TouchAction::Up: n.up=true; break;
      case TouchAction::Down: n.down=true; break;
      case TouchAction::Left: n.left=true; break;
      case TouchAction::Right: n.right=true; break;
      case TouchAction::Select: n.buttons=INPUT_MASK(INPUT_BUTTON_A); break;
      case TouchAction::Back: n.buttons=INPUT_MASK(INPUT_BUTTON_B); break;
      case TouchAction::Submit: n.buttons=INPUT_MASK(INPUT_BUTTON_X); break;
      case TouchAction::Delete: n.buttons=INPUT_MASK(INPUT_BUTTON_Y); break;
      case TouchAction::Menu: n.buttons=INPUT_MASK(INPUT_BUTTON_START); break;
      case TouchAction::Function: n.buttons=INPUT_MASK(INPUT_BUTTON_SELECT); break;
      default: break;
    }
    if (action != TouchAction::None) break;
  }
  return n;
}
void wizardPump() {
  oledTouchContext(0x10000u + static_cast<uint32_t>(getWizardCurrentPage()));
  if (!pending.buttons && !pending.up && !pending.down && !pending.left && !pending.right) pending = next();
}
}
void oledTouchContext(uint32_t value) {
  if (context == value) return;
  context = value; tap.reset(); pending = {};
  inputTouchDiscard();
}
TouchNavigation oledTouchRead() { return next(); }
uint32_t oledWizardButtonPresses(uint32_t& previous, bool& initialized) {
  wizardPump();
  uint32_t buttons = pending.buttons; pending.buttons = 0;
  initialized = true; previous = UINT32_MAX;
  return buttons;
}
TouchNavigation oledWizardTouchNavigation() {
  wizardPump();
  TouchNavigation n = pending;
  n.buttons = 0;
  pending.up=pending.down=pending.left=pending.right=false;
  return n;
}
#else
void oledTouchContext(uint32_t) {}
TouchNavigation oledTouchRead() { return {}; }
uint32_t oledWizardButtonPresses(uint32_t& previous, bool& initialized) {
  return inputConsumeButtonPresses(previous, initialized);
}
TouchNavigation oledWizardTouchNavigation() { return {}; }
#endif
