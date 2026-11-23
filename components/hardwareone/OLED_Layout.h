#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Geometry shared by monochrome layouts. No driver dependency: host tests can
// exercise the same capacity calculation used by drawing and navigation.
//
// Two orientations (owner, 2026-11-23: `oledrotation 0|90|180|270`, SSD1306
// only): landscape (128 x 64, rotations 0 and 180) and portrait (64 x 128
// logical, rotations 90 and 270; the panel stays 128 x 64 physically). Every
// layout reads the live size through Geometry (the firmware keeps one copy,
// gOledGeometry, refreshed whenever the render task applies a rotation), never
// the compile-time panel size.
namespace oled_layout {
inline constexpr int kHeaderHeight = 10;
inline constexpr int kFooterHeight = 10;
inline constexpr int kContentGap = 1;
inline constexpr int kOledContentHeight = 64 - kHeaderHeight - kFooterHeight - kContentGap;

// baselineRows preserves a screen's established 64px layout, including any
// deliberate spare space. Taller canvases use all remaining complete rows.
// reservedHeight accounts for status/title rows and any top/bottom padding.
constexpr int visibleRows(int contentHeight, int rowHeight,
                          int reservedHeight = 0, int baselineRows = 0) {
  if (rowHeight <= 0) return 1;
  if (baselineRows > 0 && contentHeight == kOledContentHeight) return baselineRows;
  const int rows = (contentHeight - reservedHeight) / rowHeight;
  return rows > 0 ? rows : 1;
}

// ---- glyph metrics (Adafruit glcdfont: a 6 x 8 cell, 5 x 7 ink) -------------
inline constexpr int kCellW = 6;
inline constexpr int kCellH = 8;
// Ink width of n glyphs at a text size (the last cell's blank column dropped).
constexpr int textWidth(int glyphs, int size = 1) {
  return glyphs <= 0 ? 0 : kCellW * size * glyphs - size;
}
// Whole glyph cells that fit in px.
constexpr int cellsIn(int px, int size = 1) { return px <= 0 ? 0 : px / (kCellW * size); }
// Cursor x that ends n glyphs' ink on column `lastX` (inclusive).
constexpr int rightAlignX(int glyphs, int lastX, int size = 1) {
  return lastX + 1 - textWidth(glyphs, size);
}
// Cursor x that centres n glyphs' ink across [0, width).
constexpr int centerX(int glyphs, int width, int size = 1) {
  return (width - textWidth(glyphs, size)) / 2;
}

// ---- rotation -----------------------------------------------------------------
// The GFX rotation (quarter turns) for a setting value in degrees; -1 when the
// value is not one of 0, 90, 180, 270.
constexpr int rotationFromDegrees(int degrees) {
  return degrees == 0 ? 0 : degrees == 90 ? 1 : degrees == 180 ? 2 : degrees == 270 ? 3 : -1;
}
constexpr int degreesFromRotation(int rotation) { return (rotation & 3) * 90; }
constexpr bool rotationIsPortrait(int rotation) { return (rotation & 1) != 0; }

// ---- the portrait grid (the agreed hybrid layout, 64 x 128) ---------------------
// Rows: status y0-8, title y9-18, rule y19, y20 blank, content y21-105, y106
// blank, footer rule y107, chip rows y109-117 (A, B) and y119-127 (X, Y).
namespace portrait {
inline constexpr int kWidth = 64;
inline constexpr int kHeight = 128;
inline constexpr int kStatusTextY = 1;
inline constexpr int kTitleX = 1;
inline constexpr int kTitleY = 10;
inline constexpr int kTitleCells = 10;
inline constexpr int kHeaderRuleY = 19;
inline constexpr int kHeaderHeight = 20;  // rows 0-19; the content starts one row below
inline constexpr int kContentY = 21;
inline constexpr int kContentBottom = 105;  // the last row a screen draws on
inline constexpr int kFooterRuleY = 107;
inline constexpr int kFooterHeight = kHeight - kFooterRuleY - 1;  // rows under the rule: 20
inline constexpr int kChipRow1Y = 109;
inline constexpr int kChipRow2Y = 119;
inline constexpr int kChipW = 7;
inline constexpr int kChipH = 9;
inline constexpr int kChipLeftX = 0;    // A and X
inline constexpr int kChipRightX = 33;  // B and Y
inline constexpr int kVerbMax = 3;
// Status row slots (fixed, they never move).
inline constexpr int kBusyX = 1;
inline constexpr int kRecX = 11;
inline constexpr int kBellX = 19;
inline constexpr int kNotifyCountX = 26;
inline constexpr int kPowerLastX = 62;  // the power text ends here (cursor x = 64 - 6n)
// The 10-cell text grid: x = 1 + 6i, ink x1-59; x60-61 gutter; x62-63 scrollbar.
inline constexpr int kTextX = 1;
inline constexpr int kTextLastX = 59;
inline constexpr int kCols = 10;
inline constexpr int kScrollbarX = 62;
// Hero card (ToF, RTC, FM, Mic): label, size-2 value, unit rows.
inline constexpr int kHeroLabelY = 23;
inline constexpr int kHeroValueY = 35;
inline constexpr int kHeroUnitY = 53;
inline constexpr int kHeroBandY = 64;  // rows below the hero (KV rows or a meter)
inline constexpr int kHeroValueMaxGlyphs = 5;
// Ink-centred hero value / unit cursor x (rules from the design).
constexpr int heroValueX(int glyphs) { return 33 - 6 * glyphs; }
constexpr int heroUnitX(int glyphs) { return (65 - 6 * glyphs) / 2; }
// Vertical gauge (ToF): frame x52-63 y21-100, inner fill x54-61 y23-98.
inline constexpr int kGaugeX = 52;
inline constexpr int kGaugeY = 21;
inline constexpr int kGaugeW = 12;
inline constexpr int kGaugeH = 80;
inline constexpr int kGaugeFillX = 54;
inline constexpr int kGaugeFillW = 8;
inline constexpr int kGaugeFillTop = 23;
inline constexpr int kGaugeFillBottom = 98;
inline constexpr int kGaugeFillSpan = kGaugeFillBottom - kGaugeFillTop + 1;  // 76
inline constexpr int kGaugeLabelX = 40;
// Stacked pane (launcher, Sensors submenu): icon above the list.
inline constexpr int kPaneIconX = 16;
inline constexpr int kPaneIconY = 22;
inline constexpr int kPaneDotX = 56;
inline constexpr int kPaneDotY = 49;
inline constexpr int kPaneBadgeX = 55;
inline constexpr int kPaneBadgeY = 23;
inline constexpr int kPaneRuleY = 55;
inline constexpr int kPaneListY = 57;
inline constexpr int kListRowH = 10;
inline constexpr int kListSelectW = 61;  // fillRect(0, y, 61, 9)
}  // namespace portrait

// Filled length of a gauge of `span` px for value/max, clamped to [0, span].
constexpr int gaugeFill(long value, long maxValue, int span) {
  return maxValue <= 0 || value <= 0 ? 0
         : value >= maxValue        ? span
                                    : static_cast<int>((value * span) / maxValue);
}

// ---- runtime geometry -----------------------------------------------------------
struct Geometry {
  int16_t width;
  int16_t height;
  bool portrait;
  int16_t headerHeight;   // rows 0..headerHeight-1 belong to the header (rule on the last)
  int16_t footerHeight;   // rows under the footer rule
  int16_t contentY;       // first content row
  int16_t contentHeight;  // contentY + contentHeight - 1 = the footer rule row
  int16_t contentBottom;  // the last row a screen should put ink on
  int16_t cols;           // size-1 text cells per row
};

// The layout for a canvas of w x h. Portrait (taller than wide) is the hybrid
// grid above; anything else keeps the landscape bars (10 px header, 10 px
// footer) and grows the content with the canvas, as the P4 canvases do.
constexpr Geometry geometryFor(int w, int h) {
  return h > w ? Geometry{static_cast<int16_t>(w),
                          static_cast<int16_t>(h),
                          true,
                          static_cast<int16_t>(portrait::kHeaderHeight),
                          static_cast<int16_t>(portrait::kFooterHeight),
                          static_cast<int16_t>(portrait::kHeaderHeight + kContentGap),
                          static_cast<int16_t>(h - portrait::kHeaderHeight - portrait::kFooterHeight -
                                               kContentGap),
                          static_cast<int16_t>(h - portrait::kFooterHeight - 3),
                          static_cast<int16_t>(cellsIn(w))}
               : Geometry{static_cast<int16_t>(w),
                          static_cast<int16_t>(h),
                          false,
                          static_cast<int16_t>(kHeaderHeight),
                          static_cast<int16_t>(kFooterHeight),
                          static_cast<int16_t>(kHeaderHeight + kContentGap),
                          static_cast<int16_t>(h - kHeaderHeight - kFooterHeight - kContentGap),
                          static_cast<int16_t>(h - kFooterHeight - 2),
                          static_cast<int16_t>(cellsIn(w))};
}
static_assert(geometryFor(64, 128).contentY == portrait::kContentY, "portrait content starts at y21");
static_assert(geometryFor(64, 128).headerHeight + geometryFor(64, 128).contentHeight == portrait::kFooterRuleY,
              "portrait footer rule at y107");
static_assert(geometryFor(64, 128).contentBottom == portrait::kContentBottom, "portrait content ends at y105");
static_assert(geometryFor(128, 64).contentHeight == kOledContentHeight, "landscape keeps 43 content rows");

// ---- scrollbar (portrait rule) --------------------------------------------------
// A track over [top, top + span) and a thumb: height max(6, round(span *
// visible / total)), y = top + round(first * span / total), clamped so it
// ends inside the span.
struct Thumb {
  int y;
  int h;
};
constexpr Thumb scrollThumb(int top, int span, int visible, int total, int first) {
  if (total <= 0 || visible >= total || span <= 0) return Thumb{top, span > 0 ? span : 0};
  int h = (span * visible * 2 + total) / (2 * total);
  if (h < 6) h = 6;
  if (h > span) h = span;
  int y = top + (first * span * 2 + total) / (2 * total);
  if (y + h > top + span) y = top + span - h;
  if (y < top) y = top;
  return Thumb{y, h};
}

// ---- KV rows ------------------------------------------------------------------
// A label/value pair shares one row when label + one space + value fits the
// row; otherwise it stacks (label, then the value right-aligned on the next
// row). Returns the rows the pair takes.
inline constexpr int kvRows(int labelGlyphs, int valueGlyphs, int cols) {
  return labelGlyphs + 1 + valueGlyphs <= cols ? 1 : 2;
}

// ---- the footer chips ------------------------------------------------------------
// The portrait footer maps today's "L:Verb" hint strings onto four fixed
// slots, A and B on row 1, X and Y on row 2. A verb is cut to 3 glyphs through
// a fixed abbreviation table; tokens with another letter (S:OK, the arrow
// tokens) are not drawn. A slot without a token stays blank.
enum FooterSlot : uint8_t { kSlotA = 0, kSlotB, kSlotX, kSlotY, kSlotCount };
struct FooterChips {
  char verb[kSlotCount][portrait::kVerbMax + 1];  // "" = blank slot
};

struct VerbAbbrev {
  const char* word;
  const char* shortForm;
};
inline constexpr VerbAbbrev kVerbAbbrevs[] = {
    {"Select", "Sel"}, {"Back", "Bck"},     {"Cancel", "Esc"}, {"Edit", "Edt"},   {"Delete", "Del"},
    {"Stop", "Stp"},   {"Start", "Go"},     {"Mute", "Mut"},   {"Volume", "Vol"}, {"Power", "Pwr"},
    {"Page", "Pg+"},   {"Detail", "Det"},   {"Clear", "Clr"},  {"Open", "Opn"},   {"Toggle", "Tog"},
    {"Send", "Snd"},   {"Mode", "Mod"},     {"Photo", "Pic"},  {"Hold", "Hld"},   {"Undo", "Und"},
    {"Done", "OK"},    {"Pick", "Sel"},     {"Menu", "Mnu"},   {"Setup", "Set"},  {"Enable", "On"},
    {"Type", "Typ"},   {"Broadcast", "All"}, {"Cycle", "Cyc"}, {"Scan", "Scn"},   {"Play", "Ply"},
    {"Pause", "Pau"},  {"Zoom", "Zm"},      {"Track", "Trk"},  {"Connect", "Con"}, {"Refresh", "Rfr"},
};

inline bool asciiEqualNoCase(const char* a, size_t aLen, const char* b) {
  size_t i = 0;
  for (; i < aLen && b[i]; ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return i == aLen && b[i] == '\0';
}

// The verb as a chip shows it: the table's short form, else its first 3 glyphs.
inline void abbreviateVerb(const char* verb, size_t len, char out[portrait::kVerbMax + 1]) {
  for (const VerbAbbrev& a : kVerbAbbrevs) {
    if (asciiEqualNoCase(verb, len, a.word)) {
      size_t n = strlen(a.shortForm);
      if (n > static_cast<size_t>(portrait::kVerbMax)) n = portrait::kVerbMax;
      memcpy(out, a.shortForm, n);
      out[n] = '\0';
      return;
    }
  }
  size_t n = 0;
  for (; n < len && n < static_cast<size_t>(portrait::kVerbMax); ++n) {
    const char c = verb[n];
    out[n] = (c >= 32 && c < 127) ? c : '?';
  }
  out[n] = '\0';
}

// Parses a hint string ("A:Select X:Stop B:Back", tokens separated by spaces)
// into the four slots. The first token for a letter wins.
inline void parseFooterHints(const char* hints, FooterChips& out) {
  for (int i = 0; i < kSlotCount; ++i) out.verb[i][0] = '\0';
  if (!hints) return;
  const char* p = hints;
  while (*p) {
    while (*p == ' ') ++p;
    if (!*p) break;
    const char* tok = p;
    while (*p && *p != ' ') ++p;
    const size_t len = static_cast<size_t>(p - tok);
    if (len < 3 || tok[1] != ':') continue;
    int slot = -1;
    switch (tok[0]) {
      case 'A': slot = kSlotA; break;
      case 'B': slot = kSlotB; break;
      case 'X': slot = kSlotX; break;
      case 'Y': slot = kSlotY; break;
      default: break;
    }
    if (slot < 0 || out.verb[slot][0]) continue;
    abbreviateVerb(tok + 2, len - 2, out.verb[slot]);
  }
}

// Chip geometry for a slot: the chip's left edge and row top.
constexpr int chipX(int slot) {
  return (slot == kSlotA || slot == kSlotX) ? portrait::kChipLeftX : portrait::kChipRightX;
}
constexpr int chipY(int slot) {
  return (slot == kSlotA || slot == kSlotB) ? portrait::kChipRow1Y : portrait::kChipRow2Y;
}

// ---- the keyboard grid -------------------------------------------------------------
// Every character page holds 30 keys in row-major order (10 x 3 in landscape).
// Portrait re-wraps the same order into 5 x 6 cells. The cursor is kept in
// the landscape (col, row) form; a move steps on the grid as it is drawn and
// wraps around its edges.
inline constexpr int kKeyboardPortraitCols = 5;
inline void keyboardStep(int& col, int& row, int dCol, int dRow, int cols, int rows, bool portraitGrid) {
  const int total = cols * rows;
  if (total <= 0) return;
  const int drawnCols = portraitGrid ? kKeyboardPortraitCols : cols;
  const int drawnRows = total / drawnCols;
  int index = row * cols + col;
  if (index < 0 || index >= total) index = 0;
  int c = index % drawnCols, r = index / drawnCols;
  c = ((c + dCol) % drawnCols + drawnCols) % drawnCols;
  r = ((r + dRow) % drawnRows + drawnRows) % drawnRows;
  index = r * drawnCols + c;
  col = index % cols;
  row = index / cols;
}

// ---- the portrait title ---------------------------------------------------------
// Cut order: prefix + text when it fits; then the leaf (the part of the text
// after its last '>', which is the whole text for "Menu>" or "Set>"); a path
// (keepTail) keeps its end behind the one-cell mark; anything else keeps the
// leaf's first (cells - 1) glyphs and the mark. Writes the glyphs to out
// (NUL-terminated, at least cells + 1 bytes), the mark's cell to ellipsisAt
// (-1 = none) and returns the glyphs written.
inline int fitPortraitTitle(const char* prefix, size_t pLen, const char* text, size_t tLen, int cells,
                            bool keepTail, char* out, int& ellipsisAt) {
  ellipsisAt = -1;
  out[0] = '\0';
  if (cells <= 0) return 0;
  const size_t room = static_cast<size_t>(cells);
  size_t n = 0;
  size_t leafStart = 0;
  for (size_t i = 0; i < tLen; ++i)
    if (text[i] == '>') leafStart = i + 1;
  const char* leaf = text + leafStart;
  const size_t leafLen = tLen - leafStart;
  if (pLen + tLen <= room) {
    memcpy(out, prefix, pLen);
    memcpy(out + pLen, text, tLen);
    n = pLen + tLen;
  } else if (leafLen <= room) {
    memcpy(out, leaf, leafLen);
    n = leafLen;
  } else if (keepTail) {
    ellipsisAt = 0;
    out[0] = '.';
    const size_t keep = room - 1;
    memcpy(out + 1, leaf + (leafLen - keep), keep);
    n = room;
  } else {
    memcpy(out, leaf, room - 1);
    ellipsisAt = static_cast<int>(room - 1);
    out[room - 1] = '.';
    n = room;
  }
  out[n] = '\0';
  return static_cast<int>(n);
}
}  // namespace oled_layout
