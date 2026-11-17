// Drives the shipping bounded text field (OLED_TextFieldCore.h compiled
// directly, OLED_TextField.cpp's implementation spliced in) over a pixel
// recorder, at each canvas geometry. Every check is on pixel writes: nothing
// outside the field or the content area, glyphs exactly where the policy puts
// them, no trails, and frames requested only when a visible animated field
// is due to move. Assembled by test_oled_text_field.py (ASan/UBSan).
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <utility>

#include "OLED_TextFieldCore.h"
#include "glcdfont.c"
#include "oled_text_field_gfx_double.h"

#define DISPLAY_WIDTH 128
#define OLED_CONTENT_START_Y (HW1_HEADER + 1)
#define OLED_CONTENT_HEIGHT (HW1_CANVAS_HEIGHT - HW1_HEADER - HW1_FOOTER - 1)
enum OledMode { OLED_MENU, OLED_SETTINGS };
static OledMode currentOledMode = OLED_MENU;
static uint32_t gNow = 10000;
static uint32_t millis() { return gNow; }

// INSERT_PRODUCTION

using namespace oled_text;
using Pixel = std::pair<int, int>;

struct Canvas : Adafruit_GFX {
  std::set<Pixel> on, off;
  Canvas() : Adafruit_GFX(DISPLAY_WIDTH, HW1_CANVAS_HEIGHT) {}
  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    assert(x >= 0 && x < DISPLAY_WIDTH && y >= 0 && y < HW1_CANVAS_HEIGHT);
    (color ? on : off).insert({x, y});
  }
};

// Reference rendering: the lit pixels of `text` drawn at (x, y), kept inside
// `clip` = {x0, y0, x1, y1}.
static std::set<Pixel> glyphsAt(const char* text, int x, int y, const int clip[4]) {
  std::set<Pixel> px;
  for (size_t i = 0; text[i]; ++i) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    if (c >= 176) c++;
    for (int col = 0; col < 5; ++col) {
      uint8_t line = font[c * 5 + col];
      for (int row = 0; row < 8; ++row, line >>= 1) {
        if (!(line & 1)) continue;
        const int px_ = x + static_cast<int>(i) * 6 + col, py = y + row;
        if (px_ >= clip[0] && px_ < clip[2] && py >= clip[1] && py < clip[3]) px.insert({px_, py});
      }
    }
  }
  return px;
}

static const int kTop = OLED_CONTENT_START_Y;
static const int kBottom = OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT;

static bool inside(const std::set<Pixel>& px, int x0, int y0, int x1, int y1) {
  for (const auto& p : px) {
    if (p.first < x0 || p.first >= x1 || p.second < y0 || p.second >= y1) return false;
  }
  return true;
}

// One frame: bracket, draw the field, bracket.
static Canvas frame(int x, int y, int w, const char* text, bool animate, const void* owner,
                    uint16_t fg = 1, uint16_t bg = 1) {
  Canvas c;
  oledTextFieldFrameBegin();
  oledTextFieldDraw(&c, x, y, w, text, fg, bg, animate, owner);
  oledTextFieldFrameEnd();
  return c;
}

static void checkCore() {
  char out[32];
  assert(ellipsize("abcdefgh", 8, 6, out, sizeof(out)) == 6 && std::string(out) == "abc...");
  assert(ellipsize("abcdefgh", 8, 4, out, sizeof(out)) == 4 && std::string(out) == "a...");
  assert(ellipsize("abcdefgh", 8, 3, out, sizeof(out)) == 3 && std::string(out) == "...");
  assert(ellipsize("abcdefgh", 8, 2, out, sizeof(out)) == 2 && std::string(out) == "..");
  assert(ellipsize("abcdefgh", 8, 1, out, sizeof(out)) == 1 && std::string(out) == ".");
  assert(ellipsize("abcdefgh", 8, 0, out, sizeof(out)) == 0 && out[0] == '\0');
  assert(ellipsize("ab", 2, 4, out, sizeof(out)) == 4 && std::string(out) == "a...");  // only ever asked for overflow
  char tiny[3];
  assert(ellipsize("abcdefgh", 8, 6, tiny, sizeof(tiny)) == 2 && std::string(tiny) == "ab");
  bool cut = false;
  assert(flatten("a\tb\r\nc\x1b[1m d\xc3\xa9", out, sizeof(out), &cut) == 8 && std::string(out) == "a b c de" && !cut);
  assert(flatten("\xe2\x00tail", out, sizeof(out)) == 0 && out[0] == '\0');
  std::string longText(300, 'q');
  char label[kLabelMax];
  assert(flatten(longText.c_str(), label, sizeof(label), &cut) == kLabelMax - 1 && cut);
  assert(glyphsThatFit(0) == 0 && glyphsThatFit(5) == 0 && glyphsThatFit(6) == 1 && glyphsThatFit(116) == 19);
  // Timing: initial pause, 20 px/s, end pause, repeat; next-change bookkeeping.
  MarqueePhase p = marqueePhase(0, 100);
  assert(p.offsetPx == 0 && p.nextChangeInMs == 1000);
  p = marqueePhase(999, 100);
  assert(p.offsetPx == 0 && p.nextChangeInMs == 1);
  p = marqueePhase(1000, 100);
  assert(p.offsetPx == 0 && p.nextChangeInMs == 50);
  p = marqueePhase(1075, 100);
  assert(p.offsetPx == 1 && p.nextChangeInMs == 25);
  p = marqueePhase(1000 + 100 * 50 - 1, 100);
  assert(p.offsetPx == 99 && p.nextChangeInMs == 1);
  p = marqueePhase(1000 + 100 * 50, 100);
  assert(p.offsetPx == 100 && p.nextChangeInMs == 1500);
  p = marqueePhase(1000 + 100 * 50 + 1499, 100);
  assert(p.offsetPx == 100 && p.nextChangeInMs == 1);
  p = marqueePhase(1000 + 100 * 50 + 1500, 100);
  assert(p.offsetPx == 0 && p.nextChangeInMs == 1000);  // the cycle repeats from the start
  assert(marqueePhase(123, 0).offsetPx == 0);
  // Slot table: identity by owner/hash/width, two slots, LRU eviction.
  Marquee m;
  const MarqueeKey a{&m, 1, 60}, b{&m, 2, 60}, c{&m, 3, 60};
  m.beginFrame(0);
  assert(m.offset(a, 100, 0) == 0 && m.offset(b, 100, 0) == 0);
  m.endFrame();
  m.beginFrame(1000);
  assert(m.offset(c, 100, 1000) == 0);  // evicts the least recently claimed
  m.endFrame();
  assert(m.activeCount() == 1);
  m.reset();
  assert(m.activeCount() == 0 && !m.needsFrame(5000));
}

static void checkDraw() {
  static int tag;
  const int y = kTop + 8;

  // Empty and whitespace-only text draws nothing and requests nothing.
  Canvas c = frame(10, y, 60, "", false, &tag);
  assert(c.on.empty() && c.off.empty() && !oledTextFieldNeedsFrame());
  c = frame(10, y, 60, "\n\t", true, &tag);
  assert(c.on.empty() && !oledTextFieldNeedsFrame());
  c = frame(10, y, 0, "abc", true, &tag);
  assert(c.on.empty());

  // Fitting and exact-fit text: the same pixels print() would draw, static.
  int clip[4] = {10, y, 10 + 18, y + 8};
  c = frame(10, y, 18, "abc", true, &tag);
  assert(c.on == glyphsAt("abc", 10, y, clip) && c.off.empty() && !oledTextFieldNeedsFrame());
  const int wide[4] = {0, kTop, DISPLAY_WIDTH, kBottom};
  c = frame(10, y, 60, "abc", false, &tag);
  assert(c.on == glyphsAt("abc", 10, y, wide));

  // One glyph too long: ellipsis when static, never a write outside the field.
  c = frame(10, y, 18, "abcd", false, &tag);
  assert(c.on == glyphsAt("...", 10, y, clip) && inside(c.on, 10, y, 28, y + 8));
  c = frame(10, y, 24, "abcde", false, &tag);
  int clip4[4] = {10, y, 34, y + 8};
  assert(c.on == glyphsAt("a...", 10, y, clip4) && !oledTextFieldNeedsFrame());
  c = frame(10, y, 12, "abcde", false, &tag);
  int clip2[4] = {10, y, 22, y + 8};
  assert(c.on == glyphsAt("..", 10, y, clip2));
  c = frame(10, y, 5, "abcde", false, &tag);
  assert(c.on.empty());  // no whole cell: nothing, and nothing outside
  // A tiny animated viewport draws only the visible columns of the first glyph.
  c = frame(10, y, 3, "abcde", true, &tag);
  int clip3[4] = {10, y, 13, y + 8};
  assert(c.on == glyphsAt("abcde", 10, y, clip3) && inside(c.on, 10, y, 13, y + 8));

  // Opaque cells (inverse rows) stay inside the field too.
  c = frame(10, y, 18, "ab", 0 /*fg*/, &tag, 0, 1);
  assert(inside(c.on, 10, y, 28, y + 8) && inside(c.off, 10, y, 28, y + 8) && !c.on.empty());
  c = frame(10, y, 18, "abcd", true, &tag, 0, 1);
  assert(inside(c.on, 10, y, 28, y + 8) && inside(c.off, 10, y, 28, y + 8));

  // Content-area intersection: rows that would cross the chrome are clipped.
  c = frame(10, kBottom - 4, 60, "abc", false, &tag);
  int low[4] = {10, kBottom - 4, 70, kBottom};
  assert(c.on == glyphsAt("abc", 10, kBottom - 4, low) && inside(c.on, 10, kBottom - 4, 70, kBottom));
  c = frame(10, kTop - 5, 60, "abc", false, &tag);
  int high[4] = {10, kTop, 70, kTop + 3};
  assert(c.on == glyphsAt("abc", 10, kTop - 5, high));
  c = frame(120, y, 60, "abc", false, &tag);  // past the right edge
  int edge[4] = {120, y, DISPLAY_WIDTH, y + 8};
  assert(c.on == glyphsAt("abc", 120, y, edge) && !c.on.empty());
  c = frame(-12, y, 24, "abcd", false, &tag);  // starts left of the canvas
  int left[4] = {0, y, 12, y + 8};
  assert(c.on == glyphsAt("abcd", -12, y, left) && !c.on.empty());

  // Hostile input never breaks measurement or drawing.
  c = frame(10, y, 60, "a\tb\nc", false, &tag);
  assert(c.on == glyphsAt("a b c", 10, y, wide));
  c = frame(10, y, 60, "\xe2\x82", false, &tag);
  assert(c.on.empty());
  std::string huge(400, 'w');
  c = frame(10, y, 60, huge.c_str(), false, &tag);
  assert(c.on == glyphsAt("wwwwwww...", 10, y, wide));
}

static void checkMarquee() {
  static int list, other;
  const int y = kTop + 16, x = 12, w = 60;  // ten cells
  const std::string text = "0123456789abcdefghijklmnopqrst";  // 30 glyphs: 180 px, overflow 120
  const int overflow = 180 - w;
  int field[4] = {x, y, x + w, y + 8};
  auto expect = [&](int offset) { return glyphsAt(text.c_str(), x - offset, y, field); };

  gNow = 50000;
  const uint32_t t0 = gNow;
  Canvas c = frame(x, y, w, text.c_str(), true, &list);
  assert(c.on == expect(0) && inside(c.on, x, y, x + w, y + 8));
  // Waiting for the initial pause does not request frames until it ends.
  gNow = t0 + 999;
  assert(!oledTextFieldNeedsFrame());
  gNow = t0 + 1000;
  assert(oledTextFieldNeedsFrame());
  c = frame(x, y, w, text.c_str(), true, &list);
  assert(c.on == expect(0) && !oledTextFieldNeedsFrame());
  // Elapsed time sets the position, whatever the frame pacing: 20 px/s.
  gNow = t0 + 1000 + 75;  // fractional glyph offset
  assert(oledTextFieldNeedsFrame());
  c = frame(x, y, w, text.c_str(), true, &list);
  assert(c.on == expect(1) && inside(c.on, x, y, x + w, y + 8));
  gNow = t0 + 1000 + 50 * 37;  // a skipped stretch of frames
  c = frame(x, y, w, text.c_str(), true, &list);
  assert(c.on == expect(37) && inside(c.on, x, y, x + w, y + 8));
  // The end position, then the end pause without frames, then the repeat. A
  // live screen keeps drawing on the way there (a field unseen for longer
  // than the stale window restarts, which the view-change case covers).
  while (gNow + 2000 < t0 + 1000 + 50 * overflow) {
    gNow += 2000;
    c = frame(x, y, w, text.c_str(), true, &list);
    assert(c.on == expect(static_cast<int>((gNow - t0 - 1000) / 50)));
  }
  gNow = t0 + 1000 + 50 * overflow;
  c = frame(x, y, w, text.c_str(), true, &list);
  assert(c.on == expect(overflow) && inside(c.on, x, y, x + w, y + 8));
  gNow += 1499;
  assert(!oledTextFieldNeedsFrame());
  gNow += 1;
  assert(oledTextFieldNeedsFrame());
  c = frame(x, y, w, text.c_str(), true, &list);
  assert(c.on == expect(0));
  // No trails: a frame holds only the current position's pixels.
  gNow += 1000 + 50 * 5;
  c = frame(x, y, w, text.c_str(), true, &list);
  assert(c.on == expect(5));

  // Routine rebuilds and unrelated value columns keep the progress: the same
  // owner/text/width in the next frame continues; a static field beside it
  // changes nothing.
  gNow += 50;
  Canvas two;
  oledTextFieldFrameBegin();
  oledTextFieldDraw(&two, x + w + 6, y, 40, "value 1", 1, 1, false, &list);
  oledTextFieldDraw(&two, x, y, w, text.c_str(), 1, 1, true, &list);
  oledTextFieldFrameEnd();
  {
    std::set<Pixel> label;
    for (const auto& p : two.on) if (p.first < x + w) label.insert(p);
    assert(label == expect(6));
  }

  // A different selection (text), width or owner restarts from the beginning.
  gNow += 50;
  c = frame(x, y, w, "another long label that overflows", true, &list);
  int fieldW[4] = {x, y, x + w, y + 8};
  assert(c.on == glyphsAt("another long label that overflows", x, y, fieldW));
  gNow += 2000;
  c = frame(x, y, w + 6, text.c_str(), true, &list);
  int fieldW2[4] = {x, y, x + w + 6, y + 8};
  assert(c.on == glyphsAt(text.c_str(), x, y, fieldW2));
  gNow += 2000;
  c = frame(x, y, w, text.c_str(), true, &other);
  assert(c.on == expect(0));
  assert(!oledTextFieldNeedsFrame());

  // Unselected rows never animate: a static field claims no slot.
  gNow += 5000;
  c = frame(x, y, w, text.c_str(), false, &list);
  assert(c.on == glyphsAt("0123456...", x, y, field) && !oledTextFieldNeedsFrame());

  // A view that stops drawing the field stops its frames; drawing it again
  // after the stale window restarts the initial pause.
  gNow += 100;
  c = frame(x, y, w, text.c_str(), true, &list);
  gNow += 1200;
  assert(oledTextFieldNeedsFrame());
  Canvas blank;
  oledTextFieldFrameBegin();
  oledTextFieldFrameEnd();
  assert(!oledTextFieldNeedsFrame());
  gNow += 5000;
  c = frame(x, y, w, text.c_str(), true, &list);
  assert(c.on == expect(0));

  // A modal pause freezes the position and stops frames; resuming continues
  // from where it stood.
  gNow += 1000 + 50 * 10;
  c = frame(x, y, w, text.c_str(), true, &list);
  assert(c.on == expect(10));
  oledTextFieldSetPaused(true);
  gNow += 7000;
  assert(!oledTextFieldNeedsFrame());
  c = frame(x, y, w, text.c_str(), true, &list);
  assert(c.on == expect(10));
  oledTextFieldSetPaused(false);
  assert(!oledTextFieldNeedsFrame());
  gNow += 50;
  assert(oledTextFieldNeedsFrame());
  c = frame(x, y, w, text.c_str(), true, &list);
  assert(c.on == expect(11));

  // Mode and identity changes clear everything in flight.
  currentOledMode = OLED_SETTINGS;
  gNow += 50;
  c = frame(x, y, w, text.c_str(), true, &list);
  assert(c.on == expect(0));
  currentOledMode = OLED_MENU;
  gNow += 50;
  c = frame(x, y, w, text.c_str(), true, &list);
  assert(c.on == expect(0));
  gNow += 1500;
  c = frame(x, y, w, text.c_str(), true, &list);
  assert(c.on == expect(10));
  oledTextFieldReset();
  gNow += 50;
  c = frame(x, y, w, text.c_str(), true, &list);
  assert(c.on == expect(0));

  // Two declared lines of one item animate together; a third field waits.
  gNow += 3000;
  Canvas lines;
  oledTextFieldFrameBegin();
  oledTextFieldDraw(&lines, x, y, w, text.c_str(), 1, 1, true, &list);
  oledTextFieldDraw(&lines, x, y + 8, w, "second line that is also too long", 1, 1, true, &list);
  oledTextFieldFrameEnd();
  gNow += 1000;
  assert(oledTextFieldNeedsFrame());
  gNow += 100;
  Canvas moved;
  oledTextFieldFrameBegin();
  oledTextFieldDraw(&moved, x, y, w, text.c_str(), 1, 1, true, &list);
  oledTextFieldDraw(&moved, x, y + 8, w, "second line that is also too long", 1, 1, true, &list);
  oledTextFieldFrameEnd();
  {
    std::set<Pixel> first, second;
    for (const auto& p : moved.on) (p.second < y + 8 ? first : second).insert(p);
    int row2[4] = {x, y + 8, x + w, y + 16};
    assert(first == expect(2));
    assert(second == glyphsAt("second line that is also too long", x - 2, y + 8, row2));
  }

  // Millisecond rollover: a field started before the wrap keeps moving after it.
  oledTextFieldReset();
  gNow = UINT32_MAX - 600;
  c = frame(x, y, w, text.c_str(), true, &list);
  assert(c.on == expect(0));
  gNow += 1000 + 50 * 20;  // wraps past zero
  assert(oledTextFieldNeedsFrame());
  c = frame(x, y, w, text.c_str(), true, &list);
  assert(c.on == expect(20));
  gNow = 10000;
}

int main() {
  checkCore();
  checkDraw();
  checkMarquee();
  printf("canvas 128x%d: text field bounds, ellipsis, marquee timing, continuity, pause and reset passed\n",
         HW1_CANVAS_HEIGHT);
}
