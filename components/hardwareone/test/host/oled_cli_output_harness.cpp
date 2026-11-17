// Drives the shipping CLI Output page (OLED_Mode_CLI.cpp) over the shipping
// console owner (OLED_ConsoleBuffer.cpp + OLED_ConsoleCore.h) with a
// framebuffer double that asserts every draw stays inside the content area,
// never wraps past the canvas and never happens while the ring's mutex is
// held. Assembled by test_oled_cli_output.py for each canvas geometry
// (HW1_CANVAS_HEIGHT / HW1_HEADER / HW1_FOOTER) with ASan/UBSan.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "OLED_Layout.h"
#include "OLED_ConsoleBuffer.h"
#include "System_TextPager.h"
#include "glcdfont.c"

using std::min;
using std::max;
using String = std::string;

// ---- geometry ----------------------------------------------------------------
#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT HW1_CANVAS_HEIGHT
#define OLED_CONTENT_START_Y (HW1_HEADER + 1)
#define OLED_CONTENT_HEIGHT (HW1_CANVAS_HEIGHT - HW1_HEADER - HW1_FOOTER - 1)
#define DISPLAY_COLOR_WHITE 1
#define DISPLAY_COLOR_BLACK 0
constexpr int oledVisibleRows(int rowHeight, int reservedHeight = 0, int baselineRows = 0) {
  return oled_layout::visibleRows(OLED_CONTENT_HEIGHT, rowHeight, reservedHeight, baselineRows);
}

// ---- platform doubles for the console owner -----------------------------------
using SemaphoreHandle_t = void*;
static std::timed_mutex gMutex;
static std::atomic<int> gHeld{0};
static int gTestHolds = 0;  // holds taken by the checks themselves (busy-ring cases)
static SemaphoreHandle_t xSemaphoreCreateMutex() { return (void*)&gMutex; }
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY 0xFFFFFFFFu
#define pdTRUE 1
static int xSemaphoreTake(SemaphoreHandle_t h, uint32_t ticks) {
  auto* m = static_cast<std::timed_mutex*>(h);
  bool ok;
  if (ticks == portMAX_DELAY) { m->lock(); ok = true; }
  else ok = m->try_lock_for(std::chrono::milliseconds(ticks));
  if (ok) gHeld.fetch_add(1);
  return ok ? 1 : 0;
}
static void xSemaphoreGive(SemaphoreHandle_t h) {
  gHeld.fetch_sub(1);
  static_cast<std::timed_mutex*>(h)->unlock();
}
enum class AllocPref { RequireInternal };
static std::vector<void*> gAllocs;
static void* ps_calloc(size_t n, size_t size, AllocPref, const char*) {
  void* p = calloc(n, size);
  gAllocs.push_back(p);
  return p;
}
static void heap_caps_free(void* p) { free(p); }
static uint32_t gNow = 1000;
static uint32_t millis() { return gNow; }
static struct { int oledCliHistorySize = 16; } gSettings;  // more slots than the tallest canvas has rows
static void noLog(const char*, ...) {}
#define ERROR_SYSTEMF(...) noLog(__VA_ARGS__)
#define DEBUG_SYSTEMF(...) noLog(__VA_ARGS__)

// INSERT_CONSOLE_METHODS

OledConsoleBuffer gOledConsole;

// ---- framebuffer double --------------------------------------------------------
struct Adafruit_SSD1306 {
  std::vector<uint8_t> buffer;
  int x = 0, y = 0;
  struct Text { int x, y; std::string text; };
  std::vector<Text> texts;
  Adafruit_SSD1306() : buffer(128 * ((HW1_CANVAS_HEIGHT + 7) / 8)) {}
  void pixel(int px, int py, int value) {
    if (px < 0 || px >= 128 || py < 0 || py >= HW1_CANVAS_HEIGHT) return;
    auto& byte = buffer[(py / 8) * 128 + px];
    if (value) byte |= 1u << (py & 7); else byte &= ~(1u << (py & 7));
  }
  void fillRect(int px, int py, int w, int h, int value) {
    assert(gHeld.load() == gTestHolds);
    assert(px >= 0 && py >= OLED_CONTENT_START_Y && px + w <= 128 &&
           py + h <= OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT);
    for (int yy = py; yy < py + h; ++yy) for (int xx = px; xx < px + w; ++xx) pixel(xx, yy, value);
  }
  void setTextSize(int size) { assert(size == 1); }
  void setTextColor(int) {}
  void setCursor(int px, int py) { x = px; y = py; }
  void print(const char* text) {
    assert(gHeld.load() == gTestHolds);  // the page never draws under the producer lock
    const int len = (int)strlen(text);
    // No wrapping: a row must fit the canvas and the content area.
    assert(x >= 0 && x + len * 6 <= 128);
    assert(y >= OLED_CONTENT_START_Y && y + 8 <= OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT);
    texts.push_back({x, y, text});
    for (; *text; ++text) {
      auto ch = static_cast<unsigned char>(*text);
      assert(ch != '\n');
      for (int xx = 0; xx < 6; ++xx) for (int yy = 0; yy < 8; ++yy) {
        if (xx < 5 && ((font[ch * 5 + xx] >> yy) & 1)) pixel(x + xx, y + yy, 1);
      }
      x += 6;
    }
  }
};
static Adafruit_SSD1306* oledDisplay = nullptr;

// ---- input and mode doubles ------------------------------------------------------
static struct { bool up = false, down = false, left = false, right = false; int wheelDelta = 0; } gNavEvents;
enum { INPUT_BUTTON_A = 1, INPUT_BUTTON_B = 2, INPUT_BUTTON_X = 4, INPUT_BUTTON_Y = 8 };
#define INPUT_CHECK(state, btn) (((state) & (btn)) != 0)
enum OledMode { OLED_MENU, OLED_CLI_VIEWER };
static OledMode currentOledMode = OLED_CLI_VIEWER;

// INSERT_PAGE

// ---- checks -----------------------------------------------------------------------
static const int kRows = (OLED_CONTENT_HEIGHT - 9) / 8;

static Adafruit_SSD1306 frame() {
  Adafruit_SSD1306 canvas;
  oledDisplay = &canvas;
  displayCliViewer();
  oledDisplay = nullptr;
  return canvas;
}
static std::string statusRow(const Adafruit_SSD1306& c) {
  for (const auto& t : c.texts) if (t.y == OLED_CONTENT_START_Y && t.x == 0) return t.text;
  return "";
}
// Text rows below the status row, in draw order, excluding cursor and marks.
static std::vector<std::string> rows(const Adafruit_SSD1306& c) {
  std::vector<std::string> out;
  for (const auto& t : c.texts) {
    if (t.y == OLED_CONTENT_START_Y || t.text == ">" || t.text == "\x18" || t.text == "\x19") continue;
    out.push_back(t.text);
  }
  return out;
}
static int cursorRow(const Adafruit_SSD1306& c) {
  for (const auto& t : c.texts) if (t.text == ">" && t.x == 0) return (t.y - CLI_ROWS_Y) / CLI_ROW_H;
  return -1;
}
static bool hasMark(const Adafruit_SSD1306& c, const char* mark) {
  for (const auto& t : c.texts) if (t.text == mark && t.x == CLI_GUTTER_X) return true;
  return false;
}
static bool press(uint32_t buttons) {
  const bool handled = handleCliViewerInput(0, 0, buttons);
  gNavEvents = {};
  return handled;
}
static bool nav(bool up, bool down, bool left = false, bool right = false) {
  gNavEvents.up = up; gNavEvents.down = down; gNavEvents.left = left; gNavEvents.right = right;
  return press(0);
}
// A wheel frame: the detent sum plus the one boolean the input layer derives from its sign.
static bool wheel(int detents) {
  gNavEvents.wheelDelta = detents;
  gNavEvents.up = detents < 0;
  gNavEvents.down = detents > 0;
  return press(0);
}
// Queue then drain, as debugOutputTask does: the envelope carries the delivery
// generation that was live when it was queued.
static void appendAt(const char* text, uint32_t ts) {
  gOledConsole.append(text, ts, gOledConsole.deliveryGeneration());
}
static void append(const char* text) { appendAt(text, gNow++); }
static void clearConsole() { gOledConsole.clear(); }
static std::string hint() { return oledCliViewerFooterHint(); }
static void preview(const char* dir, const char* name, const Adafruit_SSD1306& c) {
  if (!dir) return;
  std::ofstream out(std::string(dir) + "/" + name, std::ios::binary);
  out << "P5\n128 " << HW1_CANVAS_HEIGHT << "\n255\n";
  for (int yy = 0; yy < HW1_CANVAS_HEIGHT; ++yy)
    for (int xx = 0; xx < 128; ++xx)
      out.put(((c.buffer[(yy / 8) * 128 + xx] >> (yy & 7)) & 1) ? char(255) : char(0));
}
static std::string readAll() {
  // Everything the Reader shows, scrolling from its top to the last line
  // (never past it: that would continue into the next record).
  std::string joined;
  sReaderTop = 0;
  auto c = frame();
  std::vector<std::string> seen = rows(c);
  while (hasMark(c, "\x19")) {
    nav(false, true);
    c = frame();
    seen.push_back(rows(c).back());
  }
  for (auto& line : seen) joined += line;
  return joined;
}

static void run(const char* dir) {
  assert(CLI_ROWS == kRows && CLI_LIST_COLS == 19 && CLI_READER_COLS == 20);
  gOledConsole.init();
  assert(isCliViewerAvailable(nullptr));
  const int C = gSettings.oledCliHistorySize;  // ring slots
  const std::string cap = std::to_string(C);
  assert(C > kRows);

  // Empty page: a Live status, the empty notice, Back only, no retry frames.
  cliViewerOnEnter(true);  // the mode change itself forces the first frame
  auto c = frame();
  assert(statusRow(c) == "Live" && rows(c).size() == 1 && rows(c)[0] == "No CLI output yet");
  assert(hint() == "B:Back" && !oledCliViewerNeedsFrame());
  assert(press(INPUT_BUTTON_A) && !press(INPUT_BUTTON_B) && sView == CliView::LIVE);
  currentOledMode = OLED_MENU;
  append("elsewhere");
  assert(!oledCliViewerNeedsFrame());  // another page: the console does not force frames
  currentOledMode = OLED_CLI_VIEWER;
  assert(oledCliViewerNeedsFrame());
  clearConsole();

  // Live follows new output, including two records in one millisecond.
  const uint32_t tick = gNow++;
  appendAt("alpha", tick);
  appendAt("beta", tick);
  assert(oledCliViewerNeedsFrame());
  c = frame();
  assert(!oledCliViewerNeedsFrame());
  assert(statusRow(c) == "Live 2" && (rows(c) == std::vector<std::string>{"alpha", "beta"}));
  assert(cursorRow(c) == 1 && hint() == "A:Read Y:Old B:Back");
  preview(dir, "live.pgm", c);
  appendAt("gamma", tick);
  assert(oledCliViewerNeedsFrame());
  c = frame();
  assert(rows(c).back() == "gamma" && cursorRow(c) == 2);

  // Up pauses on the record above the newest, by ID, not by timestamp.
  oled_console::Status st0{};
  assert(gOledConsole.tryStatus(st0) && st0.count == 3);
  assert(nav(true, false) && sView == CliView::PAUSED && sSelectedId == st0.newestId - 1);
  c = frame();
  assert(statusRow(c) == "2/3" && cursorRow(c) == 1 && hint() == "A:Read X:Live B:Back");
  assert(nav(true, false) && sSelectedId == st0.oldestId);
  assert(nav(true, false) && sSelectedId == st0.oldestId);  // oldest: stays
  c = frame();
  assert(statusRow(c) == "1/3" && cursorRow(c) == 0);
  // Arrivals while paused are counted; the anchor does not move.
  append("delta");
  append("epsilon");
  c = frame();
  assert(statusRow(c) == "1/5 +2" && cursorRow(c) == 0 && rows(c)[0] == "alpha");
  preview(dir, "paused.pgm", c);
  // Down browses newer; Down past the newest resumes Live.
  for (int i = 0; i < 4; ++i) assert(nav(false, true));
  assert(sView == CliView::PAUSED && sSelectedId == st0.newestId + 2);
  assert(nav(false, true) && sView == CliView::LIVE);
  // X from Paused resumes Live; Y goes to the oldest retained.
  assert(press(INPUT_BUTTON_Y) && sView == CliView::PAUSED && sSelectedId == st0.oldestId);
  assert(press(INPUT_BUTTON_X) && sView == CliView::LIVE);
  assert(press(INPUT_BUTTON_X) && sView == CliView::LIVE);  // already following
  assert(nav(false, true) && sView == CliView::LIVE);

  // A long window: Live shows the newest rows with the older-above mark.
  for (int i = 0; i < kRows + 3; ++i) {  // 5 + kRows + 3 records, under C
    char text[16];
    snprintf(text, sizeof(text), "line %d", i);
    append(text);
  }
  c = frame();
  assert((int)rows(c).size() == kRows && cursorRow(c) == kRows - 1);
  assert(rows(c).back() == std::string("line ") + std::to_string(kRows + 2));
  assert(hasMark(c, "\x18") && !hasMark(c, "\x19"));
  // Paused at the oldest: the newer-below mark, no older-above mark.
  assert(press(INPUT_BUTTON_Y));
  c = frame();
  assert(cursorRow(c) == 0 && !hasMark(c, "\x18") && hasMark(c, "\x19"));
  // Browsing down keeps the selection visible by moving the window.
  for (int i = 0; i < kRows; ++i) nav(false, true);
  c = frame();
  assert(cursorRow(c) == kRows - 1 && hasMark(c, "\x18"));

  // A paused selection that is evicted expires visibly and moves to the
  // oldest survivor; the next navigation clears the notice.
  assert(press(INPUT_BUTTON_X) && sView == CliView::LIVE);
  clearConsole();
  for (int i = 1; i <= C; ++i) {
    char text[16];
    snprintf(text, sizeof(text), "rec %d", i);
    append(text);
  }
  oled_console::Status st{};
  assert(gOledConsole.tryStatus(st) && st.count == C && st.capacity == C);
  assert(press(INPUT_BUTTON_Y) && sSelectedId == st.oldestId);
  append("rec +1");
  append("rec +2");
  append("rec +3");
  c = frame();
  assert(statusRow(c) == "1/" + cap + " +3 expired" && sSelectedId == st.oldestId + 3 && rows(c)[0] == "rec 4");
  assert(nav(true, false) && !sSelectionExpired);
  c = frame();
  assert(statusRow(c) == "1/" + cap + " +3");
  preview(dir, "expired.pgm", c);

  // Reader: a long record is wrapped to 20 columns and reachable to its end.
  std::string longText;
  for (int i = 0; i < 10; ++i) longText += "0123456789";
  longText += "tail";  // 104 characters: 6 wrapped lines
  assert(press(INPUT_BUTTON_X));
  append(longText.c_str());
  assert(press(INPUT_BUTTON_A) && sView == CliView::READER && sReaderFromLive);
  c = frame();
  assert(gOledConsole.tryStatus(st));
  assert(statusRow(c) == cap + "/" + cap + " T+" + std::to_string((gNow - 1) / 1000) + "." +
                             std::to_string((gNow - 1) % 1000 + 1000).substr(1) + "s");
  assert(hint() == "\x18\x19:Scroll X:Live B:Back");
  assert((int)rows(c).size() == min(kRows, 6) && rows(c)[0] == "01234567890123456789");
  assert(hasMark(c, "\x19") == (kRows < 6) && !hasMark(c, "\x18"));
  assert(readAll() == longText);
  if (kRows < 6) { c = frame(); assert(hasMark(c, "\x18") && !hasMark(c, "\x19")); }
  preview(dir, "reader.pgm", c);
  // Scrolling up at the first line continues into the older record, opened
  // at its last page; a wheel has no Left/Right. Down at the newest's last
  // line stays. A fast spin clamps at the end and does not skip records.
  {
    const uint32_t longId = sReader.id;
    while (sReaderTop > 0) nav(true, false);  // back to the first line without crossing
    assert(nav(true, false) && sReader.id == longId - 1 && sReaderTop == 0);  // one-line record
    assert(nav(false, true) && sReader.id == longId && sReaderTop == 0);      // back into the long record
    assert(wheel(+9) && sReader.id == longId && sReaderTop == max(0, 6 - kRows));
    assert(nav(false, true) && sReader.id == longId);                          // newest: stays
    if (kRows < 6) {   // the record scrolls: a spin back stays inside it
      assert(wheel(-2) && sReader.id == longId && sReaderTop == max(0, 6 - kRows - 2));
      sReaderTop = 0;
    }                  // (a record that fits has no inside to scroll: Up crosses at once)
    assert(nav(true, false) && sReader.id == longId - 1);
    assert(nav(false, false, false, true) && sReader.id == longId && sReaderTop == 0);  // Right: top
    // Left opens the older record at its top; Up from there goes older again.
    assert(nav(false, false, true, false) && sReader.id == longId - 1 && sReaderTop == 0);
    assert(nav(true, false) && sReader.id == longId - 2);
    assert(nav(false, false, false, true) && nav(false, false, false, true) && sReader.id == longId);
  }
  // B from a Reader opened out of Live on the newest record returns to Live.
  assert(press(INPUT_BUTTON_B) && sView == CliView::LIVE && sReader.id == 0);

  // Multiline records keep their line breaks; previews show one line with a mark.
  append("first\nsecond");
  c = frame();
  assert(rows(c).back() == "first~");
  assert(press(INPUT_BUTTON_A));
  c = frame();
  assert((rows(c) == std::vector<std::string>{"first", "second"}));
  // Left/Right change records and the list returns on the record that was open.
  assert(nav(false, false, true, false) && sReader.id == st.newestId && std::string(sReader.text) == longText);
  assert(press(INPUT_BUTTON_B) && sView == CliView::PAUSED && sSelectedId == st.newestId);
  c = frame();
  assert(statusRow(c) == std::to_string(C - 1) + "/" + cap);
  assert(press(INPUT_BUTTON_A) && sView == CliView::READER && !sReaderFromLive);
  assert(nav(false, false, false, true) && sReader.id == st.newestId + 1);
  assert(nav(false, false, false, true) && sReader.id == st.newestId + 1);  // newest: stays
  assert(press(INPUT_BUTTON_A) && sView == CliView::PAUSED && sSelectedId == st.newestId + 1);
  assert(press(INPUT_BUTTON_X) && sView == CliView::LIVE);

  // A Reader keeps its owned copy after eviction and never shows another
  // record in its place; Left has nothing older, Right continues at the
  // oldest survivor.
  assert(press(INPUT_BUTTON_Y) && press(INPUT_BUTTON_A));
  const std::string pinned = sReader.text;
  const uint32_t pinnedId = sReader.id;
  for (int i = 0; i < C; ++i) append("newer output");
  c = frame();
  assert(statusRow(c).rfind("expired T+", 0) == 0 && std::string(sReader.text) == pinned);
  assert(rows(c)[0] == pinned.substr(0, 20));
  assert(nav(false, false, true, false) && sReader.id == pinnedId);
  assert(nav(true, false) && sReader.id == pinnedId);                 // nothing older than an expired record
  assert(gOledConsole.tryStatus(st));
  assert(nav(false, true) && sReader.id == st.oldestId);             // Down continues at the oldest survivor
  assert(nav(false, false, false, true) && sReader.id == st.oldestId + 1);   // Right: newer, at its top
  assert(nav(false, false, true, false) && sReader.id == st.oldestId);       // Left: older, at its top
  assert(press(INPUT_BUTTON_Y) && sReader.id == st.oldestId);
  assert(press(INPUT_BUTTON_B) && sView == CliView::PAUSED && sSelectedId == st.oldestId && !sSelectionExpired);
  // Closing a Reader whose record expired lands on the oldest survivor, flagged.
  assert(press(INPUT_BUTTON_A));
  for (int i = 0; i < C; ++i) append("flood");
  assert(press(INPUT_BUTTON_B) && sView == CliView::PAUSED && sSelectionExpired);
  assert(press(INPUT_BUTTON_X));

  // Hostile text reaches the page sanitized and in bounds.
  append("\x1b[31mred\x1b[0m");
  append("\xe2\x00");
  append("caf\xc3\xa9 \xe2\x80\xa6");
  c = frame();
  auto r = rows(c);
  assert(r[r.size() - 3] == "red" && r[r.size() - 2] == "" && r.back() == "cafe ...");
  // A 300-character envelope keeps 255 characters, flagged in the Reader.
  std::string huge(300, 'h');
  append(huge.c_str());
  assert(press(INPUT_BUTTON_A));
  c = frame();
  assert(statusRow(c).find(" ~") != std::string::npos);
  assert(readAll() == std::string(255, 'h'));
  assert(press(INPUT_BUTTON_X));
  // A 40-character line previews as 18 characters and a cut mark.
  append("0123456789012345678901234567890123456789");
  c = frame();
  assert(rows(c).back() == "012345678901234567~");

  // A busy ring: the page says so, asks for a retry frame, drops no input
  // silently into another state, and recovers once the ring is free.
  assert(xSemaphoreTake(&gMutex, 0) == pdTRUE);
  gTestHolds = 1;
  c = frame();
  assert(statusRow(c) == "Console busy..." && oledCliViewerNeedsFrame() && hint() == "B:Back");
  assert(!press(INPUT_BUTTON_A) && sView == CliView::LIVE);
  gTestHolds = 0;
  xSemaphoreGive(&gMutex);
  c = frame();
  assert(statusRow(c).rfind("Live", 0) == 0 && !oledCliViewerNeedsFrame());
  // Output dropped by a held ring is reported on the Live status row.
  assert(xSemaphoreTake(&gMutex, 0) == pdTRUE);
  append("dropped");
  xSemaphoreGive(&gMutex);
  c = frame();
  assert(statusRow(c) == "Live " + cap + " lost 1");

  // The identity boundary erases the owned Reader copy and every anchor.
  assert(press(INPUT_BUTTON_A) && sReader.length > 0);
  oledCliViewerResetSessionState();
  assert(sView == CliView::LIVE && sReader.length == 0 && sReader.id == 0 && sReader.text[0] == '\0');
  assert(sSelectedId == 0 && sPauseNewestId == 0 && hint() == "B:Back");
  clearConsole();
  c = frame();
  assert(rows(c)[0] == "No CLI output yet");
  // Entering the page always starts Live, whatever it showed before.
  append("one");
  press(INPUT_BUTTON_Y);
  assert(sView == CliView::PAUSED);
  cliViewerOnEnter(false);
  assert(sView == CliView::LIVE);
  assert(gHeld.load() == 0);
  printf("canvas %dx%d: %d rows passed\n", 128, HW1_CANVAS_HEIGHT, kRows);
}

int main(int argc, char** argv) {
  run(argc > 1 ? argv[1] : nullptr);
  for (void* p : gAllocs) free(p);
}
