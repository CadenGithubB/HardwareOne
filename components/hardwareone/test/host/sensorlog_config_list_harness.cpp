// test_sensorlog_ui.py, second unit: the Logging "Configure" list drawn by the
// shipping scroll code. The driver splices in the sensor catalog, the
// OLEDScrollItem/OLEDScrollState types (OLED_Utils.h), the scroll primitives and
// both list renderers (OLED_Utils.cpp), the Logging row constants,
// handleLoggingModeInput and the LOG_MENU_SENSOR_CONFIG render block. The
// display records every draw so each frame can be checked against the
// 128x64 panel: rows inside the content area, every glyph on the panel, and a
// visible scroll indicator while rows are hidden.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
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

class String {
 public:
  String(const char* value = "") : value_(value) {}
  String(const std::string& value) : value_(value) {}
  explicit String(unsigned long value) : value_(std::to_string(value)) {}
  const char* c_str() const { return value_.c_str(); }
  friend String operator+(const String& a, const String& b) {
    return String(a.value_ + b.value_);
  }
 private:
  std::string value_;
};

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
  void setTextColor(uint16_t, uint16_t) {}
  void setCursor(int x, int y) { cx_ = x; cy_ = y; }
  void print(const char* text) {
    const std::string value(text ? text : "");
    if (!value.empty()) ops.push_back({'T', cx_, cy_, 6 * static_cast<int>(value.size()), 8, value});
    cx_ += 6 * static_cast<int>(value.size());
  }
  void println(const char* text) { print(text); println(); }
  void println() { cx_ = 0; cy_ += 8; }
  void fillRect(int x, int y, int w, int h, uint16_t) { ops.push_back({'R', x, y, w, h, ""}); }
  void drawFastVLine(int x, int y, int h, uint16_t) { ops.push_back({'V', x, y, 1, h, ""}); }
 private:
  int cx_ = 0, cy_ = 0;
};
struct OLEDFooterHints;
void oledRenderFooter(Adafruit_SSD1306*, const OLEDFooterHints*) {}
bool oledDrawIcon(Adafruit_SSD1306*, const char*, int, int, int) { return true; }

struct NavEvents {
  bool up, down, left, right;
  int deltaX, deltaY, wheelDelta;
};
NavEvents gNavEvents = {};

// INSERT_TYPES

// Declarations with the header's default arguments (OLED_Utils.h).
void oledScrollInit(OLEDScrollState* state, const char* title = nullptr, int visibleLines = 4);
bool oledScrollAddItem(OLEDScrollState* state, const char* line1, const char* line2 = nullptr,
                       bool selectable = true, void* userData = nullptr);
void oledScrollClear(OLEDScrollState* state);
void oledScrollClampSelection(OLEDScrollState* state);
void oledScrollUp(OLEDScrollState* state);
void oledScrollDown(OLEDScrollState* state);
bool oledScrollHandleNav(OLEDScrollState* state, bool leftRightNav = false);
void oledScrollSetSplitPane(OLEDScrollState* state, int listWidth, int separatorX, int iconSize = 32);
void oledScrollRender(Adafruit_SSD1306* display, OLEDScrollState* state, bool showScrollbar = true,
                      bool showSelection = true, const OLEDFooterHints* footerHints = nullptr);
void oledScrollRenderSimple(Adafruit_SSD1306* display, OLEDScrollState* state,
                            bool showSelection = true, int startY = OLED_CONTENT_START_Y);

// INSERT_SCROLL

static uint8_t selectedMask = 0;
uint8_t sensorLogSelectedMask() { return selectedMask; }
static bool gSensorLoggingRunning = false;
static bool gSystemLogRunning = false;
static uint32_t sensorLogIntervalMs() { return 5000; }
static SensorLogFormat sensorLogCurrentFormat() { return SENSOR_LOG_CSV; }
static struct { String sensorLogPath{"/logs/sensors.txt"}; } gSettings;

static LoggingMenuState loggingCurrentState = LOG_MENU_MAIN;
static int loggingMenuSelection = 0, loggingSensorConfigSelection = 0;
static const int loggingMainMenuCount = 3, loggingSensorMenuCount = 3;
static const int loggingSystemMenuCount = 2;
static OLEDScrollState loggingConfigScroll;
static Adafruit_SSD1306 display;
static Adafruit_SSD1306* oledDisplay = &display;
enum { INPUT_BUTTON_A = 1, INPUT_BUTTON_B = 2 };
#define INPUT_CHECK(value, button) ((value) & (button))
bool oledGuestBlocksMutate() { return false; }
static std::vector<std::string> requests;
void executeOLEDCommand(const String& command) { requests.emplace_back(command.c_str()); }
bool oledRunCommand(const String& command) { requests.emplace_back(command.c_str()); return true; }

// INSERT_PRODUCTION

static std::set<std::string> failures;
#define CHECK(condition, message) do { if (!(condition)) failures.insert(message); } while (false)

static std::string glyphName(const std::string& text) {
  if (text == "\x18") return "up arrow";
  if (text == "\x19") return "down arrow";
  return "'" + text + "'";
}

// One rendered frame of the Configure list, checked against the panel.
static void checkFrame(const std::vector<std::string>& labels, int selected) {
  display.ops.clear();
  renderSensorConfig();
  std::set<int> rowYs;
  bool selectedShown = false;
  bool indicatorVisible = false;
  for (const DrawOp& op : display.ops) {
    const bool isRow = std::find(labels.begin(), labels.end(), op.text) != labels.end();
    if (op.kind == 'T') {
      char message[160];
      std::snprintf(message, sizeof(message),
                    "%s drawn at x=%d..%d, y=%d: not fully on the %dx%d panel",
                    glyphName(op.text).c_str(), op.x, op.x + op.w - 1, op.y, SCREEN_WIDTH, SCREEN_HEIGHT);
      CHECK(op.x >= 0 && op.x + op.w <= SCREEN_WIDTH && op.y >= 0 && op.y + op.h <= SCREEN_HEIGHT, message);
    }
    if (isRow) {
      rowYs.insert(op.y);
      CHECK(op.y >= OLED_CONTENT_START_Y && op.y + 8 <= OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT,
            "a row is drawn outside the content area (header or footer)");
      if (op.text == labels[static_cast<size_t>(selected)]) selectedShown = true;
    } else if (op.x >= SCREEN_WIDTH - 8 && op.x < SCREEN_WIDTH && op.x + op.w > 0) {
      indicatorVisible = true;  // right-edge indicator with at least one pixel on the panel
    }
  }
  const int rows = static_cast<int>(rowYs.size());
  CHECK(selectedShown, "the selected row is not drawn");
  if (rows < static_cast<int>(labels.size())) {
    CHECK(indicatorVisible, "rows are hidden but no scroll indicator is visible on the panel");
  }
  const int fits = min(static_cast<int>(labels.size()), OLED_CONTENT_HEIGHT / 8);
  char message[120];
  std::snprintf(message, sizeof(message),
                "the list shows %d rows; the content area holds %d single-line rows", rows, fits);
  CHECK(rows == fits, message);
}

int main() {
  selectedMask = 0x05;
  // Enter Configure from the sensor menu through the real handler.
  loggingCurrentState = LOG_MENU_SENSOR;
  loggingMenuSelection = 2;
  if (!handleLoggingModeInput(0, 0, INPUT_BUTTON_A) || loggingCurrentState != LOG_MENU_SENSOR_CONFIG) {
    std::fprintf(stderr, "Configure did not open the sensor config list\n");
    return 1;
  }
  std::vector<std::string> labels;
  for (const auto& sensor : kSensorLogSensors) {
    labels.push_back(std::string(sensor.label) + ((selectedMask & sensor.bit) ? ": ON" : ": OFF"));
  }
  labels.push_back("Int: 5000ms");
  labels.push_back("Fmt: CSV");
  labels.push_back("Back");
  if (static_cast<int>(labels.size()) != loggingSensorConfigCount) {
    std::fprintf(stderr, "row count drifted: %zu labels, %d rows\n", labels.size(), loggingSensorConfigCount);
    return 1;
  }

  int expected = 0;
  checkFrame(labels, expected);
  // Walk to the bottom (Back) and on past it, then back to the top.
  for (int step = 0; step < loggingSensorConfigCount + 1; ++step) {
    gNavEvents = {};
    gNavEvents.down = true;
    handleLoggingModeInput(0, 0, 0);
    expected = min(expected + 1, loggingSensorConfigCount - 1);
    CHECK(loggingSensorConfigSelection == expected, "Down did not move the cursor one row");
    checkFrame(labels, expected);
  }
  for (int step = 0; step < loggingSensorConfigCount + 1; ++step) {
    gNavEvents = {};
    gNavEvents.up = true;
    handleLoggingModeInput(0, 0, 0);
    expected = max(expected - 1, 0);
    CHECK(loggingSensorConfigSelection == expected, "Up did not move the cursor one row");
    checkFrame(labels, expected);
  }
  gNavEvents = {};
  // A on Back returns to the sensor menu with Configure selected.
  for (int step = 0; step < loggingBackRow; ++step) {
    gNavEvents.down = true;
    handleLoggingModeInput(0, 0, 0);
  }
  gNavEvents = {};
  handleLoggingModeInput(0, 0, INPUT_BUTTON_A);
  CHECK(loggingCurrentState == LOG_MENU_SENSOR && loggingMenuSelection == 2,
        "A on Back did not return to the sensor menu");
  CHECK(requests.empty(), "navigating the list submitted a command");

  for (const std::string& failure : failures) std::fprintf(stderr, "FAIL [config list] %s\n", failure.c_str());
  if (!failures.empty()) return 1;
  std::printf("sensorlog config list: %zu rows, every frame on the panel\n", labels.size());
  return 0;
}
