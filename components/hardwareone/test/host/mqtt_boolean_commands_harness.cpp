#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

// Arduino String storage boundary; parsing remains the real production helper.
class String : public std::string {
 public:
  using std::string::string;
  String(const std::string& value) : std::string(value) {}
  void trim() {
    const auto nonspace = [](unsigned char c) { return !std::isspace(c); };
    const auto first = std::find_if(begin(), end(), nonspace);
    const auto last = std::find_if(rbegin(), rend(), nonspace).base();
    *this = first < last ? std::string(first, last) : "";
  }
  void toLowerCase() {
    std::transform(begin(), end(), begin(), [](unsigned char c) { return std::tolower(c); });
  }
  bool equalsIgnoreCase(const char* other) const {
    String left = *this, right = other;
    left.toLowerCase(); right.toLowerCase();
    return left == right;
  }
};

struct SettingsBoundary {
  bool mqttEnabled, mqttAutoStart, mqttSubscribeExternal;
  bool mqttPublishWiFi, mqttPublishSystem, mqttPublishThermal, mqttPublishToF;
  bool mqttPublishIMU, mqttPublishPresence, mqttPublishGPS, mqttPublishAPDS;
  bool mqttPublishRTC, mqttPublishInput;
} gSettings;

bool gCLIValidateOnly;
static bool stopResult, persistResult;
static int stopCalls, persistCalls, notifyCalls;
static const void* notifiedField;
static std::vector<std::string> events;

static bool requestSettingsPersist() {
  ++persistCalls; events.push_back("persist"); return persistResult;
}
static void notifySettingChanged(const void* field) {
  ++notifyCalls; notifiedField = field; events.push_back("notify");
}
static bool stopMQTT() {
  ++stopCalls; events.push_back("stop"); return stopResult;
}

// INSERT_PRODUCTION_DEFINITIONS

struct Command {
  const char* name;
  const char* (*handler)(const String&);
  bool SettingsBoundary::*field;
  const char* queryOn;
  const char* queryOff;
  const char* setOn;
  const char* setOff;
};

#define PUBLISH_CASE(name, field, label) \
  {"publish " label, cmd_mqttpublish##name, &SettingsBoundary::field, \
   "MQTT publish " label ": ON", "MQTT publish " label ": OFF", \
   "MQTT publish " label " enabled", "MQTT publish " label " disabled"}

static const Command commands[] = {
  {"clientenabled", cmd_mqttclientenabled, &SettingsBoundary::mqttEnabled,
   "MQTT client: enabled", "MQTT client: disabled", "MQTT client enabled", "MQTT client disabled"},
  {"autostart", cmd_mqttautostart, &SettingsBoundary::mqttAutoStart,
   "MQTT auto-start: ON", "MQTT auto-start: OFF", "MQTT auto-start enabled", "MQTT auto-start disabled"},
  {"subscribe", cmd_mqttsubscribe, &SettingsBoundary::mqttSubscribeExternal,
   "MQTT external subscriptions: enabled", "MQTT external subscriptions: disabled",
   "MQTT external subscriptions enabled - restart MQTT to apply", "MQTT external subscriptions disabled"},
  PUBLISH_CASE(wifi, mqttPublishWiFi, "WiFi"),
  PUBLISH_CASE(system, mqttPublishSystem, "System"),
  PUBLISH_CASE(thermal, mqttPublishThermal, "Thermal"),
  PUBLISH_CASE(tof, mqttPublishToF, "ToF"),
  PUBLISH_CASE(imu, mqttPublishIMU, "IMU"),
  PUBLISH_CASE(presence, mqttPublishPresence, "Presence"),
  PUBLISH_CASE(gps, mqttPublishGPS, "GPS"),
  PUBLISH_CASE(apds, mqttPublishAPDS, "APDS"),
  PUBLISH_CASE(rtc, mqttPublishRTC, "RTC"),
#if ENABLE_ANO_ENCODER
  PUBLISH_CASE(input, mqttPublishInput, "ANO Encoder"),
#else
  PUBLISH_CASE(input, mqttPublishInput, "Gamepad"),
#endif
};

static int cases, failures, checks;
static std::string context;
static void check(bool condition, const char* message) {
  ++checks;
  if (condition) return;
  ++failures;
  // Keep a baseline replay useful without printing thousands of repeated lines.
  if (failures <= 24) std::fprintf(stderr, "%s: %s\n", context.c_str(), message);
}

static void reset(const Command& command, bool initial, const char* category,
                  const std::string& input) {
  ++cases;
  context = std::string(command.name) + " / " + category + " / [" + input + "]";
  // Alternate unrelated settings to catch changing the wrong field.
  for (size_t i = 0; i < std::size(commands); ++i) {
    gSettings.*commands[i].field = (i % 2) != 0;
  }
  gSettings.*command.field = initial;
  gCLIValidateOnly = false; stopResult = persistResult = true;
  stopCalls = persistCalls = notifyCalls = 0;
  notifiedField = nullptr; events.clear();
}

static void unchangedOthers(const Command& command, const SettingsBoundary& before) {
  for (const auto& other : commands) {
    if (other.field != command.field)
      check(gSettings.*other.field == before.*other.field, "unrelated setting changed");
  }
}

static void noEffects(const Command& command, const SettingsBoundary& before) {
  unchangedOthers(command, before);
  check(gSettings.*command.field == before.*command.field, "setting changed");
  check(stopCalls == 0, "stop requested");
  check(persistCalls == 0 && notifyCalls == 0 && events.empty(), "persistence or notification requested");
}

static void queryAndInvalid() {
  const std::vector<std::string> invalid = {
    "onn", "tru", "of", "enabled", "disabled", "yes", "no", "2", "-1", "01",
    "1x", "0x", "1.0", "on off", "on extra", "true\nfalse", " enable! ",
    "\"on\"", "\xc3\xa9", std::string(2048, 'x')
  };
  for (const auto& command : commands) {
    for (bool initial : {false, true}) {
      for (const char* input : {"", " ", "\t\r\n\v\f"}) {
        reset(command, initial, "query", input);
        const auto before = gSettings;
        check(std::string(command.handler(input)) == (initial ? command.queryOn : command.queryOff),
              "query response changed");
        noEffects(command, before);
      }
      for (const auto& input : invalid) {
        reset(command, initial, "invalid", input);
        const auto before = gSettings;
        check(std::string(command.handler(input)) == "Error: Use 0/1, true/false, on/off, or enable/disable",
              "invalid input did not return the shared error");
        noEffects(command, before);
      }
      for (const char* input : {"", "on", "off", "disable", "nonsense"}) {
        reset(command, initial, "validate only", input);
        gCLIValidateOnly = true;
        const auto before = gSettings;
        check(std::string(command.handler(input)) == "VALID", "validation-only response changed");
        noEffects(command, before);
      }
    }
  }
}

static void validAliases() {
  struct Value { const char* text; bool value; };
  const Value values[] = {{"1", true}, {"true", true}, {"on", true}, {"enable", true},
                          {"0", false}, {"false", false}, {"off", false}, {"disable", false}};
  for (const auto& command : commands) {
    for (bool initial : {false, true}) {
      for (const auto& value : values) {
        std::string upper = value.text, mixed = value.text;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });
        for (size_t i = 0; i < mixed.size(); i += 2) mixed[i] = std::toupper(static_cast<unsigned char>(mixed[i]));
        for (const std::string& input : {std::string(value.text), upper, " \t" + mixed + "\r\n"}) {
          for (bool stopped : {false, true}) {
            for (bool persisted : {false, true}) {
              reset(command, initial, "valid", input);
              stopResult = stopped; persistResult = persisted;
              const auto before = gSettings;
              const bool stops = command.field == &SettingsBoundary::mqttEnabled && !value.value;
              const bool changes = initial != value.value;
              const std::string expected = stops && stopped
                  ? "MQTT client disabled; client stop scheduled" : value.value ? command.setOn : command.setOff;
              check(std::string(command.handler(input)) == expected, "valid-input response changed");
              check(gSettings.*command.field == value.value, "valid alias stored wrong value");
              check(persistCalls == (changes ? 1 : 0), "changed-only persistence count differs");
              check(notifyCalls == (changes ? 1 : 0), "changed-only notification count differs");
              check(notifiedField == (changes ? &(gSettings.*command.field) : nullptr), "wrong setting notified");
              check(stopCalls == (stops ? 1 : 0), "stop request differs (including already-disabled case)");
              std::vector<std::string> expectedEvents;
              if (changes) expectedEvents = {"persist", "notify"};
              if (stops) expectedEvents.push_back("stop");
              check(events == expectedEvents, "persistence/notification/stop ordering differs");
              unchangedOthers(command, before);
            }
          }
        }
      }
    }
  }
}

int main() {
  static_assert(std::size(commands) == 13, "exercise all thirteen MQTT toggles");
  queryAndInvalid();
  validAliases();
  std::printf("MQTT boolean commands (%s): %d cases, %d checks, %d failures\n",
              ENABLE_ANO_ENCODER ? "ANO Encoder" : "Gamepad", cases, checks, failures);
  return failures ? 1 : 0;
}
