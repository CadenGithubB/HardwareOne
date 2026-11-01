#include "System_BuildConfig.h"
#include "OLED_Touch.h"
#if ENABLE_GT911_TOUCH && ENABLE_OLED_DISPLAY
#include "HAL_Display.h"
#include "OLED_TouchLayout.h"
#include <Adafruit_GFX.h>
#include <cstring>
namespace {
void drawControls(Adafruit_GFX& canvas) {
  canvas.setTextSize(3);
  canvas.setTextColor(0xffff);
  for (const auto& c : touchControls) {
    canvas.drawRoundRect(c.x, c.y, c.w, c.h, 8, 0xffff);
    canvas.setCursor(c.x + (c.w - strlen(c.label)*18)/2, c.y+27);
    canvas.print(c.label);
  }
}
}
bool oledTouchDrawControls() { return displayPaintSurface(drawControls); }
#else
bool oledTouchDrawControls() { return false; }
#endif
