// Host harness for the OLED header status bar (OLED_HeaderCore.h, compiled
// unchanged). Draws through a GFX double with the shipping glcdfont.c and
// Adafruit_GFX's classic drawChar, checks the layout rules exhaustively, the
// title cut, the Power text (shared battery backend), the staleness signature
// and the pixel regions, and prints the named mockup frames for the golden
// comparison in test_oled_header.py. Built with ASan/UBSan.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "OLED_HeaderCore.h"
#include "glcdfont.c"

using namespace oled_header;

namespace {

int gFailures = 0;
int gChecks = 0;
void check(bool ok, const std::string& what) {
  ++gChecks;
  if (ok) return;
  if (++gFailures <= 40) std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

// The calls draw() makes, as Adafruit_GFX makes them at text size 1 (print
// goes through write(): drawChar at the cursor, then the cursor moves 6 px;
// a glyph that would cross the right edge wraps, which the bar must never do).
struct Canvas {
  static constexpr int W = 128, H = 16;
  uint8_t px[H][W] = {};
  int cx = 0, cy = 0;
  uint16_t color = 1;
  bool wrapped = false, outside = false;

  void drawPixel(int x, int y, uint16_t c) {
    if (x < 0 || x >= W || y < 0 || y >= H) { outside = true; return; }
    px[y][x] = c ? 1 : 0;
  }
  void fillRect(int x, int y, int w, int h, uint16_t c) {
    for (int j = y; j < y + h; ++j)
      for (int i = x; i < x + w; ++i) drawPixel(i, j, c);
  }
  void drawFastHLine(int x, int y, int w, uint16_t c) { fillRect(x, y, w, 1, c); }
  void setTextSize(int s) { assert(s == 1); }
  void setTextColor(uint16_t c) { color = c; }
  void setCursor(int x, int y) { cx = x; cy = y; }
  void print(char ch) {
    if (ch == '\0') return;
    if (cx + 6 > W) { wrapped = true; cx = 0; cy += 8; }
    unsigned char c = static_cast<unsigned char>(ch);
    if (c >= 176) c++;  // the classic font's cp437 remap
    for (int i = 0; i < 5; ++i) {
      uint8_t line = font[c * 5 + i];
      for (int j = 0; j < 8; ++j, line >>= 1)
        if (line & 1) drawPixel(cx + i, cy + j, color);
    }
    cx += 6;
  }
  void print(const char* s) { while (*s) print(*s++); }
  std::string row(int y) const {
    std::string r;
    for (int x = 0; x < W; ++x) r += px[y][x] ? '#' : '.';
    return r;
  }
};

OledHeaderStatus status(const char* power, int unread, bool busy, bool rec) {
  OledHeaderStatus s{};
  std::snprintf(s.power, sizeof s.power, "%s", power);
  s.unread = static_cast<uint8_t>(unread);
  s.busy = busy;
  s.rec = rec;
  return s;
}

OledHeaderSpec spec(const char* prefix, const char* text, OledTitleCut cut = OledTitleCut::End,
                    const char* context = "") {
  OledHeaderSpec s{};
  s.prefix = prefix;
  s.text = text;
  s.cut = cut;
  std::snprintf(s.context, sizeof s.context, "%s", context);
  return s;
}

std::string titleText(const Title& t) {
  std::string s;
  for (int i = 0; i < t.cells; ++i) s += (i == t.ellipsisAt) ? '~' : t.glyphs[i];
  return s;
}

std::string fit(const char* prefix, const char* text, int cells, OledTitleCut cut) {
  Title t{};
  fitTitle(prefix, strlen(prefix), text, strlen(text), cells, cut, t);
  return titleText(t);
}

// ---- layout, exhaustive ------------------------------------------------------
void layoutCases() {
  const uint8_t powers[] = {0, 18, 24, 30};
  const uint8_t contexts[] = {0, 12, 18, 24, 30};
  int cases = 0;
  for (uint8_t power : powers)
    for (int notify = 0; notify < 2; ++notify)
      for (int rec = 0; rec < 2; ++rec)
        for (uint8_t context : contexts)
          for (int title = 0; title <= 24; ++title) {
            int16_t base[kItemCount];
            int baseCells = 0;
            for (int busy = 0; busy < 2; ++busy) {
              uint8_t w[kItemCount] = {};
              w[kPower] = power;
              w[kNotify] = notify ? kNotifyWidth : 0;
              w[kContext] = context;
              w[kRec] = rec ? kRecDotW : 0;
              w[kBusy] = busy ? kHourglassW : 0;
              int16_t x[kItemCount];
              const int cells = layout(w, title, x);
              ++cases;
              const std::string at = "power " + std::to_string(power) + " notify " + std::to_string(notify) +
                                     " rec " + std::to_string(rec) + " busy " + std::to_string(busy) +
                                     " context " + std::to_string(context) + " title " + std::to_string(title);
              const int reserve = title < kTitleReserveMax ? title : kTitleReserveMax;
              check(cells >= reserve && cells <= kTitleMaxCells, "title room within reserve..21: " + at);
              int left = kWidth;
              int prevRight = kWidth + kGap;  // next slot must end 3 px before the previous one starts
              for (int i = 0; i < kItemCount; ++i) {
                if (x[i] < 0) continue;
                check(x[i] >= 0 && x[i] + w[i] <= kWidth, "slot inside 0-127: " + at);
                check(x[i] + w[i] == prevRight - kGap, "slots packed 3 px apart from the right: " + at);
                prevRight = x[i];
                left = x[i];
              }
              if (left < kWidth) check(cells * kCell <= left - kGap, "title ends 3 px before the first slot: " + at);
              // The title is cut only when its text would really reach an item
              // (or, while busy, at the room it had before: it never grows back).
              if (title > cells && cells < kTitleMaxCells && (busy == 0 || cells < baseCells))
                check(title * kCell > left - kGap, "title cut without need: " + at);
              for (int i = 0; i < kItemCount; ++i) {
                if (w[i] == 0) { check(x[i] < 0, "absent item not shown: " + at); continue; }
                if (i != kContext) check(x[i] >= 0, "only Context may drop: " + at);
              }
              // A dropped item never outranks a shown one.
              for (int d = 0; d < kItemCount; ++d) {
                if (w[d] == 0 || x[d] >= 0) continue;
                for (int s = 0; s < kItemCount; ++s)
                  if (x[s] >= 0 && !kRules[s].pinned)
                    check(kRules[s].priority >= kRules[d].priority, "drop order follows priority: " + at);
              }
              if (busy == 0) {
                memcpy(base, x, sizeof base);
                baseCells = cells;
              } else {
                // Owner, 2026-11-22: nothing else in the bar moves when busy comes or goes.
                for (int i = 0; i < kItemCount; ++i)
                  if (i != kBusy && base[i] >= 0 && x[i] >= 0)
                    check(base[i] == x[i], "an item moved when busy came: " + at);
                check(base[kPower] == x[kPower] && base[kNotify] == x[kNotify], "power and notify fixed: " + at);
                check(cells <= baseCells, "busy never widens the title: " + at);
              }
            }
          }
  check(cases == 4 * 2 * 2 * 5 * 25 * 2, "case count");
}

// ---- title cut ---------------------------------------------------------------
void titleCases() {
  const auto E = OledTitleCut::End, K = OledTitleCut::KeepPrefix;
  check(fit("", "System Status", 13, E) == "System Status", "exact fit has no mark");
  check(fit("", "System Status", 12, E) == "System Stat~", "one over ends in the mark");
  check(fit("Set>", "notifications", 10, E) == "Set>notif~", "End keeps the start");
  check(fit("Files>", "/system/logs/boot", 14, K) == "Files>~gs/boot", "KeepPrefix keeps the folder");
  check(fit("Files>", "/system/logs/boot", 10, K) == "Files>~oot", "KeepPrefix with exactly 3 glyphs");
  check(fit("Files>", "/system/logs/boot", 9, K) == "Files>/s~", "KeepPrefix falls back to End");
  check(fit("Files>", "/a", 14, K) == "Files>/a", "KeepPrefix that fits is whole");
  check(fit("", "", 10, E).empty(), "empty title");
  check(fit("", "abc", 0, E).empty(), "no room");
  check(fit("", "abcdef", 1, E) == "~", "one cell is the mark alone");
  check(fit("", "abcdefghijklmnopqrstuvwxyz", 40, E).size() == static_cast<size_t>(kTitleMaxCells),
        "room capped at 21 cells");
  // Control characters and UTF-8 through the shared sanitizer.
  Frame f{};
  OledHeaderSpec s = spec("", "Caf\xC3\xA9\x01\tBar\x1B[31m!");
  compose(s, status("", 0, false, false), "Mode", f);
  check(titleText(f.title) == "Cafe Bar!", "title through the sanitizer: " + titleText(f.title));
  s = spec(nullptr, nullptr);
  compose(s, status("", 0, false, false), "Mode name", f);
  check(titleText(f.title) == "Mode name", "nullptr text is the mode name");
  // The whole of a long path reaches the cut (no small copy first).
  char path[128];
  std::memset(path, 'a', sizeof path - 6);
  std::memcpy(path + sizeof path - 6, "/tail", 6);
  s = spec("Files>", path, K);
  compose(s, status("42%", 0, false, false), "Files", f);
  check(titleText(f.title).substr(titleText(f.title).size() - 5) == "/tail", "deep path keeps its end");
}

// ---- items -------------------------------------------------------------------
std::string power(bool usb, bool avail, bool valid, bool est, float pct) {
  char out[6];
  formatPower(usb, avail, valid, est, pct, out);
  return out;
}

void itemCases() {
  check(power(true, false, false, false, 0) == "USB", "USB wins");
  check(power(true, true, true, true, 42) == "USB", "USB over the percentage");
  check(power(false, false, true, false, 42).empty(), "no battery measurement: hidden (Q4)");
  check(power(false, true, false, false, 0) == "--%", "unknown percentage");
  check(power(false, true, true, false, 0.0f / 0.0f) == "--%", "NaN percentage");
  check(power(false, true, true, false, 9.7f) == "9%", "9%");
  check(power(false, true, true, true, 42.2f) == "~42%", "estimated");
  check(power(false, true, true, true, 100) == "~100%", "~100%");
  check(power(false, true, true, true, 104) == "~100%", "clamped high");
  check(power(false, true, true, false, -3) == "0%", "clamped low");

  // Hidden power takes no slot; hide bits; context limits.
  Frame f{};
  OledHeaderSpec s = spec("", "Title");
  compose(s, status("", 3, false, false), "", f);
  check(f.x[kPower] < 0 && f.x[kNotify] == kWidth - kNotifyWidth, "no power slot when hidden");
  s.hide = 1u << kNotify;
  compose(s, status("42%", 3, false, false), "", f);
  check(f.x[kNotify] < 0 && f.x[kPower] == kWidth - 18, "hidden notify");
  s = spec("", "T", OledTitleCut::End, "12345");
  compose(s, status("42%", 0, false, false), "", f);
  check(f.x[kContext] >= 0 && f.w[kContext] == 30, "5-glyph context shown");
  s = spec("", "T", OledTitleCut::End, "123456");
  compose(s, status("42%", 0, false, false), "", f);
  check(f.x[kContext] < 0, "longer context not shown");
  s = spec("", "T", OledTitleCut::End, "3/12");
  s.hide = 1u << kContext;
  compose(s, status("42%", 0, false, false), "", f);
  check(f.x[kContext] < 0, "hidden context");
  s.hide = 0xFF;  // Busy and Rec cannot be hidden
  compose(s, status("42%", 1, true, true), "", f);
  check(f.x[kBusy] >= 0 && f.x[kRec] >= 0, "busy and rec ignore hide");
  // Power slot widths: 18 / 24 / 30, right-aligned.
  for (const char* p : {"9%", "42%", "~42%", "~100%"}) {
    compose(spec("", "T"), status(p, 0, false, false), "", f);
    const int n = static_cast<int>(strlen(p));
    check(f.w[kPower] == (n <= 3 ? 18 : n * 6), std::string("power slot for ") + p);
  }

  // Staleness signature: what is drawn, nothing else.
  const uint32_t a = signature(status("42%", 12, false, false));
  check(a == signature(status("42%", 13, false, false)), "10+ unread reads the same");
  check(a != signature(status("43%", 12, false, false)), "battery change");
  check(a != signature(status("USB", 12, false, false)), "USB change");
  check(a != signature(status("42%", 9, false, false)), "unread change");
  check(a != signature(status("42%", 12, true, false)), "busy change");
  check(a != signature(status("42%", 12, false, true)), "rec change");
  check(signature(status("42%", 0, false, false)) != signature(status("42%", 1, false, false)), "first unread");
}

// ---- pixels ------------------------------------------------------------------
void pixelCases() {
  const char* titles[] = {"", "Speech", "System Status", "notifications and more text"};
  const char* powers[] = {"", "9%", "USB", "~42%", "~100%"};
  const char* contexts[] = {"", "2/5", "12/34"};
  for (const char* t : titles)
    for (const char* p : powers)
      for (const char* c : contexts)
        for (int unread : {0, 3, 12})
          for (int flags = 0; flags < 4; ++flags) {
            Canvas g;
            g.fillRect(0, 0, Canvas::W, Canvas::H, 1);  // stale ink everywhere
            Frame f{};
            compose(spec("", t, OledTitleCut::End, c), status(p, unread, flags & 1, flags & 2), "", f);
            draw(g, f, 1, 0);
            const std::string at = std::string("title '") + t + "' power " + p + " ctx " + c;
            check(!g.wrapped && !g.outside, "no wrap, nothing outside: " + at);
            check(g.row(0) == std::string(128, '.'), "row 0 blank: " + at);
            check(g.row(9) == std::string(128, '#'), "row 9 is the rule: " + at);
            check(g.row(10) == std::string(128, '#'), "nothing below the band touched: " + at);
            for (int y = 1; y <= 8; ++y)
              for (int x = 0; x < 128; ++x) {
                if (!g.px[y][x]) continue;
                int owners = x < f.title.cells * kCell ? 1 : 0;
                for (int i = 0; i < kItemCount; ++i)
                  if (f.x[i] >= 0 && x >= f.x[i] && x < f.x[i] + f.w[i]) ++owners;
                check(owners == 1, "pixel owned by one region: " + at);
              }
          }
}

// ---- golden frames -----------------------------------------------------------
void dump(const char* name, const OledHeaderSpec& s, const OledHeaderStatus& st) {
  Canvas g;
  Frame f{};
  compose(s, st, "", f);
  draw(g, f, 1, 0);
  std::printf("%s\n", name);
  for (int y = 0; y < kHeight; ++y) std::printf("%s\n", g.row(y).c_str());
}

void goldens() {
  dump("M1", spec("", "System Status"), status("42%", 3, false, false));
  dump("M2", spec("", "System Status"), status("42%", 3, true, false));
  dump("M3", spec("Set>", "notifications", OledTitleCut::End, "12/34"), status("~100%", 10, true, true));
  dump("M4", spec("", "Speech", OledTitleCut::End, "2/5"), status("~100%", 3, true, false));
  dump("M5", spec("", "Automations", OledTitleCut::End, "3/12"), status("9%", 0, false, false));
  dump("M6", spec("Files>", "/system/logs/boot", OledTitleCut::KeepPrefix), status("~42%", 1, false, false));
  dump("M7", spec("", "Microphone"), status("USB", 0, true, true));
}

}  // namespace

int main() {
  layoutCases();
  titleCases();
  itemCases();
  pixelCases();
  if (gFailures) {
    std::fprintf(stderr, "%d of %d header checks failed\n", gFailures, gChecks);
    return 1;
  }
  goldens();
  std::fprintf(stderr, "OLED header: %d checks passed\n", gChecks);
  return 0;
}
