#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Arduino storage/input boundary; application and handler control flow are real.
class String : public std::string {
 public:
  using std::string::string;
  String(const std::string& value) : std::string(value) {}
  void trim() {
    const size_t first = find_first_not_of(" \t\r\n");
    *this = first == npos ? "" : substr(first, find_last_not_of(" \t\r\n") - first + 1);
  }
  void toLowerCase() {
    std::transform(begin(), end(), begin(), [](unsigned char c) { return std::tolower(c); });
  }
  bool equalsIgnoreCase(const char* value) const {
    String left = *this, right = value;
    left.toLowerCase(); right.toLowerCase();
    return left == right;
  }
};

// INSERT_PRODUCTION_TYPES

struct SettingsBoundary {
  String bleDeviceName, espnowDeviceName, espnowFriendlyName, espnowRoom, espnowZone;
  bool espnowStationary, espnowEnabled;
} gSettings, beforeInput;

static std::vector<String> inputs;
static size_t inputIndex;
static int cancelInput, introChoice, previousCalls, nextCalls, clearCalls, drawLoops;
static bool cancelRequested, hasNextPage, stationarySerial;
static SetupWizardPage currentPage;
static char deviceName[21];
static uint32_t nextButtons;
static bool nextUp, nextDown, sJoyUpHeld, sJoyDownHeld;

static void unchanged() {
  assert(gSettings.bleDeviceName == beforeInput.bleDeviceName);
  assert(gSettings.espnowDeviceName == beforeInput.espnowDeviceName);
  assert(gSettings.espnowFriendlyName == beforeInput.espnowFriendlyName);
  assert(gSettings.espnowRoom == beforeInput.espnowRoom && gSettings.espnowZone == beforeInput.espnowZone);
  assert(gSettings.espnowStationary == beforeInput.espnowStationary);
}
static String readInput() {
  unchanged();  // No collected field becomes live while another prompt is pending.
  assert(inputIndex < inputs.size());
  if (static_cast<int>(inputIndex) == cancelInput) cancelRequested = true;
  return inputs[inputIndex++];
}
struct SerialBoundary {
  void println(const char* = "") {}
  void print(const char*) {}
  template <typename... Args> void printf(const char*, Args...) {}
  bool available() { return stationarySerial && inputIndex < inputs.size(); }
  String readStringUntil(char) { return readInput(); }
} Serial;
static String waitForSerialInputBlocking() { return readInput(); }
static String getOLEDTextInput(const char*, bool, const char*, int, bool* cancelled) {
  String value = readInput(); *cancelled = cancelRequested; return value;
}
static bool wizardPrevPage() { ++previousCalls; currentPage = WIZARD_PAGE_SYSTEM; return true; }
static bool wizardNextPage(SetupWizardResult& result) {
  ++nextCalls;
  currentPage = hasNextPage ? WIZARD_PAGE_MQTT : WIZARD_PAGE_COUNT;
  if (!hasNextPage) result.completed = true;
  return hasNextPage;
}
static int showWizardOptionalPageIntro(SetupWizardPage, const char*, const char*) {
  if (introChoice == -1) wizardPrevPage();
  return introChoice;
}
static bool isWizardCancelRequested() { return cancelRequested; }
static int getWizardPageNumber(SetupWizardPage) { return 2; }
static int getWizardTotalPages() { return 4; }
static SetupWizardPage getWizardCurrentPage() { return currentPage; }
static void broadcastOutput(const char*) {}
static void appendPromptTo(char*, size_t) {}
static void clearWizardWifiChoices() { ++clearCalls; }
static char* getWizardDeviceNameBuf() { return deviceName; }
static void delay(int) { unchanged(); assert(++drawLoops < 20); }
struct DisplayBoundary {
  void clearDisplay() {} void setCursor(int, int) {} void print(const char*) {}
  void println(const char*) {} void display() {}
} display;
static DisplayBoundary* oledDisplay = &display;
#define OLED_TRANSACTION(operation) do { operation; } while (0)
static void drawWizardHeader(int, int, const char*) {}
static void drawWizardFooter(const char*, const char*, const char*) {}
struct JoystickNav { bool up, down; };
static JoystickNav readWizardJoystickNav() {
  JoystickNav value{nextUp, nextDown}; nextUp = nextDown = false; return value;
}
enum { INPUT_BUTTON_A, INPUT_BUTTON_B };
#define INPUT_MASK(button) (1u << (button))
static uint32_t inputConsumeButtonPresses(uint32_t&, bool&) { unchanged(); return nextButtons; }
static CLIModeInputResult unrelatedPage(const String&, char*, size_t) {
  assert(false && "test unexpectedly left ESP-NOW input routing"); return CLI_MODE_HANDLED;
}
static auto dispatchTopLevelPage = unrelatedPage;
static auto dispatchModePage = unrelatedPage;
static auto dispatchMQTT = unrelatedPage;
static auto dispatchWiFi = unrelatedPage;

// INSERT_PRODUCTION_FUNCTIONS

static void reset() {
  gSettings = {"old-ble", "old-device", "old-friendly", "old-room", "old-zone", true, true};
  beforeInput = gSettings;
  inputs.clear(); inputIndex = 0; cancelInput = -1; introChoice = 1;
  previousCalls = nextCalls = clearCalls = drawLoops = 0;
  cancelRequested = false; hasNextPage = stationarySerial = true;
  nextButtons = 0; nextUp = nextDown = sJoyUpHeld = sJoyDownHeld = false;
  currentPage = WIZARD_PAGE_ESPNOW;
  sWizard = {}; sWizard.subMode = WizardSubMode::ESPNOW_INTRO;
}
static void identity(const char* name, const char* room = "old-room", const char* zone = "old-zone",
                     bool stationary = false, const char* friendly = nullptr) {
  assert(gSettings.bleDeviceName == name && gSettings.espnowDeviceName == name);
  assert(gSettings.espnowFriendlyName == (friendly ? friendly : name));
  assert(gSettings.espnowRoom == room && gSettings.espnowZone == zone);
  assert(gSettings.espnowStationary == stationary && gSettings.espnowEnabled);
}
static CLIModeInputResult cli(const char* line) {
  char output[128] = {}; return wizardMode_onInput(line, nullptr, output, sizeof(output));
}

static void helperCases() {
  reset(); SetupWizardResult result{};
  wizardApplyESPNowSettings(result);
  identity("old-device", "old-room", "old-zone", false, "old-friendly");
  assert(nextCalls == 0 && previousCalls == 0);
  reset(); gSettings.espnowDeviceName = "";
  wizardApplyESPNowSettings(result);
  identity("HardwareOne", "old-room", "old-zone", false, "old-friendly");
  reset(); result.espnowFriendlyName = "  Name  "; result.espnowRoom = "New room";
  result.espnowZone = "New zone"; result.espnowStationary = true;
  wizardApplyESPNowSettings(result);
  identity("  Name  ", "New room", "New zone", true);  // Input policy remains at callers.
  assert(nextCalls == 0 && previousCalls == 0);
}

static void serialCases() {
  SetupWizardResult result{}; bool running = true;
  reset(); inputs = {"c", " new-device ", " new-room ", " new-zone ", "stationary"};
  handleSerialESPNowPage(result, running);
  identity("new-device", "new-room", "new-zone", true);
  assert(nextCalls == 1 && running && inputIndex == 5);
  reset(); result = {}; inputs = {"direct-name", "", "", ""}; hasNextPage = false;
  handleSerialESPNowPage(result, running);
  identity("direct-name"); assert(!running && nextCalls == 1 && result.completed);
  reset(); result = {}; running = true; inputs = {"c", "", "", "", ""};
  handleSerialESPNowPage(result, running); identity("old-device");

  // Early finish at each field keeps unsubmitted room/zone values. Name-stage
  // finish uses the current device name even when an earlier draft exists.
  for (int finish = 1; finish <= 3; ++finish) {
    reset(); result = {}; running = true; result.espnowFriendlyName = "stale-name";
    inputs = {"c", "new-device", "new-room", "new-zone", "s"}; inputs[finish] = "n";
    handleSerialESPNowPage(result, running);
    identity(finish == 1 ? "old-device" : "new-device", finish == 3 ? "new-room" : "old-room",
             "old-zone", false, finish == 1 ? "old-friendly" : nullptr);
    assert(nextCalls == 1 && inputIndex == static_cast<size_t>(finish + 1));
  }
  for (int back = 0; back < 5; ++back) {
    reset(); result = {}; running = true;
    inputs = {"c", "new-device", "new-room", "new-zone", "s"}; inputs[back] = "back";
    handleSerialESPNowPage(result, running);
    unchanged(); assert(gSettings.espnowEnabled && previousCalls == 1 && nextCalls == 0 && running);
  }
  for (int timeout = 1; timeout < 5; ++timeout) {
    reset(); result = {}; running = true;
    inputs = {"c", "new-device", "new-room", "new-zone", "s"}; cancelInput = timeout;
    handleSerialESPNowPage(result, running);
    unchanged(); assert(!running && nextCalls == 0 && gSettings.espnowEnabled);
  }
  for (const char* skip : {"", "n", "next"}) {
    reset(); result = {}; running = true; inputs = {skip};
    handleSerialESPNowPage(result, running);
    unchanged(); assert(!gSettings.espnowEnabled && nextCalls == 1);
  }
  reset(); result = {}; running = true; gSettings.espnowDeviceName = ""; beforeInput = gSettings;
  inputs = {"c", "n"}; handleSerialESPNowPage(result, running);
  identity("HardwareOne", "old-room", "old-zone", false, "old-friendly");
}

static void oledCases() {
  SetupWizardResult result{}; bool running = true;
  reset(); inputs = {"new-device", "new-room", "new-zone", "s"};
  handleOLEDESPNowPage(result, running); identity("new-device", "new-room", "new-zone", true);
  assert(nextCalls == 1 && running && inputIndex == 4);
  reset(); result = {}; inputs = {"", "", "", ""}; hasNextPage = false;
  handleOLEDESPNowPage(result, running); identity("old-device");
  assert(!running && nextCalls == 1 && result.completed);
  for (int cancel = 0; cancel < 3; ++cancel) {
    reset(); result = {}; running = true; inputs = {"new-device", "new-room", "new-zone", "s"};
    cancelInput = cancel; handleOLEDESPNowPage(result, running);
    unchanged(); assert(previousCalls == 1 && nextCalls == 0 && running && gSettings.espnowEnabled);
  }
  reset(); result = {}; running = true; inputs = {"new-device", "new-room", "new-zone", "back"};
  handleOLEDESPNowPage(result, running);
  unchanged(); assert(previousCalls == 1 && nextCalls == 0 && running);
  reset(); result = {}; running = true; stationarySerial = false;
  inputs = {"new-device", "new-room", "new-zone"}; nextButtons = INPUT_MASK(INPUT_BUTTON_B);
  handleOLEDESPNowPage(result, running);
  unchanged(); assert(previousCalls == 1 && nextCalls == 0 && running);
  for (bool toggle : {false, true}) {
    reset(); result = {}; running = true; stationarySerial = false; nextDown = toggle;
    inputs = {"new-device", "", ""}; nextButtons = INPUT_MASK(INPUT_BUTTON_A);
    handleOLEDESPNowPage(result, running);
    identity("new-device", "old-room", "old-zone", !toggle);
  }
  for (int intro : {-1, 0}) {
    reset(); result = {}; running = true; introChoice = intro;
    handleOLEDESPNowPage(result, running); unchanged();
    assert(inputIndex == 0 && running && nextCalls == (intro == 0));
    assert(gSettings.espnowEnabled == (intro == -1));
  }
}

static void cliCases() {
  reset();
  for (const char* value : {"c", "new-device", "new-room", "new-zone"}) {
    assert(cli(value) == CLI_MODE_HANDLED); unchanged();
  }
  hasNextPage = false;
  assert(cli("stationary") == CLI_MODE_HANDLED_AND_EXIT);
  identity("new-device", "new-room", "new-zone", true);
  assert(nextCalls == 1 && sWizard.result.completed);
  reset(); for (const char* value : {"direct-name", "", "", ""}) cli(value);
  identity("direct-name"); assert(nextCalls == 1);
  reset(); for (const char* value : {"c", "", "", "", ""}) cli(value);
  identity("old-device");
  for (int finish = 1; finish <= 3; ++finish) {
    reset(); sWizard.result.espnowFriendlyName = "stale-name";
    const char* prefix[] = {"c", "new-device", "new-room"};
    for (int i = 0; i < finish; ++i) { cli(prefix[i]); unchanged(); }
    cli("next");
    identity(finish == 1 ? "old-device" : "new-device", finish == 3 ? "new-room" : "old-room",
             "old-zone", false, finish == 1 ? "old-friendly" : nullptr);
    assert(nextCalls == 1);
  }
  for (int field = 0; field < 5; ++field) {
    const char* prefix[] = {"c", "new-device", "new-room", "new-zone"};
    reset(); for (int i = 0; i < field; ++i) cli(prefix[i]);
    cli("b"); unchanged(); assert(nextCalls == 0 && gSettings.espnowEnabled);
    reset(); for (int i = 0; i < field; ++i) cli(prefix[i]);
    assert(cli("cancel") == CLI_MODE_HANDLED_AND_EXIT);
    unchanged(); assert(!sWizard.result.completed && clearCalls == 1 && nextCalls == 0);
  }
  reset(); cli("c"); cli("draft-name"); cli("b"); cli("n");
  identity("old-device", "old-room", "old-zone", false, "old-friendly");
  reset(); gSettings.espnowDeviceName = ""; cli("c"); cli("n");
  identity("HardwareOne", "old-room", "old-zone", false, "old-friendly");
  for (const char* skip : {"", "n", "next"}) {
    reset(); cli(skip); unchanged(); assert(!gSettings.espnowEnabled && nextCalls == 1);
  }
}

int main() {
  helperCases(); serialCases(); oledCases(); cliCases();
  std::puts("ESP-NOW setup: production shared apply and three interfaces passed (normal, early finish, back/cancel, OLED buttons)");
}
