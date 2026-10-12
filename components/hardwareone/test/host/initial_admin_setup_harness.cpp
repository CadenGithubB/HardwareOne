#include <Arduino.h>
#include <ArduinoJson.h>
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>
#include <algorithm>

// INSERT_TYPES

static unsigned checks;
static void checked(bool condition, int line) {
  ++checks;
  if (!condition) std::fprintf(stderr, "initial admin check at line %d\n", line);
  assert(condition);
}
#define check(condition) checked((condition), __LINE__)
static int allocationFailureAt = -1, allocationCalls;
static bool allocationFailed;
class TestAllocator final : public ArduinoJson::Allocator {
 public:
  void* allocate(size_t size) override {
    if (allocationCalls++ == allocationFailureAt) { allocationFailed = true; return nullptr; }
    return std::malloc(size);
  }
  void deallocate(void* ptr) override { std::free(ptr); }
  void* reallocate(void* ptr, size_t size) override {
    if (allocationCalls++ == allocationFailureAt) { allocationFailed = true; return nullptr; }
    return std::realloc(ptr, size);
  }
};
static TestAllocator allocator;
#define PSRAM_JSON_DOC(name) JsonDocument name(&allocator)
#define DEBUG_SYSTEMF(...) ((void)0)
#define INFO_SYSTEMF(...) ((void)0)
static std::vector<std::string> warnings;
#define WARN_SYSTEMF(...) warnings.emplace_back("warn")
static std::vector<std::string> operations, output;
static String credentialJson, usersJson;
static bool credentialOk, indexOk, hashOk, automationExists, credentialOverflow;
static unsigned hashCalls, credentialCalls, indexCalls, timestampCalls, rebootCalls;
static unsigned runtimeApplyCalls, settingsWriteCalls, automationCalls, wifiCalls;
static unsigned normalAttempts;
static bool repairAfterFirstAttempt;
static unsigned long nowMs;
static time_t wallTime;
static uint32_t gNTPAnchorId, gBootCounter, nvsBootCount;
static bool gFirstTimeSetupPerformed, gOledRunning, oledConnected, gInputRunning;
static FirstTimeSetupState gFirstTimeSetupState;
static SetupProgressStage gSetupProgressStage;
static const char* USERS_JSON_FILE = "/system/users/users.json";
static const char* AUTOMATIONS_JSON_FILE = "/system/automations.json";
static struct { bool oledEnabled, inputAutoStart; } gSettings;

void webBatchTestStringAppend() {}
static unsigned long millis() { return nowMs; }
static time_t testWallTime(time_t*) { return wallTime; }
#define time testWallTime
static String hashUserPassword(const String& plaintext) {
  ++hashCalls; operations.emplace_back("hash");
  check(plaintext == "fixture-secret");
  return hashOk ? "fixture-password-hash" : "";
}
static void broadcastOutput(const String& message) { output.emplace_back(message.c_str()); }
static bool saveUserSettings(uint32_t id, const JsonDocument& doc) {
  ++credentialCalls; operations.emplace_back("credential.save");
  check(id == 1 && usersJson.length() == 0);
  // The separately tested saveUserSettings owner refuses overflowed documents.
  credentialOverflow = doc.overflowed();
  if (!credentialOk || credentialOverflow) return false;
  serializeJson(doc, credentialJson);
  return true;
}
static bool writeTextAtomic(const char* path, const String& text) {
  ++indexCalls; operations.emplace_back("index.save");
  check(std::string(path) == USERS_JSON_FILE);
  check(credentialJson.length() > 0 && credentialCalls > 0);
  if (!indexOk) return false;
  usersJson = text; return true;
}
static uint32_t bootStateGetBootCount() {
  operations.emplace_back("boot.sync"); return nvsBootCount;
}
static void resolvePendingUserCreationTimes() {
  ++timestampCalls; operations.emplace_back("timestamp.resolve"); check(usersJson.length() > 0);
}
static void setFirstTimeSetupState(FirstTimeSetupState state) {
  gFirstTimeSetupState = state;
  if (state == SETUP_NOT_NEEDED || state == SETUP_COMPLETE) {
    check(usersJson.length() > 0); operations.emplace_back("setup.complete");
  }
}
static void setSetupProgressStage(SetupProgressStage stage) {
  gSetupProgressStage = stage;
  if (stage == SETUP_FINISHED) check(usersJson.length() > 0);
}
namespace VFS {
static int systemAuth(const char*) { return 0; }
static bool existsGuarded(const char* path, int) {
  check(std::string(path) == AUTOMATIONS_JSON_FILE);
  return automationExists;
}
}
static bool writeAutomationsJsonAtomic(const String&) {
  ++automationCalls; operations.emplace_back("automations.save"); check(usersJson.length() > 0); return true;
}
static bool settingsSaveSucceeds = true;
static bool writeSettingsJson() { ++settingsWriteCalls; operations.emplace_back("settings.save"); return settingsSaveSucceeds; }
static void applySettings() { ++runtimeApplyCalls; operations.emplace_back("settings.apply"); }
static void connectToBestWiFiNetwork() { ++wifiCalls; operations.emplace_back("wifi.connect"); }
static void noteNtpStateBeforeSetupReboot() { operations.emplace_back("ntp.note"); }
struct Reboot {};
static void rebootWithMessage(const char*) {
  ++rebootCalls; operations.emplace_back("reboot"); check(usersJson.length() > 0); throw Reboot{};
}
static void showOLEDMessage(const char*, bool) {}
static std::vector<String> inputs;
static size_t inputIndex;
static int cancelInput;
static String waitForSerialInputBlocking() {
  check(inputIndex < inputs.size()); return inputs[inputIndex++];
}
static String getOLEDTextInput(const char*, bool, const char*, int, bool* cancelled, bool wizardInput) {
  check(!wizardInput); // Account values must not inherit the feature wizard's cancel-word policy.
  *cancelled = static_cast<int>(inputIndex) == cancelInput;
  return waitForSerialInputBlocking();
}

// INSERT_OWNER_AND_RESTORE

static void normalCommitTail(const String& u, const String& p, bool useDarkTheme,
                             bool wifiConfigured = false, bool i2cDisabledByUser = false,
                             unsigned maxAttempts = 1) {
  // Earlier input collection is a boundary. The shipping continue/break and
  // every effect after the account call are kept intact in this bounded loop.
  for (unsigned attempt = 0; attempt < maxAttempts; ++attempt) {
    ++normalAttempts;
    if (attempt && repairAfterFirstAttempt) { credentialOk = indexOk = true; }
// INSERT_NORMAL_COMMIT_TAIL
  }
}

static void reset() {
  operations.clear(); output.clear(); credentialJson = ""; usersJson = "";
  warnings.clear(); settingsSaveSucceeds = true;
  credentialOk = indexOk = hashOk = true; automationExists = credentialOverflow = false;
  hashCalls = credentialCalls = indexCalls = timestampCalls = rebootCalls = 0;
  runtimeApplyCalls = settingsWriteCalls = automationCalls = wifiCalls = normalAttempts = 0;
  nowMs = 123456; wallTime = 0; gNTPAnchorId = 42; gBootCounter = 17; nvsBootCount = 23;
  gFirstTimeSetupPerformed = gOledRunning = oledConnected = gInputRunning = false;
  gFirstTimeSetupState = SETUP_IN_PROGRESS; gSetupProgressStage = SETUP_SAVING_CONFIG;
  gSettings = {}; inputs.clear(); inputIndex = 0; cancelInput = -1;
  allocationFailureAt = -1; allocationCalls = 0; allocationFailed = false;
  setupTestReserveFails = false; setupTestAppendsBeforeFailure = -1;
  repairAfterFirstAttempt = false;
}
static void noSecrets() {
  for (const auto& message : output) {
    check(message.find("fixture-secret") == std::string::npos);
    check(message.find("fixture-password-hash") == std::string::npos);
  }
}
static void recordAndOrder(bool dark) {
  JsonDocument doc, settings;
  check(!deserializeJson(doc, usersJson));
  check(!deserializeJson(settings, credentialJson));
  check(doc.size() == 2 && doc["nextId"].as<int>() == 2);
  const JsonArray users = doc["users"].as<JsonArray>();
  check(users.size() == 1);
  const JsonObject admin = users[0].as<JsonObject>();
  check(admin.size() == 9 && admin["id"].as<int>() == 1);
  check(admin["username"].as<String>() == "fixture-owner");
  check(admin["role"].as<String>() == "superadmin");
  check(admin["createdAt"].isNull());
  check(admin["createdBy"].as<String>() == "firstsetup");
  check(admin["createdAtSource"].as<String>() == "pending");
  check(admin["createdMs"].as<unsigned long>() == nowMs);
  check(admin["ntpAnchorId"].as<uint32_t>() == gNTPAnchorId);
  check(admin["bootCount"].as<int>() == 1);
  check(!admin["password"].is<String>() && !doc["bootAnchors"].is<JsonArray>());
  check(settings.size() == 2 && settings["password"].as<String>() == "fixture-password-hash");
  check(settings["theme"].as<String>() == (dark ? "dark" : "light"));
  check(gBootCounter == nvsBootCount);
  const auto credential = std::find(operations.begin(), operations.end(), "credential.save");
  const auto index = std::find(operations.begin(), operations.end(), "index.save");
  const auto sync = std::find(operations.begin(), operations.end(), "boot.sync");
  check(credential < index && index < sync);
  noSecrets();
}
static void rejectedBeforePublication() {
  check(usersJson.length() == 0 && indexCalls == 0);
  check(credentialCalls == static_cast<unsigned>(credentialOverflow) && credentialJson.length() == 0);
  check(gBootCounter == 17 && timestampCalls == 0 && rebootCalls == 0);
  noSecrets();
}
static void helperCases() {
  for (bool dark : {false, true}) for (bool clockReady : {false, true}) {
    reset(); wallTime = clockReady ? 1234 : 0;
    check(createInitialAdminUser("fixture-owner", "fixture-secret", dark));
    check(hashCalls == 1 && credentialCalls == 1 && indexCalls == 1);
    check(timestampCalls == static_cast<unsigned>(clockReady)); recordAndOrder(dark);
  }
  reset(); check(!createInitialAdminUser("", "fixture-secret", false));
  check(hashCalls == 0); rejectedBeforePublication();
  reset(); check(!createInitialAdminUser("fixture-owner", "", false));
  check(hashCalls == 0); rejectedBeforePublication();
  reset(); hashOk = false; check(!createInitialAdminUser("fixture-owner", "fixture-secret", false));
  check(hashCalls == 1); rejectedBeforePublication();
  reset(); setupTestReserveFails = true;
  check(!createInitialAdminUser("fixture-owner", "fixture-secret", false)); rejectedBeforePublication();
  for (int successfulAppends : {0, 1}) {
    reset(); setupTestAppendsBeforeFailure = successfulAppends;
    check(!createInitialAdminUser("fixture-owner", "fixture-secret", false)); rejectedBeforePublication();
  }
  reset(); check(createInitialAdminUser("fixture-owner", "fixture-secret", false));
  const int successfulAllocations = allocationCalls;
  check(successfulAllocations > 0);
  for (int failAt = 0; failAt < successfulAllocations; ++failAt) {
    reset(); allocationFailureAt = failAt;
    check(!createInitialAdminUser("fixture-owner", "fixture-secret", false));
    check(allocationFailed); rejectedBeforePublication();
  }
  for (bool failCredential : {false, true}) {
    reset(); credentialOk = !failCredential; indexOk = failCredential;
    check(!createInitialAdminUser("fixture-owner", "fixture-secret", true));
    check(usersJson.length() == 0 && credentialCalls == 1 && indexCalls == static_cast<unsigned>(!failCredential));
    check(gBootCounter == 17 && timestampCalls == 0);
    credentialOk = indexOk = true;
    check(createInitialAdminUser("fixture-owner", "fixture-secret", true)); recordAndOrder(true);
  }
}
static void normalCallerCases() {
  for (bool failCredential : {false, true}) {
    reset(); credentialOk = !failCredential; indexOk = failCredential;
    normalCommitTail("fixture-owner", "fixture-secret", false, true, true);
    check(normalAttempts == 1 && !gFirstTimeSetupPerformed && gFirstTimeSetupState != SETUP_NOT_NEEDED);
    check(gSetupProgressStage != SETUP_FINISHED && usersJson.length() == 0);
    check(automationCalls == 0 && settingsWriteCalls == 0 && runtimeApplyCalls == 0 && wifiCalls == 0 && rebootCalls == 0);
    noSecrets();
    repairAfterFirstAttempt = true;
    normalCommitTail("fixture-owner", "fixture-secret", false, false, false, 2);
    check(gFirstTimeSetupPerformed && normalAttempts == 3);
    check(gFirstTimeSetupState == SETUP_NOT_NEEDED && gSetupProgressStage == SETUP_FINISHED);
    check(automationCalls == 1 && settingsWriteCalls == 1 && runtimeApplyCalls == 1);
    recordAndOrder(false);
  }
  reset(); normalCommitTail("fixture-owner", "fixture-secret", true, true);
  check(gFirstTimeSetupPerformed && wifiCalls == 1); recordAndOrder(true);
  check(warnings.empty());
  // A refused settings save (its retry is retained by the writer) is reported
  // to the log and the operator; setup still completes.
  reset(); settingsSaveSucceeds = false;
  normalCommitTail("fixture-owner", "fixture-secret", true, true);
  check(gFirstTimeSetupPerformed && settingsWriteCalls == 1 && warnings.size() == 1);
  bool told = false;
  for (const auto& line : output) told = told || line.find("could not be saved yet") != std::string::npos;
  check(told);
  reset();
  try { normalCommitTail("fixture-owner", "fixture-secret", false, false, true); }
  catch (const Reboot&) {}
  check(gFirstTimeSetupPerformed && rebootCalls == 1 && usersJson.length() > 0);
}
#if ENABLE_MIGRATION_TOOL
static void restoreCallerCases() {
  for (bool display : {false, true}) {
    for (bool failCredential : {false, true}) {
      reset(); gOledRunning = oledConnected = display;
      inputs = {"fixture-owner", "fixture-secret"};
      credentialOk = !failCredential; indexOk = failCredential;
      check(!runPostRestoreCredentialSetup());
      check(!gFirstTimeSetupPerformed && gFirstTimeSetupState != SETUP_NOT_NEEDED && rebootCalls == 0);
      check(gSetupProgressStage != SETUP_FINISHED && usersJson.length() == 0); noSecrets();
    }
    reset(); gOledRunning = oledConnected = display;
    inputs = {"fixture-owner", "fixture-secret"};
    try { (void)runPostRestoreCredentialSetup(); } catch (const Reboot&) {}
    check(gFirstTimeSetupPerformed && gFirstTimeSetupState == SETUP_NOT_NEEDED);
    check(rebootCalls == 1 && gSetupProgressStage == SETUP_FINISHED); recordAndOrder(false);
  }
#if ENABLE_OLED_DISPLAY
  for (int field : {0, 1}) {
    reset(); gOledRunning = oledConnected = true; cancelInput = field;
    inputs = {"fixture-owner", "fixture-secret"};
    check(!runPostRestoreCredentialSetup());
    check(hashCalls == 0 && !gFirstTimeSetupPerformed && rebootCalls == 0);
    rejectedBeforePublication();
  }
#endif
}
#endif
int main() {
  helperCases(); normalCallerCases();
#if ENABLE_MIGRATION_TOOL
  restoreCallerCases();
#endif
  std::printf("initial admin setup (migration=%d, OLED=%d): %u checks passed\n",
              ENABLE_MIGRATION_TOOL, ENABLE_OLED_DISPLAY, checks);
}
