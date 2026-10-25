// test_systemlog_config_ui.py: the OLED System Logging > Configure screens.
// The driver splices in the shipping debug-flag table (System_DebugFlags.h)
// with DebugFlagMask/DEBUG_BIT (System_Debug.h), the mask parse/format,
// systemLogApplyPersistedFlags and systemLogConfiguredFlags (System_Debug.cpp),
// the scroll primitives and single-line renderer (OLED_Utils.cpp), and the
// whole System Logging > Configure block of OLED_Mode_Logging.cpp. The display
// records every draw so each frame is checked against the 128x64 panel.
// Commands go to the executor boundary, which stores them the way the
// `logcategorytags` / `systemlogflags` set-commands do.
#include "Arduino.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using std::max;
using std::min;

#define EXT_RAM_BSS_ATTR
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_CONTENT_START_Y 11  // 10 px header + 1 px gap
#define OLED_CONTENT_HEIGHT 43   // 64 - 10 header - 10 footer - 1
#define DISPLAY_COLOR_WHITE 1
#define DISPLAY_COLOR_BLACK 0

static int checks = 0;
#define CHECK(condition) do { \
  if (!(condition)) { std::fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #condition); std::abort(); } \
  ++checks; } while (0)

struct DrawOp {
  char kind;  // 'T' text, 'R' filled rect, 'V' vertical line
  int x, y, w, h;
  std::string text;
};

// Text is 6x8 per glyph at size 1 (Adafruit GFX classic font).
class Adafruit_SSD1306 {
 public:
  std::vector<DrawOp> ops;
  void setTextSize(int) {}
  void setTextColor(uint16_t) {}
  void setCursor(int x, int y) { cx_ = x; cy_ = y; }
  void print(const char* text) {
    const std::string value(text ? text : "");
    if (!value.empty()) ops.push_back({'T', cx_, cy_, 6 * static_cast<int>(value.size()), 8, value});
    cx_ += 6 * static_cast<int>(value.size());
  }
  void println() { cx_ = 0; cy_ += 8; }
  void fillRect(int x, int y, int w, int h, uint16_t) { ops.push_back({'R', x, y, w, h, ""}); }
  void drawFastVLine(int x, int y, int h, uint16_t) { ops.push_back({'V', x, y, 1, h, ""}); }
 private:
  int cx_ = 0, cy_ = 0;
};

struct NavEvents {
  bool up, down, left, right;
  int deltaX, deltaY, wheelDelta;
};
NavEvents gNavEvents = {};

// INSERT_TYPES

// Declarations with the header defaults (OLED_Utils.h, System_Debug.h).
void oledScrollInit(OLEDScrollState* state, const char* title = nullptr, int visibleLines = 4);
bool oledScrollAddItem(OLEDScrollState* state, const char* line1, const char* line2 = nullptr,
                       bool selectable = true, void* userData = nullptr);
void oledScrollClearKeepSelection(OLEDScrollState* state);
void oledScrollClampSelection(OLEDScrollState* state);
void oledScrollUp(OLEDScrollState* state);
void oledScrollDown(OLEDScrollState* state);
bool oledScrollHandleNav(OLEDScrollState* state, bool leftRightNav = false);
void oledScrollRenderSimple(Adafruit_SSD1306* display, OLEDScrollState* state,
                            bool showSelection = true, int startY = OLED_CONTENT_START_Y);
bool parseSystemLogFlags(const String& flagsStr, DebugFlagMask& out);
void formatSystemLogFlags(DebugFlagMask mask, char* out, size_t outSize);
bool systemLogApplyPersistedFlags();
DebugFlagMask systemLogConfiguredFlags();

DebugFlagMask gDebugFlags;
struct HostSettings {
  bool systemLogCategoryTags = true;
  String systemLogPath;
  String systemLogFlags;
} gSettings;

static LoggingMenuState loggingCurrentState = LOG_MENU_SYSTEM;
static int loggingMenuSelection = 0;
static OLEDScrollState loggingConfigScroll;
static Adafruit_SSD1306 display;
static Adafruit_SSD1306* oledDisplay = &display;
enum { INPUT_BUTTON_A = 1, INPUT_BUTTON_B = 2 };
#define INPUT_CHECK(value, button) ((value) & (button))

// The executor boundary: record the request, then store it as the generic
// set-command does (and, for systemlogflags, apply it live like its handler).
static std::vector<std::string> requests;
bool oledRunCommand(const String& command) {
  const std::string text = command.c_str();
  requests.push_back(text);
  const std::string tags = "logcategorytags ", flags = "systemlogflags ";
  if (text.rfind(tags, 0) == 0) gSettings.systemLogCategoryTags = text.substr(tags.size()) == "1";
  if (text.rfind(flags, 0) == 0) {
    gSettings.systemLogFlags = text.substr(flags.size()).c_str();
    systemLogApplyPersistedFlags();
  }
  return true;
}

// INSERT_PRODUCTION

// ---------------------------------------------------------------------------

static const int kContentBottom = OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT;

// Draw one frame and check it against the panel. Returns the text ops minus
// the "> " cursor, in draw order.
static std::vector<DrawOp> frame() {
  display.ops.clear();
  renderSystemConfig();
  bool scrollbar = false;
  for (const DrawOp& op : display.ops) scrollbar |= op.kind == 'V';
  std::vector<DrawOp> text;
  for (const DrawOp& op : display.ops) {
    CHECK(op.x >= 0 && op.y >= OLED_CONTENT_START_Y && op.y + op.h <= kContentBottom);
    if (op.kind != 'T') continue;
    // A glyph cell's last column is blank, so a cell may end at 126 beside the
    // 3 px scrollbar thumb (x 125..127) and at 128 without one.
    CHECK(op.x + op.w <= (scrollbar ? SCREEN_WIDTH - 2 : SCREEN_WIDTH));
    if (op.text != "> " && op.text != "  ") text.push_back(op);
  }
  return text;
}

static std::vector<std::string> rows() {
  std::vector<std::string> out;
  for (const DrawOp& op : frame()) out.push_back(op.text);
  return out;
}

static std::string selectedRow() {
  frame();
  for (size_t i = 0; i + 1 < display.ops.size(); ++i) {
    if (display.ops[i].kind == 'T' && display.ops[i].text == "> ") return display.ops[i + 1].text;
  }
  return "";
}

static void press(uint32_t button) { CHECK(handleSystemConfigInput(button)); }

static void down(int times = 1) {
  for (int i = 0; i < times; ++i) {
    gNavEvents.down = true;
    CHECK(handleSystemConfigInput(0));
    gNavEvents.down = false;
    frame();
  }
}

// Move the cursor to the row whose text is `row`.
static void select(const std::string& row) {
  loggingConfigScroll.selectedIndex = 0;
  loggingConfigScroll.scrollOffset = 0;
  for (int guard = 0; guard < OLED_SCROLL_MAX_ITEMS && selectedRow() != row; ++guard) down();
  CHECK(selectedRow() == row);
}

static std::string bankRow(int bank, DebugFlagMask mask) {
  int on = 0, total = 0;
  for (int i = 0; i < DBG_FLAG_COUNT; ++i) {
    if (kDbgBank[i] != bank || kDbgTag[i][0] == '\0') continue;
    ++total;
    if (mask & kDbgMask[i]) ++on;
  }
  return total ? std::string(kDbgBankLabel[bank]) + " " + std::to_string(on) + "/" + std::to_string(total) : "";
}

int main() {
  gDebugFlags = DEBUG_AUTH | DEBUG_HTTP;

  // System Logging > Configure opens on Tags. Nothing saved yet: the counts
  // follow the live mask that a plain `log start` keeps.
  openSystemConfig();
  CHECK(loggingCurrentState == LOG_MENU_SYSTEM_CONFIG);
  CHECK(rows() == std::vector<std::string>({"Tags: ON", "Categories: 2 >", "Path: auto", "Back"}));
  CHECK(selectedRow() == "Tags: ON");

  // Tags toggles the saved setting through logcategorytags.
  press(INPUT_BUTTON_A);
  CHECK(requests.back() == "logcategorytags 0" && rows()[0] == "Tags: OFF");
  press(INPUT_BUTTON_A);
  CHECK(requests.back() == "logcategorytags 1" && rows()[0] == "Tags: ON");

  // Path: the row shows the clipped file name; A opens the full path, and
  // either button comes back to the Path row.
  const std::string path = "/logging_captures/system/system-2026-10-25T12-00-00.log";
  gSettings.systemLogPath = path.c_str();
  CHECK(rows()[2] == "Path: system-2026-1");
  select("Path: system-2026-1");
  requests.clear();
  press(INPUT_BUTTON_A);
  CHECK(loggingCurrentState == LOG_MENU_SYSTEM_PATH);
  {
    const std::vector<std::string> lines = rows();
    CHECK(!lines.empty() && lines[0] == "Log file path:");
    std::string shown;
    for (size_t i = 1; i < lines.size(); ++i) shown += lines[i];
    CHECK(shown == path);
  }
  gNavEvents.down = true;
  CHECK(handleSystemConfigInput(0));  // consumed, nothing to move
  gNavEvents.down = false;
  CHECK(loggingCurrentState == LOG_MENU_SYSTEM_PATH);
  press(INPUT_BUTTON_B);
  CHECK(loggingCurrentState == LOG_MENU_SYSTEM_CONFIG && selectedRow() == "Path: system-2026-1");
  press(INPUT_BUTTON_A);
  press(INPUT_BUTTON_A);
  CHECK(loggingCurrentState == LOG_MENU_SYSTEM_CONFIG && selectedRow() == "Path: system-2026-1");
  CHECK(requests.empty());

  // Categories lists every family that has categories, never Control.
  select("Categories: 2 >");
  press(INPUT_BUTTON_A);
  CHECK(loggingCurrentState == LOG_MENU_SYSTEM_BANKS);
  std::vector<std::string> banks;
  for (int b = 0; b < DBG_BANK_COUNT; ++b) {
    if (!bankRow(b, gDebugFlags).empty()) banks.push_back(bankRow(b, gDebugFlags));
  }
  banks.push_back("Back");
  CHECK(banks.size() == static_cast<size_t>(DBG_BANK_COUNT));  // every bank but Control, + Back
  {
    std::vector<std::string> shown;
    loggingConfigScroll.selectedIndex = 0;
    shown.push_back(selectedRow());
    for (size_t i = 1; i < banks.size(); ++i) {
      down();
      shown.push_back(selectedRow());
    }
    CHECK(shown == banks);
    for (const std::string& row : shown) CHECK(row.rfind("Control", 0) != 0);
  }

  // Every family's flag list fits the panel at every scroll position, keeps
  // its heading, and labels match the shared table (clipped to 19 chars).
  for (int b = 0; b < DBG_BANK_COUNT; ++b) {
    if (bankRow(b, gDebugFlags).empty()) continue;
    loggingCurrentState = LOG_MENU_SYSTEM_BANKS;
    select(bankRow(b, gDebugFlags));
    press(INPUT_BUTTON_A);
    CHECK(loggingCurrentState == LOG_MENU_SYSTEM_FLAGS);
    std::vector<std::string> expected;
    for (int i = 0; i < DBG_FLAG_COUNT; ++i) {
      if (kDbgBank[i] != b || kDbgTag[i][0] == '\0') continue;
      expected.push_back(((gDebugFlags & kDbgMask[i]) ? "[+] " : "[ ] ") +
                         std::string(kDbgLabel[i]).substr(0, 15));
    }
    expected.push_back("Back");
    std::vector<std::string> shown{selectedRow()};
    for (size_t i = 1; i < expected.size(); ++i) {
      down();
      CHECK(frame().front().text == bankRow(b, gDebugFlags));  // heading stays put
      shown.push_back(selectedRow());
    }
    CHECK(shown == expected);
    press(INPUT_BUTTON_B);
    CHECK(loggingCurrentState == LOG_MENU_SYSTEM_BANKS && selectedRow() == bankRow(b, gDebugFlags));
  }

  // Toggling a flag saves the whole mask through systemlogflags: the other
  // categories survive, and the change is live.
  const DebugFlagMask before = gDebugFlags;
  select(bankRow(DBG_BANK_ESPNOW, before));
  press(INPUT_BUTTON_A);
  requests.clear();
  const std::string espnowCore = std::string(kDbgLabel[DBG_ESPNOW_CORE]);
  CHECK(selectedRow() == "[ ] " + espnowCore);
  press(INPUT_BUTTON_A);
  CHECK(requests.size() == 1 && requests[0].rfind("systemlogflags 0x", 0) == 0);
  CHECK(systemLogConfiguredFlags() == (before | DEBUG_ESPNOW_CORE));
  CHECK(gDebugFlags == (before | DEBUG_ESPNOW_CORE));
  CHECK(selectedRow() == "[+] " + espnowCore);
  CHECK(frame().front().text == bankRow(DBG_BANK_ESPNOW, before | DEBUG_ESPNOW_CORE));
  press(INPUT_BUTTON_A);
  CHECK(systemLogConfiguredFlags() == before && selectedRow() == "[ ] " + espnowCore);

  // A saved mask wins over the live one; the counts follow it.
  gSettings.systemLogFlags = "0x1";
  gDebugFlags = DEBUG_AUTH | DEBUG_HTTP | DEBUG_WIFI;
  CHECK(systemLogConfiguredFlags() == DEBUG_AUTH);
  CHECK(frame().front().text == bankRow(DBG_BANK_ESPNOW, DEBUG_AUTH));

  // Back walks up one level at a time, landing on the row it came from.
  select("Back");
  press(INPUT_BUTTON_A);
  CHECK(loggingCurrentState == LOG_MENU_SYSTEM_BANKS && selectedRow() == bankRow(DBG_BANK_ESPNOW, DEBUG_AUTH));
  select("Back");
  press(INPUT_BUTTON_A);
  CHECK(loggingCurrentState == LOG_MENU_SYSTEM_CONFIG && selectedRow() == "Categories: 1 >");
  press(INPUT_BUTTON_B);
  CHECK(loggingCurrentState == LOG_MENU_SYSTEM && loggingMenuSelection == 2);
  CHECK(!loggingStateIsSystemConfig(loggingCurrentState));

  std::printf("systemlog config UI: %d checks passed\n", checks);
  return 0;
}
