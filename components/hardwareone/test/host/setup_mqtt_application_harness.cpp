#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Arduino storage/input boundary; no setup or MQTT policy lives in this fake.
class String : public std::string {
 public:
  using std::string::string;
  String(const std::string& value) : std::string(value) {}
  String(int value) : std::string(std::to_string(value)) {}
  bool isEmpty() const { return empty(); }
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
  long toInt() const { return std::strtol(c_str(), nullptr, 10); }
};

// INSERT_PRODUCTION_TYPES

struct SettingsBoundary {
  String mqttHost, mqttUser, mqttPassword;
  int mqttPort;
  bool mqttAutoStart, mqttEnabled;
} gSettings;
static SetupWizardResult draftValues;
static std::vector<SetupWizardResult> staged;
static bool draftAutoStart, stageSucceeds;
static const SetupWizardResult& getWizardDraftValues() { return draftValues; }
static void wizardSetMQTTAutoStart(bool enabled) { draftAutoStart = enabled; }
static bool wizardStageMQTTSettings(const SetupWizardResult& value) {
  staged.push_back(value); return stageSucceeds;
}

static std::vector<String> inputs;
static size_t inputIndex;
static int cancelInput, introChoice, previousCalls, nextCalls, clearCalls;
static int saveCalls, startCalls;
static bool cancelRequested, hasNextPage;
static SetupWizardPage currentPage;
static char deviceName[21];

struct SerialBoundary {
  void println(const char* = "") {}
  void print(const char*) {}
  template <typename... Args> void printf(const char*, Args...) {}
} Serial;

static String readInput() {
  assert(gSettings.mqttHost == "old.host" && gSettings.mqttPort == 8883);
  assert(gSettings.mqttUser == "old-user" && gSettings.mqttPassword == "old-secret");
  assert(gSettings.mqttAutoStart && gSettings.mqttEnabled && saveCalls == 0 && startCalls == 0);
  if (cancelRequested) return "";
  assert(inputIndex < inputs.size());
  if (static_cast<int>(inputIndex) == cancelInput) cancelRequested = true;
  return inputs[inputIndex++];
}
static String waitForSerialInputBlocking() { return readInput(); }
static String getOLEDTextInput(const char*, bool, const char*, int, bool* cancelled) {
  String value = readInput();
  *cancelled = cancelRequested;
  return value;
}
static int showWizardOptionalPageIntro(SetupWizardPage, const char*, const char*) { return introChoice; }
static bool isWizardCancelRequested() { return cancelRequested; }
static void requestWizardCancel() { cancelRequested = true; }
static void noteWizardInputActivity() {}
static int getWizardPageNumber(SetupWizardPage) { return 2; }
static int getWizardTotalPages() { return 4; }
static SetupWizardPage getWizardCurrentPage() { return currentPage; }
static bool wizardPrevPage() { ++previousCalls; currentPage = WIZARD_PAGE_ESPNOW; return true; }
static bool wizardNextPage(SetupWizardResult& result) {
  ++nextCalls;
  result.completed = !hasNextPage;
  currentPage = hasNextPage ? WIZARD_PAGE_WIFI : WIZARD_PAGE_COUNT;
  return hasNextPage;
}
static void broadcastOutput(const char*) {}
static void appendPromptTo(char*, size_t) {}
static void clearWizardWifiChoices() { ++clearCalls; }
static char* getWizardDeviceNameBuf() { return deviceName; }
static bool writeSettingsJson() { ++saveCalls; return true; }
static bool startMQTT() { ++startCalls; return true; }
static CLIModeInputResult unrelatedPage(const String&, char*, size_t) {
  assert(false && "test unexpectedly left MQTT input routing");
  return CLI_MODE_HANDLED;
}
static auto dispatchTopLevelPage = unrelatedPage;
static auto dispatchModePage = unrelatedPage;
static auto dispatchESPNow = unrelatedPage;
static auto dispatchWiFi = unrelatedPage;

static bool wizardIsGroupPage(SetupWizardPage page) {
  return page == WIZARD_PAGE_CONNECTIONS || page == WIZARD_PAGE_DISPLAY ||
         page == WIZARD_PAGE_SENSORS || page == WIZARD_PAGE_SYSTEM;
}
// INSERT_PRODUCTION_FUNCTIONS

static void reset() {
  gSettings = {"old.host", "old-user", "old-secret", 8883, true, true};
  inputs.clear(); inputIndex = 0; cancelInput = -1; introChoice = 1;
  previousCalls = nextCalls = clearCalls = saveCalls = startCalls = 0;
  cancelRequested = false; hasNextPage = true; currentPage = WIZARD_PAGE_MQTT;
  sWizard = {}; sWizard.subMode = WizardSubMode::MQTT_INTRO;
  draftValues = {}; draftValues.mqttHost = "old.host"; draftValues.mqttPort = 8883;
  draftValues.mqttUser = "old-user"; draftValues.mqttPassword = "old-secret";
  staged.clear(); draftAutoStart = stageSucceeds = true;
}
static void broker(const char* host = "old.host", int port = 8883,
                   const char* user = "old-user", const char* password = "old-secret") {
  assert(gSettings.mqttHost == host && gSettings.mqttPort == port);
  assert(gSettings.mqttUser == user && gSettings.mqttPassword == password);
  assert(gSettings.mqttEnabled && saveCalls == 0 && startCalls == 0);
}
static CLIModeInputResult cli(const char* line) {
  char output[128] = {};
  return wizardMode_onInput(line, nullptr, output, sizeof(output));
}

static void submitted(const char* host, int port, const char* user, const char* password) {
  broker(); assert(staged.size() == 1);
  const auto& patch = staged.back();
  assert(patch.mqttHost == host && patch.mqttPort == port);
  assert(patch.mqttUser == user && patch.mqttPassword == password);
}
static void serialCases() {
  SetupWizardResult result{}; bool running = true;
  reset(); inputs = {"c", " new.host ", "65535", "new-user", "new-secret"};
  handleSerialMQTTPage(result, running);
  submitted("new.host", 65535, "new-user", "new-secret");
  assert(nextCalls == 1 && running && draftAutoStart);
  for (const char* port : {"70000", "0", "-1", "1883junk", "bad"}) {
    reset(); result = {}; inputs = {"c", "new.host", port};
    handleSerialMQTTPage(result, running);
    broker(); assert(staged.empty() && nextCalls == 0);
  }
  for (int back = 0; back < 5; ++back) {
    reset(); result = {}; running = true;
    inputs = {"c", "new.host", "2000", "new-user", "new-secret"}; inputs[back] = "back";
    handleSerialMQTTPage(result, running);
    broker(); assert(staged.empty() && previousCalls == 1 && nextCalls == 0 && running);
  }
  reset(); result = {}; inputs = {"c", "new.host", "2000", "new-user", "new-secret"}; cancelInput = 2;
  handleSerialMQTTPage(result, running);
  broker(); assert(staged.empty() && !running && nextCalls == 0 && draftAutoStart);
  reset(); result = {}; running = true; inputs = {"n"};
  handleSerialMQTTPage(result, running);
  broker(); assert(!draftAutoStart && nextCalls == 1 && staged.empty());
  reset(); result = {}; inputs = {"c", "new.host", "2000", "user", "pass"}; stageSucceeds = false;
  handleSerialMQTTPage(result, running); broker(); assert(nextCalls == 0 && staged.size() == 1);
}
static void oledCases() {
  SetupWizardResult result{}; bool running = true;
  reset(); inputs = {"new.host", "65535", "new-user", "new-secret"};
  handleOLEDMQTTPage(result, running);
  submitted("new.host", 65535, "new-user", "new-secret"); assert(nextCalls == 1 && running);
  reset(); result = {}; inputs = {"new.host", "70000", "1883", "user", "pass"};
  handleOLEDMQTTPage(result, running);
  submitted("new.host", 1883, "user", "pass"); assert(inputIndex == 5);
  for (int cancel = 0; cancel < 4; ++cancel) {
    reset(); result = {}; running = true; inputs = {"new.host", "2000", "new-user", "new-secret"}; cancelInput = cancel;
    handleOLEDMQTTPage(result, running);
    broker(); assert(staged.empty() && previousCalls == 0 && nextCalls == 0 && !running);
  }
  reset(); result = {}; introChoice = 0;
  handleOLEDMQTTPage(result, running);
  broker(); assert(!draftAutoStart && staged.empty() && nextCalls == 1);
  reset(); result = {}; inputs = {"new.host", "1883", "user", "pass"}; stageSucceeds = false;
  handleOLEDMQTTPage(result, running); broker(); assert(nextCalls == 0 && staged.size() == 1);
}
static void cliCases() {
  reset();
  for (const char* value : {"c", "new.host", "65535", "new-user"}) {
    assert(cli(value) == CLI_MODE_HANDLED); broker(); assert(staged.empty());
  }
  hasNextPage = false;
  assert(cli("new-secret") == CLI_MODE_HANDLED_AND_EXIT);
  submitted("new.host", 65535, "new-user", "new-secret");
  assert(sWizard.result.completed && nextCalls == 1 && draftAutoStart);
  reset(); for (const char* value : {"c", "", "", "", ""}) cli(value);
  submitted("", 0, "", "");
  reset(); cli("c"); cli("new.host"); cli("n");
  submitted("new.host", 0, "", ""); assert(nextCalls == 1);
  for (const char* invalid : {"70000", "0", "-1", "1883junk", "bad"}) {
    reset(); cli("c"); cli("new.host"); cli(invalid);
    broker(); assert(staged.empty() && nextCalls == 0 && sWizard.subMode == WizardSubMode::MQTT_PORT);
  }
  for (int field = 0; field < 5; ++field) {
    const char* prefix[] = {"c", "new.host", "2000", "new-user"};
    reset(); for (int i = 0; i < field; ++i) cli(prefix[i]);
    cli("b"); broker(); assert(staged.empty() && nextCalls == 0);
    reset(); for (int i = 0; i < field; ++i) cli(prefix[i]);
    assert(cli("cancel") == CLI_MODE_HANDLED_AND_EXIT);
    broker(); assert(staged.empty() && !sWizard.result.completed && clearCalls == 1 && nextCalls == 0);
  }
  reset(); cli("n"); broker(); assert(!draftAutoStart && nextCalls == 1);
  reset(); stageSucceeds = false;
  for (const char* value : {"c", "new.host", "1883", "user", "pass"}) cli(value);
  broker(); assert(staged.size() == 1 && nextCalls == 0 && !sWizard.result.completed);
}
int main() {
  serialCases(); oledCases(); cliCases();
  std::puts("MQTT setup: three production interfaces stage only, strict ports, skip parity, failure/back/cancel passed");
}
