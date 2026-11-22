// test_settings_mutation.py inserts shipping mutators, parsing, notification
// and the persistence adapter. (`features` only lists since 2026-11-20; its
// read-only contract is test_features_list_only.py.) Only platform boundaries and
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
bool gCliValidateOnly = false;
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
  {"bool", SETTING_BOOL, &boolField, 0, 0, nullptr, 0, 1, "Boolean", nullptr, false, nullptr, "tofautostart"},
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

enum { SYSEVT_SETTING_CHANGED = 1 };
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
  effectOrder.emplace_back(kind == SYSEVT_SETTING_CHANGED ? "setting event" : "other event");
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
// No setting write runs a service command (the <x>enabled switches and their
// stop table were removed 2026-11-21); a call would show up in stopRequests.
[[maybe_unused]] static const char* executeCommandThroughRegistry(const char* command) {
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
  gCliValidateOnly = false;
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

static void booleanPolicy() {
  for (const char* input : {"1", "true", "TRUE", "on", "enable", " On "}) {
    reset(); context = std::string("true alias ") + input;
    check(!error(command(5, input)) && boolField, "true alias rejected");
    changedOnce(); check(stopRequests.empty(), "enable ran a service command");
  }
  // Off is a plain save: value, persistence request, event. A saved switch
  // only says what starts at boot; it never stops a running service.
  for (const char* input : {"0", "false", "FALSE", "off", "disable", " OFF "}) {
    reset(); boolField = true; context = std::string("false alias ") + input;
    check(!error(command(5, input)) && !boolField, "false alias rejected");
    changedOnce(); check(stopRequests.empty(), "disable ran a service command");
    check(persistedBoolValues == std::vector<bool>{false}, "disable persisted before publishing false");
    check(effectOrder == std::vector<std::string>{"persist", "setting event"},
          "disable lost value/persistence/event ordering");
    clearEffects();
    check(!error(command(5, input)), "repeated disable rejected");
    noMutationEffects();
    check(stopRequests.empty(), "repeated disable ran a service command");
  }
  for (const char* input : {"trueish", "10", "false extra", "-1", "garbage"}) {
    reset(); boolField = true; context = std::string("invalid boolean ") + input;
    check(error(command(5, input)) && boolField, "invalid bool changed state");
    noMutationEffects(); check(stopRequests.empty(), "invalid boolean ran a service command");
  }
  // The removed switch names, were a row to carry one, stop nothing either.
  for (const char* key : {"bleenabled", "wifienabled", "bleautostart"}) {
    reset(); boolField = true; context = std::string("no stop table: ") + key;
    SettingEntry row = entries[5]; row.cmdKey = key;
    check(!error(handleSettingCommand(&row, "off")) && stopRequests.empty(), "a setting write ran a stop");
  }
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
  // SET-6: the subject is the row's key (cmdKey, else jsonKey), not its label.
  check(events.size() == 1 && events[0].subject == "alternate" && events[0].value == "5",
        "descriptor alias notification lost its key or duplicated");
  reset(); context = "typed setter first registered descriptor";
  setSetting(signedField.value, 5);
  changedOnce();
  check(events.size() == 1 && events[0].subject == "signed" && events[0].value == "5",
        "typed setter did not use the first registered alias exactly once");
  reset(); context = "subject is the command name";
  setSetting(boolField, !boolField);
  changedOnce();
  check(events.size() == 1 && events[0].subject == "tofautostart",
        "a row with a command name did not post it as the subject");
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
    gCliValidateOnly = true;
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

static void persistenceBoundaries() {
  reset(); context = "owner-scoped persistence";
  const SettingsBatchOwner owner{1, 101}, other{1, 102};
  currentOwner = owner;
  check(sSettingsWriteBatches.begin(owner, hw1_settings_batch::BeginMode::Idempotent, millis()), "batch begin failed");
  check(!error(command(0, "0")), "unchanged batch command rejected");
  check(sSettingsWriteBatches.finish(owner).dirtyMask == 0, "no-op dirtied owner batch");
  check(sSettingsWriteBatches.begin(owner, hw1_settings_batch::BeginMode::Idempotent, millis()), "batch restart failed");
  check(!error(command(0, "9")), "owned mutation failed");
  check(!error(command(5, "on")), "second owned mutation failed");
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


// A select-style number is named in the reply and the event; a value its list
// does not name, a bitmask list and a setting without a list keep the number.
static uint8_t levelField;
static void optionNames() {
  const SettingEntry level = {"level", SETTING_U8, &levelField, 1, 0, nullptr, 0, 3, "Level",
                              "0:Off,1:Alert,2:Standard,3:Verbose"};
  const SettingEntry piped = {"level", SETTING_U8, &levelField, 1, 0, nullptr, 0, 9, "Level",
                              "0|Off,1|Alert,10|Ten"};
  const SettingEntry mask = {"level", SETTING_U8, &levelField, 1, 0, nullptr, 0, 9, "Level",
                             "bitmask:1|Thermal,2|ToF"};
  struct Example { const SettingEntry* entry; const char* input; const char* reply; const char* event; };
  for (const auto& example : std::vector<Example>{
      {&level, "3", "level set to Verbose", "Verbose"},
      {&level, "0", "level set to Off", "Off"},
      {&piped, "1", "level set to Alert", "Alert"},
      {&piped, "9", "level set to 9", "9"},
      {&mask, "2", "level set to 2", "2"}}) {
    reset(); levelField = 5; context = std::string("option name ") + example.input;
    check(!error(handleSettingCommand(example.entry, example.input)), "option value rejected");
    check(broadcasts.size() == 1 && broadcasts[0] == example.reply, "reply does not name the option");
    check(eventCount(SYSEVT_SETTING_CHANGED) == 1 && events[0].value == example.event,
          "event does not name the option");
  }
  char name[8];
  check(!settingOptionLabel(&entries[1], 1, name, sizeof(name)), "setting without a list named a value");
  check(settingOptionLabel(&level, 2, name, 4) && std::string(name) == "Sta", "long name not cut to the buffer");
}

int main() {
  numericValues(); booleanPolicy(); stringsPrivacyAndPreflight();
  persistenceBoundaries(); optionNames();
  std::printf("settings mutation (oled=%d): %u checks, %u failures\n", ENABLE_OLED_DISPLAY, checks, failures);
  return failures ? 1 : 0;
}
