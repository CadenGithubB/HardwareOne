#include <algorithm>
#include <cassert>
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
static int getWizardPageNumber(SetupWizardPage) { return 2; }
static int getWizardTotalPages() { return 4; }
static SetupWizardPage getWizardCurrentPage() { return currentPage; }
static bool wizardPrevPage() { ++previousCalls; currentPage = WIZARD_PAGE_ESPNOW; return true; }
static bool wizardNextPage(SetupWizardResult&) {
  ++nextCalls;
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

// INSERT_PRODUCTION_FUNCTIONS

static void reset() {
  gSettings = {"old.host", "old-user", "old-secret", 8883, true, true};
  inputs.clear(); inputIndex = 0; cancelInput = -1; introChoice = 1;
  previousCalls = nextCalls = clearCalls = saveCalls = startCalls = 0;
  cancelRequested = false; hasNextPage = true; currentPage = WIZARD_PAGE_MQTT;
  sWizard = {}; sWizard.subMode = WizardSubMode::MQTT_INTRO;
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

static void helperCases() {
  reset();
  SetupWizardResult result{};
  for (int port : {0, -1, -70000}) {
    result.mqttPort = port;
    wizardApplyMQTTSettings(result);
    broker();
  }
  result.mqttHost = " host with spaces ";
  wizardApplyMQTTSettings(result);
  broker(" host with spaces ");
  result = {}; result.mqttPassword = "new-secret";
  wizardApplyMQTTSettings(result);
  broker(" host with spaces ", 8883, "old-user", "new-secret");
  result = {}; result.mqttUser = "new-user"; result.mqttPort = 70000;
  wizardApplyMQTTSettings(result);
  broker(" host with spaces ", 70000, "new-user", "new-secret");
  assert(gSettings.mqttAutoStart);  // Neither helper nor page auto-starts a client.
}

static void serialCases() {
  SetupWizardResult result{}; bool running = true;
  reset(); inputs = {"c", " new.host ", "70000", "new-user", "new-secret"};
  handleSerialMQTTPage(result, running);
  broker("new.host", 70000, "new-user", "new-secret");
  assert(nextCalls == 1 && running && gSettings.mqttAutoStart);

  reset(); result = {}; inputs = {"c", "", "", ""}; hasNextPage = false;
  handleSerialMQTTPage(result, running);
  broker(); assert(!running && nextCalls == 1 && inputIndex == 4);

  for (int back = 0; back < 5; ++back) {
    reset(); result = {}; running = true;
    inputs = {"c", "new.host", "2000", "new-user", "new-secret"};
    inputs[back] = "back";
    handleSerialMQTTPage(result, running);
    broker(); assert(previousCalls == 1 && nextCalls == 0 && running);
  }
  reset(); result = {}; running = true; inputs = {"c", "new.host", "2000", "new-user", "new-secret"};
  cancelInput = 2;
  handleSerialMQTTPage(result, running);
  broker(); assert(!running && nextCalls == 0 && gSettings.mqttAutoStart);

  reset(); result = {}; running = true; inputs = {"n"};
  handleSerialMQTTPage(result, running);
  broker(); assert(!gSettings.mqttAutoStart && nextCalls == 1);
}

static void oledCases() {
  SetupWizardResult result{}; bool running = true;
  reset(); inputs = {"new.host", "70000", "new-user", "new-secret"};
  handleOLEDMQTTPage(result, running);
  broker("new.host", 70000, "new-user", "new-secret");
  assert(nextCalls == 1 && running && gSettings.mqttAutoStart);

  reset(); result = {}; inputs = {"", "", ""}; hasNextPage = false;
  handleOLEDMQTTPage(result, running);
  broker(); assert(!running && nextCalls == 1 && inputIndex == 3);

  for (int cancel = 0; cancel < 4; ++cancel) {
    reset(); result = {}; running = true;
    inputs = {"new.host", "2000", "new-user", "new-secret"}; cancelInput = cancel;
    handleOLEDMQTTPage(result, running);
    broker(); assert(previousCalls == 1 && nextCalls == 0 && running);
  }
  reset(); result = {}; running = true; introChoice = -1;
  handleOLEDMQTTPage(result, running);
  broker(); assert(nextCalls == 0 && running && gSettings.mqttAutoStart);
  reset(); introChoice = 0;
  handleOLEDMQTTPage(result, running);
  broker(); assert(!gSettings.mqttAutoStart && nextCalls == 1);
}

static void cliCases() {
  reset();
  for (const char* value : {"c", "new.host", "70000", "new-user"}) {
    assert(cli(value) == CLI_MODE_HANDLED); broker();
  }
  hasNextPage = false;
  assert(cli("new-secret") == CLI_MODE_HANDLED_AND_EXIT);
  broker("new.host", 70000, "new-user", "new-secret");
  assert(sWizard.result.completed && nextCalls == 1 && gSettings.mqttAutoStart);

  reset();
  for (const char* value : {"c", "", "", "", ""}) cli(value);
  broker("old.host", 1883);  // CLI's existing blank-port default differs from FTS.
  reset(); cli("c"); cli("new.host"); cli("n");
  broker("new.host"); assert(nextCalls == 1);  // Apply a partial page without completed=true.
  reset(); cli("c"); cli("new.host"); cli("-1"); cli(""); cli("");
  broker("new.host");

  for (int field = 0; field < 5; ++field) {
    reset();
    const char* prefix[] = {"c", "new.host", "2000", "new-user"};
    for (int i = 0; i < field; ++i) cli(prefix[i]);
    cli("b"); broker(); assert(nextCalls == 0);
    reset();
    for (int i = 0; i < field; ++i) cli(prefix[i]);
    assert(cli("cancel") == CLI_MODE_HANDLED_AND_EXIT);
    broker(); assert(!sWizard.result.completed && clearCalls == 1 && nextCalls == 0);
  }
  reset(); cli("n");
  broker(); assert(gSettings.mqttAutoStart && nextCalls == 1);  // Legacy CLI skip keeps auto-start.
}

int main() {
  helperCases(); serialCases(); oledCases(); cliCases();
  std::puts("MQTT setup: shared apply, partial inputs, back/cancel and legacy page behavior passed");
}
