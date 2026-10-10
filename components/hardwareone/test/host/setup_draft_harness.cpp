// Shipping owner code is inserted by test_setup_draft.py. This fixture supplies
// registry/storage and platform boundaries, never wizard policy.
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static bool failReserve;
class String : public std::string {
 public:
  using std::string::string;
  String(const std::string& value) : std::string(value) {}
  explicit String(long value) : std::string(std::to_string(value)) {}
  bool isEmpty() const { return empty(); }
  bool reserve(size_t size) { if (failReserve) return false; std::string::reserve(size); return true; }
  void trim() {
    const size_t first = find_first_not_of(" \t\r\n");
    *this = first == npos ? "" : substr(first, find_last_not_of(" \t\r\n") - first + 1);
  }
  void toLowerCase() {
    std::transform(begin(), end(), begin(), [](unsigned char c) { return std::tolower(c); });
  }
  bool equalsIgnoreCase(const char* value) const {
    String left = *this, right = value;
    left.toLowerCase(); right.toLowerCase(); return left == right;
  }
  long toInt() const { return std::strtol(c_str(), nullptr, 10); }
};
#define EXT_RAM_BSS_ATTR
#define DEBUGF(...) do {} while (0)
// INSERT_TYPES

static struct SettingsBoundary {
  bool wifiEnabled, wifiAutoStart, httpAutoStart, httpsEnabled, bleAutoStart;
  bool espnowEnabled, espnowmesh, mqttEnabled, mqttAutoStart;
  bool automationEnabled, oledEnabled, i2cEnabled, tofAutoStart;
  bool ledEnabled;
  int bleMode, tzOffsetMinutes, logLevel, mqttPort, unrelated;
  String espnowDeviceName, bleDeviceName, espnowFriendlyName, espnowRoom, espnowZone;
  bool espnowStationary;
  String ntpServer, mqttHost, mqttUser, mqttPassword;
#if ENABLE_NEOPIXEL
  String ledStartupEffect;
#endif
} gSettings;

static bool compiledWifi() { return ENABLE_WIFI; }
static bool compiledWeb() { return ENABLE_HTTP_SERVER; }
static bool compiledBluetooth() { return ENABLE_BLUETOOTH; }
static bool compiledEspnow() { return ENABLE_ESPNOW; }
static bool compiledMqtt() { return ENABLE_MQTT; }
static bool compiledOled() { return ENABLE_OLED_DISPLAY; }
static bool compiledLed() { return ENABLE_NEOPIXEL; }
static bool neverCompiled() { return false; }
static const FeatureEntry featureRegistry[] = {
  {"wifi", "WiFi", FEATURE_CAT_NETWORK, 20, FEATURE_FLAG_RUNTIME_TOGGLE,
   &gSettings.wifiEnabled, compiledWifi, "WiFi fixture"},
  {"http", "HTTP", FEATURE_CAT_NETWORK, 10, FEATURE_FLAG_RUNTIME_TOGGLE,
   &gSettings.httpAutoStart, compiledWeb, "HTTP fixture"},
  {"bluetooth", "Bluetooth", FEATURE_CAT_NETWORK, 12, FEATURE_FLAG_RUNTIME_TOGGLE,
   &gSettings.bleAutoStart, compiledBluetooth, "Bluetooth fixture"},
  {"espnow", "ESP-NOW", FEATURE_CAT_NETWORK, 4, FEATURE_FLAG_RUNTIME_TOGGLE,
   &gSettings.espnowEnabled, compiledEspnow, "ESP-NOW fixture"},
  {"mqtt", "MQTT", FEATURE_CAT_NETWORK, 8, FEATURE_FLAG_RUNTIME_TOGGLE,
   &gSettings.mqttEnabled, compiledMqtt, "MQTT fixture"},
  {"automation", "Automation", FEATURE_CAT_SYSTEM, 2, FEATURE_FLAG_RUNTIME_TOGGLE,
   &gSettings.automationEnabled, nullptr, "Automation fixture"},
  {"i2c", "I2C", FEATURE_CAT_DISPLAY, 3, FEATURE_FLAG_RUNTIME_TOGGLE,
   &gSettings.i2cEnabled, nullptr, "I2C fixture"},
  {"oled", "OLED", FEATURE_CAT_DISPLAY, 3, FEATURE_FLAG_RUNTIME_TOGGLE,
   &gSettings.oledEnabled, compiledOled, "OLED fixture"},
  {"tof", "ToF", FEATURE_CAT_SENSOR, 1, FEATURE_FLAG_RUNTIME_TOGGLE,
   &gSettings.tofAutoStart, nullptr, "ToF fixture"},
  {"led", "LED", FEATURE_CAT_DISPLAY, 1, FEATURE_FLAG_RUNTIME_TOGGLE,
   &gSettings.ledEnabled, compiledLed, "LED fixture"},
  {"missing", "Missing", FEATURE_CAT_NETWORK, 5, FEATURE_FLAG_RUNTIME_TOGGLE,
   &gSettings.automationEnabled, neverCompiled, "Absent fixture"},
  {"fixed", "Fixed", FEATURE_CAT_SYSTEM, 1, FEATURE_FLAG_COMPILE_TIME,
   nullptr, nullptr, "Fixed fixture"},
};
static constexpr size_t featureRegistryCount = sizeof(featureRegistry) / sizeof(featureRegistry[0]);
// INSERT_REGISTRY

static std::vector<std::string> operations, output;
static std::vector<const void*> changed;
static bool mainSave = true, debugSave = true, connected, cancelRequested;
bool isWizardCancelRequested() { return cancelRequested; }
static WifiNetworkSaveResult wifiResult;
static unsigned wifiSaveCalls;

static bool scopeAvailable = true, scopeResult = true, scopeActive;
static unsigned mainWrites, debugWrites, applyCalls, certCalls, timezoneCalls, scopeFinishes;
static size_t hw1InternalTotalBytes() { return 300 * 1024; }
static size_t hw1InternalFreeBytes() { return 250 * 1024; }
static void broadcastOutput(const String& line) { output.emplace_back(line); }
static bool writeSettingsJson() { ++mainWrites; operations.emplace_back("main.write"); return mainSave; }
static bool writeDebugJson() { ++debugWrites; operations.emplace_back("debug.write"); return debugSave; }
static void applySettings() { ++applyCalls; operations.emplace_back("settings.apply"); }
namespace Clock { static void applyTimezone() { ++timezoneCalls; } }
const char* cmd_certgen(const String&) { ++certCalls; operations.emplace_back("certificate"); return "fixture certificate generated"; }
static WifiNetworkSaveResult saveSetupWiFiNetwork(const String& ssid, const String& password) {
  assert(ssid == "fixture-network" && password == "fixture-secret");
  assert(scopeActive); ++wifiSaveCalls; operations.emplace_back("wifi.save"); return wifiResult;
}
struct WifiBoundary { bool isConnected() const { return connected; } } WiFi;
static void setupNTP() { operations.emplace_back("ntp.setup"); }
struct SettingEntry;
static void finishSettingChange(const void* field, const SettingEntry*) {
  assert(scopeActive && "wizard mutation escaped its persistence scope");
  changed.push_back(field);
  operations.emplace_back("setting.change");
}
static bool requestDebugSettingsPersist() {
  assert(scopeActive && "wizard debug mutation escaped its persistence scope");
  operations.emplace_back("debug.change");
  return true;
}
// The separate settings_write_scope suite executes the real scope/adapter/core.
// Here the scope is a boundary: observe entry/finish ordering and inject its
// documented flush outcome without reimplementing persistence or retry logic.
class SettingsWriteScope {
 public:
  SettingsWriteScope() { scopeActive = scopeAvailable; operations.emplace_back("scope.begin"); }
  ~SettingsWriteScope() { scopeActive = false; }
  bool active() const { return scopeActive; }
  bool finish() { ++scopeFinishes; operations.emplace_back("scope.finish"); scopeActive = false; return scopeResult; }
};
// INSERT_MUTATORS
// INSERT_PRODUCTION

static unsigned checks;
static void checked(bool good, int line) { ++checks; if (!good) std::fprintf(stderr, "draft check line %d\n", line); assert(good); }
#define check(good) checked((good), __LINE__)
static std::vector<std::string> liveValues() {
  return {std::to_string(gSettings.wifiEnabled), std::to_string(gSettings.wifiAutoStart),
    std::to_string(gSettings.httpAutoStart), std::to_string(gSettings.httpsEnabled),
    std::to_string(gSettings.bleAutoStart), std::to_string(gSettings.bleMode),
    std::to_string(gSettings.espnowEnabled), std::to_string(gSettings.espnowmesh),
    std::to_string(gSettings.mqttEnabled), std::to_string(gSettings.mqttAutoStart),
    std::to_string(gSettings.automationEnabled), std::to_string(gSettings.oledEnabled),
    std::to_string(gSettings.i2cEnabled), std::to_string(gSettings.tofAutoStart),
    std::to_string(gSettings.ledEnabled), std::to_string(gSettings.tzOffsetMinutes),
    std::to_string(gSettings.logLevel), std::to_string(gSettings.mqttPort),
    std::to_string(gSettings.unrelated), gSettings.espnowDeviceName, gSettings.bleDeviceName,
    gSettings.espnowFriendlyName, gSettings.espnowRoom, gSettings.espnowZone,
    std::to_string(gSettings.espnowStationary), gSettings.ntpServer, gSettings.mqttHost,
    gSettings.mqttUser, gSettings.mqttPassword,
#if ENABLE_NEOPIXEL
    gSettings.ledStartupEffect,
#endif
  };
}
static void reset() {
  discardSetupWizard();
  gSettings = {};
  gSettings.espnowDeviceName = "old-device"; gSettings.bleDeviceName = "old-ble";
  gSettings.espnowFriendlyName = "old-friendly"; gSettings.espnowRoom = "old-room";
  gSettings.espnowZone = "old-zone"; gSettings.espnowStationary = true;
  gSettings.tzOffsetMinutes = 123; gSettings.logLevel = 2;
  gSettings.ntpServer = "custom.ntp"; gSettings.mqttHost = "old.host";
  gSettings.mqttPort = 8883; gSettings.mqttUser = "old-user"; gSettings.mqttPassword = "old-secret";
  gSettings.unrelated = 42;
#if ENABLE_NEOPIXEL
  gSettings.ledStartupEffect = "custom-effect";
#endif
  operations.clear(); output.clear(); changed.clear();
  mainSave = debugSave = scopeAvailable = scopeResult = true;
  connected = scopeActive = failReserve = cancelRequested = false;
  wifiResult = WifiNetworkSaveResult::Accepted; wifiSaveCalls = 0;
  mainWrites = debugWrites = applyCalls = certCalls = timezoneCalls = scopeFinishes = 0;
}
static SetupWizardResult completed() {
  SetupWizardResult result{}; result.completed = true; return result;
}
static void noLiveEffects(const std::vector<std::string>& before) {
  check(liveValues() == before);
  check(changed.empty() && operations.empty());
  check(mainWrites == 0 && debugWrites == 0 && applyCalls == 0 && certCalls == 0 && timezoneCalls == 0);
}
static void toggleFeature(const char* id) {
  for (int page = 0; page < WIZARD_PAGE_COUNT; ++page) {
    const auto group = static_cast<SetupWizardPage>(page);
    for (size_t index = 0; index < getWizardMenuItemCount(group); ++index) {
      const auto* row = getWizardMenuItem(group, static_cast<int>(index));
      if (!row || std::strcmp(row->id, id)) continue;
      setWizardCurrentPage(group);
      setWizardCurrentSelection(static_cast<int>(index));
      check(wizardActivateCurrentItem()); return;
    }
  }
  assert(false && "missing feature row");
}

static void cancellationAndRestart() {
  reset(); const auto original = liveValues(); initSetupWizard();
  toggleFeature("automation");
  check(!gSettings.automationEnabled && wizardFeatureEnabled("automation"));
  setWizardCurrentPage(WIZARD_PAGE_SYSTEM);
  setWizardTimezoneSelection(0); setWizardLogLevelSelection(0);
  SetupWizardResult result{};
  (void)wizardNextPage(result);
  noLiveEffects(original);
  discardSetupWizard(); noLiveEffects(original);
  result = completed();
  check(!wizardSaveAndApplySettings(result));
  check(!result.completed && liveValues() == original && changed.empty());
  operations.clear(); output.clear();
  gSettings.automationEnabled = true; gSettings.tzOffsetMinutes = 321;
  initSetupWizard();
  check(wizardFeatureEnabled("automation"));
  check(getWizardDraftValues().timezoneOffset == 321);
  discardSetupWizard();
  check(gSettings.automationEnabled && gSettings.tzOffsetMinutes == 321);
}

static void navigationAndModes() {
  reset(); const auto original = liveValues(); initSetupWizard();
  check(!wizardShouldShowWiFi() && !wizardShouldShowESPNow() && !wizardShouldShowMQTT());
#if ENABLE_WIFI
  wizardSetFeatureEnabled("wifi", true); check(wizardShouldShowWiFi());
#endif
#if ENABLE_ESPNOW
  wizardSetFeatureEnabled("espnow", true); check(wizardShouldShowESPNow());
#endif
#if ENABLE_MQTT
  wizardSetFeatureEnabled("mqtt", true); wizardSetMQTTAutoStart(true);
  check(wizardShouldShowMQTT());
  wizardSetMQTTAutoStart(false); check(!wizardShouldShowMQTT());
#endif
  uint32_t beforeCost, maxCost; int percentage;
  getHeapBarData(&beforeCost, &maxCost, &percentage);
#if ENABLE_HTTP_SERVER && ENABLE_HTTPS
  wizardSetFeatureEnabled("http", true);
  const auto* web = wizardModeMenuForFeature("http");
  check(web && wizardIsPageVisible(WIZARD_PAGE_WEBMODE));
  check(web->boolSetting != &gSettings.httpsEnabled);
  wizardModeApply(web, 1); check(wizardModeCurrentIndex(web) == 1);
  check(getWizardDraftValues().httpsEnabled && !gSettings.httpsEnabled);
  uint32_t afterCost; getHeapBarData(&afterCost, &maxCost, &percentage);
  check(afterCost == beforeCost + 30);
#else
  check(!wizardIsPageVisible(WIZARD_PAGE_WEBMODE));
#endif
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
  wizardSetFeatureEnabled("bluetooth", true);
  const auto* bluetooth = wizardModeMenuForFeature("bluetooth");
  check(bluetooth && wizardIsPageVisible(WIZARD_PAGE_BTMODE));
  wizardModeApply(bluetooth, 1); check(getWizardDraftValues().bleMode == 1 && gSettings.bleMode == 0);
#endif
  for (size_t i = 0; i < getWizardMenuItemCount(WIZARD_PAGE_SENSORS); ++i) {
    const auto* row = getWizardMenuItem(WIZARD_PAGE_SENSORS, static_cast<int>(i));
    if (std::strcmp(row->id, "tof")) continue;
    check(row->setting != &gSettings.tofAutoStart);
    setWizardCurrentPage(WIZARD_PAGE_SENSORS); setWizardCurrentSelection(static_cast<int>(i));
    check(wizardActivateCurrentItem() && !gSettings.tofAutoStart);
    check(wizardFeatureEnabled("tof"));
  }
  setWizardCurrentPage(WIZARD_PAGE_CONNECTIONS); setWizardCurrentSelection(-1);
  check(!wizardActivateCurrentItem());
  setWizardCurrentSelection(999); check(!wizardActivateCurrentItem());
  for (int page = WIZARD_PAGE_CONNECTIONS; page < WIZARD_PAGE_COUNT; ++page) {
    const auto next = wizardAdvanceFrom(static_cast<SetupWizardPage>(page));
    check(next == WIZARD_PAGE_COUNT || wizardIsPageVisible(next));
  }
  noLiveEffects(original); discardSetupWizard();
}

static void stagingAndPortValidation() {
  reset(); const auto original = liveValues(); initSetupWizard();
  SetupWizardResult patch;
  patch.mqttHost = "new.host"; patch.mqttPort = 65535;
  patch.mqttUser = "new-user"; patch.mqttPassword = "new-secret";
  check(wizardStageMQTTSettings(patch));
  check(getWizardDraftValues().mqttHost == "new.host" && getWizardDraftValues().mqttPort == 65535);
  patch = {}; check(wizardStageMQTTSettings(patch));
  check(getWizardDraftValues().mqttHost == "new.host" && getWizardDraftValues().mqttPassword == "new-secret");
  patch.espnowFriendlyName = "new-name"; patch.espnowRoom = "new-room"; patch.espnowStationary = false;
  check(wizardStageESPNowSettings(patch));
  check(getWizardDraftValues().deviceName == "new-name" && getWizardDraftValues().bleDeviceName == "new-name");
  check(getWizardDraftValues().espnowRoom == "new-room" && getWizardDraftValues().espnowZone == "old-zone");
  noLiveEffects(original);
  for (const char* invalid : {"0", "-1", "65536", "70000", "1883junk", "one", "1.5", "1 2", "99999999999999999999999"}) {
    int port = 99; check(!wizardParseMQTTPort(invalid, port)); check(port == 99);
  }
  int port = 99;
  check(wizardParseMQTTPort("", port) && port == 0);
  check(wizardParseMQTTPort(" 1883 ", port) && port == 1883);
  check(wizardParseMQTTPort("65535", port) && port == 65535);
  discardSetupWizard(); noLiveEffects(original);
}

// Text adapters edit the owner's buffer. Navigation must collect it without
// publishing live settings, and later visits must show the accepted draft.
static void editDeviceName(const char* name) {
  check(getWizardCurrentPage() == WIZARD_PAGE_SYSTEM);
  bool found = false;
  for (size_t index = 0; index < getWizardMenuItemCount(WIZARD_PAGE_SYSTEM); ++index) {
    const auto* row = getWizardMenuItem(WIZARD_PAGE_SYSTEM, static_cast<int>(index));
    if (std::strcmp(row->id, "device_name")) continue;
    check(row->type == WIZARD_ITEM_DEVICE_NAME);
    setWizardCurrentSelection(static_cast<int>(index));
    check(!wizardActivateCurrentItem()); // The ordinary action does not cycle text.
    check(std::strlen(name) <= 19);
    std::strcpy(getWizardDeviceNameBuf(), name);
    char label[32], value[64];
    check(wizardFormatMenuItem(WIZARD_PAGE_SYSTEM, static_cast<int>(index),
                              label, sizeof(label), value, sizeof(value)));
    check(std::strcmp(value, name) == 0);
    found = true;
    break;
  }
  check(found); // Present in both ESP-NOW compiled and disabled variants.
}

static void deviceNameBackAndApply() {
  reset(); const auto original = liveValues(); initSetupWizard();
  setWizardCurrentPage(WIZARD_PAGE_SYSTEM);
  editDeviceName("system-edited");
  check(wizardPrevPage());
  check(getWizardCurrentPage() == WIZARD_PAGE_SENSORS);
  check(getWizardDraftValues().deviceName == "system-edited" &&
        getWizardDraftValues().bleDeviceName == "system-edited");
  noLiveEffects(original);

  SetupWizardResult result;
  check(wizardNextPage(result));
  check(getWizardCurrentPage() == WIZARD_PAGE_SYSTEM && !result.completed);
  check(std::strcmp(getWizardDeviceName(), "system-edited") == 0);
  check(!wizardNextPage(result) && result.completed);
  noLiveEffects(original);
  check(wizardSaveAndApplySettings(result));
  check(gSettings.espnowDeviceName == "system-edited" && gSettings.bleDeviceName == "system-edited");
  check(gSettings.espnowFriendlyName == "old-friendly");
}

static void deviceNameRevertPreservesExternalChanges() {
  reset();
  gSettings.bleDeviceName = gSettings.espnowDeviceName;
  const auto original = liveValues(); initSetupWizard();
  setWizardCurrentPage(WIZARD_PAGE_SYSTEM);
  editDeviceName("temporary-name");
  check(wizardPrevPage());
  SetupWizardResult result;
  check(wizardNextPage(result));
  editDeviceName("old-device");
  check(wizardPrevPage());
  check(getWizardDraftValues().deviceName == "old-device" &&
        getWizardDraftValues().bleDeviceName == "old-device");
  noLiveEffects(original);

  // Reverting the accepted draft must remove this wizard's ownership of the
  // edit, allowing names subsequently changed by another surface to survive.
  gSettings.espnowDeviceName = "outside-device";
  gSettings.bleDeviceName = "outside-ble";
  check(wizardNextPage(result));
  check(!wizardNextPage(result) && result.completed);
  check(wizardSaveAndApplySettings(result));
  check(gSettings.espnowDeviceName == "outside-device" && gSettings.bleDeviceName == "outside-ble");
  check(changed.empty());
}

static void deviceNameESPNowRevisit() {
#if ENABLE_ESPNOW
  reset(); const auto original = liveValues(); initSetupWizard();
  wizardSetFeatureEnabled("espnow", true);
  setWizardCurrentPage(WIZARD_PAGE_ESPNOW);
  SetupWizardResult patch;
  patch.espnowFriendlyName = "radio-first"; patch.espnowStationary = true;
  check(wizardStageESPNowSettings(patch));
  check(std::strcmp(getWizardDeviceName(), "radio-first") == 0);
  SetupWizardResult result;
  for (int step = 0; getWizardCurrentPage() != WIZARD_PAGE_SYSTEM && step < WIZARD_PAGE_COUNT; ++step) {
    check(wizardNextPage(result) && !result.completed);
  }
  check(getWizardCurrentPage() == WIZARD_PAGE_SYSTEM);
  editDeviceName("system-last");
  for (int step = 0; getWizardCurrentPage() != WIZARD_PAGE_ESPNOW && step < WIZARD_PAGE_COUNT; ++step) {
    check(wizardPrevPage());
  }
  check(getWizardCurrentPage() == WIZARD_PAGE_ESPNOW);
  check(getWizardDraftValues().deviceName == "system-last");
  patch = {}; patch.espnowStationary = true;
  check(wizardStageESPNowSettings(patch)); // Empty revisited name keeps the System edit.
  check(getWizardDraftValues().deviceName == "system-last" &&
        getWizardDraftValues().bleDeviceName == "system-last");
  check(std::strcmp(getWizardDeviceName(), "system-last") == 0);

  patch.espnowFriendlyName = "radio-latest";
  check(wizardStageESPNowSettings(patch));
  check(std::strcmp(getWizardDeviceName(), "radio-latest") == 0);
  for (int step = 0; getWizardCurrentPage() != WIZARD_PAGE_SYSTEM && step < WIZARD_PAGE_COUNT; ++step) {
    check(wizardNextPage(result) && !result.completed);
  }
  check(getWizardCurrentPage() == WIZARD_PAGE_SYSTEM);
  check(std::strcmp(getWizardDeviceName(), "radio-latest") == 0);
  check(!wizardNextPage(result) && result.completed);
  noLiveEffects(original);
  check(wizardSaveAndApplySettings(result));
  check(gSettings.espnowDeviceName == "radio-latest" && gSettings.bleDeviceName == "radio-latest" &&
        gSettings.espnowFriendlyName == "radio-latest");
#endif
}

static void untouchedLongDeviceName() {
  reset();
  gSettings.espnowDeviceName = "device-name-longer-than-nineteen-bytes";
  gSettings.bleDeviceName = "separate-ble-name-longer-than-nineteen";
  const auto original = liveValues(); initSetupWizard();
  check(std::strlen(getWizardDeviceName()) == 19);
  check(std::string(getWizardDeviceName()) == gSettings.espnowDeviceName.substr(0, 19));
  check(getWizardDraftValues().deviceName == gSettings.espnowDeviceName);
  setWizardCurrentPage(WIZARD_PAGE_SYSTEM);
  check(wizardPrevPage());
  SetupWizardResult result;
  check(wizardNextPage(result));
  check(!wizardNextPage(result) && result.completed);
  noLiveEffects(original);
  check(wizardSaveAndApplySettings(result));
  check(liveValues() == original && changed.empty());
}

static void touchedOnlyApply() {
  reset(); initSetupWizard();
  toggleFeature("automation");
  SetupWizardResult patch; patch.mqttHost = "edited.host";
  check(wizardStageMQTTSettings(patch));
  // These settings change through another surface after the wizard starts.
  gSettings.unrelated = 777; gSettings.ntpServer = "external.ntp";
  gSettings.tzOffsetMinutes = 456; gSettings.mqttUser = "external-user";
  gSettings.tofAutoStart = true;
  auto result = completed();
  check(wizardSaveAndApplySettings(result));
  check(result.completed && gSettings.automationEnabled);
#if ENABLE_MQTT
  check(gSettings.mqttHost == "edited.host");
#endif
  check(gSettings.unrelated == 777 && gSettings.ntpServer == "external.ntp" &&
        gSettings.tzOffsetMinutes == 456 && gSettings.mqttUser == "external-user" && gSettings.tofAutoStart);
  check(scopeFinishes == 1 && applyCalls == 1);
  const auto finish = std::find(operations.begin(), operations.end(), "scope.finish");
  const auto apply = std::find(operations.begin(), operations.end(), "settings.apply");
  check(finish < apply);
  for (const auto& line : output) check(line.find("old-secret") == std::string::npos && line.find("new-secret") == std::string::npos);
  const auto state = liveValues(); const unsigned finishes = scopeFinishes;
  check(!wizardSaveAndApplySettings(result));
  check(scopeFinishes == finishes && liveValues() == state);
}

static void completionFailuresAndNoop() {
  reset(); failReserve = true; initSetupWizard(); failReserve = false;
  const auto initialFailureState = liveValues();
  auto unavailableDraft = completed();
  check(!wizardSaveAndApplySettings(unavailableDraft));
  check(liveValues() == initialFailureState && changed.empty() && applyCalls == 0);

  reset(); initSetupWizard(); toggleFeature("automation");
  auto cancelledResult = completed(); cancelledResult.wifiConfigured = true;
  cancelledResult.wifiPassword = "fixture-secret"; cancelRequested = true;
  check(!wizardSaveAndApplySettings(cancelledResult));
  check(!cancelledResult.completed && !cancelledResult.wifiConfigured && cancelledResult.wifiPassword.empty());
  check(!gSettings.automationEnabled && changed.empty() && applyCalls == 0 && wifiSaveCalls == 0);

  reset(); initSetupWizard(); const auto original = liveValues();
  auto result = completed(); check(wizardSaveAndApplySettings(result));
  check(liveValues() == original && changed.empty());
  reset(); initSetupWizard(); toggleFeature("automation");
  scopeAvailable = false; result = completed();
  check(!wizardSaveAndApplySettings(result)); check(!gSettings.automationEnabled && changed.empty());
  reset(); initSetupWizard(); toggleFeature("automation");
  scopeResult = false; result = completed();
  check(wizardSaveAndApplySettings(result));
  check(result.completed && gSettings.automationEnabled && applyCalls == 1 && scopeFinishes == 1);
  bool warning = false;
  for (const auto& line : output) if (line.find("could not be saved") != std::string::npos) warning = true;
  check(warning);
  reset(); initSetupWizard();
  SetupWizardResult patch; patch.mqttHost = "long changed broker value";
  check(wizardStageMQTTSettings(patch));
  failReserve = true; result = completed();
  check(!wizardSaveAndApplySettings(result));
  check(gSettings.mqttHost == "old.host" && changed.empty() && applyCalls == 0);
  failReserve = false;
}

static void completionEffects() {
  for (bool online : {false, true})
    for (auto outcome : {WifiNetworkSaveResult::Rejected, WifiNetworkSaveResult::PersistFailed,
                         WifiNetworkSaveResult::Accepted}) {
      reset(); initSetupWizard(); connected = online; wifiResult = outcome;
      setWizardLogLevelSelection(0); setWizardTimezoneSelection(0);
#if ENABLE_HTTP_SERVER && ENABLE_HTTPS
      wizardSetFeatureEnabled("http", true);
      wizardModeApply(wizardModeMenuForFeature("http"), 1);
#endif
      auto result = completed(); result.wifiConfigured = true;
      result.wifiSSID = "fixture-network"; result.wifiPassword = "fixture-secret";
      check(wizardSaveAndApplySettings(result));
      check(gSettings.logLevel == 0 && gSettings.tzOffsetMinutes == timezones[0].offsetMinutes);
      check(std::count(operations.begin(), operations.end(), "debug.change") == 1);
      check(applyCalls == 1 && scopeFinishes == 1);
      check(std::find(operations.begin(), operations.end(), "scope.finish") <
            std::find(operations.begin(), operations.end(), "settings.apply"));
#if ENABLE_WIFI
      check(wifiSaveCalls == 1);
      check(std::count(operations.begin(), operations.end(), "ntp.setup") == static_cast<int>(online));
      check(std::count(output.begin(), output.end(), "WiFi network: fixture-network") ==
            static_cast<int>(outcome == WifiNetworkSaveResult::Accepted));
#else
      check(wifiSaveCalls == 0 && std::count(operations.begin(), operations.end(), "ntp.setup") == 0);
#endif
#if ENABLE_HTTP_SERVER && ENABLE_HTTPS
      check(certCalls == 1 && gSettings.httpAutoStart && gSettings.httpsEnabled);
#else
      check(certCalls == 0);
#endif
      for (const auto& line : output) check(line.find("fixture-secret") == std::string::npos);
    }
  reset(); initSetupWizard(); toggleFeature("automation"); toggleFeature("automation");
  gSettings.automationEnabled = true;
  auto result = completed(); check(wizardSaveAndApplySettings(result));
  check(gSettings.automationEnabled && changed.empty()); // Reverted draft leaves the external edit intact.
}

int main() {
  cancellationAndRestart(); navigationAndModes(); stagingAndPortValidation();
  deviceNameBackAndApply(); deviceNameRevertPreservesExternalChanges();
  deviceNameESPNowRevisit(); untouchedLongDeviceName();
  touchedOnlyApply(); completionFailuresAndNoop(); completionEffects();
  std::printf("wizard draft (network=%d): %u checks passed\n", ENABLE_WIFI, checks);
}
