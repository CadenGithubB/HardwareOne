// The OLED's runtime rotation without the hardware: the layout helpers and
// portrait grid (OLED_Layout.h), the footer chips, the portrait title cut,
// the keyboard's re-wrapped grid, and how input directions follow the screen
// and the device's mount (Input_RotationCore.h).

#include <cassert>
#include <cstdio>
#include <cstring>

#include "Input_RotationCore.h"
#include "OLED_HeaderCore.h"
#include "OLED_Layout.h"

namespace {

using namespace oled_layout;
namespace P = oled_layout::portrait;

void testRotationValues() {
  assert(rotationFromDegrees(0) == 0 && rotationFromDegrees(90) == 1);
  assert(rotationFromDegrees(180) == 2 && rotationFromDegrees(270) == 3);
  assert(rotationFromDegrees(45) == -1 && rotationFromDegrees(-90) == -1 && rotationFromDegrees(360) == -1);
  for (int r = 0; r < 4; ++r) assert(rotationFromDegrees(degreesFromRotation(r)) == r);
  assert(!rotationIsPortrait(0) && rotationIsPortrait(1) && !rotationIsPortrait(2) && rotationIsPortrait(3));
}

void testGeometry() {
  const Geometry land = geometryFor(128, 64);
  assert(!land.portrait && land.width == 128 && land.height == 64);
  assert(land.headerHeight == 10 && land.contentY == 11 && land.contentHeight == 43);
  assert(land.headerHeight + land.contentHeight == 53);  // the footer rule
  assert(land.cols == 21 && land.contentBottom == 52);
  const Geometry tall = geometryFor(64, 128);
  assert(tall.portrait && tall.cols == 10);
  assert(tall.contentY == P::kContentY && tall.contentBottom == P::kContentBottom);
  assert(tall.headerHeight + tall.contentHeight == P::kFooterRuleY);
  // The P4 canvases (128 x 128, 128 x 88) are not portrait: they keep the
  // landscape bars and grow the content.
  const Geometry square = geometryFor(128, 128);
  assert(!square.portrait && square.contentHeight == 128 - 21);
  // Rows a list gets: the landscape rule (baseline kept) and portrait rows.
  assert(visibleRows(land.contentHeight, 10, 1, 4) == 4);
  assert(visibleRows(tall.contentHeight, 10, 1, 4) == 8);
}

void testTextMetrics() {
  assert(textWidth(0) == 0 && textWidth(1) == 5 && textWidth(10) == 59 && textWidth(5, 2) == 58);
  assert(cellsIn(64) == 10 && cellsIn(128) == 21 && cellsIn(64, 2) == 5 && cellsIn(-3) == 0);
  // Right-aligned to x59: 3 glyphs start at x43 and end on x59.
  assert(rightAlignX(3, 59) == 43 && rightAlignX(3, 59) + textWidth(3) - 1 == 59);
  // The hero value is ink-centred: 5 size-2 glyphs (58 px) at x3, 3 at x15.
  assert(P::heroValueX(5) == 3 && P::heroValueX(5) + textWidth(5, 2) - 1 == 60);
  assert(P::heroValueX(3) == 15);
  assert(P::heroUnitX(3) == 23);
  // Every portrait footer verb (3 glyphs) ends by x57.
  assert(P::kChipRightX + P::kChipW + 1 + textWidth(P::kVerbMax) - 1 <= 57);
}

void testGauge() {
  assert(gaugeFill(0, 2000, P::kGaugeFillSpan) == 0);
  assert(gaugeFill(-5, 2000, P::kGaugeFillSpan) == 0);
  assert(gaugeFill(842, 2000, P::kGaugeFillSpan) == 31);
  assert(gaugeFill(2000, 2000, P::kGaugeFillSpan) == 76);
  assert(gaugeFill(5000, 2000, P::kGaugeFillSpan) == 76);
  assert(gaugeFill(10, 0, 76) == 0);
}

void testScrollThumb() {
  // The design's worked examples: Home (6 rows, 5 visible, 49 px) and the
  // Settings list (8 items, 5 visible, 85 px).
  Thumb home = scrollThumb(57, 49, 5, 6, 0);
  assert(home.y == 57 && home.h == 41);
  Thumb sensors = scrollThumb(57, 49, 5, 14, 1);
  assert(sensors.h == 18 && sensors.y == 61);
  Thumb settings = scrollThumb(21, 85, 5, 8, 0);
  assert(settings.y == 21 && settings.h == 53);
  // The last page ends on the span's last row; a tiny share is 6 px.
  Thumb last = scrollThumb(21, 85, 5, 100, 95);
  assert(last.h == 6 && last.y + last.h == 21 + 85);
  // Nothing to scroll: the whole track.
  Thumb all = scrollThumb(21, 85, 5, 3, 0);
  assert(all.y == 21 && all.h == 85);
}

void testKvRows() {
  assert(kvRows(4, 5, 10) == 1);   // "Heap 142K"
  assert(kvRows(4, 7, 10) == 2);   // "WiFi HomeNet" stacks
  assert(kvRows(4, 7, 21) == 1);
}

void testFooterChips() {
  FooterChips c;
  parseFooterHints("A:Select B:Back", c);
  assert(!strcmp(c.verb[kSlotA], "Sel") && !strcmp(c.verb[kSlotB], "Bck"));
  assert(!c.verb[kSlotX][0] && !c.verb[kSlotY][0]);
  parseFooterHints("A:Mute Y:Vol X:Pwr B:Back", c);
  assert(!strcmp(c.verb[kSlotA], "Mut") && !strcmp(c.verb[kSlotY], "Vol"));
  assert(!strcmp(c.verb[kSlotX], "Pwr") && !strcmp(c.verb[kSlotB], "Bck"));
  // Arrow tokens and START are not drawn; an unknown verb keeps 3 glyphs.
  parseFooterHints("\x18\x19:Scroll S:OK X:Broadcast Y:Whatever", c);
  assert(!c.verb[kSlotA][0] && !c.verb[kSlotB][0]);
  assert(!strcmp(c.verb[kSlotX], "All") && !strcmp(c.verb[kSlotY], "Wha"));
  // Cancel is Esc, Start is Go, Done is OK, and the first token of a letter wins.
  parseFooterHints("A:Done B:Cancel X:Start X:Stop", c);
  assert(!strcmp(c.verb[kSlotA], "OK") && !strcmp(c.verb[kSlotB], "Esc") && !strcmp(c.verb[kSlotX], "Go"));
  parseFooterHints(nullptr, c);
  for (int i = 0; i < kSlotCount; ++i) assert(!c.verb[i][0]);
  // Slots are fixed: A/X left, B/Y right; A/B row 1, X/Y row 2.
  assert(chipX(kSlotA) == 0 && chipX(kSlotY) == 33 && chipY(kSlotB) == 109 && chipY(kSlotX) == 119);
}

void testPortraitTitle() {
  char out[16];
  int mark = 0;
  // Fits whole.
  assert(fitPortraitTitle("Set>", 4, "oled", 4, 10, false, out, mark) == 8 && !strcmp(out, "Set>oled") && mark == -1);
  // Leaf-first: "Menu>Hardware>Sensors" -> "Sensors".
  fitPortraitTitle("Menu>", 5, "Hardware>Sensors", 16, 10, false, out, mark);
  assert(!strcmp(out, "Sensors") && mark == -1);
  // Prefix dropped when only the leaf fits.
  fitPortraitTitle("Menu>", 5, "Hardware", 8, 10, false, out, mark);
  assert(!strcmp(out, "Hardware"));
  // Too long: 9 glyphs and the one-cell mark at cell 9.
  fitPortraitTitle("Set>", 4, "notifications", 13, 10, false, out, mark);
  assert(!strncmp(out, "notificat", 9) && mark == 9 && strlen(out) == 10);
  // A path keeps its tail behind the mark.
  fitPortraitTitle("Files>", 6, "/logs/system/boot", 17, 10, true, out, mark);
  assert(mark == 0 && !strcmp(out + 1, "stem/boot"));
}

void testKeyboardGrid() {
  // Landscape: 10 x 3, wrapping at the edges (as before).
  int col = 9, row = 0;
  keyboardStep(col, row, 1, 0, 10, 3, false);
  assert(col == 0 && row == 0);
  keyboardStep(col, row, 0, -1, 10, 3, false);
  assert(col == 0 && row == 2);
  // Portrait: the same 30 keys in 5 x 6. 'h' (row 0, col 7) is drawn at
  // column 2 of row 1; down moves 5 keys on.
  col = 7; row = 0;
  keyboardStep(col, row, 0, 1, 10, 3, true);
  assert(row * 10 + col == 12);
  keyboardStep(col, row, 1, 0, 10, 3, true);
  assert(row * 10 + col == 13);
  // Right from the drawn row's last key wraps to its first.
  col = 4; row = 0;
  keyboardStep(col, row, 1, 0, 10, 3, true);
  assert(row == 0 && col == 0);
  // Up from the top wraps to the drawn grid's last row (keys 25-29).
  col = 2; row = 0;
  keyboardStep(col, row, 0, -1, 10, 3, true);
  assert(row * 10 + col == 27);
}

void testInputFollowsTheScreen() {
  using namespace input_rotation;
  // Rotation 1 shows the panel's right edge at the top.
  assert(screenDir(kRight, 1) == kUp && screenDir(kDown, 1) == kRight);
  assert(screenDir(kLeft, 3) == kUp && screenDir(kUp, 2) == kDown);
  for (int r = 0; r < 4; ++r)
    for (int d = 0; d < 4; ++d) assert(screenDir(static_cast<Dir>(d), r) == static_cast<Dir>((d - r + 4) % 4));
  // The joystick (y grows up): pushed toward the panel's right at rotation 1
  // is up on the screen; at 3 it is down.
  int x = 512 + 300, y = 512;
  rotateStick(1, 512, x, y);
  assert(x == 512 && y == 812);
  x = 812; y = 512;
  rotateStick(3, 512, x, y);
  assert(x == 512 && y == 212);
  x = 600; y = 400;
  rotateStick(2, 512, x, y);
  assert(x == 424 && y == 624);
  x = 600; y = 400;
  rotateStick(0, 512, x, y);
  assert(x == 600 && y == 400);
  // Four quarter turns are the identity.
  x = 700; y = 300;
  for (int i = 0; i < 4; ++i) rotateStick(1, 512, x, y);
  assert(x == 700 && y == 300);
  // Direction bits move with their direction; other bits pass.
  const uint32_t U = 1, R = 2, D = 4, L = 8, IN = 16;
  assert(rotateDirectionBits(R | IN, 1, U, R, D, L) == (U | IN));
  assert(rotateDirectionBits(U | D, 1, U, R, D, L) == (L | R));
  assert(rotateDirectionBits(U, 0, U, R, D, L) == U);
}

void testMounts() {
  using namespace input_rotation;
  // The eight codes and nothing else.
  int valid = 0;
  for (int c = -1; c < 400; ++c) valid += mountCodeValid(c) ? 1 : 0;
  assert(valid == 8);
  assert(mountFromCode(91).quarterTurns == 1 && mountFromCode(91).mirror);
  assert(mountFromCode(180).quarterTurns == 2 && !mountFromCode(180).mirror);
  assert(mountFromCode(45).quarterTurns == 0 && !mountFromCode(45).mirror);
  assert(mountDegrees(271) == 270 && !strcmp(mountName(181), "180 mirror") && !strcmp(mountName(0), "0"));
  // The argument forms.
  int code = -7;
  assert(parseMountArg("90", code) && code == 90);
  assert(parseMountArg(" 270  Mirror ", code) && code == 271);
  assert(parseMountArg("181", code) && code == 181);  // the pick-list's code
  assert(!parseMountArg("45", code) && code == 181);
  assert(!parseMountArg("91 mirror", code));
  assert(!parseMountArg("90 flip", code));
  assert(!parseMountArg("mirror", code));
  assert(!parseMountArg("", code));
  assert(!parseMountArg("12345", code));
  // Mount first, then the screen: a device turned 90 on a screen turned 270
  // reads straight.
  int vx = 1, vy = 0;
  orientVector(Mount{1, false}, 3, vx, vy);
  assert(vx == 1 && vy == 0);
  // Mirror exchanges left and right before the turn.
  vx = 1; vy = 0;
  orientVector(Mount{0, true}, 0, vx, vy);
  assert(vx == -1 && vy == 0);
  vx = 0; vy = 1;
  orientVector(Mount{0, true}, 0, vx, vy);
  assert(vx == 0 && vy == 1);
  // Bits and vectors agree for every mount and screen.
  const uint32_t U = 1, R = 2, D = 4, L = 8;
  const uint32_t bit[4] = {U, R, D, L};
  const int vec[4][2] = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}};
  static const int kCodes[] = {0, 1, 90, 91, 180, 181, 270, 271};
  for (int c : kCodes) {
    for (int s = 0; s < 4; ++s) {
      for (int d = 0; d < 4; ++d) {
        int ax = vec[d][0], ay = vec[d][1];
        orientVector(mountFromCode(c), s, ax, ay);
        const uint32_t out = orientDirectionBits(bit[d], mountFromCode(c), s, U, R, D, L);
        int want = -1;
        for (int k = 0; k < 4; ++k)
          if (vec[k][0] == ax && vec[k][1] == ay) want = k;
        assert(want >= 0 && out == bit[want]);
      }
      // The teach step: whichever device direction is up on the screen,
      // learning it gives back the mount's turns.
      for (int d = 0; d < 4; ++d) {
        int ax = vec[d][0], ay = vec[d][1];
        orientVector(mountFromCode(c), s, ax, ay);
        if (ax == 0 && ay == 1) {
          const Mount m = mountFromCode(c);
          assert(learnQuarterTurns(static_cast<Dir>(d), m.mirror, s) == m.quarterTurns);
        }
      }
    }
  }
}

// A drawing double that records where text lands and the rule rows.
struct Recorder {
  int x = 0, y = 0;
  int maxX = 0, maxY = 0, ruleY = -1, pixels = 0;
  char printed[8][16] = {};
  int printedX[8] = {}, printedY[8] = {}, count = 0;
  void fillRect(int, int, int w, int h, int) { assert(w <= 64 && h <= 20); }
  void drawPixel(int px, int py, int) { assert(px >= 0 && px < 64 && py >= 0 && py < 20); ++pixels; }
  void drawFastHLine(int, int py, int w, int) { assert(w == 64); ruleY = py; }
  void setTextSize(int) {}
  void setTextColor(int) {}
  void setCursor(int px, int py) { x = px; y = py; }
  void print(char c) { char t[2] = {c, 0}; print(t); }
  void print(const char* t) {
    if (count < 8) {
      snprintf(printed[count], sizeof printed[count], "%s", t);
      printedX[count] = x;
      printedY[count] = y;
      ++count;
    }
    const int end = x + 6 * static_cast<int>(strlen(t)) - 1;
    assert(x >= 0 && end <= 63 && y >= 0 && y + 7 <= 19);
    x += 6 * static_cast<int>(strlen(t));
  }
};

void testPortraitHeader() {
  // Busy, capture, 10+ unread and "~100%": every slot where the design puts it.
  OledHeaderSpec spec{};
  spec.prefix = "Set>";
  spec.text = "notifications";
  OledHeaderStatus status{};
  memcpy(status.power, "~100%", 6);
  status.unread = 12;
  status.busy = true;
  status.rec = true;
  oled_header::tall::Frame f;
  oled_header::tall::compose(spec, status, "Settings", f);
  assert(f.notify == '+' && !strcmp(f.power, "~100%") && f.ellipsisAt == 9);
  Recorder r;
  oled_header::tall::draw(r, f, 64, 1, 0);
  assert(r.ruleY == 19);
  bool sawPower = false, sawCount = false;
  for (int i = 0; i < r.count; ++i) {
    if (!strcmp(r.printed[i], "~100%")) { sawPower = true; assert(r.printedX[i] == 34 && r.printedY[i] == 1); }
    if (!strcmp(r.printed[i], "+")) { sawCount = true; assert(r.printedX[i] == 26); }
  }
  assert(sawPower && sawCount);
  // The keyboard profile: the page in the Power slot, no rule.
  oled_header::tall::composeKeyboard("WiFi SSID:", "abc", false, false, 3, f);
  Recorder k;
  oled_header::tall::draw(k, f, 64, 1, 0, false);
  assert(k.ruleY == -1);
  bool sawPage = false;
  for (int i = 0; i < k.count; ++i)
    if (!strcmp(k.printed[i], "abc")) { sawPage = true; assert(k.printedX[i] == 46); }
  assert(sawPage);
}

}  // namespace

int main() {
  testRotationValues();
  testGeometry();
  testTextMetrics();
  testGauge();
  testScrollThumb();
  testKvRows();
  testFooterChips();
  testPortraitTitle();
  testKeyboardGrid();
  testInputFollowsTheScreen();
  testMounts();
  testPortraitHeader();
  std::puts("OLED rotation core tests passed (geometry, text metrics, gauge, scrollbar, footer chips, title cut, "
            "keyboard grid, input directions, mounts, teach step, portrait header)");
  return 0;
}
