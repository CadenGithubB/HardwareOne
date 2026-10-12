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
// INSERT_STRING
// INSERT_TYPES

static std::deque<String> inputs, pickedNetworks;
static std::deque<int> introChoices;
static std::vector<String> output;
static bool cancelRequested, passwordBack, cachedNameAvailable;
static int nextCalls, previousCalls, scanClears, scans, prompts;
static SetupWizardPage currentPage;
static char deviceName[20];
static void requestWizardCancel() { cancelRequested = true; }
static bool isWizardCancelRequested() { return cancelRequested; }
static void noteWizardInputActivity() {}
static String waitForSerialInputBlocking() {
  assert(!inputs.empty()); const auto value = inputs.front(); inputs.pop_front();
  // The idle timeout returns nothing and raises the cancel flag, like production.
  if (value == "<timeout>") { requestWizardCancel(); return String(); }
  return value;
}
struct SerialBoundary {
  void println(const char* = "") {} void print(const char*) {}
  template<typename... Args> void printf(const char*, Args...) {}
} Serial;
static void broadcastOutput(const String& value) { output.push_back(value); }
static int getWizardPageNumber(SetupWizardPage) { return 2; }
static int getWizardTotalPages() { return 6; }
static bool wizardNextPage(SetupWizardResult& result) {
  ++nextCalls; currentPage = WIZARD_PAGE_DISPLAY; result.completed = false; return true;
}
static bool wizardPrevPage() { ++previousCalls; currentPage = WIZARD_PAGE_CONNECTIONS; return true; }
static SetupWizardPage getWizardCurrentPage() { return currentPage; }
static bool wizardIsGroupPage(SetupWizardPage page) {
  return page == WIZARD_PAGE_CONNECTIONS || page == WIZARD_PAGE_DISPLAY ||
         page == WIZARD_PAGE_SENSORS || page == WIZARD_PAGE_SYSTEM;
}
static void wifiScanClearNamed() { ++scanClears; }
static int wifiScanPrintNamed(int) { ++scans; return 1; }
static bool wifiScanGetNamedSSID(int index, String& ssid) {
  assert(index == 0); ssid = "scan-network"; return cachedNameAvailable;
}
static void clearWizardWifiChoices() {
  wifiScanClearNamed(); sWizard.wifiScanValid = false; sWizard.wifiNamedCount = 0;
}
static void appendPromptTo(char*, size_t) { ++prompts; }
static char* getWizardDeviceNameBuf() { return deviceName; }
static int showWizardOptionalPageIntro(SetupWizardPage page, const char*, const char*) {
  assert(page == WIZARD_PAGE_WIFI && !introChoices.empty());
  int value = introChoices.front(); introChoices.pop_front();
  if (value == -1) wizardPrevPage();
  if (value == -2) requestWizardCancel();
  return value;
}
static bool getOLEDWiFiSelection(String& ssid) {
  assert(!pickedNetworks.empty()); ssid = pickedNetworks.front(); pickedNetworks.pop_front();
  return !ssid.empty();
}
static String getOLEDTextInput(const char*, bool masked, const char*, int, bool* cancelled) {
  assert(masked); String value = waitForSerialInputBlocking();
  // A masked keyboard prompt takes text literally; only the timeout cancels it.
  *cancelled = passwordBack || cancelRequested;
  passwordBack = false; return value;
}
static CLIModeInputResult unexpectedDispatch(const String&, char*, size_t) {
  assert(false && "unexpected non-WiFi input"); return CLI_MODE_HANDLED;
}
static auto dispatchTopLevelPage = unexpectedDispatch;
static auto dispatchModePage = unexpectedDispatch;
static auto dispatchESPNow = unexpectedDispatch;
static auto dispatchMQTT = unexpectedDispatch;
// INSERT_PRODUCTION

static unsigned checks;
#define check(value) do { ++checks; assert(value); } while (0)
static SetupWizardResult acceptedBefore() {
  SetupWizardResult result{}; result.wifiConfigured = true;
  result.wifiSSID = "old-network"; result.wifiPassword = "old-secret"; return result;
}
static void reset() {
  inputs.clear(); pickedNetworks.clear(); introChoices.clear(); output.clear();
  cancelRequested = passwordBack = false; cachedNameAvailable = true;
  nextCalls = previousCalls = scanClears = scans = prompts = 0;
  currentPage = WIZARD_PAGE_WIFI; sWizard = {}; sWizard.subMode = WizardSubMode::WIFI_SSID;
  sWizard.wifiScanValid = true; sWizard.wifiNamedCount = 1; sWizard.result = acceptedBefore();
}
static void advanced(const SetupWizardResult& result) {
  check(nextCalls == 1 && previousCalls == 0 && currentPage == WIZARD_PAGE_DISPLAY);
  check(!result.completed);
}
static void preserved(const SetupWizardResult& result) {
  check(result.wifiConfigured && result.wifiSSID == "old-network" && result.wifiPassword == "old-secret");
}
static CLIModeInputResult cli(const char* line) {
  char response[160]{}; return wizardMode_onInput(line, nullptr, response, sizeof(response));
}
static void serialCases() {
#if ENABLE_WIFI
  for (const char* name : {"typed-network", "1", "2WIRE123"}) {
    reset(); auto result = acceptedBefore(); bool running = true; inputs = {name, "new-secret"};
    handleSerialWiFiPage(result, running); advanced(result);
    check(running && result.wifiConfigured && result.wifiPassword == "new-secret");
    check(result.wifiSSID == (std::string(name) == "1" ? "scan-network" : name));
  }
  for (const char* skip : {"skip", ""}) {
    reset(); auto result = acceptedBefore(); bool running = true; inputs = {skip};
    handleSerialWiFiPage(result, running); advanced(result);
    check(running && !result.wifiConfigured && result.wifiSSID.empty() && result.wifiPassword.empty());
  }
  for (bool password : {false, true}) {
    reset(); auto result = acceptedBefore(); bool running = true;
    inputs = password ? std::deque<String>{"new-network", "back", "back"} : std::deque<String>{"back"};
    handleSerialWiFiPage(result, running); preserved(result);
    check(running && !result.completed && previousCalls == 1 && nextCalls == 0);
  }
  reset(); auto result = acceptedBefore(); bool running = true; inputs = {"rescan", "skip"};
  handleSerialWiFiPage(result, running); advanced(result); check(scans == 2);
  reset(); result = acceptedBefore(); running = true; inputs = {"cancel"};
  handleSerialWiFiPage(result, running); preserved(result);
  check(!running && !result.completed && cancelRequested && nextCalls == 0);
  // The password prompt takes the cancel word literally; only the idle timeout cancels there.
  reset(); result = acceptedBefore(); running = true; inputs = {"new-network", "cancel"};
  handleSerialWiFiPage(result, running); advanced(result);
  check(running && !cancelRequested && result.wifiSSID == "new-network" && result.wifiPassword == "cancel");
  reset(); result = acceptedBefore(); running = true; inputs = {"new-network", "<timeout>"};
  handleSerialWiFiPage(result, running); preserved(result);
  check(!running && !result.completed && cancelRequested && nextCalls == 0);
#else
  reset(); auto result = acceptedBefore(); bool running = true;
  handleSerialWiFiPage(result, running); advanced(result); check(running);
#endif
}
static void oledCases() {
  reset(); auto result = acceptedBefore(); introChoices = {1}; pickedNetworks = {"new-network"}; inputs = {"new-secret"};
  check(renderWiFiPage(result)); advanced(result);
  check(result.wifiConfigured && result.wifiSSID == "new-network" && result.wifiPassword == "new-secret");
  reset(); result = acceptedBefore(); introChoices = {0};
  check(renderWiFiPage(result)); advanced(result);
  check(!result.wifiConfigured && result.wifiSSID.empty() && result.wifiPassword.empty());
  reset(); result = acceptedBefore(); introChoices = {-1};
  check(!renderWiFiPage(result)); preserved(result); check(previousCalls == 1 && !nextCalls);
  reset(); result = acceptedBefore(); introChoices = {1, -1}; pickedNetworks = {"new-network", ""};
  inputs = {"abandoned-secret"}; passwordBack = true;
  check(!renderWiFiPage(result)); preserved(result); check(previousCalls == 1 && !nextCalls);
  reset(); result = acceptedBefore(); introChoices = {1, 0}; pickedNetworks = {""};
  check(renderWiFiPage(result)); advanced(result); check(!result.wifiConfigured);
  reset(); result = acceptedBefore(); introChoices = {1}; pickedNetworks = {"new-network"}; inputs = {"<timeout>"};
  check(!renderWiFiPage(result)); preserved(result); check(cancelRequested && !nextCalls);
  // The masked keyboard prompt takes the cancel word as the password.
  reset(); result = acceptedBefore(); introChoices = {1}; pickedNetworks = {"new-network"}; inputs = {"cancel"};
  check(renderWiFiPage(result)); advanced(result);
  check(!cancelRequested && result.wifiConfigured && result.wifiSSID == "new-network" && result.wifiPassword == "cancel");
}
static void cliCases() {
#if ENABLE_WIFI
  for (const char* name : {"typed-network", "1", "2WIRE123"}) {
    reset(); check(cli(name) == CLI_MODE_HANDLED); preserved(sWizard.result);
    check(nextCalls == 0 && sWizard.subMode == WizardSubMode::WIFI_PASSWORD);
    check(cli("new-secret") == CLI_MODE_HANDLED); advanced(sWizard.result);
    check(sWizard.result.wifiSSID == (std::string(name) == "1" ? "scan-network" : name));
    check(sWizard.result.wifiPassword == "new-secret" && sWizard.subMode == WizardSubMode::PAGE_GROUP);
    check(sWizard.wifiPendingSsid.empty());
  }
  for (const char* skip : {"skip", ""}) {
    reset(); check(cli(skip) == CLI_MODE_HANDLED); advanced(sWizard.result);
    check(!sWizard.result.wifiConfigured && sWizard.result.wifiSSID.empty() && sWizard.result.wifiPassword.empty());
  }
  reset(); check(cli("new-network") == CLI_MODE_HANDLED); check(cli("back") == CLI_MODE_HANDLED);
  preserved(sWizard.result); check(sWizard.wifiPendingSsid.empty() && !nextCalls && !previousCalls);
  check(cli("back") == CLI_MODE_HANDLED); preserved(sWizard.result); check(previousCalls == 1 && !nextCalls);
  reset(); check(cli("rescan") == CLI_MODE_HANDLED); preserved(sWizard.result);
  check(!sWizard.wifiScanValid && scanClears == 1 && !nextCalls);
  reset(); cachedNameAvailable = false; check(cli("1") == CLI_MODE_HANDLED); preserved(sWizard.result);
  check(sWizard.subMode == WizardSubMode::WIFI_SSID && sWizard.wifiPendingSsid.empty() && !nextCalls);
  reset(); check(cli("cancel") == CLI_MODE_HANDLED_AND_EXIT);
  check(!sWizard.result.completed && !nextCalls);
  // At the password prompt the cancel word is the password.
  reset(); check(cli("new-network") == CLI_MODE_HANDLED); check(cli("cancel") == CLI_MODE_HANDLED);
  advanced(sWizard.result);
  check(sWizard.result.wifiSSID == "new-network" && sWizard.result.wifiPassword == "cancel");
#else
  reset(); check(cli("skip") == CLI_MODE_HANDLED); advanced(sWizard.result);
#endif
}
int main() {
  serialCases(); oledCases(); cliCases();
  std::printf("setup Wi-Fi navigation (wifi=%d): %u checks passed\n", ENABLE_WIFI, checks);
}
