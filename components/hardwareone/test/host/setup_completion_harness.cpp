// Shipping helper, exit adapters and timeout/cancel code are inserted by runner.
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
static bool mainSave, debugSave, connected, dialogTimeout;
static int connectivityAfterApply;
static unsigned long fakeNow, expectedDialogTimeout;
static uint32_t heapUsed, heapTotal;
static SetupWizardResult dialogResult;
static WifiNetworkSaveResult wifiResult;
static unsigned finalizeCalls, scanClearCalls, dialogCalls;
static bool finalizeChangesTimezone;

static unsigned long millis() { return fakeNow; }
static void delay(unsigned long ms) { fakeNow += ms; }
struct SerialBoundary {
  bool available() const { return false; }
  String readStringUntil(char) { assert(false && "unexpected serial input"); return ""; }
} Serial;
static void broadcastOutput(const String& text) { output.push_back(text); }
static bool writeSettingsJson() { operations.emplace_back("main.write"); return mainSave; }
static bool writeDebugJson() { operations.emplace_back("debug.write"); return debugSave; }
static void applySettings() {
  operations.emplace_back("settings.apply");
  assert(std::count(output.begin(), output.end(),
      "Warning: one or more setup settings files could not be saved.") ==
      static_cast<int>(!mainSave || !debugSave));
  if (connectivityAfterApply >= 0) connected = connectivityAfterApply != 0;
}
#if ENABLE_WIFI
static WifiNetworkSaveResult saveSetupWiFiNetwork(const String& ssid, const String& password) {
  assert(ssid == "fixture-network" && password == "fixture-secret");
  operations.emplace_back("wifi.save");
  return wifiResult;
}
struct WifiBoundary {
  bool isConnected() { operations.emplace_back("wifi.connected"); return connected; }
} WiFi;
static void setupNTP() { operations.emplace_back("ntp.setup"); }
static void wifiScanClearNamed() { ++scanClearCalls; }
#endif
static void wizardFinalize(SetupWizardResult& result) {
  ++finalizeCalls;
  operations.emplace_back("wizard.finalize");
  if (finalizeChangesTimezone) result.timezoneAbbrev = "CHANGED";
}
static void getHeapBarData(uint32_t* used, uint32_t* total, int* pct) {
  operations.emplace_back("heap.present");
  *used = heapUsed; *total = heapTotal; *pct = 0;
}
String waitForSerialInputBlocking();
static SetupWizardResult runSetupWizard() {
  ++dialogCalls;
  operations.emplace_back("dialog");
  assert(gWizardOwnsSerial && sSerialWaitTimeoutMs == expectedDialogTimeout);
  assert(!sWizardCancelRequested);  // The entry setter clears earlier cancellation.
  if (dialogTimeout) {
    assert(expectedDialogTimeout > 0);
    fakeNow += expectedDialogTimeout + 1;
    assert(waitForSerialInputBlocking().empty());
    assert(sWizardCancelRequested);
    return SetupWizardResult{};
  }
  // The dialog is the boundary here; it returns results after its separately
  // owned finalization, whose source placement the Python runner pins.
  return dialogResult;
}
#define DEBUGF(...) do {} while (0)
static void appendPromptTo(char*, size_t) { assert(false && "unexpected navigation"); }
static char* getWizardDeviceNameBuf() { static char buffer[21]; return buffer; }
static CLIModeInputResult unexpectedDispatch(const String&, char*, size_t) {
  assert(false && "completion test unexpectedly entered another page");
  return CLI_MODE_HANDLED;
}
static auto dispatchTopLevelPage = unexpectedDispatch;
static auto dispatchModePage = unexpectedDispatch;
static auto dispatchESPNow = unexpectedDispatch;
static auto dispatchMQTT = unexpectedDispatch;
static auto dispatchWiFi = unexpectedDispatch;

// INSERT_PRODUCTION_FUNCTIONS

static void reset() {
  operations.clear(); output.clear();
  mainSave = debugSave = true;
  connected = dialogTimeout = finalizeChangesTimezone = false;
  connectivityAfterApply = -1;
  fakeNow = 100; expectedDialogTimeout = 0;
  heapUsed = 20; heapTotal = 100;
  sWizard = {}; dialogResult = {};
  sSerialWaitTimeoutMs = sSerialWaitLastActivityMs = 0;
  sWizardCancelRequested = gWizardOwnsSerial = false;
  wifiResult = WifiNetworkSaveResult::Accepted;
  finalizeCalls = scanClearCalls = dialogCalls = 0;
}
static SetupWizardResult completed(bool wifi = true) {
  SetupWizardResult result{};
  result.completed = true;
  result.wifiConfigured = wifi;
  result.wifiSSID = "fixture-network";
  result.wifiPassword = "fixture-secret";
  result.timezoneAbbrev = "PDT";
  return result;
}
static bool hasOutput(const char* text) {
  return std::find(output.begin(), output.end(), text) != output.end();
}
static size_t outputCount(const char* text) { return std::count(output.begin(), output.end(), text); }
static void expectNoSecrets() {
  for (const auto& line : output) assert(line.find("fixture-secret") == std::string::npos);
}
static std::vector<std::string> expectedOperations(const SetupWizardResult& result) {
  if (!result.completed) return {};
  std::vector<std::string> expected;
#if ENABLE_WIFI
  if (result.wifiConfigured && !result.wifiSSID.empty()) expected.emplace_back("wifi.save");
#endif
  expected.insert(expected.end(), {"main.write", "debug.write", "settings.apply"});
#if ENABLE_WIFI
  expected.emplace_back("wifi.connected");
  if (connected) expected.emplace_back("ntp.setup");
#endif
  return expected;
}
static void expectSharedMessages(const SetupWizardResult& result) {
  const char* warning = "Warning: one or more setup settings files could not be saved.";
  assert(outputCount(warning) == static_cast<size_t>(result.completed && (!mainSave || !debugSave)));
#if ENABLE_WIFI
  const bool saveWifi = result.completed && result.wifiConfigured && !result.wifiSSID.empty();
  assert(outputCount(wifiNetworkSaveResultMessage(wifiResult)) == static_cast<size_t>(saveWifi));
  assert(outputCount("WiFi network: fixture-network") == static_cast<size_t>(saveWifi && wifiResult == WifiNetworkSaveResult::Accepted));
#else
  for (const auto& line : output) assert(line.find("WiFi") == std::string::npos);
#endif
  expectNoSecrets();
}

static void helperCases() {
  // Invalid/incomplete results must not write, apply, probe connectivity or leak input.
  reset(); auto incomplete = completed(); incomplete.completed = false;
  wizardSaveAndApplySettings(incomplete);
  assert(operations.empty() && output.empty() && finalizeCalls == 0);

  for (int wifiFields = 0; wifiFields < 3; ++wifiFields)
    for (auto outcome : {WifiNetworkSaveResult::Rejected, WifiNetworkSaveResult::PersistFailed, WifiNetworkSaveResult::Accepted})
      for (bool mainOk : {false, true}) for (bool debugOk : {false, true}) for (bool online : {false, true}) {
        reset(); auto result = completed(wifiFields != 0);
        if (wifiFields == 2) result.wifiSSID.clear();
        wifiResult = outcome; mainSave = mainOk; debugSave = debugOk; connected = online;
        wizardSaveAndApplySettings(result);
        assert(operations == expectedOperations(result));
        assert(finalizeCalls == 0); // Includes certificate generation: not repeated here.
        expectSharedMessages(result);
      }
  // Runtime application may change connectivity; the NTP decision must use the
  // resulting connection state, not one sampled before applySettings().
  for (bool onlineAfterApply : {false, true}) {
    reset(); auto result = completed(false);
    connected = !onlineAfterApply;
    connectivityAfterApply = onlineAfterApply ? 1 : 0;
    wizardSaveAndApplySettings(result);
    assert(operations == expectedOperations(result));
#if ENABLE_WIFI
    assert(std::count(operations.begin(), operations.end(), "ntp.setup") == static_cast<int>(onlineAfterApply));
#endif
  }
}

static void completedAdapterCases() {
  for (bool cli : {false, true})
    for (auto outcome : {WifiNetworkSaveResult::Rejected, WifiNetworkSaveResult::PersistFailed, WifiNetworkSaveResult::Accepted})
      for (bool mainOk : {false, true}) for (bool debugOk : {false, true}) for (bool online : {false, true}) {
        reset(); auto result = completed();
        wifiResult = outcome; mainSave = mainOk; debugSave = debugOk; connected = online;
        auto expected = expectedOperations(result);
        if (cli) {
          sWizard.result = result;
          sWizard.wifiNamedCount = 3; sWizard.wifiScanValid = true;
          wizardMode_onExit(nullptr);
          expected.insert(expected.begin(), "wizard.finalize");
          assert(finalizeCalls == 1 && dialogCalls == 0);
          assert(!sWizard.wifiScanValid && sWizard.wifiNamedCount == 0);
          assert(output.size() >= 3 && output[0].empty());
          assert(!hasOutput("Heap estimate: ~80 KB"));
        } else {
          dialogResult = result; expectedDialogTimeout = 1234;
          sWizardCancelRequested = true; // Old run's cancellation must be cleared on entry.
          const auto returned = runAndApplyFeatureWizard(expectedDialogTimeout);
          assert(returned.completed && returned.timezoneAbbrev == "PDT");
          expected.insert(expected.begin(), {"dialog", "heap.present"});
          assert(finalizeCalls == 0 && dialogCalls == 1);
          assert(!gWizardOwnsSerial && sSerialWaitTimeoutMs == 0);
          assert(hasOutput("Heap estimate: ~80 KB"));
        }
        assert(operations == expected);
        assert(outputCount("Feature configuration complete.") == 1 && outputCount("Timezone: PDT") == 1);
        expectSharedMessages(result);
      }
  // Keep the legacy heap estimate saturating at zero, rather than underflowing.
  reset(); dialogResult = completed(false); heapUsed = 150;
  runAndApplyFeatureWizard(0);
  assert(hasOutput("Heap estimate: ~0 KB"));
  // CLI's current presentation precedes finalization; this extraction must not reorder it.
  reset(); sWizard.result = completed(false); finalizeChangesTimezone = true;
  wizardMode_onExit(nullptr);
  assert(hasOutput("Timezone: PDT") && !hasOutput("Timezone: CHANGED"));
  assert(sWizard.result.timezoneAbbrev == "CHANGED");
}

static void cancelledAdapterCases() {
  for (bool timeout : {false, true}) {
    reset(); dialogResult = completed(); dialogResult.completed = false;
    dialogTimeout = timeout; expectedDialogTimeout = timeout ? 250 : 0;
    const auto returned = runAndApplyFeatureWizard(expectedDialogTimeout);
    assert(!returned.completed && operations == std::vector<std::string>{"dialog"});
    assert(finalizeCalls == 0 && !gWizardOwnsSerial && sSerialWaitTimeoutMs == 0);
    assert(output.size() == 1 && output[0].find("No changes saved.") != std::string::npos);
    // The actual timeout setter clears the cancel flag when restoring timeout=0.
    // No final persistence occurs regardless of the existing cancellation label.
    assert(!sWizardCancelRequested);
    expectNoSecrets();
  }
  // Timeout/session loss exits a CLI mode whose result was never completed.
  reset(); sWizard.result = completed(); sWizard.result.completed = false;
  sWizard.wifiNamedCount = 8; sWizard.wifiScanValid = true;
  wizardMode_onExit(nullptr);
  assert(operations.empty() && finalizeCalls == 0 && !sWizard.wifiScanValid && sWizard.wifiNamedCount == 0);
  assert(output == std::vector<std::string>{"Feature setup cancelled. No changes saved."});

  // Exercise the real universal cancel dispatch from an input sub-page, then its exit.
  reset(); sWizard.subMode = WizardSubMode::WIFI_PASSWORD; sWizard.result = completed();
  sWizard.wifiNamedCount = 4; sWizard.wifiScanValid = true;
  char response[256] = {};
  assert(wizardMode_onInput("CaNcEl", nullptr, response, sizeof(response)) == CLI_MODE_HANDLED_AND_EXIT);
  assert(!sWizard.result.completed && std::strstr(response, "No changes saved."));
  wizardMode_onExit(nullptr);
  assert(operations.empty() && finalizeCalls == 0 && !sWizard.wifiScanValid && sWizard.wifiNamedCount == 0);
  expectNoSecrets();
}

int main() {
  helperCases();
  completedAdapterCases();
  cancelledAdapterCases();
  std::cout << "Setup completion (Wi-Fi=" << ENABLE_WIFI
            << "): production helper, adapter completion/cancel/timeout, save outcomes and ordering passed\n";
}
