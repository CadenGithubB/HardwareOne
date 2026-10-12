// Shipping exit adapters and timeout/cancel code are inserted by runner.
// test_setup_draft executes the real owner; apply here records boundary calls.
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

class String : public std::string {
 public:
  using std::string::string;
  String(const std::string& value) : std::string(value) {}
  void trim() {
    const size_t first = find_first_not_of(" \t\r\n");
    *this = first == npos ? "" : substr(first, find_last_not_of(" \t\r\n") - first + 1);
  }
  bool equalsIgnoreCase(const char* text) const {
    std::string left = *this, right = text;
    for (char& c : left) c = std::tolower(static_cast<unsigned char>(c));
    for (char& c : right) c = std::tolower(static_cast<unsigned char>(c));
    return left == right;
  }
};

// INSERT_PRODUCTION_TYPES
// INSERT_PRODUCTION_STATE

static std::vector<std::string> operations, output;
static bool dialogTimeout, dialogCancel, applyAccepted, exitCanComplete;
static bool cliModeExitCanComplete() { return exitCanComplete; }
static unsigned long fakeNow, expectedDialogTimeout;
static uint32_t heapUsed, heapTotal;
static SetupWizardResult dialogResult, appliedResult;
static unsigned scanClearCalls, dialogCalls, applyCalls, discardCalls, checks;
static std::vector<String> inputs;
static size_t inputIndex;
static void checked(bool condition, int line) { ++checks; if (!condition) std::fprintf(stderr, "check failed line %d\n", line); assert(condition); }
#define check(condition) checked((condition), __LINE__)
static unsigned long millis() { return fakeNow; }
static void delay(unsigned long ms) { fakeNow += ms; }
struct SerialBoundary {
  bool available() const { return inputIndex < inputs.size(); }
  String readStringUntil(char) { return inputs.at(inputIndex++); }
} Serial;
static void broadcastOutput(const String& text) {
  output.push_back(text);
  if (text == "Feature configuration complete.") {
    check(applyCalls == 1 && applyAccepted);
    operations.emplace_back("complete.message");
  }
}
static void discardSetupWizard() { ++discardCalls; operations.emplace_back("draft.discard"); }
static bool wizardSaveAndApplySettings(SetupWizardResult& result) {
  ++applyCalls; appliedResult = result; operations.emplace_back("draft.apply");
  if (!applyAccepted) result = {};
  else { result.timezoneAbbrev = "APPLIED"; result.applied = true; }
  return applyAccepted;
}
#if ENABLE_WIFI
static void wifiScanClearNamed() { ++scanClearCalls; }
#endif
static void getHeapBarData(uint32_t* used, uint32_t* total, int* pct) {
  operations.emplace_back("heap.present"); *used = heapUsed; *total = heapTotal; *pct = 0;
}
String waitForSerialInputBlocking();
void requestWizardCancel();
static SetupWizardResult runSetupWizard() {
  ++dialogCalls; operations.emplace_back("dialog");
  check(gWizardOwnsSerial && sSerialWaitTimeoutMs == expectedDialogTimeout);
  check(!sWizardCancelRequested);
  if (dialogTimeout) {
    check(expectedDialogTimeout > 0);
    fakeNow += expectedDialogTimeout + 1;
    check(waitForSerialInputBlocking().empty());
    check(sWizardCancelRequested);
  }
  if (dialogCancel) requestWizardCancel();
  return dialogResult;
}
#define DEBUGF(...) do {} while (0)
static void appendPromptTo(char*, size_t) { assert(false && "unexpected navigation"); }
static char* getWizardDeviceNameBuf() { static char buffer[21]; return buffer; }
static CLIModeInputResult unexpectedDispatch(const String&, char*, size_t) {
  assert(false && "completion test unexpectedly entered another page"); return CLI_MODE_HANDLED;
}
static auto dispatchTopLevelPage = unexpectedDispatch;
static auto dispatchModePage = unexpectedDispatch;
static auto dispatchESPNow = unexpectedDispatch;
static auto dispatchMQTT = unexpectedDispatch;
static auto dispatchWiFi = unexpectedDispatch;

static SetupWizardPage getWizardCurrentPage() { return WIZARD_PAGE_SYSTEM; }
static WizardSubMode subModeForPage(SetupWizardPage) { return WizardSubMode::PAGE_GROUP; }
// INSERT_PRODUCTION_FUNCTIONS

static void reset() {
  operations.clear(); output.clear(); inputs.clear(); inputIndex = 0;
  dialogTimeout = dialogCancel = false; applyAccepted = exitCanComplete = true;
  fakeNow = 100; expectedDialogTimeout = 0; heapUsed = 20; heapTotal = 100;
  sWizard = {}; dialogResult = {}; appliedResult = {};
  sSerialWaitTimeoutMs = sSerialWaitLastActivityMs = 0;
  sWizardCancelRequested = gWizardOwnsSerial = false;
  scanClearCalls = dialogCalls = applyCalls = discardCalls = 0;
}
static SetupWizardResult completed() {
  SetupWizardResult result{};
  result.completed = true; result.wifiConfigured = true;
  result.wifiSSID = "fixture-network"; result.wifiPassword = "fixture-secret";
  result.timezoneAbbrev = "BEFORE"; return result;
}
static bool hasOutput(const char* text) {
  return std::find(output.begin(), output.end(), text) != output.end();
}
static void noSecrets() {
  for (const auto& line : output) check(line.find("fixture-secret") == std::string::npos);
}
static void scratchCleared() {
  check(sWizard.wifiPendingSsid.empty() && !sWizard.wifiScanValid && sWizard.wifiNamedCount == 0);
  check(!sWizard.result.completed && !sWizard.result.wifiConfigured && sWizard.result.wifiPassword.empty());
  check(!sWizard.espnowConfiguring && sWizard.subMode == WizardSubMode::DONE);
}
static void completedAdapterCases() {
  for (bool cli : {false, true}) for (bool accepted : {false, true}) {
    reset(); applyAccepted = accepted;
    if (cli) {
      sWizard.result = completed(); sWizard.wifiPendingSsid = "pending";
      sWizard.wifiNamedCount = 3; sWizard.wifiScanValid = sWizard.espnowConfiguring = true;
      wizardMode_onExit(nullptr);
      scratchCleared(); check(discardCalls == 1 && dialogCalls == 0);
    } else {
      dialogResult = completed(); expectedDialogTimeout = 1234; sWizardCancelRequested = true;
      const auto result = runAndApplyFeatureWizard(expectedDialogTimeout);
      check(result.completed == accepted && result.wifiConfigured == accepted && result.applied == accepted);
      check(!gWizardOwnsSerial && sSerialWaitTimeoutMs == 0 && !sWizardCancelRequested);
      check(hasOutput("Heap estimate: ~80 KB") == accepted);
    }
    check(applyCalls == 1 && appliedResult.wifiPassword == "fixture-secret");
    check(hasOutput("Feature configuration complete.") == accepted);
    check(hasOutput("Timezone: APPLIED") == accepted && !hasOutput("Timezone: BEFORE"));
    if (accepted) check(std::find(operations.begin(), operations.end(), "draft.apply") <
                        std::find(operations.begin(), operations.end(), "complete.message"));
    noSecrets();
  }
  reset(); dialogResult = completed(); heapUsed = 150;
  runAndApplyFeatureWizard(0); check(hasOutput("Heap estimate: ~0 KB"));
}
static void cancelledAdapterCases() {
  for (int cancelKind = 0; cancelKind < 3; ++cancelKind) {
    reset(); dialogResult = completed();
    dialogTimeout = cancelKind == 1; dialogCancel = cancelKind == 2;
    if (cancelKind == 0) dialogResult.completed = false;
    expectedDialogTimeout = dialogTimeout ? 250 : 0;
    const auto result = runAndApplyFeatureWizard(expectedDialogTimeout);
    check(!result.completed && !result.applied && !result.wifiConfigured && result.wifiPassword.empty());
    check(applyCalls == 0 && discardCalls == 1 && !gWizardOwnsSerial && sSerialWaitTimeoutMs == 0);
    check(hasOutput(cancelKind ? "Feature setup cancelled or timed out. No changes saved."
                               : "Feature setup cancelled. No changes saved."));
    check(!sWizardCancelRequested); noSecrets();
  }
  for (bool explicitCancel : {false, true}) {
    reset(); sWizard.result = completed(); sWizard.wifiPendingSsid = "pending";
    sWizard.wifiNamedCount = 8; sWizard.wifiScanValid = sWizard.espnowConfiguring = true;
    if (explicitCancel) {
      // Any prompt but a password one (see setup_wifi_navigation for those).
      sWizard.subMode = WizardSubMode::WIFI_SSID;
      char response[256] = {};
      check(wizardMode_onInput("CaNcEl", nullptr, response, sizeof(response)) == CLI_MODE_HANDLED_AND_EXIT);
      check(!sWizard.result.completed && std::strstr(response, "No changes saved."));
    } else sWizard.result.completed = false; // Session loss/idle timeout invokes onExit.
    wizardMode_onExit(nullptr); scratchCleared();
    check(applyCalls == 0 && discardCalls == 1);
    check(hasOutput("Feature setup cancelled. No changes saved.")); noSecrets();
  }
}
static void inputCancellationCases() {
  reset(); setSerialWaitTimeout(50); fakeNow += 51;
  check(isWizardCancelRequested()); // Polling alone detects idle timeout.
  check(waitForSerialInputBlocking().empty());
  setSerialWaitTimeout(50); fakeNow += 40; noteWizardInputActivity(); fakeNow += 40;
  check(!isWizardCancelRequested());
  for (const char* word : {"cancel", " CANCEL "}) {
    setSerialWaitTimeout(0); inputs = {word}; inputIndex = 0;
    check(waitForWizardInput().empty()); check(isWizardCancelRequested());
  }
  setSerialWaitTimeout(0); inputs = {" ordinary text "}; inputIndex = 0;
  check(waitForWizardInput() == "ordinary text" && !isWizardCancelRequested());
  // Secret prompts take the cancel word literally; only the idle timeout cancels them.
  setSerialWaitTimeout(0); inputs = {"cancel"}; inputIndex = 0;
  check(waitForWizardSecret() == "cancel" && !isWizardCancelRequested());
  setSerialWaitTimeout(50); fakeNow += 51; inputs.clear(); inputIndex = 0;
  check(waitForWizardSecret().empty() && isWizardCancelRequested());
  requestWizardCancel(); check(waitForSerialInputBlocking().empty());
}
static void exitEligibilityCases() {
  // A completed page draft cannot authorize an exit caused by session loss,
  // timeout or replacement. The CLI owner supplies eligibility; the adapter
  // must combine it with the wizard's independent cancellation state.
  for (bool eligible : {false, true}) for (bool cancelled : {false, true}) {
    reset(); exitCanComplete = eligible; setSerialWaitTimeout(500);
    sWizard.result = completed(); sWizard.wifiPendingSsid = "pending";
    sWizard.wifiNamedCount = 4; sWizard.wifiScanValid = sWizard.espnowConfiguring = true;
    if (cancelled) requestWizardCancel();
    wizardMode_onExit(nullptr);
    const bool accepted = eligible && !cancelled;
    check(applyCalls == static_cast<unsigned>(accepted) && discardCalls == 1);
    check(hasOutput("Feature configuration complete.") == accepted);
    check(hasOutput("Feature setup cancelled. No changes saved.") == !accepted);
    scratchCleared();
    check(sSerialWaitTimeoutMs == 0 && !sWizardCancelRequested);
    noSecrets();
  }
}
int main() {
  completedAdapterCases(); cancelledAdapterCases(); inputCancellationCases(); exitEligibilityCases();
  std::cout << "Setup completion (Wi-Fi=" << ENABLE_WIFI << "): " << checks
            << " checks passed: success gating, scratch cleanup, cancel/timeout and input activity\n";
}
