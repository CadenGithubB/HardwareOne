// =============================================================================
// OLED_HeaderCore — the header status bar (top 10 rows), laid out by rules
// =============================================================================
// Dependency-free (only OLED_TextFieldCore.h, itself dependency-free) so host
// tests compile the shipping layout, title fitting and drawing unchanged. The
// firmware (oledRenderHeader in OLED_Utils.cpp) collects an OledHeaderStatus,
// asks the current mode for its OledHeaderSpec, composes a Frame here and
// draws it with draw(). Nothing else draws in rows 0-9 while the bar is shown.
//
// The band: row 0 blank, ink in rows 1-8 (icons 1-7, text descenders reach 8),
// the rule on row 9. The title starts at x 0 and never moves. Items are placed
// from the right edge inwards, 3 px apart, in this order:
//
//   Power, Notify, Rec, Context, Busy
//
// Every item has a slot whose width does not follow the live value (Power:
// 18 px, 24 for four glyphs, 30 for five), so the bell and the battery never
// move when a value changes. Busy sits left of everything else, so nothing
// moves when a command starts or ends (owner, 2026-11-22): only the title's
// room shrinks, the title is cut only when its text would really reach an
// item, and it never grows back while busy (when Busy pushes Context out).
//
// The title reserves min(its length, 10) cells. While the items do not fit the
// rest, the shown non-pinned item with the lowest priority drops; the
// static_assert below guarantees that only Context can ever drop.
//
// The on-screen keyboard's top line (prompt + keyboard page) is a second
// profile of the same rules (namespace kb at the end).
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "OLED_TextFieldCore.h"  // oled_text::flatten (the shared sanitizer), 6x8 cell metrics
#include "OLED_Layout.h"         // the portrait grid (oled_layout::portrait)

// How a title that does not fit is cut. End keeps the start ("Set>notif…");
// KeepPrefix keeps the whole prefix and the END of the text ("Files>…gs/boot"),
// falling back to End when the prefix, the mark and 3 glyphs do not fit.
enum class OledTitleCut : uint8_t { End = 0, KeepPrefix = 1 };

// What the bar shows besides the title, collected once per frame (and by the
// staleness poll, so what is watched is exactly what is drawn).
struct OledHeaderStatus {
  char power[6];   // "42%", "~42%", "~100%", "USB", "--%"; "" = no power item
  uint8_t unread;  // unread notifications (drawn as 1-9 or '+')
  bool busy;       // the OLED command in flight
  bool rec;        // the microphone or the camera is capturing (one dot for both)
};

// What the screen says. Filled with the default title chain, then handed to
// the mode's headerFunc (OledModeEntry), which may change any field.
struct OledHeaderSpec {
  const char* prefix;  // "Set>", "Files>"... (nullptr = none); never cut by KeepPrefix
  const char* text;    // nullptr = the mode name
  OledTitleCut cut;
  char context[8];     // short screen text such as "3/12"; more than 5 glyphs is not shown
  uint8_t hide;        // 1u << oled_header::kPower / kNotify / kContext (Busy and Rec cannot hide)
  char buf[24];        // scratch a hook may format its title into (then text = buf)
};

namespace oled_header {

constexpr int kWidth = 128;  // every canvas that draws the bar is 128 px wide
constexpr int kHeight = 10;  // rows 0-9
constexpr int kRuleY = 9;
constexpr int kTextY = 1;
constexpr int kGap = 3;
constexpr int kCell = oled_text::kGlyphW;
constexpr int kTitleReserveMax = 10;  // cells a long title always keeps
constexpr int kTitleMaxCells = kWidth / kCell;  // 21
constexpr int kContextMaxGlyphs = 5;
constexpr int kPowerMinWidth = 18;
constexpr int kNotifyWidth = 12;

// Order from the right edge.
enum Item : uint8_t { kPower = 0, kNotify, kRec, kContext, kBusy, kItemCount };

struct Rule {
  uint8_t maxWidth;  // px
  uint8_t priority;  // higher stays longer
  bool pinned;       // never drops, cannot be hidden
};

constexpr Rule kRules[kItemCount] = {
    {30, 60, false},   // Power: 18 px, 24 for 4 glyphs, 30 for 5
    {12, 80, false},   // Notify: bell + one glyph
    {5, 110, true},    // Rec: 5x5 dot
    {30, 40, false},   // Context: 6 px per glyph, at most 5
    {7, 100, true},    // Busy: 7x7 hourglass
};

// Items at or above this priority must always fit next to the shortest title
// reserve, so in practice only the items below it (Context) can drop. Any
// future row that breaks this fails the build.
constexpr uint8_t kGuaranteedPriority = 50;

constexpr int guaranteedWidth(const Rule* rules, int n) {
  int total = 0, count = 0;
  for (int i = 0; i < n; ++i) {
    if (rules[i].pinned || rules[i].priority >= kGuaranteedPriority) {
      total += rules[i].maxWidth;
      ++count;
    }
  }
  return total + (count > 0 ? (count - 1) * kGap : 0);
}
constexpr int guaranteedWidth() { return guaranteedWidth(kRules, kItemCount); }
constexpr int itemBudget(int reserveCells) { return kWidth - kCell * reserveCells - kGap; }

static_assert(guaranteedWidth() <= itemBudget(kTitleReserveMax),
              "the always-shown header items must fit beside a 10-cell title");
static_assert(kRules[kContext].maxWidth == kContextMaxGlyphs * kCell, "context slot = 5 glyphs");
static_assert(kRules[kContext].priority < kGuaranteedPriority, "context is the item that drops");

// ---- bitmaps (rows top to bottom, bit (w-1-col) = column col) ---------------
constexpr uint8_t kBellW = 5, kBellH = 7;  // drawn from row 1
constexpr uint8_t kBell[kBellH] = {0x04, 0x0E, 0x0E, 0x0E, 0x1F, 0x00, 0x04};
constexpr uint8_t kHourglassW = 7, kHourglassH = 7;  // drawn from row 1
constexpr uint8_t kHourglass[kHourglassH] = {0x7F, 0x22, 0x14, 0x08, 0x1C, 0x3E, 0x7F};
constexpr uint8_t kRecDotW = 5, kRecDotH = 5;  // drawn from row 2
constexpr uint8_t kRecDot[kRecDotH] = {0x0E, 0x1F, 0x1F, 0x1F, 0x0E};
constexpr int kEllipsisY = 7;  // three dots on the text baseline of one cell

// ---- layout -------------------------------------------------------------------

// The most items any profile has (the bar 5, the keyboard line 3).
constexpr int kMaxItems = 8;
static_assert(kItemCount <= kMaxItems, "the bar fits the placement scratch");

// One placement pass over a rule table: drop by priority until the items fit,
// pack them from the right, return the title room in cells.
inline int placeRules(const Rule* rules, int n, const uint8_t* widths, int titleGlyphs, int16_t* x) {
  bool shown[kMaxItems];
  for (int i = 0; i < n; ++i) shown[i] = widths[i] > 0;
  const int reserve = titleGlyphs < 0 ? 0 : (titleGlyphs < kTitleReserveMax ? titleGlyphs : kTitleReserveMax);
  const int budget = itemBudget(reserve);
  for (;;) {
    int total = 0, count = 0;
    for (int i = 0; i < n; ++i) {
      if (shown[i]) { total += widths[i]; ++count; }
    }
    if (count > 0) total += (count - 1) * kGap;
    if (total <= budget) break;
    int drop = -1;
    for (int i = 0; i < n; ++i) {
      if (!shown[i] || rules[i].pinned) continue;
      if (drop < 0 || rules[i].priority < rules[drop].priority) drop = i;
    }
    if (drop < 0) break;  // only pinned items left (the static_asserts rule this out)
    shown[drop] = false;
  }
  int right = kWidth;  // the next slot ends here
  bool any = false;
  for (int i = 0; i < n; ++i) {
    x[i] = -1;
    if (!shown[i]) continue;
    if (any) right -= kGap;
    x[i] = static_cast<int16_t>(right - widths[i]);
    right = x[i];
    any = true;
  }
  int cells = any ? (right - kGap) / kCell : kTitleMaxCells;
  if (cells < reserve) cells = reserve;
  if (cells > kTitleMaxCells) cells = kTitleMaxCells;
  if (cells < 0) cells = 0;
  return cells;
}

// placeRules, and while the item `busy` is shown the title keeps at most the
// room it has without it (it never grows back when Busy pushes an item out).
inline int layoutRules(const Rule* rules, int n, const uint8_t* widths, int titleGlyphs, int16_t* x,
                       int busy) {
  int cells = placeRules(rules, n, widths, titleGlyphs, x);
  if (widths[busy] > 0) {
    uint8_t calm[kMaxItems];
    int16_t ignored[kMaxItems];
    memcpy(calm, widths, static_cast<size_t>(n));
    calm[busy] = 0;
    const int calmCells = placeRules(rules, n, calm, titleGlyphs, ignored);
    if (calmCells < cells) cells = calmCells;
  }
  return cells;
}

inline int place(const uint8_t widths[kItemCount], int titleGlyphs, int16_t x[kItemCount]) {
  return placeRules(kRules, kItemCount, widths, titleGlyphs, x);
}

// widths[i] = the slot item i asks for (0 = absent). Writes each shown item's
// slot left edge to x[i] (-1 = not shown) and returns the title room in cells.
// While busy the title keeps at most the room it has without Busy.
inline int layout(const uint8_t widths[kItemCount], int titleGlyphs, int16_t x[kItemCount]) {
  return layoutRules(kRules, kItemCount, widths, titleGlyphs, x, kBusy);
}

// ---- title --------------------------------------------------------------------

struct Title {
  char glyphs[kTitleMaxCells + 1];  // NUL-terminated; the mark's cell holds a placeholder
  int8_t ellipsisAt;                // cell of the one-cell "…" mark, -1 = none
  uint8_t cells;                    // glyph cells used, the mark included
};

// Fits prefix (pLen glyphs) + text (tLen glyphs), both already sanitized, into
// `cells` cells.
inline void fitTitle(const char* prefix, size_t pLen, const char* text, size_t tLen, int cells,
                     OledTitleCut cut, Title& out) {
  out.ellipsisAt = -1;
  out.cells = 0;
  out.glyphs[0] = '\0';
  if (cells > kTitleMaxCells) cells = kTitleMaxCells;
  if (cells <= 0) return;
  const size_t room = static_cast<size_t>(cells);
  size_t n = 0;
  auto put = [&](char c) { out.glyphs[n++] = c; };
  auto glyphAt = [&](size_t i) { return i < pLen ? prefix[i] : text[i - pLen]; };
  if (pLen + tLen <= room) {
    for (size_t i = 0; i < pLen + tLen; ++i) put(glyphAt(i));
  } else if (cut == OledTitleCut::KeepPrefix && pLen + 1 + 3 <= room) {
    for (size_t i = 0; i < pLen; ++i) put(prefix[i]);
    out.ellipsisAt = static_cast<int8_t>(n);
    put('.');
    const size_t keep = room - pLen - 1;
    for (size_t i = tLen - keep; i < tLen; ++i) put(text[i]);
  } else {
    for (size_t i = 0; i + 1 < room; ++i) put(glyphAt(i));
    out.ellipsisAt = static_cast<int8_t>(n);
    put('.');
  }
  out.glyphs[n] = '\0';
  out.cells = static_cast<uint8_t>(n);
}

// ---- items --------------------------------------------------------------------

// The Power item's text from the shared battery backend (getBatterySnapshot):
// "USB" when USB power is known present; nothing on a board with no battery
// measurement (owner, 2026-11-22: hide it there); otherwise the percentage
// ("~" when estimated) or "--%" while it is unknown.
inline void formatPower(bool usbPresent, bool measurementAvailable, bool percentValid,
                        bool percentEstimated, float percent, char out[6]) {
  out[0] = '\0';
  if (usbPresent) {
    memcpy(out, "USB", 4);
    return;
  }
  if (!measurementAvailable) return;
  if (!percentValid || isnan(percent)) {
    memcpy(out, "--%", 4);
    return;
  }
  int value = static_cast<int>(percent);
  if (value < 0) value = 0;
  if (value > 100) value = 100;
  size_t n = 0;
  if (percentEstimated) out[n++] = '~';
  if (value >= 100) out[n++] = '1';
  if (value >= 10) out[n++] = static_cast<char>('0' + (value / 10) % 10);
  out[n++] = static_cast<char>('0' + value % 10);
  out[n++] = '%';
  out[n] = '\0';
}

inline char notifyGlyph(uint8_t unread) {
  return unread == 0 ? '\0' : (unread < 10 ? static_cast<char>('0' + unread) : '+');
}

// ---- compose ------------------------------------------------------------------

struct Frame {
  int16_t x[kItemCount];  // slot left edge, -1 = not shown
  uint8_t w[kItemCount];  // slot width
  Title title;
  char power[6];
  char context[kContextMaxGlyphs + 1];
  char notify;  // '1'-'9', '+'
};

inline void compose(const OledHeaderSpec& spec, const OledHeaderStatus& status, const char* modeName,
                    Frame& f) {
  char prefix[16];
  char text[oled_text::kLabelMax];
  const size_t pLen = oled_text::flatten(spec.prefix ? spec.prefix : "", prefix, sizeof prefix);
  const char* rawText = spec.text ? spec.text : (modeName ? modeName : "");
  const size_t tLen = oled_text::flatten(rawText, text, sizeof text);

  uint8_t widths[kItemCount] = {};
  f.power[0] = '\0';
  f.context[0] = '\0';
  f.notify = '\0';
  if (!(spec.hide & (1u << kPower)) && status.power[0]) {
    const size_t n = strnlen(status.power, sizeof status.power - 1);
    memcpy(f.power, status.power, n);
    f.power[n] = '\0';
    const int w = static_cast<int>(n) * kCell;
    widths[kPower] = static_cast<uint8_t>(w < kPowerMinWidth ? kPowerMinWidth : w);
  }
  if (!(spec.hide & (1u << kNotify)) && status.unread > 0) {
    f.notify = notifyGlyph(status.unread);
    widths[kNotify] = kNotifyWidth;
  }
  if (!(spec.hide & (1u << kContext)) && spec.context[0]) {
    char ctx[sizeof spec.context + 1];
    char raw[sizeof spec.context + 1];
    memcpy(raw, spec.context, sizeof spec.context);
    raw[sizeof spec.context] = '\0';
    const size_t n = oled_text::flatten(raw, ctx, sizeof ctx);
    if (n > 0 && n <= static_cast<size_t>(kContextMaxGlyphs)) {
      memcpy(f.context, ctx, n + 1);
      widths[kContext] = static_cast<uint8_t>(n * kCell);
    }
  }
  if (status.rec) widths[kRec] = kRecDotW;
  if (status.busy) widths[kBusy] = kHourglassW;

  const int cells = layout(widths, static_cast<int>(pLen + tLen), f.x);
  for (int i = 0; i < kItemCount; ++i) f.w[i] = f.x[i] >= 0 ? widths[i] : 0;
  fitTitle(prefix, pLen, text, tLen, cells, spec.cut, f.title);
}

// FNV-1a over what the status makes the bar show. The staleness poll compares
// it with the signature of the last drawn bar.
inline uint32_t signature(const OledHeaderStatus& s) {
  uint32_t h = 2166136261u;
  auto mix = [&h](uint8_t b) { h ^= b; h *= 16777619u; };
  for (size_t i = 0; i < sizeof s.power && s.power[i]; ++i) mix(static_cast<uint8_t>(s.power[i]));
  mix(0);
  mix(static_cast<uint8_t>(notifyGlyph(s.unread)));
  mix(s.busy ? 1 : 0);
  mix(s.rec ? 1 : 0);
  return h;
}

// ---- draw ---------------------------------------------------------------------

template <class Gfx>
inline void drawBits(Gfx& g, int x, int y, const uint8_t* rows, int h, int w, uint16_t fg) {
  for (int r = 0; r < h; ++r) {
    for (int c = 0; c < w; ++c) {
      if (rows[r] & (1u << (w - 1 - c))) g.drawPixel(x + c, y + r, fg);
    }
  }
}

// Uses only fillRect, drawPixel, drawFastHLine, setTextSize, setTextColor,
// setCursor and print, so the host double and Adafruit_SSD1306 run the same code.
template <class Gfx>
inline void draw(Gfx& g, const Frame& f, uint16_t fg, uint16_t bg) {
  g.fillRect(0, 0, kWidth, kHeight, bg);
  g.setTextSize(1);
  g.setTextColor(fg);
  for (int i = 0; i < f.title.cells; ++i) {
    const int x = i * kCell;
    if (i == f.title.ellipsisAt) {
      for (int d = 0; d < 3; ++d) g.drawPixel(x + 2 * d, kEllipsisY, fg);
      continue;
    }
    g.setCursor(x, kTextY);
    g.print(f.title.glyphs[i]);
  }
  if (f.x[kPower] >= 0) {
    const int n = static_cast<int>(strlen(f.power));
    g.setCursor(f.x[kPower] + f.w[kPower] - n * kCell, kTextY);
    g.print(f.power);
  }
  if (f.x[kNotify] >= 0) {
    drawBits(g, f.x[kNotify], kTextY, kBell, kBellH, kBellW, fg);
    g.setCursor(f.x[kNotify] + kCell, kTextY);
    g.print(f.notify);
  }
  if (f.x[kContext] >= 0) {
    g.setCursor(f.x[kContext], kTextY);
    g.print(f.context);
  }
  if (f.x[kRec] >= 0) drawBits(g, f.x[kRec], kTextY + 1, kRecDot, kRecDotH, kRecDotW, fg);
  if (f.x[kBusy] >= 0) drawBits(g, f.x[kBusy], kTextY, kHourglass, kHourglassH, kHourglassW, fg);
  g.drawFastHLine(0, kRuleY, kWidth, fg);
}


// ---- the keyboard line ------------------------------------------------------
// The on-screen keyboard owns the whole display (the bar is off while it is
// up), and its top line is a second profile of the same rules: the prompt from
// x 0 (cut with the one-cell mark), then from the right Mode, Rec, Busy, 3 px
// apart. Mode is the keyboard page (abc ABC 123 sym PAT), the live microphone
// on the dictation page (PDM, G2), or the start of the typed text beside
// "Suggestions:". It sits at the right edge in an 18 px slot (wider only for
// that text), and Busy is leftmost, so nothing moves when a command starts or
// ends. All three are pinned; only the prompt gives up room. The line is rows
// y..y+8 with the text on row y; the keyboard's preview box is its rule.
namespace kb {

enum Item : uint8_t { kMode = 0, kRec, kBusy, kItemCount };
constexpr int kModeMinWidth = 18;
constexpr int kModeMaxGlyphs = 7;
constexpr int kLineHeight = 9;

constexpr Rule kRules[kItemCount] = {
    {kModeMaxGlyphs * kCell, 90, true},  // Mode: 18 px, up to 7 glyphs for the typed text
    {kRecDotW, 110, true},               // Rec: 5x5 dot
    {kHourglassW, 100, true},            // Busy: 7x7 hourglass
};
static_assert(kItemCount <= kMaxItems, "the keyboard line fits the placement scratch");
static_assert(guaranteedWidth(kRules, kItemCount) <= itemBudget(kTitleReserveMax),
              "the keyboard line's items must fit beside a 10-cell prompt");

struct Frame {
  int16_t x[kItemCount];  // slot left edge, -1 = not shown
  uint8_t w[kItemCount];
  Title title;
  char mode[kModeMaxGlyphs + 1];
};

inline void compose(const char* prompt, const char* mode, bool rec, bool busy, Frame& f) {
  char text[oled_text::kLabelMax];
  const size_t tLen = oled_text::flatten(prompt ? prompt : "", text, sizeof text);
  const size_t mLen = oled_text::flatten(mode ? mode : "", f.mode, sizeof f.mode);  // at most 7 glyphs
  uint8_t widths[kItemCount] = {};
  if (mLen > 0) {
    const int w = static_cast<int>(mLen) * kCell;
    widths[kMode] = static_cast<uint8_t>(w < kModeMinWidth ? kModeMinWidth : w);
  }
  if (rec) widths[kRec] = kRecDotW;
  if (busy) widths[kBusy] = kHourglassW;
  const int cells = layoutRules(kRules, kItemCount, widths, static_cast<int>(tLen), f.x, kBusy);
  for (int i = 0; i < kItemCount; ++i) f.w[i] = f.x[i] >= 0 ? widths[i] : 0;
  fitTitle("", 0, text, tLen, cells, OledTitleCut::End, f.title);
}

// Rows y..y+8: cleared, then the prompt, the mode text right-aligned in its
// slot, the capture dot and the hourglass. Same calls as the bar's draw().
template <class Gfx>
inline void draw(Gfx& g, const Frame& f, int y, uint16_t fg, uint16_t bg) {
  g.fillRect(0, y, kWidth, kLineHeight, bg);
  g.setTextSize(1);
  g.setTextColor(fg);
  for (int i = 0; i < f.title.cells; ++i) {
    const int x = i * kCell;
    if (i == f.title.ellipsisAt) {
      for (int d = 0; d < 3; ++d) g.drawPixel(x + 2 * d, y + kEllipsisY - kTextY, fg);
      continue;
    }
    g.setCursor(x, y);
    g.print(f.title.glyphs[i]);
  }
  if (f.x[kMode] >= 0) {
    const int n = static_cast<int>(strlen(f.mode));
    g.setCursor(f.x[kMode] + f.w[kMode] - n * kCell, y);
    g.print(f.mode);
  }
  if (f.x[kRec] >= 0) drawBits(g, f.x[kRec], y + 1, kRecDot, kRecDotH, kRecDotW, fg);
  if (f.x[kBusy] >= 0) drawBits(g, f.x[kBusy], y, kHourglass, kHourglassH, kHourglassW, fg);
}

}  // namespace kb

// ---- portrait (64 x 128, `oledrotation 90|270`) ----------------------------------
// The agreed hybrid layout: a status row of fixed slots (y 0-8) that never
// move on any screen, Busy x1, Rec x11, Notify (bell x19, count x26) and
// Power right-aligned to end on x62; the title on its own row (y 9-18, 10
// cells from x1, cut by oled_layout::fitPortraitTitle: whole, then the leaf,
// then 9 glyphs and the one-cell mark; a path keeps its tail); the rule on
// y19. There is no Context slot in portrait (a page counter has no room).
// The keyboard profile keeps Busy, Rec and Notify, shows the keyboard page in
// the Power slot and the prompt on the title row; its preview box is the rule.
namespace tall {

namespace P = oled_layout::portrait;
constexpr int kHeight = P::kHeaderHeight;  // rows 0-19

struct Frame {
  char power[8];   // or the keyboard page
  char notify;     // '1'-'9', '+', '\0'
  bool busy;
  bool rec;
  char title[P::kTitleCells + 1];
  int ellipsisAt;  // cell of the mark, -1 = none
  int titleLen;
};

inline void composeTitle(const char* prefixRaw, const char* textRaw, bool keepTail, Frame& f) {
  char prefix[16];
  char text[oled_text::kLabelMax];
  const size_t pLen = oled_text::flatten(prefixRaw ? prefixRaw : "", prefix, sizeof prefix);
  const size_t tLen = oled_text::flatten(textRaw ? textRaw : "", text, sizeof text);
  f.titleLen = oled_layout::fitPortraitTitle(prefix, pLen, text, tLen, P::kTitleCells, keepTail, f.title,
                                             f.ellipsisAt);
}

inline void compose(const OledHeaderSpec& spec, const OledHeaderStatus& status, const char* modeName,
                    Frame& f) {
  f.power[0] = '\0';
  f.notify = '\0';
  if (!(spec.hide & (1u << kPower)) && status.power[0]) {
    const size_t n = strnlen(status.power, sizeof status.power - 1);
    memcpy(f.power, status.power, n);
    f.power[n] = '\0';
  }
  if (!(spec.hide & (1u << kNotify))) f.notify = notifyGlyph(status.unread);
  f.busy = status.busy;
  f.rec = status.rec;
  composeTitle(spec.prefix, spec.text ? spec.text : (modeName ? modeName : ""),
               spec.cut == OledTitleCut::KeepPrefix, f);
}

// The keyboard's top rows: the prompt on the title row, the page in the
// Power slot (at most 7 glyphs), Busy, Rec and Notify in their slots.
inline void composeKeyboard(const char* prompt, const char* mode, bool rec, bool busy, uint8_t unread,
                            Frame& f) {
  const size_t n = oled_text::flatten(mode ? mode : "", f.power, sizeof f.power);
  f.power[n] = '\0';
  f.notify = notifyGlyph(unread);
  f.busy = busy;
  f.rec = rec;
  composeTitle("", prompt, false, f);
}

// Rows 0..(rule ? 19 : 18): cleared, the status slots, the title and the rule.
template <class Gfx>
inline void draw(Gfx& g, const Frame& f, int width, uint16_t fg, uint16_t bg, bool rule = true) {
  g.fillRect(0, 0, width, rule ? kHeight : kHeight - 1, bg);
  g.setTextSize(1);
  g.setTextColor(fg);
  if (f.busy) drawBits(g, P::kBusyX, P::kStatusTextY, kHourglass, kHourglassH, kHourglassW, fg);
  if (f.rec) drawBits(g, P::kRecX, P::kStatusTextY + 1, kRecDot, kRecDotH, kRecDotW, fg);
  if (f.notify) {
    drawBits(g, P::kBellX, P::kStatusTextY, kBell, kBellH, kBellW, fg);
    g.setCursor(P::kNotifyCountX, P::kStatusTextY);
    g.print(f.notify);
  }
  if (f.power[0]) {
    const int n = static_cast<int>(strlen(f.power));
    g.setCursor(width - kCell * n, P::kStatusTextY);
    g.print(f.power);
  }
  for (int i = 0; i < f.titleLen; ++i) {
    const int x = P::kTitleX + i * kCell;
    if (i == f.ellipsisAt) {
      for (int d = 0; d < 3; ++d) g.drawPixel(x + 2 * d, P::kTitleY + kEllipsisY - kTextY, fg);
      continue;
    }
    g.setCursor(x, P::kTitleY);
    g.print(f.title[i]);
  }
  if (rule) g.drawFastHLine(0, P::kHeaderRuleY, width, fg);
}

}  // namespace tall

}  // namespace oled_header
