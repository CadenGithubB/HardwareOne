#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

class String : public std::string {
 public:
  using std::string::string;
  String() = default;
  String(const std::string& value) : std::string(value) {}
  explicit String(int value) : std::string(std::to_string(value)) {}
  void trim() {
    const size_t first = find_first_not_of(" \t\r\n");
    *this = first == npos ? "" : substr(first, find_last_not_of(" \t\r\n") - first + 1);
  }
  void toLowerCase() {
    std::transform(begin(), end(), begin(), [](unsigned char c) { return std::tolower(c); });
  }
  bool equalsIgnoreCase(const char* other) const {
    String left = *this, right = other;
    left.toLowerCase(); right.toLowerCase();
    return left == right;
  }
  int toInt() const { return std::atoi(c_str()); }
};
using std::min;

// INSERT_TYPES

static unsigned long nowMs;
static int delayCalls;
static unsigned long millis() { return nowMs; }
static void delay(unsigned long amount) {
  nowMs += amount;
  assert(++delayCalls < 500 && "production input loop did not terminate");
}
struct SerialInput { unsigned long at; String text; };
static std::deque<SerialInput> serialInputs;
struct SerialBoundary {
  bool available() const { return !serialInputs.empty() && serialInputs.front().at <= nowMs; }
  String readStringUntil(char) {
    assert(available());
    String value = serialInputs.front().text;
    serialInputs.pop_front();
    return value;
  }
  template<typename T> void print(const T&) {}
  template<typename T> void println(const T&) {}
  void println() {}
  template<typename... Args> void printf(const char*, Args...) {}
} Serial;

// INSERT_TIMEOUT_STATE
// INSERT_TIMEOUT_FUNCTIONS

volatile bool gWizardOwnsSerial = true;
static bool gOledRunning = true, oledConnected = true;
static constexpr int OLED_HEADER_HEIGHT = 12, OLED_CONTENT_HEIGHT = 40;
static constexpr int DISPLAY_COLOR_WHITE = 1, DISPLAY_COLOR_BLACK = 0, SSD1306_WHITE = 1;
static constexpr int OLED_KEYBOARD_MAX_LENGTH = 128, JOYSTICK_DEADZONE = 20;
#define EXT_RAM_BSS_ATTR
#define OLED_TRANSACTION(...) do { __VA_ARGS__; } while (0)
enum { INPUT_BUTTON_A, INPUT_BUTTON_B, INPUT_BUTTON_X, INPUT_BUTTON_Y, INPUT_BUTTON_START };
#define INPUT_MASK(button) (1u << (button))
#define INPUT_CHECK(buttons, button) ((buttons) & INPUT_MASK(button))

struct DisplayBoundary {
  void clearDisplay() {} void display() {} void setCursor(int, int) {}
  void setTextSize(int) {} void setTextColor(int) {}
  void fillRect(int, int, int, int, int) {} void drawFastHLine(int, int, int, int) {}
  template<typename T> void print(const T&) {}
  template<typename T> void println(const T&) {}
} display;
static DisplayBoundary* oledDisplay = &display;
static std::vector<String> output;
static void broadcastOutput(const String& value) { output.push_back(value); }
static void oledDrawBackArrowIcon(DisplayBoundary*, int) {}

struct PhysicalInput { unsigned long at; uint32_t buttons; int x = 0, y = 0, wheel = 0; };
static std::deque<PhysicalInput> physicalInputs;
static PhysicalInput currentInput{};
static struct { int wheelDelta = 0; } gNavEvents;
static void updateInputState() {
  currentInput = {};
  if (!physicalInputs.empty() && physicalInputs.front().at <= nowMs) {
    currentInput = physicalInputs.front();
    physicalInputs.pop_front();
  }
  gNavEvents.wheelDelta = currentInput.wheel;
}
static uint32_t getNewlyPressedButtons() { return currentInput.buttons; }
static void getJoystickDelta(int& x, int& y) { x = currentInput.x; y = currentInput.y; }

enum class OLEDKeyboardDictationPolicy { DENY, ALLOW_PLAINTEXT };
static struct { char text[OLED_KEYBOARD_MAX_LENGTH + 1] = {}; int textLength = 0; } gOledKeyboardState;
static bool keyboardActive, keyboardCancelled;
static int keyboardResetCalls;
static OLEDKeyboardDictationPolicy lastDictationPolicy;
static void oledKeyboardInit(const char*, const char* value, int, OLEDKeyboardDictationPolicy policy) {
  keyboardActive = true; keyboardCancelled = false; lastDictationPolicy = policy;
  std::snprintf(gOledKeyboardState.text, sizeof(gOledKeyboardState.text), "%s", value ? value : "");
  gOledKeyboardState.textLength = static_cast<int>(std::strlen(gOledKeyboardState.text));
}
static bool oledKeyboardIsActive() { return keyboardActive; }
static bool oledKeyboardIsCancelled() { return keyboardCancelled; }
static void oledKeyboardComplete() { keyboardActive = false; }
static void oledKeyboardDisplay(DisplayBoundary*) {}
static const char* oledKeyboardGetText() { return gOledKeyboardState.text; }
static void oledKeyboardReset() {
  ++keyboardResetCalls; keyboardActive = keyboardCancelled = false;
  std::memset(gOledKeyboardState.text, 0, sizeof(gOledKeyboardState.text));
  gOledKeyboardState.textLength = 0;
}
static void oledKeyboardHandleInput(int, int, uint32_t buttons) {
  if (buttons & INPUT_MASK(INPUT_BUTTON_B)) { keyboardCancelled = true; keyboardActive = false; }
  if (buttons & INPUT_MASK(INPUT_BUTTON_A)) keyboardActive = false;
}

struct WifiScanRecord { char ssid[33]; int8_t rssi; };
struct WifiScanResult {
  bool accepted; uint16_t found; int driverError = 0; int status = 0;
  bool ok() const { return accepted; }
};
static bool scanAccepted;
static std::vector<WifiScanRecord> scannedNetworks;
static int scanCalls;
static const char* wifiScanStatusText(int) { return "unavailable"; }
static WifiScanResult wifiScanForEach(bool,
    bool (*visitor)(const WifiScanRecord&, uint16_t, uint16_t, void*), void* context, int) {
  ++scanCalls;
  if (scanAccepted) {
    for (size_t i = 0; i < scannedNetworks.size(); ++i) {
      if (!visitor(scannedNetworks[i], static_cast<uint16_t>(i),
                   static_cast<uint16_t>(scannedNetworks.size()), context)) break;
    }
  }
  return {scanAccepted, static_cast<uint16_t>(scannedNetworks.size())};
}

static uint32_t tickButtons, sLastButtons;
static bool sLastButtonsValid;
static JoystickNav tickNav{};
static SetupWizardPage currentPage, sLastRendered;
static int currentSelection, sLastRenderedSel;
static SetupWizardResult sDummyResult;
static int toggleCalls, nextCalls, previousCalls, cycleCalls, exitCalls, activityCalls, wifiClearCalls;
static bool nextCompletes, exitSawCompleted;
static char deviceName[20];
static JoystickNav readWizardJoystickNav() { const auto value = tickNav; tickNav = {}; return value; }
static uint32_t inputConsumeButtonPresses(uint32_t&, bool& valid) {
  valid = true; const uint32_t value = tickButtons; tickButtons = 0; return value;
}
static bool wizardActivateCurrentItem() { ++toggleCalls; return true; }
static bool wizardMoveUp() { --currentSelection; return true; }
static bool wizardMoveDown() { ++currentSelection; return true; }
static bool wizardNextPage(SetupWizardResult& result) {
  ++nextCalls;
  if (nextCompletes) { result.completed = true; return false; }
  currentPage = WIZARD_PAGE_SYSTEM; return true;
}
static bool wizardPrevPage() { ++previousCalls; currentPage = WIZARD_PAGE_SYSTEM; return true; }
static bool wizardIsFirstPage() { return currentPage == WIZARD_PAGE_CONNECTIONS; }
static bool wizardIsGroupPage(SetupWizardPage page) {
  return page == WIZARD_PAGE_CONNECTIONS || page == WIZARD_PAGE_DISPLAY ||
         page == WIZARD_PAGE_SENSORS || page == WIZARD_PAGE_SYSTEM;
}
static WizardMenuItemType menuItemType = WIZARD_ITEM_NTP;
static const WizardMenuItem* getWizardMenuItem(SetupWizardPage, int) {
  static WizardMenuItem item{"ntp", "NTP", WIZARD_ITEM_NTP, nullptr, 0};
  item.type = menuItemType;
  return &item;
}
static const char* paintAfterTransition() {
  return sWizard.subMode == WizardSubMode::PAGE_DEVICE_NAME ? "Device Name [fixture]: " : "";
}
static char* getWizardDeviceNameBuf() { return deviceName; }
static const char* getWizardDeviceName() { return deviceName; }
static int getWizardCurrentSelection() { return currentSelection; }
static SetupWizardPage getWizardCurrentPage() { return currentPage; }
static void setWizardCurrentSelection(int value) { currentSelection = value; }
static void renderWizardPage() {}
static void clearWizardWifiChoices() { ++wifiClearCalls; }
static bool cliModeNoteActivity() { ++activityCalls; return true; }
static void cliExitMode() { ++exitCalls; exitSawCompleted = sWizard.result.completed; }
static String getOLEDTextInput(const char*, bool, const char*, int, bool* = nullptr, bool = true);

// INSERT_PRODUCTION

static void reset() {
  nowMs = 0; delayCalls = 0; setSerialWaitTimeout(0);
  serialInputs.clear(); physicalInputs.clear(); currentInput = {}; gNavEvents = {};
  output.clear(); gWizardOwnsSerial = true; gOledRunning = oledConnected = true; oledDisplay = &display;
  oledKeyboardReset(); keyboardResetCalls = 0;
  scanAccepted = true; scannedNetworks = {{{"test-network"}, -40}}; scanCalls = 0;
  tickButtons = sLastButtons = 0; sLastButtonsValid = true; tickNav = {};
  currentPage = WIZARD_PAGE_CONNECTIONS; currentSelection = 0;
  sLastRendered = WIZARD_PAGE_COUNT; sLastRenderedSel = -1;
  sWizard.result = {}; sWizard.subMode = WizardSubMode::PAGE_GROUP; sDummyResult = {};
  toggleCalls = nextCalls = previousCalls = cycleCalls = exitCalls = activityCalls = wifiClearCalls = 0;
  nextCompletes = exitSawCompleted = false;
  deviceName[0] = '\0'; menuItemType = WIZARD_ITEM_NTP;
}

static void assertKeyboardReleased() {
  assert(!keyboardActive && !keyboardCancelled && keyboardResetCalls == 1);
  assert(gOledKeyboardState.textLength == 0 && gOledKeyboardState.text[0] == '\0');
}

static void testKeyboardCancelAndTimeout() {
  // A masked prompt takes the text literally: the cancel word is a password,
  // never echoed; only the idle timeout cancels it.
  reset(); serialInputs.push_back({0, " CaNcEl "});
  bool cancelled = false;
  assert(getOLEDTextInput("Password", true, "not-persisted", 32, &cancelled) == "CaNcEl");
  assert(!cancelled && !isWizardCancelRequested());
  assert(lastDictationPolicy == OLEDKeyboardDictationPolicy::DENY && output.empty());
  assertKeyboardReleased();
  reset(); setSerialWaitTimeout(75); cancelled = false;
  assert(getOLEDTextInput("Password", true, "", 32, &cancelled).empty());
  assert(cancelled && isWizardCancelRequested() && nowMs == 100);
  assertKeyboardReleased();

  reset(); setSerialWaitTimeout(75);
  cancelled = false;
  assert(getOLEDTextInput("Name", false, "draft", 32, &cancelled).empty());
  assert(cancelled && isWizardCancelRequested() && nowMs == 100);
  assertKeyboardReleased();

  reset(); physicalInputs.push_back({0, INPUT_MASK(INPUT_BUTTON_B)});
  cancelled = false;
  assert(getOLEDTextInput("Name", false, "draft", 32, &cancelled).empty());
  assert(cancelled && !isWizardCancelRequested()); // B means Back within text input.
  assertKeyboardReleased();

  reset(); setSerialWaitTimeout(75);
  physicalInputs.push_back({50, 0, 0, 0, 1});
  physicalInputs.push_back({100, INPUT_MASK(INPUT_BUTTON_A)});
  cancelled = false;
  assert(getOLEDTextInput("Name", false, "draft", 32, &cancelled) == "draft");
  assert(!cancelled && !isWizardCancelRequested() && nowMs == 150);
  assertKeyboardReleased();

  reset(); requestWizardCancel(); gWizardOwnsSerial = false;
  serialInputs.push_back({0, "cancel"}); cancelled = false;
  assert(getOLEDTextInput("Basic text", false, "", 32, &cancelled) == "cancel");
  assert(!cancelled); // Basic/Import callers do not reserve the wizard cancel word.
  assertKeyboardReleased();

  reset(); oledConnected = false; serialInputs.push_back({0, "cancel"}); cancelled = false;
  assert(getOLEDTextInput("Name", false, "", 32, &cancelled).empty());
  assert(cancelled && isWizardCancelRequested());
}

static void testButtonWaitCancel() {
  reset(); serialInputs.push_back({0, "cancel"});
  assert(waitForButtonPress() == 0 && isWizardCancelRequested());
  reset(); setSerialWaitTimeout(15);
  assert(waitForButtonPress() == 0 && isWizardCancelRequested() && nowMs == 20);
  reset(); gWizardOwnsSerial = false; requestWizardCancel();
  physicalInputs.push_back({0, INPUT_MASK(INPUT_BUTTON_A)});
  assert(waitForButtonPress() == INPUT_MASK(INPUT_BUTTON_A));
}

static void testRetryOrSkipWait() {
  // The picker's retry/skip wait acts on the serial words it advertises.
  reset(); serialInputs.push_back({0, "skip"});
  assert(waitForRetryOrSkip() == INPUT_MASK(INPUT_BUTTON_B) && !isWizardCancelRequested());
  reset(); serialInputs.push_back({0, " rescan "});
  assert(waitForRetryOrSkip() == INPUT_MASK(INPUT_BUTTON_A) && !isWizardCancelRequested());
  reset(); serialInputs.push_back({0, "cancel"});
  assert(waitForRetryOrSkip() == 0 && isWizardCancelRequested());
  reset(); serialInputs.push_back({0, ""}); physicalInputs.push_back({20, INPUT_MASK(INPUT_BUTTON_B)});
  assert(waitForRetryOrSkip() == INPUT_MASK(INPUT_BUTTON_B)); // a blank line is not a choice
  reset(); setSerialWaitTimeout(15);
  assert(waitForRetryOrSkip() == 0 && isWizardCancelRequested() && nowMs == 20);
  reset(); gWizardOwnsSerial = false; serialInputs.push_back({0, "skip"});
  physicalInputs.push_back({0, INPUT_MASK(INPUT_BUTTON_A)});
  assert(waitForRetryOrSkip() == INPUT_MASK(INPUT_BUTTON_A)); // serial belongs to the CLI here
}

static void testWifiPickerCancel() {
  reset(); serialInputs.push_back({0, "cancel"});
  String selected = "old scratch";
  assert(!getOLEDWiFiSelection(selected) && isWizardCancelRequested());
#if ENABLE_WIFI
  assert(selected.empty() && sFtsWifiStoredCount == 0);
  reset(); setSerialWaitTimeout(75); selected = "old scratch";
  assert(!getOLEDWiFiSelection(selected) && isWizardCancelRequested() && nowMs == 100);
  assert(selected.empty() && sFtsWifiStoredCount == 0);

  for (bool failedScan : {false, true}) {
    reset(); scanAccepted = !failedScan; scannedNetworks.clear();
    serialInputs.push_back({0, "cancel"});
    assert(!getOLEDWiFiSelection(selected) && isWizardCancelRequested());
    assert(scanCalls == 1 && sFtsWifiStoredCount == 0);
    // Serial 'skip' and 'rescan' act like the B and A the display offers.
    reset(); scanAccepted = !failedScan; scannedNetworks.clear();
    serialInputs.push_back({0, "skip"});
    assert(!getOLEDWiFiSelection(selected) && !isWizardCancelRequested() && scanCalls == 1);
    reset(); scanAccepted = !failedScan; scannedNetworks.clear();
    serialInputs.push_back({0, "rescan"}); serialInputs.push_back({10, "skip"});
    assert(!getOLEDWiFiSelection(selected) && !isWizardCancelRequested() && scanCalls == 2);
  }

  reset(); requestWizardCancel(); gWizardOwnsSerial = false;
  serialInputs.push_back({0, "cancel"});
  assert(getOLEDWiFiSelection(selected) && selected == "cancel");
  assert(sFtsWifiStoredCount == 0); // Ordinary callers still accept this SSID.
#endif
}

static void testDisplayPageCancel() {
  reset(); JoystickNav nav{};
  assert(handleWizardPageInput(INPUT_MASK(INPUT_BUTTON_B), nav, sWizard.result));
  assert(isWizardCancelRequested() && !nextCalls && !toggleCalls);
  reset(); nav = {false, false, true, false};
  assert(handleWizardPageInput(0, nav, sWizard.result) && isWizardCancelRequested());

  reset(); requestWizardCancel(); oledConnected = false;
  sWizard.result.completed = true;
  wizardMode_onTick(nullptr);
  assert(exitCalls == 1 && !exitSawCompleted && !sWizard.result.completed);

  reset(); tickButtons = INPUT_MASK(INPUT_BUTTON_B);
  wizardMode_onTick(nullptr);
  assert(exitCalls == 1 && !exitSawCompleted && !nextCalls && !toggleCalls);

  reset(); currentPage = WIZARD_PAGE_SYSTEM; sWizard.subMode = WizardSubMode::PAGE_GROUP;
  tickButtons = INPUT_MASK(INPUT_BUTTON_Y); // Ignored input must not mean Finish.
  wizardMode_onTick(nullptr);
  assert(exitCalls == 0 && !sWizard.result.completed && nextCalls == 0);

  reset(); currentPage = WIZARD_PAGE_SYSTEM; sWizard.subMode = WizardSubMode::PAGE_GROUP;
  tickButtons = INPUT_MASK(INPUT_BUTTON_A);
  wizardMode_onTick(nullptr);
  assert(toggleCalls == 1 && exitCalls == 0 && !sWizard.result.completed);

  reset(); currentPage = WIZARD_PAGE_SYSTEM; sWizard.subMode = WizardSubMode::PAGE_GROUP;
  nextCompletes = true;
#if ENABLE_GPIO_ENCODER
  tickButtons = INPUT_MASK(INPUT_BUTTON_X);
#else
  tickButtons = INPUT_MASK(INPUT_BUTTON_START);
#endif
  wizardMode_onTick(nullptr);
  assert(nextCalls == 1 && exitCalls == 1 && exitSawCompleted);

  reset(); currentPage = WIZARD_PAGE_SYSTEM; sWizard.subMode = WizardSubMode::PAGE_GROUP;
  tickButtons = INPUT_MASK(INPUT_BUTTON_B);
  wizardMode_onTick(nullptr);
  assert(previousCalls == 1 && exitCalls == 0 && !sWizard.result.completed);

  reset(); currentPage = WIZARD_PAGE_WIFI; sWizard.subMode = WizardSubMode::WIFI_PASSWORD;
  tickNav.left = true;
  wizardMode_onTick(nullptr);
  assert(previousCalls == 1 && wifiClearCalls == 1 && exitCalls == 0);

  // A on the device-name row must not open the blocking keyboard under the
  // mode lock: the CLI wizard switches to its typed-name detour instead.
  reset(); currentPage = WIZARD_PAGE_SYSTEM; sWizard.subMode = WizardSubMode::PAGE_GROUP;
  menuItemType = WIZARD_ITEM_DEVICE_NAME; tickButtons = INPUT_MASK(INPUT_BUTTON_A);
  wizardMode_onTick(nullptr);
  assert(sWizard.subMode == WizardSubMode::PAGE_DEVICE_NAME && toggleCalls == 0 && exitCalls == 0);
  assert(!keyboardActive && output.size() == 1 && output.back() == "Device Name [fixture]: ");
}

int main() {
  testKeyboardCancelAndTimeout(); testButtonWaitCancel(); testRetryOrSkipWait(); testWifiPickerCancel();
  testDisplayPageCancel();
  std::puts("setup display cancellation contracts passed");
}
