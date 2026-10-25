// test_sensorlog_ui.py inserts the shipping catalog, row construction and
// input handlers. This harness records requests without executing them, so
// rapid taps all see the same displayed logger state.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
  std::abort(); } } while (false)
#define EXT_RAM_BSS_ATTR
#define OLED_CONTENT_HEIGHT 43

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

// INSERT_TYPES

static uint8_t selectedMask = 0;
static unsigned maskReads = 0;
static bool changeMaskAfterRead = false;
uint8_t sensorLogSelectedMask() {
  ++maskReads;
  uint8_t result = selectedMask;
  if (changeMaskAfterRead) selectedMask ^= 0xff;
  return result;
}
static bool gSensorLoggingRunning = false;
static bool gSystemLogRunning = false;
static uint32_t gSensorLogIntervalMs = 1000;
static uint32_t sensorLogIntervalMs() {
  return gSensorLogIntervalMs;
}
static int gSensorLogFormat = SENSOR_LOG_TEXT;
static SensorLogFormat sensorLogCurrentFormat() {
  return static_cast<SensorLogFormat>(gSensorLogFormat);
}
static struct { String sensorLogPath{"/logs/a path.csv"}; } gSettings;

static G2HijackPage page = G2_HIJACK_PAGE_LOGGING;
static unsigned systemMenus = 0, listsShown = 0, delayedMs = 0;
static std::string shownText;
using Completion = void (*)(bool, const char*, const G2CmdCookie&, void*);
struct G2Request {
  std::string command;
  G2CmdCookie cookie;
  Completion completion;
  void* user;
};
static std::vector<G2Request> g2Requests;
static void onLoggingCmdDone(bool, const char*, const G2CmdCookie&, void*) {}
G2HijackPage g2GetHijackPage() { return page; }
void g2SetHijackPage(G2HijackPage value) { page = value; }
void g2ShowSystemMenu() { ++systemMenus; }
void g2ShowText(const char* value) { shownText = value; }
bool g2ShowListPage(const char* const*, size_t) { ++listsShown; return true; }
bool g2SubmitHijackCommand(const char* command, const G2CmdCookie& cookie,
                          Completion completion, void* user) {
  g2Requests.push_back({command, cookie, completion, user});
  return true;
}
#define pdMS_TO_TICKS(ms) (ms)
void vTaskDelay(unsigned ms) { delayedMs += ms; }

static LoggingMenuState loggingCurrentState = LOG_MENU_MAIN;
static int loggingMenuSelection = 0, loggingSensorConfigSelection = 0;
static const int loggingMainMenuCount = 3, loggingSensorMenuCount = 3;
static const int loggingSystemMenuCount = 3;
struct OLEDScrollState {
  int selectedIndex = 0, scrollOffset = 0;
  std::vector<std::string> rows;
};
static OLEDScrollState loggingConfigScroll;
static struct { bool up = false, down = false; } gNavEvents;
static bool guest = false, navigationPending = false;
static int navigationSelection = 0;
static unsigned scrollRenders = 0;
static void* oledDisplay = nullptr;
static std::vector<std::string> oledRequests;
enum { INPUT_BUTTON_A = 1, INPUT_BUTTON_B = 2 };
#define INPUT_CHECK(value, button) ((value) & (button))
bool oledGuestBlocksMutate() { return guest; }
bool oledScrollHandleNav(OLEDScrollState* state) {
  if (!navigationPending) return false;
  navigationPending = false;
  state->selectedIndex = navigationSelection;
  return true;
}
void executeOLEDCommand(const String& command) { oledRequests.emplace_back(command.c_str()); }
bool oledRunCommand(const String& command) { oledRequests.emplace_back(command.c_str()); return true; }
// System Logging > Configure has its own block and test (systemlog_config_ui);
// these record the hand-off from the shared handler.
static bool systemConfigOpened = false;
static bool loggingStateIsSystemConfig(LoggingMenuState) { return false; }
static bool handleSystemConfigInput(uint32_t) { return false; }
static void openSystemConfig() { systemConfigOpened = true; }
void oledScrollInit(OLEDScrollState* state, const char*, int) { *state = {}; }
void oledScrollSetSplitPane(OLEDScrollState*, int, int, int) {}
void oledScrollClear(OLEDScrollState* state) { *state = {}; }
void oledScrollAddItem(OLEDScrollState* state, const char* row, void*) {
  state->rows.emplace_back(row);
}
void oledScrollRender(void*, OLEDScrollState*, bool, bool) { ++scrollRenders; }
void oledScrollRenderSimple(void*, OLEDScrollState*) { ++scrollRenders; }

// INSERT_PRODUCTION

static void reset() {
  selectedMask = 0;
  maskReads = 0;
  changeMaskAfterRead = false;
  gSensorLoggingRunning = false;
  gSystemLogRunning = false;
  gSensorLogIntervalMs = 1000;
  gSensorLogFormat = SENSOR_LOG_TEXT;
  page = G2_HIJACK_PAGE_LOGGING;
  systemMenus = listsShown = delayedMs = scrollRenders = 0;
  shownText.clear();
  g2Requests.clear();
  oledRequests.clear();
  loggingCurrentState = LOG_MENU_SENSOR_CONFIG;
  loggingMenuSelection = loggingSensorConfigSelection = 0;
  loggingConfigScroll = {};
  gNavEvents = {};
  guest = navigationPending = false;
}

static void checkG2Request(size_t index, const std::string& command) {
  CHECK(index < g2Requests.size());
  const auto& request = g2Requests[index];
  CHECK(request.command == command);
  CHECK(request.cookie.targetPage == G2_HIJACK_PAGE_LOGGING);
  CHECK(request.cookie.targetNetSub == 0);
  CHECK(request.completion == onLoggingCmdDone);
  CHECK(request.user == nullptr);
}

int main() {
  const char* names[] = {"thermal", "tof", "imu", "gamepad", "apds", "gps", "presence", "r1"};
  const char* labels[] = {"Thermal", "ToF", "IMU", "Gamepad", "APDS", "GPS", "Presence", "R1 Health"};
  CHECK(kSensorLogSensorCount == 8);
  for (size_t i = 0; i < kSensorLogSensorCount; ++i) {
    CHECK(std::strcmp(kSensorLogSensors[i].name, names[i]) == 0);
    CHECK(std::strcmp(kSensorLogSensors[i].label, labels[i]) == 0);
    CHECK(kSensorLogSensors[i].bit == (1u << i));
  }

  // A renderer samples the selection exactly once, even if it changes during drawing.
  for (uint8_t mask : {uint8_t(0), uint8_t(0x55), uint8_t(0xff)}) {
    reset();
    selectedMask = mask;
    changeMaskAfterRead = true;
    CHECK(g2BuildLoggingRows() == kSensorLogSensorCount + 2);
    CHECK(maskReads == 1);
    CHECK(std::strcmp(gLogRowPtrs[0], "<- System") == 0);
    CHECK(std::strcmp(gLogRowPtrs[1], "Start logging") == 0);
    for (size_t i = 0; i < kSensorLogSensorCount; ++i) {
      std::string expected = (mask & (1u << i)) ? "[x] " : "[ ] ";
      CHECK(gLogRowPtrs[i + 2] == expected + labels[i]);
    }
    selectedMask = mask;
    maskReads = 0;
    loggingSensorConfigSelection = 4;
    loggingConfigScroll.scrollOffset = 2;
    renderSensorConfig();
    CHECK(maskReads == 1);
    CHECK(scrollRenders == 1);
    CHECK(loggingConfigScroll.selectedIndex == 4);
    CHECK(loggingConfigScroll.scrollOffset == 2);
    CHECK(loggingConfigScroll.rows.size() == kSensorLogSensorCount + 3);
    for (size_t i = 0; i < kSensorLogSensorCount; ++i) {
      std::string expected = std::string(labels[i]) + ((mask & (1u << i)) ? ": ON" : ": OFF");
      CHECK(loggingConfigScroll.rows[i] == expected);
    }
    CHECK(loggingConfigScroll.rows[loggingIntervalRow] == "Int: 1000ms");
    CHECK(loggingConfigScroll.rows[loggingFormatRow] == "Fmt: TXT");
    CHECK(loggingConfigScroll.rows[loggingBackRow] == "Back");
  }

  // Requests remain individual intents before the owner executes any of them.
  for (uint8_t mask : {uint8_t(0), uint8_t(0xff)}) {
    reset();
    selectedMask = mask;
    for (size_t i = 0; i < kSensorLogSensorCount; ++i) {
      g2LoggingHandleTap(static_cast<uint32_t>(i + 2));
      loggingSensorConfigSelection = static_cast<int>(i);
      CHECK(handleLoggingModeInput(0, 0, INPUT_BUTTON_A));
      std::string command = std::string("sensorlog sensors toggle ") + names[i];
      checkG2Request(i, command);
      CHECK(oledRequests[i] == command);
    }
    CHECK(selectedMask == mask);
    CHECK(maskReads == 0);
    CHECK(g2Requests.size() == kSensorLogSensorCount);
    CHECK(oledRequests.size() == kSensorLogSensorCount);
    g2LoggingHandleTap(2);
    loggingSensorConfigSelection = 0;
    CHECK(handleLoggingModeInput(0, 0, INPUT_BUTTON_A));
    checkG2Request(kSensorLogSensorCount, "sensorlog sensors toggle thermal");
    CHECK(oledRequests.back() == "sensorlog sensors toggle thermal");
  }

  reset();
  for (uint32_t index : {uint32_t(kSensorLogSensorCount + 2), std::numeric_limits<uint32_t>::max()})
    g2LoggingHandleTap(index);
  CHECK(g2Requests.empty());
  for (int index : {-1, loggingSensorConfigCount, std::numeric_limits<int>::max()}) {
    loggingSensorConfigSelection = index;
    CHECK(handleLoggingModeInput(0, 0, INPUT_BUTTON_A));
  }
  CHECK(oledRequests.empty());
  guest = true;
  loggingSensorConfigSelection = 0;
  CHECK(handleLoggingModeInput(0, 0, INPUT_BUTTON_A));
  CHECK(oledRequests.empty());
  guest = false;
  navigationPending = true;
  navigationSelection = 3;
  CHECK(handleLoggingModeInput(0, 0, INPUT_BUTTON_A));
  CHECK(oledRequests.back() == "sensorlog sensors toggle gamepad");

  reset();
  g2LoggingHandleTap(0);
  CHECK(systemMenus == 1);
  g2LoggingHandleTap(1);
  CHECK(g2Requests.empty());
  CHECK(shownText == "Pick a sensor first");
  CHECK(delayedMs == 1200);
  CHECK(listsShown == 1);
  selectedMask = LOG_GPS;
  g2LoggingHandleTap(1);
  checkG2Request(0, "sensorlog start \"/logs/a path.csv\"");
  gSensorLoggingRunning = true;
  g2LoggingHandleTap(1);
  checkG2Request(1, "sensorlog stop");
  CHECK(g2BuildLoggingRows() == kSensorLogSensorCount + 2);
  CHECK(std::strcmp(gLogRowPtrs[1], "Stop logging") == 0);

  reset();
  loggingSensorConfigSelection = loggingIntervalRow;
  for (unsigned long interval : {1000ul, 5000ul, 10000ul, 30000ul, 60000ul, 42ul}) {
    gSensorLogIntervalMs = interval;
    CHECK(handleLoggingModeInput(0, 0, INPUT_BUTTON_A));
  }
  CHECK(oledRequests == std::vector<std::string>({"sensorlog interval 5000", "sensorlog interval 10000",
      "sensorlog interval 30000", "sensorlog interval 60000", "sensorlog interval 1000", "sensorlog interval 1000"}));
  oledRequests.clear();
  loggingSensorConfigSelection = loggingFormatRow;
  for (int format : {SENSOR_LOG_TEXT, SENSOR_LOG_CSV, SENSOR_LOG_TRACK}) {
    gSensorLogFormat = format;
    CHECK(handleLoggingModeInput(0, 0, INPUT_BUTTON_A));
    renderSensorConfig();
    const char* displayNames[] = {"Fmt: TXT", "Fmt: CSV", "Fmt: TRK"};
    CHECK(loggingConfigScroll.rows[loggingFormatRow] == displayNames[format]);
  }
  CHECK(oledRequests == std::vector<std::string>({"sensorlog format csv", "sensorlog format track", "sensorlog format text"}));
  loggingSensorConfigSelection = loggingBackRow;
  CHECK(handleLoggingModeInput(0, 0, INPUT_BUTTON_A));
  CHECK(loggingCurrentState == LOG_MENU_SENSOR);
  CHECK(loggingMenuSelection == 2);

  // System Logging's third row hands off to its Configure screen.
  oledRequests.clear();
  loggingCurrentState = LOG_MENU_SYSTEM;
  loggingMenuSelection = 2;
  CHECK(handleLoggingModeInput(0, 0, INPUT_BUTTON_A));
  CHECK(systemConfigOpened && oledRequests.empty());
  std::printf("sensorlog UI: %u checks passed\n", checks);
}
