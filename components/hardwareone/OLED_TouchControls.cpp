#include "System_BuildConfig.h"
#include "OLED_Touch.h"
#if ENABLE_GT911_TOUCH && ENABLE_OLED_DISPLAY
#include "HAL_Display.h"
#include "OLED_TouchLayout.h"
#include <Adafruit_GFX.h>
#include <cstring>
namespace {
void drawControls(Adafruit_GFX& canvas) {
  const bool landscape = displayIsLandscape();
  constexpr int textSize = 3;
  canvas.setTextSize(textSize);
  canvas.setTextColor(0xffff);
  for (size_t i = 0; i < touchControlCount; ++i) {
    const auto& c = touchControlsFor(landscape)[i];
    canvas.drawRoundRect(c.x, c.y, c.w, c.h, 8, 0xffff);
    canvas.setCursor(c.x + (c.w - strlen(c.label)*6*textSize)/2, c.y + (c.h - 8*textSize)/2);
    canvas.print(c.label);
  }
}
}
bool oledTouchDrawControls() { return displayPaintSurface(drawControls); }
#else
bool oledTouchDrawControls() { return false; }
#endif
