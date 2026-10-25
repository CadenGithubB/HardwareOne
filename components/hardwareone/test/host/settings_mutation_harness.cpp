// test_settings_mutation.py inserts shipping mutators, parsing, notification,
// feature commands and the persistence adapter. Only platform boundaries and
// registry fixtures are supplied here; the batch bookkeeping is also real.
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
#include <Arduino.h>
#include <ArduinoJson.h>
#include "System_SettingsWriteBatchCore.h"
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#define EXT_RAM_BSS_ATTR
#define PSRAM_JSON_DOC(name) JsonDocument name
#define PSRAM_STATIC_BUF(name, capacity) static char name[capacity]; constexpr size_t name##_SIZE = capacity
void webBatchTestStringAppend() {}
static bool failNextStringCopy, failStringReserve;
bool settingsTestStringCopyAllowed() {
  const bool allowed = !failNextStringCopy;
  failNextStringCopy = false;
  return allowed;
}
bool settingsTestStringReserveAllowed() { return !failStringReserve; }
bool gCLIValidateOnly = false;
// INSERT_TYPES

template<class T> struct Field {
  uint8_t before[sizeof(T)];
  T value{};
  uint8_t after[8];
  Field() {
    std::memset(before, 0x55, sizeof(before));
    std::memset(after, 0xaa, sizeof(after));
  }
  bool intact() const {
    for (auto byte : before) if (byte != 0x55) return false;
    for (auto byte : after) if (byte != 0xaa) return false;
    return true;
  }
};
static Field<int> signedField;
static Field<uint8_t> byteField;
static Field<uint16_t> shortField;
static Field<uint32_t> wideField;
static Field<float> floatField;
static bool boolField, featureField, disabledFeatureField;
static String stringField, secretField;
static const SettingEntry entries[] = {
  {"signed", SETTING_INT, &signedField.value, 0, 0, nullptr, 0, 0, "Signed", nullptr},
  {"byte", SETTING_U8, &byteField.value, 0, 0, nullptr, 0, 0, "Byte", nullptr},
  {"short", SETTING_U16, &shortField.value, 0, 0, nullptr, 0, 0, "Short", nullptr},
  {"wide", SETTING_U32, &wideField.value, 0, 0, nullptr, 0, 0, "Wide", nullptr},
  {"float", SETTING_FLOAT, &floatField.value, 0, 0, nullptr, 0, 0, "Float", nullptr},
  {"bool", SETTING_BOOL, &boolField, 0, 0, nullptr, 0, 1, "Boolean", nullptr, false, nullptr, "tofenabled"},
  {"string", SETTING_STRING, &stringField, 0, 0, "", 0, 0, "String", nullptr},
  {"secret", SETTING_STRING, &secretField, 0, 0, "", 0, 0, "Secret", nullptr, true},
  {"feature", SETTING_BOOL, &featureField, 0, 0, nullptr, 0, 1, "Feature setting", nullptr},
  {"unavailable", SETTING_BOOL, &disabledFeatureField, 0, 0, nullptr, 0, 1, "Unavailable", nullptr},
  {"signedAlias", SETTING_INT, &signedField.value, 0, 0, nullptr, 0, 0, "Later signed alias", nullptr},
};
static const SettingsModule settingsModule = {"fixture", "fixture", entries,
  sizeof(entries) / sizeof(entries[0]), nullptr, "host fixture"};
static const SettingsModule* gSettingsModules[] = {nullptr, &settingsModule};
static constexpr size_t gSettingsModuleCount = sizeof(gSettingsModules) / sizeof(gSettingsModules[0]);
static bool compiledOut() { return false; }
// An owner that keeps runtime state beside its permission flag (Automations).
#define ENABLE_AUTOMATION 1
static struct {
  bool automationEnabled = false;
  bool oledEnabled = false;  // `oledenabled` (dedicated command, OLED builds)
  String oledDefaultMode = "status";
} gSettings;
static std::vector<bool> ownerToggles;
static const char* ownerToggleError = nullptr;
[[maybe_unused]] static const char* automationSetSystemEnabled(bool enable) {
  ownerToggles.push_back(enable);
  if (ownerToggleError) return ownerToggleError;
  gSettings.automationEnabled = enable;  // the owner applies permission + runtime
  return nullptr;
}
static const FeatureEntry featureRegistry[] = {
  {"runtime", "Runtime", FEATURE_CAT_SENSOR, 8, FEATURE_FLAG_RUNTIME_TOGGLE,
   &featureField, nullptr, "runtime fixture"},
  {"reboot", "Reboot", FEATURE_CAT_SYSTEM, 3,
   FEATURE_FLAG_RUNTIME_TOGGLE | FEATURE_FLAG_REQUIRES_REBOOT,
   &featureField, nullptr, "reboot fixture"},
  {"unavailable", "Unavailable", FEATURE_CAT_SENSOR, 3, FEATURE_FLAG_RUNTIME_TOGGLE,
   &disabledFeatureField, compiledOut, "compiled-out fixture"},
  {"fixed", "Fixed", FEATURE_CAT_CORE, 1, FEATURE_FLAG_COMPILE_TIME,
   nullptr, nullptr, "compile-time fixture"},
  {"automation", "Automations", FEATURE_CAT_SYSTEM, 8, FEATURE_FLAG_RUNTIME_TOGGLE,
   &gSettings.automationEnabled, nullptr, "owner-applied fixture"},
};
static constexpr size_t featureRegistryCount = sizeof(featureRegistry) / sizeof(featureRegistry[0]);
static constexpr size_t kFeaturesJsonBuf = 4096;
static uint32_t hw1InternalFreeBytes() { return 65536; }
static bool argWantsJson(const String& arg) { return arg == "json"; }
static void emitListingTrailer(const char*, const char*) {}

enum { SYSEVT_SETTING_CHANGED = 1, SYSEVT_FEATURE_TOGGLED = 2 };
struct Event { int kind; std::string subject, value; };
static std::vector<Event> events;
static std::vector<std::string> broadcasts, stopRequests;
static std::vector<std::string> effectOrder;
static std::vector<bool> persistedBoolValues;
static unsigned oledDirtyCount, writeCalls;
static bool writerSucceeds = true, writeGuardAvailable = true;
static uint8_t retainedRetryMask;
static unsigned criticalDepth;
static void systemEventPost(int kind, const char* subject, const char* value) {
  events.push_back({kind, subject ? subject : "", value ? value : ""});
  effectOrder.emplace_back(kind == SYSEVT_SETTING_CHANGED ? "setting event" : "feature event");
}
void oledMarkDirty() { ++oledDirtyCount; }
static void broadcastPrintf(const char* format, ...) {
  char buffer[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  broadcasts.emplace_back(buffer);
}
#define BROADCAST_PRINTF(...) broadcastPrintf(__VA_ARGS__)
static const char* executeCommandThroughRegistry(const char* command) {
  stopRequests.emplace_back(command);
  effectOrder.emplace_back("stop");
  return "SUCCESS";
}
using SettingsBatchCore = hw1_settings_batch::Core<16>;
using SettingsBatchOwner = hw1_settings_batch::Owner;
static SettingsBatchCore sSettingsWriteBatches;
static SettingsBatchOwner currentOwner{};
static int sSettingsWriteBatchMux;
static uint32_t millis() { return 1234; }
static void portENTER_CRITICAL(int*) { ++criticalDepth; }
static void portEXIT_CRITICAL(int*) { --criticalDepth; }
static bool currentSettingsBatchOwner(SettingsBatchOwner& out) {
  out = currentOwner;
  return currentOwner != SettingsBatchOwner{};
}
class SettingsFileWriteGuard {
 public:
  explicit operator bool() const { return writeGuardAvailable; }
};
static void retainSettingsWriteRetry(uint8_t mask) { retainedRetryMask |= mask; }
static bool writeSettingsJson() {
  if (criticalDepth) std::abort();
  ++writeCalls;
  effectOrder.emplace_back("persist");
  persistedBoolValues.push_back(boolField);
  return writerSucceeds;
}
// INSERT_PRODUCTION

#if ENABLE_OLED_DISPLAY
// Display boundaries of the dedicated `oledenabled` command (OLED_Utils.cpp).
// The enum has a fixed underlying type because the command compares
// modeFromSlug()'s (OLEDMode)-1 "unknown slug" result.
enum OLEDMode : int { OLED_OFF, OLED_SYSTEM_STATUS, OLED_MENU };
static OLEDMode currentOLEDMode = OLED_OFF;
static bool oledConnected, gOledRunning, oledInitSucceeds;
static char debugBuffer[1024];
static bool ensureDebugBuffer() { return true; }
static char* getDebugBuffer() { return debugBuffer; }
static bool initOLEDDisplay() {
  effectOrder.emplace_back("display init");
  return oledInitSucceeds;
}
static void stopOLEDDisplay() { effectOrder.emplace_back("display stop"); }
static void updateOLEDDisplay() { effectOrder.emplace_back("display update"); }
static void broadcastOutput(const char* text) { effectOrder.emplace_back(std::string("output: ") + text); }
static OLEDMode modeFromSlug(const String& slug) {
  return slug == "status" ? OLED_SYSTEM_STATUS : slug == "menu" ? OLED_MENU : (OLEDMode)-1;
}
static void requestOLEDMode(OLEDMode mode, const char*, bool) {
  currentOLEDMode = mode;
  effectOrder.emplace_back("display mode");
}
static void debugOLEDModeChange(const char*, OLEDMode, OLEDMode, const String&) {}
// INSERT_OLED_COMMAND
#endif

static unsigned checks, failures;
static std::string context;
static void check(bool good, const char* message) {
  ++checks;
  if (!good) {
    ++failures;
    std::fprintf(stderr, "%s: %s\n", context.c_str(), message);
  }
}
static void clearEffects() {
  events.clear(); broadcasts.clear(); stopRequests.clear();
  effectOrder.clear(); persistedBoolValues.clear();
  oledDirtyCount = writeCalls = retainedRetryMask = 0;
}
static void reset() {
  signedField = {}; byteField = {}; shortField = {}; wideField = {}; floatField = {};
  boolField = featureField = disabledFeatureField = false;
  stringField = ""; secretField = "";
  gCLIValidateOnly = false;
  failNextStringCopy = failStringReserve = false;
  writerSucceeds = writeGuardAvailable = true;
  currentOwner = {};
  sSettingsWriteBatches = {};
  clearEffects();
}
static unsigned eventCount(int kind) {
  unsigned count = 0;
  for (const auto& event : events) if (event.kind == kind) ++count;
  return count;
}
static void noMutationEffects() {
  check(writeCalls == 0 && events.empty() && oledDirtyCount == 0,
        "unexpected persistence/event/repaint");
}
static void changedOnce() {
  check(writeCalls == 1, "changed value did not request one write");
  check(eventCount(SYSEVT_SETTING_CHANGED) == 1, "setting event missing/duplicated");
  check(oledDirtyCount == (ENABLE_OLED_DISPLAY ? 1u : 0u), "repaint missing/duplicated");
}
static std::string command(size_t index, const char* value) {
  return handleSettingCommand(&entries[index], value);
}
static bool error(const std::string& result) { return result.rfind("Error:", 0) == 0; }

static void numericValues() {
  struct Example { size_t index; const char* input; const char* output; };
  for (const auto& example : std::vector<Example>{
      {0, "-2147483648", "signed = -2147483648"},
      {0, "+2147483647", "signed = 2147483647"},
      {1, "255", "byte = 255"}, {2, "65535", "short = 65535"},
      {3, "4294967295", "wide = 4294967295"},
      {4, "1.25e1", "float = 12.500"}}) {
    reset(); context = std::string("valid numeric ") + example.input;
    check(!error(command(example.index, example.input)), "valid numeric rejected");
    changedOnce();
    check(signedField.intact() && byteField.intact() && shortField.intact() &&
          wideField.intact() && floatField.intact(), "typed write damaged adjacent field");
    clearEffects();
    check(command(example.index, "") == example.output, "query value incorrect");
    noMutationEffects();
    check(!error(command(example.index, example.input)), "unchanged valid value rejected");
    noMutationEffects();
  }
  for (size_t index : {0u, 1u, 2u, 3u}) {
    for (const char* input : {"garbage", "42junk", "1 2", "0x10", "1.5", "+", "--1", "99999999999999999999999999999999999"}) {
      reset(); context = std::string("invalid integer ") + input;
      check(error(command(index, input)), "malformed integer accepted");
      noMutationEffects();
    }
  }
  for (const auto& example : std::vector<Example>{{0, "2147483648", ""},
      {0, "-2147483649", ""}, {1, "256", ""}, {1, "-1", ""},
      {2, "65536", ""}, {2, "-1", ""}, {3, "4294967296", ""}, {3, "-1", ""}}) {
    reset(); context = std::string("width overflow ") + example.input;
    check(error(command(example.index, example.input)), "overflow/wraparound accepted");
    noMutationEffects();
  }
  for (const char* input : {"garbage", "nan", "NAN", "inf", "-inf", "1e999", "1e-999", "1.5junk", "1 2"}) {
    reset(); context = std::string("invalid float ") + input;
    check(error(command(4, input)), "malformed/nonfinite float accepted");
    noMutationEffects();
  }
  for (size_t index : {0u, 4u}) {
    reset(); context = "numeric embedded NUL";
    const String input(std::string("12\0hidden", 9));
    check(error(handleSettingCommand(&entries[index], input)), "embedded NUL bypassed complete-token check");
    noMutationEffects();
  }
  for (size_t index : {0u, 1u, 2u, 3u, 4u}) {
    reset(); context = "declared range";
    SettingEntry bounded = entries[index]; bounded.minVal = 2; bounded.maxVal = 10;
    for (const char* input : {"1", "11"}) {
      check(error(handleSettingCommand(&bounded, input)), "declared range ignored");
      noMutationEffects();
    }
    check(!error(handleSettingCommand(&bounded, " 5 ")), "trimmed bounded value rejected");
    changedOnce();
  }
}

static void booleanAndStopPolicy() {
  for (const char* input : {"1", "true", "TRUE", "on", "enable", " On "}) {
    reset(); context = std::string("true alias ") + input;
    check(!error(command(5, input)) && boolField, "true alias rejected");
    changedOnce(); check(stopRequests.empty(), "enable unexpectedly stopped a service");
  }
  for (const char* input : {"0", "false", "FALSE", "off", "disable", " OFF "}) {
    reset(); boolField = true; context = std::string("false alias ") + input;
    check(!error(command(5, input)) && !boolField, "false alias rejected");
    changedOnce(); check(stopRequests == std::vector<std::string>{"closetof"}, "disable missed owner stop");
    check(persistedBoolValues == std::vector<bool>{false}, "disable persisted before publishing false");
    check(effectOrder == std::vector<std::string>{"persist", "stop", "setting event"},
          "disable lost value/persistence/stop/event ordering");
    clearEffects();
    check(!error(command(5, input)), "repeated disable rejected");
    noMutationEffects();
    check(stopRequests == std::vector<std::string>{"closetof"}, "repeated disable lost reconciliation");
  }
  for (const char* input : {"trueish", "10", "false extra", "-1", "garbage"}) {
    reset(); boolField = true; context = std::string("invalid boolean ") + input;
    check(error(command(5, input)) && boolField, "invalid bool changed state");
    noMutationEffects(); check(stopRequests.empty(), "invalid boolean stopped service");
  }
  reset(); context = "BLE dual teardown";
  SettingEntry ble = entries[5]; ble.cmdKey = "bleenabled";
  check(!error(handleSettingCommand(&ble, "off")), "BLE disable failed");
  // One stop verb: closeble closes every device, the console server and the shared stack.
  check(stopRequests == std::vector<std::string>{"closeble"}, "BLE teardown order changed");
  clearEffects(); ble.cmdKey = "bleautostart";
  check(!error(handleSettingCommand(&ble, "off")) && stopRequests.empty(), "autostart invoked runtime stop");
}

static void stringsPrivacyAndPreflight() {
  for (size_t index : {6u, 7u}) {
    reset(); context = index == 7 ? "secret" : "string";
    check(!error(command(index, "  local fixture value  ")), "string update rejected");
    changedOnce();
    check((index == 7 ? secretField : stringField) == "local fixture value", "string trimming changed");
    if (index == 7) {
      for (const auto& output : broadcasts) check(output.find("local fixture value") == std::string::npos, "broadcast leaked secret");
      check(events.size() == 1 && events[0].value == "********", "setting event leaked secret");
    }
    clearEffects();
    const auto query = command(index, "");
    check(query == (index == 7 ? "secret = ********" : "string = local fixture value"), "query privacy/output changed");
    noMutationEffects();
    check(!error(command(index, "local fixture value")), "unchanged string rejected");
    noMutationEffects();
  }
  reset(); context = "String overloads";
  setSetting(stringField, String("copy")); changedOnce(); clearEffects();
  setSetting(stringField, "copy"); noMutationEffects();
  setSetting(stringField, "literal"); changedOnce();
  reset(); context = "explicit descriptor alias";
  SettingEntry alias = entries[0]; alias.jsonKey = "alternate"; alias.label = "Alternate label";
  check(!error(handleSettingCommand(&alias, "5")), "alias update failed");
  check(events.size() == 1 && events[0].subject == "Alternate label" && events[0].value == "5",
        "descriptor alias notification lost its label or duplicated");
  reset(); context = "typed setter first registered descriptor";
  setSetting(signedField.value, 5);
  changedOnce();
  check(events.size() == 1 && events[0].subject == "Signed" && events[0].value == "5",
        "typed setter did not use the first registered alias exactly once");
  reset(); context = "long string event";
  const std::string longValue(79, 'x');
  check(!error(command(6, longValue.c_str())), "long string rejected");
  check(events.size() == 1 && events[0].value == longValue, "event was newly truncated below its payload capacity");
  reset(); context = "string allocation failures";
  stringField = "preserved value";
  failNextStringCopy = true;
  check(error(command(6, "replacement")) && stringField == "preserved value", "failed argument copy changed setting");
  noMutationEffects();
  failStringReserve = true;
  check(error(command(6, "replacement needing capacity")) && stringField == "preserved value", "failed reserve changed setting");
  noMutationEffects();
  for (size_t index = 0; index < sizeof(entries) / sizeof(entries[0]); ++index) {
    reset(); context = "read-only and validation";
    SettingEntry readOnly = entries[index]; readOnly.readOnly = true;
    check(!error(handleSettingCommand(&readOnly, "")), "read-only query rejected");
    check(error(handleSettingCommand(&readOnly, "1")), "read-only mutation accepted");
    noMutationEffects();
    gCLIValidateOnly = true;
    check(command(index, "invalid") == "VALID", "validation-only contract changed");
    noMutationEffects(); check(stopRequests.empty(), "validation-only invoked service stop");
  }
  reset(); context = "unregistered direct setting";
  int runtime = 0; setSetting(runtime, 1);
  check(writeCalls == 1 && events.empty() && oledDirtyCount == 0, "unregistered setting fabricated event");
  reset(); context = "invalid descriptor";
  check(error(handleSettingCommand(nullptr, "1")), "null descriptor accepted");
  SettingEntry invalid = entries[0]; invalid.valuePtr = nullptr;
  check(error(handleSettingCommand(&invalid, "1")), "null storage accepted");
  noMutationEffects();
}

static void featureCommands() {
  for (const char* value : {"on", "true", "1", "enable", "off", "false", "0", "disable"}) {
    reset(); context = std::string("feature ") + value;
    const bool expected = value[0] == '1' || value[0] == 't' || value[0] == 'e' || std::strcmp(value, "on") == 0;
    featureField = !expected;
    const std::string input = std::string("runtime ") + value;
    check(!error(cmd_features(input.c_str())) && featureField == expected, "feature toggle failed");
    changedOnce();
    check(eventCount(SYSEVT_FEATURE_TOGGLED) == 1, "feature-specific event missing/duplicated");
    check(stopRequests.empty(), "feature policy changed to runtime teardown");
    clearEffects();
    check(std::string(cmd_features(input.c_str())).find("already") != std::string::npos, "feature no-op reply changed");
    noMutationEffects();
  }
  for (const char* input : {"unavailable on", "fixed on", "runtime nonsense", "runtime on extra", "runtime \"on"}) {
    reset(); context = input;
    check(error(cmd_features(input)), "invalid/unavailable toggle accepted");
    noMutationEffects();
  }
  reset(); context = "feature queries and validation";
  for (const char* input : {"", "runtime", "unavailable", "json"}) {
    check(!error(cmd_features(input)), "feature query failed"); noMutationEffects();
  }
  check(std::string(cmd_features("reboot on")).find("reboot required") != std::string::npos, "reboot note lost");
  clearEffects(); gCLIValidateOnly = true;
  check(std::string(cmd_features("runtime off")) == "VALID" && featureField, "feature validation changed setting");
  noMutationEffects();

  // A feature whose owner keeps runtime state is toggled through that owner
  // (same transition as `automation system enable|disable`), never written raw.
  reset(); context = "owner-applied feature";
  gSettings.automationEnabled = false; ownerToggles.clear(); ownerToggleError = nullptr;
  check(!error(cmd_features("automation on")) && gSettings.automationEnabled, "owner toggle not applied");
  check((ownerToggles == std::vector<bool>{true}), "owner transition not used");
  check(eventCount(SYSEVT_FEATURE_TOGGLED) == 1 && eventCount(SYSEVT_SETTING_CHANGED) == 0 && writeCalls == 0,
        "registry wrote the owner's flag itself or lost the feature event");
  clearEffects(); ownerToggleError = "Error: owner refused";
  check(std::string(cmd_features("automation off")) == "Error: owner refused" && gSettings.automationEnabled,
        "owner refusal not returned");
  check(eventCount(SYSEVT_FEATURE_TOGGLED) == 0, "refused toggle posted an event");
  ownerToggleError = nullptr;
  check(!error(cmd_features("automation off")) && !gSettings.automationEnabled && ownerToggles.size() == 3,
        "owner disable not applied");
}

static void persistenceBoundaries() {
  reset(); context = "owner-scoped persistence";
  const SettingsBatchOwner owner{1, 101}, other{1, 102};
  currentOwner = owner;
  check(sSettingsWriteBatches.begin(owner, hw1_settings_batch::BeginMode::Idempotent, millis()), "batch begin failed");
  check(!error(command(0, "0")), "unchanged batch command rejected");
  check(sSettingsWriteBatches.finish(owner).dirtyMask == 0, "no-op dirtied owner batch");
  check(sSettingsWriteBatches.begin(owner, hw1_settings_batch::BeginMode::Idempotent, millis()), "batch restart failed");
  check(!error(command(0, "9")), "owned mutation failed");
  check(!error(cmd_features("runtime on")), "owned feature mutation failed");
  check(writeCalls == 0 && eventCount(SYSEVT_SETTING_CHANGED) == 2, "owned writes did not coalesce or events lost");
  currentOwner = other;
  check(!error(command(1, "9")) && writeCalls == 1, "other owner's change inherited batch");
  currentOwner = {};
  check(!error(command(2, "9")) && writeCalls == 2, "background change inherited batch");
  check(sSettingsWriteBatches.finish(owner).dirtyMask == hw1_settings_batch::MAIN, "batch lost main dirty intent");
  for (bool guardAvailable : {false, true}) {
    reset(); context = guardAvailable ? "writer failure" : "writer guard failure";
    writerSucceeds = false; writeGuardAvailable = guardAvailable;
    check(!error(command(0, "7")) && signedField.value == 7, "failure changed established RAM-update contract");
    check(writeCalls == (guardAvailable ? 1u : 0u), "failure write count wrong");
    check(retainedRetryMask == hw1_settings_batch::MAIN, "failed durability intent was lost");
    check(eventCount(SYSEVT_SETTING_CHANGED) == 1, "live update event lost on persistence failure");
  }
}

#if ENABLE_OLED_DISPLAY
// `oledenabled` keeps its own live actions (start, default mode, full teardown)
// but reads the same tokens as the generic boolean path and refuses anything
// else before it persists or touches the panel.
static void prepareOled(const char* input, bool enabled, bool connected, bool running) {
  reset();
  gSettings.oledEnabled = enabled;
  oledConnected = connected; gOledRunning = running; oledInitSucceeds = true;
  currentOLEDMode = OLED_OFF;
  context = std::string("oledenabled '") + input + "' from " + (enabled ? "enabled" : "disabled");
}

static void oledEnabledCommand() {
  using Effects = std::vector<std::string>;
  const std::string usage = "Error: invalid arguments — Usage: oledenabled <0|1>";
  const std::string enabledReply = "OLED display enabled (mode: status)";
  for (const char* input : {"1", "true", "TRUE", "on", "enable", " On "}) {
    prepareOled(input, true, true, true);
    check(cmd_oled_enabled(input) == enabledReply, "true alias did not report the display enabled");
    check(gSettings.oledEnabled && gOledRunning && writeCalls == 0, "true alias disabled or persisted the display");
    check(effectOrder == Effects{"display mode", "display update"}, "true alias stopped the panel");
    prepareOled(input, false, false, false);
    check(cmd_oled_enabled(input) == enabledReply && gSettings.oledEnabled, "true alias did not enable");
    check(effectOrder == Effects{"persist", "display init", "output: OLED display started",
                                 "display mode", "display update"},
          "enable lost persist/start/default-mode order");
  }
  for (const char* input : {"0", "false", "FALSE", "off", "disable", " OFF "}) {
    prepareOled(input, true, true, true);
    check(cmd_oled_enabled(input) == std::string("OLED display disabled"), "false alias reply changed");
    check(!gSettings.oledEnabled && !gOledRunning, "false alias left the display enabled");
    check(effectOrder == Effects{"persist", "display stop"}, "disable lost persist/full teardown");
  }
  for (const char* input : {"foo", "10", "truex", "1x", "onn", "yes", "-1", "on extra", "", "   "}) {
    for (bool enabled : {true, false}) {
      prepareOled(input, enabled, true, enabled);
      check(cmd_oled_enabled(input) == usage, "unknown or missing token was not refused");
      check(gSettings.oledEnabled == enabled && gOledRunning == enabled,
            "unknown token changed the display state");
      check(effectOrder.empty() && writeCalls == 0, "unknown token persisted or touched the panel");
    }
  }
  prepareOled("1", false, false, false); oledInitSucceeds = false;
  check(cmd_oled_enabled("1") == std::string("ERROR"), "init failure reply changed");
  check(effectOrder == Effects{"persist", "display init",
                               "output: Failed to initialize OLED display. Check wiring."},
        "init failure lost persist/report order");
  prepareOled("1", false, true, false);
  check(cmd_oled_enabled("1") == enabledReply && gOledRunning && currentOLEDMode == OLED_SYSTEM_STATUS,
        "connected panel did not resume in the default mode");
  check(effectOrder == Effects{"persist", "display mode", "display update"}, "resume effects changed");
  for (const char* input : {"on", "foo"}) {
    prepareOled(input, true, true, true); gCLIValidateOnly = true;
    check(cmd_oled_enabled(input) == std::string("VALID") && effectOrder.empty() && writeCalls == 0,
          "validation-only contract changed");
  }
}
#endif

int main() {
  numericValues(); booleanAndStopPolicy(); stringsPrivacyAndPreflight();
  featureCommands(); persistenceBoundaries();
#if ENABLE_OLED_DISPLAY
  oledEnabledCommand();
#endif
  std::printf("settings mutation (oled=%d): %u checks, %u failures\n", ENABLE_OLED_DISPLAY, checks, failures);
  return failures ? 1 : 0;
}
