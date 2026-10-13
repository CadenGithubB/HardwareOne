// Host harness for test_settings_schema_setters.py. The driver splices in the
// production settings registry types (System_Settings.h), buildSettingsSchemaJson
// (System_Settings.cpp), the editor core's command predicate
// (System_SettingsEditorCore.cpp) and BondedPeer's remote-edit policy and schema
// filter (System_BondedPeer.cpp). The command registry and the module list are
// doubles.
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 0
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <ArduinoJson.h>

static int gFailures = 0;
static int gChecks = 0;
#define EXPECT(cond, msg)                                                          \
  do {                                                                             \
    ++gChecks;                                                                     \
    if (!(cond)) {                                                                 \
      std::fprintf(stderr, "FAIL %s  (%s, line %d)\n", std::string(msg).c_str(),  \
                   #cond, __LINE__);                                               \
      ++gFailures;                                                                 \
    }                                                                              \
  } while (0)

// ---- the bits of Arduino String the command lookup takes -------------------------
class String {
 public:
  String(const char* text) : value_(text ? text : "") {}
  const char* c_str() const { return value_.c_str(); }
  size_t length() const { return value_.size(); }

 private:
  std::string value_;
};

// ---- command registry double ------------------------------------------------------
struct CommandEntry { const char* name; };
static std::set<std::string> gRegistered;
static CommandEntry gFound = {"registered"};
const CommandEntry* findCommand(const String& line) {
  return gRegistered.count(line.c_str()) ? &gFound : nullptr;
}

// ---- production: registry types ----------------------------------------------------
// INSERT_SETTINGS_TYPES

// ---- module list double -------------------------------------------------------------
static std::vector<SettingEntry> gDisplayEntries;
static std::vector<SettingsModule> gModules;
static std::vector<const SettingsModule*> gModulePointers;
static bool displayConnected() { return true; }
const SettingsModule** getSettingsModules(size_t& count) {
  gModulePointers.clear();
  for (const SettingsModule& m : gModules) gModulePointers.push_back(&m);
  count = gModulePointers.size();
  return gModulePointers.data();
}

static SettingEntry row(const char* key, SettingType type, const char* cmdKey) {
  SettingEntry e{};
  e.jsonKey = key;
  e.type = type;
  e.label = key;
  e.cmdKey = cmdKey;
  return e;
}

// ---- production: schema builder, command predicate, remote-edit policy ------------
// INSERT_PRODUCTION

static JsonObject entryByKey(JsonArray entries, const char* key) {
  for (JsonObject e : entries) {
    const char* k = e["key"].as<const char*>();
    if (k && std::strcmp(k, key) == 0) return e;
  }
  return JsonObject();
}

static void case_schema_cmdkey_only_for_registered_setters() {
  gRegistered = {"oledbrightness", "oleddefaultmode"};
  gDisplayEntries = {
      row("oledBrightness", SETTING_INT, "oledbrightness"),   // setter compiled
      row("oledContrast", SETTING_INT, "oledcontrast"),       // setter compiled out
      row("oledDefaultMode", SETTING_STRING, "oleddefaultmode"),
      row("uptime", SETTING_INT, nullptr),                    // no setter at all
  };
  gDisplayEntries[0].group = "display";
  gDisplayEntries[0].minVal = 0;
  gDisplayEntries[0].maxVal = 255;
  gDisplayEntries[1].options = "0|Low,1|High";
  gDisplayEntries[3].readOnly = true;
  SettingsModule display{};
  display.name = "oled";
  display.jsonSection = "hardware.sensors.oled";
  display.entries = gDisplayEntries.data();
  display.count = gDisplayEntries.size();
  display.isConnected = displayConnected;
  display.description = "OLED";
  gModules = {display};

  JsonDocument doc;
  buildSettingsSchemaJson(doc);
  JsonArray entries = doc["modules"][0]["entries"].as<JsonArray>();
  EXPECT(entries.size() == 4, "every row is still listed");
  JsonObject brightness = entryByKey(entries, "oledBrightness");
  EXPECT(std::string(brightness["cmdKey"] | "") == "oledbrightness",
         "a row whose setter is registered carries its cmdKey");
  EXPECT(std::string(brightness["group"] | "") == "display" && brightness["min"] == 0 && brightness["max"] == 255,
         "the row's other fields are unchanged");
  JsonObject contrast = entryByKey(entries, "oledContrast");
  EXPECT(!contrast.isNull() && !contrast["cmdKey"].is<const char*>(),
         "a row whose setter is compiled out carries no cmdKey (clients show it read-only)");
  EXPECT(std::string(contrast["options"] | "") == "0|Low,1|High" && std::string(contrast["type"] | "") == "int",
         "the compiled-out row keeps its other fields");
  EXPECT(std::string(entryByKey(entries, "oledDefaultMode")["cmdKey"] | "") == "oleddefaultmode",
         "a string row with a registered setter keeps its cmdKey");
  JsonObject uptime = entryByKey(entries, "uptime");
  EXPECT(!uptime["cmdKey"].is<const char*>() && uptime["readOnly"] == true, "a row without a setter has none");
  EXPECT(doc["count"] == 1, "count is the module count");
}

// (maybe_unused: a tree without the schema filter compiles the case to one failure.)
[[maybe_unused]] static const char* kWorkerSchema = R"FIXTURE({"modules":[
 {"name":"users","section":"users","entries":[{"key":"x","cmdKey":"userx"}]},
 {"name":"oled","section":"hardware.sensors.oled","description":"OLED","connected":true,"entries":[
  {"key":"oledBrightness","type":"int","cmdKey":"oledbrightness","min":0,"max":255},
  {"label":"keyless row","type":"int"},
  {"key":"uptime","type":"int","readOnly":true}]},
 {"name":"bond","section":"bond","entries":[{"key":"bondModeEnabled","cmdKey":"bondmode"}]},
 {"name":"clock","section":"system.time","entries":[
  {"key":"ntpServer","type":"string","cmdKey":"ntpserver"},
  {"key":"ntpEnabled","type":"bool","cmdKey":"ntpenabled"},
  {"key":"tzOffsetMinutes","type":"int","cmdKey":"tzoffsetminutes"}]},
 {"name":"wifi","section":"network.wifi","entries":[{"key":"wifiEnabled","cmdKey":"wifienabled"}]},
 {"entries":[{"key":"orphan"}]},
 {"name":"debug","section":"system.debug","entries":[]}
],"count":7,"extra":"kept"})FIXTURE";

[[maybe_unused]] static std::vector<std::string> moduleNames(JsonArray modules) {
  std::vector<std::string> names;
  for (JsonObject m : modules) names.emplace_back(m["name"] | "(none)");
  return names;
}
[[maybe_unused]] static std::vector<std::string> entryKeys(JsonArray modules, const char* name) {
  std::vector<std::string> keys;
  for (JsonObject m : modules) {
    if (std::strcmp(m["name"] | "", name) != 0) continue;
    for (JsonObject e : m["entries"].as<JsonArray>()) keys.emplace_back(e["key"] | "(none)");
  }
  return keys;
}

static void case_schema_narrowed_to_remote_edit_policy() {
#ifdef HW1_HAS_SCHEMA_FILTER
  JsonDocument doc;
  EXPECT(!deserializeJson(doc, kWorkerSchema), "fixture parses");
  BondedPeer::filterSchemaForRemoteEdit(doc);
  JsonArray modules = doc["modules"].as<JsonArray>();
  EXPECT((moduleNames(modules) == std::vector<std::string>{"oled", "clock", "debug"}),
         "users, bond, wifi and a nameless module are removed; order is kept");
  EXPECT((entryKeys(modules, "oled") == std::vector<std::string>{"oledBrightness", "uptime"}),
         "an allowed module keeps its keyed rows (a keyless row is dropped)");
  EXPECT((entryKeys(modules, "clock") == std::vector<std::string>{"ntpServer", "tzOffsetMinutes"}),
         "clock keeps only ntpServer and tzOffsetMinutes");
  EXPECT(std::string(modules[0]["entries"][0]["cmdKey"] | "") == "oledbrightness" &&
             modules[0]["entries"][0]["max"] == 255 && modules[0]["connected"] == true,
         "what is kept is unchanged");
  EXPECT(doc["count"] == 3 && std::string(doc["extra"] | "") == "kept",
         "count follows the module list; other top-level fields stay");
  for (JsonObject m : modules) {
    const char* name = m["name"] | "";
    EXPECT(BondedPeer::remoteEditAllowed(name, nullptr), std::string("module allowed by the policy: ") + name);
    for (JsonObject e : m["entries"].as<JsonArray>()) {
      EXPECT(BondedPeer::remoteEditAllowed(name, e["key"] | ""),
             std::string("row allowed by the policy: ") + name + "." + (e["key"] | ""));
    }
  }
  JsonDocument empty;
  BondedPeer::filterSchemaForRemoteEdit(empty);
  EXPECT(empty["modules"].isNull(), "a document without modules is left alone");
#else
  EXPECT(false, "System_BondedPeer.cpp has no filterSchemaForRemoteEdit: the web bonded view "
                "applies its own copy of the remote-edit policy");
#endif
}

int main() {
  case_schema_cmdkey_only_for_registered_setters();
  case_schema_narrowed_to_remote_edit_policy();
  std::fprintf(stderr, "checks=%d\n", gChecks);
  std::printf("settings schema setters: %s (%d failures)\n", gFailures ? "FAILED" : "PASSED", gFailures);
  return gFailures ? 1 : 0;
}
