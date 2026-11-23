// =============================================================================
// OLED_TextField — bounded one-row text fields (drawing side)
// =============================================================================
// See OLED_TextField.h. Glyphs are drawn through a stack-local clipping
// adapter that forwards drawPixel only inside the field rectangle, so the
// vendor's classic-font glyph loop does the drawing (same pixels as print())
// without write()'s wrapping and without touching the display's text state.
// Works on every backend that derives from Adafruit_GFX, including the P4X-EYE
// and Waveshare software canvases.

#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Arduino.h>
#include <Adafruit_GFX.h>

#include "HAL_Display.h"
#include "OLED_Display.h"  // currentOledMode
#include "OLED_TextField.h"
#include "OLED_TextFieldCore.h"

// BEGIN_TEXT_FIELD_IMPL — host tests splice from here to END_TEXT_FIELD_IMPL.
// The live canvas width (64 in portrait); a harness that only names the
// panel's width clips to that.
#ifndef SCREEN_WIDTH
#define SCREEN_WIDTH DISPLAY_WIDTH
#endif
namespace {

// Forwards drawPixel inside one rectangle. Adafruit_GFX::drawChar clips to the
// adapter's own width/height, so it is given the display's, and glyphs that
// start left of the field still draw their visible columns.
class ClipGfx : public Adafruit_GFX {
 public:
  ClipGfx(Adafruit_GFX& out, int x0, int y0, int x1, int y1)
      : Adafruit_GFX(out.width(), out.height()), out_(out), x0_(x0), y0_(y0), x1_(x1), y1_(y1) {}
  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if (x >= x0_ && x < x1_ && y >= y0_ && y < y1_) out_.drawPixel(x, y, color);
  }
  int left() const { return x0_; }
  int right() const { return x1_; }

 private:
  Adafruit_GFX& out_;
  int x0_, y0_, x1_, y1_;
};

oled_text::Marquee sMarquee;
int sFrameMode = -1;  // currentOledMode of the last frame; a change resets the table

void drawRun(ClipGfx& clip, int x, int y, const char* glyphs, size_t count,
             uint16_t fg, uint16_t bg) {
  for (size_t i = 0; i < count; ++i) {
    const int gx = x + static_cast<int>(i) * oled_text::kGlyphW;
    if (gx >= clip.right()) break;
    if (gx + oled_text::kGlyphW <= clip.left()) continue;
    clip.drawChar(static_cast<int16_t>(gx), static_cast<int16_t>(y),
                  static_cast<unsigned char>(glyphs[i]), fg, bg, 1);
  }
}

}  // namespace

void oledTextFieldDraw(Adafruit_GFX* display, int x, int y, int w, const char* text,
                       uint16_t fg, uint16_t bg, bool animate, const void* owner) {
  if (!display || w <= 0) return;
  // The field, intersected with the content area: nothing escapes into another
  // row, a pane, the chrome or past the canvas.
  const int top = OLED_CONTENT_START_Y;
  const int bottom = OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT;
  const int cx0 = x > 0 ? x : 0;
  const int cx1 = (x + w < SCREEN_WIDTH) ? x + w : SCREEN_WIDTH;
  const int cy0 = y > top ? y : top;
  const int cy1 = (y + oled_text::kGlyphH < bottom) ? y + oled_text::kGlyphH : bottom;
  if (cx0 >= cx1 || cy0 >= cy1) return;

  char label[oled_text::kLabelMax];
  const size_t n = oled_text::flatten(text, label, sizeof(label));
  if (n == 0) return;
  ClipGfx clip(*display, cx0, cy0, cx1, cy1);

  const int textW = oled_text::textWidthPx(n);
  if (textW <= w) {
    drawRun(clip, x, y, label, n, fg, bg);
    return;
  }
  if (!animate) {
    char shown[oled_text::kLabelMax];
    const size_t m = oled_text::ellipsize(label, n, oled_text::glyphsThatFit(w), shown, sizeof(shown));
    drawRun(clip, x, y, shown, m, fg, bg);
    return;
  }
  const oled_text::MarqueeKey key{owner, oled_text::labelHash(label, n), static_cast<uint16_t>(w)};
  const int offset = sMarquee.offset(key, textW - w, millis());
  drawRun(clip, x - offset, y, label, n, fg, bg);
}

void oledTextFieldFrameBegin() {
  const int mode = static_cast<int>(currentOledMode);
  if (mode != sFrameMode) {
    sMarquee.reset();
    sFrameMode = mode;
  }
  sMarquee.beginFrame(millis());
}

void oledTextFieldFrameEnd() { sMarquee.endFrame(); }

bool oledTextFieldNeedsFrame() { return sMarquee.needsFrame(millis()); }

void oledTextFieldSetPaused(bool paused) { sMarquee.setPaused(paused, millis()); }

void oledTextFieldReset() { sMarquee.reset(); }
// END_TEXT_FIELD_IMPL

#endif  // ENABLE_OLED_DISPLAY
