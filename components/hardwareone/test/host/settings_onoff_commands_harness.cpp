// Host harness for five <0|1> setting commands that had their own parser:
// espnowenabled, httpautostart, httpsenabled (System_Settings.cpp),
// thermalrollingminmaxenabled and thermalinterpolationenabled
// (i2csensor_mlx90640.cpp). test_settings_onoff_commands.py inserts the five
// handlers, parseBoolArg, setSetting and the validate-only macro unchanged and
// includes the real System_CommandReply.h. String storage, the reply buffer,
// persistence/notification, the command executor and the broadcast line are
// the host boundaries below.
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <strings.h>
#include <vector>

#include "System_CommandReply.h"

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
};

struct SettingsBoundary {
  bool espnowEnabled, httpAutoStart, httpsEnabled;
  bool thermalRollingMinMaxEnabled, thermalInterpolationEnabled;
} gSettings;

bool gCliValidateOnly;
static int persistCalls, notifyCalls;
static const void* notifiedField;
static std::vector<std::string> executed, broadcasts;
static bool bufferAvailable;
static char debugBuffer[1024];

struct SettingEntry;
static void finishSettingChange(const void* field, const SettingEntry*) {
  ++persistCalls; ++notifyCalls; notifiedField = field;
}
static bool ensureDebugBuffer() { return bufferAvailable; }
static char* getDebugBuffer() { return debugBuffer; }
static const char* stopReply;  // what closeespnow answers
[[maybe_unused]] static String executeCommandThroughRegistry(const char* line) {
  executed.emplace_back(line);
  return String(stopReply);
}
// ESP-NOW state before closeespnow runs (System_ESPNow.cpp).
static bool espNowRunning;
[[maybe_unused]] static bool isEspNowInitialized() { return espNowRunning; }
// An earlier stop that did not finish: no longer initialized, not stopped yet.
static bool espNowStopPending;
[[maybe_unused]] static bool espnowShutdownPending() { return espNowStopPending; }
static void broadcastPrintf(const char* format, ...) {
  char line[256];
  va_list args; va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  broadcasts.emplace_back(line);
}
#define BROADCAST_PRINTF(...) broadcastPrintf(__VA_ARGS__)

// INSERT_PRODUCTION_DEFINITIONS

struct Command {
  const char* name;
  const char* (*handler)(const String&);
  bool SettingsBoundary::*field;
  const char* setOn;        // reply for a value that parses as on
  const char* setOff;       // reply for a value that parses as off
  const char* usage;        // reply for no argument
  const char* broadcastOn;  // line printed on the way (nullptr: none)
  const char* broadcastOff;
  bool stopsEspNow;         // off runs closeespnow (when ESP-NOW is compiled)
};

static const Command commands[] = {
  {"espnowenabled", cmd_espnowenabled, &SettingsBoundary::espnowEnabled,
   "espnowenabled set to 1 (use openespnow or reboot with autostart)",
   "espnowenabled set to 0 (ESP-NOW stopped)",
   "Error: invalid arguments \xe2\x80\x94 Usage: espnowenabled <0|1>", nullptr, nullptr, true},
  {"httpautostart", cmd_httpAutoStart, &SettingsBoundary::httpAutoStart,
   "httpautostart set to 1", "httpautostart set to 0",
   "Error: invalid arguments \xe2\x80\x94 Usage: httpautostart <0|1>", nullptr, nullptr, false},
  {"httpsenabled", cmd_httpsEnabled, &SettingsBoundary::httpsEnabled,
   "httpsenabled set to 1 (takes effect after reboot)",
   "httpsenabled set to 0 (takes effect after reboot)",
   "Error: invalid arguments \xe2\x80\x94 Usage: httpsenabled <0|1>", nullptr, nullptr, false},
  {"thermalrollingminmaxenabled", cmd_thermalrollingminmaxenabled,
   &SettingsBoundary::thermalRollingMinMaxEnabled,
   "[Thermal] Setting updated", "[Thermal] Setting updated",
   "Error: invalid arguments \xe2\x80\x94 Usage: thermalrollingminmaxenabled <0|1>",
   "thermalRollingMinMaxEnabled set to 1", "thermalRollingMinMaxEnabled set to 0", false},
  {"thermalinterpolationenabled", cmd_thermalinterpolationenabled,
   &SettingsBoundary::thermalInterpolationEnabled,
   "[Thermal] Setting updated", "[Thermal] Setting updated",
   "Error: invalid arguments \xe2\x80\x94 Usage: thermalinterpolationenabled <0|1>",
   "thermalInterpolationEnabled set to 1", "thermalInterpolationEnabled set to 0", false},
};

static int cases, failures, checks;
static std::string context;
static void check(bool condition, const char* message) {
  ++checks;
  if (condition) return;
  ++failures;
  // Keep a baseline replay readable: the first lines name each kind of failure.
  if (failures <= 24) std::fprintf(stderr, "FAIL %s: %s\n", context.c_str(), message);
}

static void reset(const Command& command, bool initial, const char* category,
                  const std::string& input) {
  ++cases;
  context = std::string(command.name) + " / " + category + " / [";
  for (const char c : input.substr(0, 40)) {  // one printable line per failure
    if (c == '\t') context += "\\t";
    else if (c == '\r') context += "\\r";
    else if (c == '\n') context += "\\n";
    else context += c;
  }
  context += "]";
  // Alternate the other settings to catch a write to the wrong field.
  for (size_t i = 0; i < std::size(commands); ++i) gSettings.*commands[i].field = (i % 2) != 0;
  gSettings.*command.field = initial;
  gCliValidateOnly = false;
  espNowRunning = false;
  espNowStopPending = false;
  stopReply = "OK";
  bufferAvailable = true;
  persistCalls = notifyCalls = 0;
  notifiedField = nullptr;
  executed.clear(); broadcasts.clear();
  std::memset(debugBuffer, 0, sizeof(debugBuffer));
}

static void noEffects(const Command& command, const SettingsBoundary& before) {
  for (const auto& other : commands) {
    check(gSettings.*other.field == before.*other.field,
          other.field == command.field ? "the setting changed" : "an unrelated setting changed");
  }
  check(persistCalls == 0 && notifyCalls == 0, "a save or notification was requested");
  check(executed.empty(), "a command was run");
  check(broadcasts.empty(), "a line was printed");
}

static void refusals() {
  const std::vector<std::string> invalid = {
    "onn", "tru", "truex", "of", "enabled", "disabled", "yes", "no", "maybe", "2", "10", "-1",
    "01", "1x", "1.0", "on off", "1 extra", "true false", "\"on\"", std::string(600, 'x')
  };
  for (const auto& command : commands) {
    for (bool initial : {false, true}) {
      for (const auto& input : invalid) {
        reset(command, initial, "not on/off", input);
        const auto before = gSettings;
        check(std::string(command.handler(input.c_str())) == kCommandReplyNotOnOff,
              "a word that is neither on nor off is not refused with the shared reply");
        noEffects(command, before);
      }
      for (const char* input : {"", " ", "\t\r\n"}) {
        reset(command, initial, "no argument", input);
        const auto before = gSettings;
        check(std::string(command.handler(input)) == command.usage, "the usage reply changed");
        noEffects(command, before);
      }
      for (const char* input : {"", "on", "off", "nonsense"}) {
        reset(command, initial, "validate only", input);
        gCliValidateOnly = true;
        const auto before = gSettings;
        check(std::string(command.handler(input)) == "VALID", "the validate-only reply changed");
        noEffects(command, before);
      }
    }
  }
}

static void acceptedWords() {
  struct Value { const char* text; bool value; };
  const Value values[] = {{"1", true}, {"true", true}, {"on", true}, {"enable", true},
                          {"0", false}, {"false", false}, {"off", false}, {"disable", false}};
  for (const auto& command : commands) {
    for (bool initial : {false, true}) {
      for (const auto& value : values) {
        std::string upper = value.text, mixed = value.text;
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        for (size_t i = 0; i < mixed.size(); i += 2) {
          mixed[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(mixed[i])));
        }
        for (const std::string& input : {std::string(value.text), upper, " \t" + mixed + "\r\n"}) {
          reset(command, initial, value.value ? "on" : "off", input);
          const auto before = gSettings;
          const bool changes = initial != value.value;
          check(std::string(command.handler(input.c_str())) ==
                    (value.value ? command.setOn : command.setOff),
                "the reply for a valid value changed");
          check(gSettings.*command.field == value.value, "the word stored the wrong value");
          check(persistCalls == (changes ? 1 : 0) && notifyCalls == (changes ? 1 : 0),
                "a save is requested exactly when the value changes");
          check(notifiedField == (changes ? &(gSettings.*command.field) : nullptr),
                "the wrong setting was notified");
          // Live side effects: espnowenabled stops ESP-NOW on every off (also
          // when the setting already was off); the thermal pair print a line.
          const bool stops = command.stopsEspNow && !value.value && ENABLE_ESPNOW;
          check(executed == (stops ? std::vector<std::string>{"closeespnow"}
                                   : std::vector<std::string>{}),
                "closeespnow runs on off, only for espnowenabled, only with ESP-NOW compiled");
          const char* line = value.value ? command.broadcastOn : command.broadcastOff;
          check(broadcasts == (line ? std::vector<std::string>{line} : std::vector<std::string>{}),
                "the printed line changed");
          for (const auto& other : commands) {
            if (other.field != command.field) {
              check(gSettings.*other.field == before.*other.field, "an unrelated setting changed");
            }
          }
        }
      }
    }
  }
}

static void noReplyBuffer() {
  // The three System_Settings handlers build their reply in the shared buffer
  // and refuse before parsing when it is missing.
  for (size_t i = 0; i < 3; ++i) {
    const Command& command = commands[i];
    reset(command, false, "no reply buffer", "on");
    bufferAvailable = false;
    const auto before = gSettings;
    check(std::string(command.handler("on")) == "Error: Debug buffer unavailable",
          "the missing-buffer reply changed");
    noEffects(command, before);
  }
}

static void espNowStopFailed() {
  // espnowenabled off stores the setting either way. A running ESP-NOW, or
  // one with an earlier stop still pending, whose stop failed is answered with
  // an error that carries the stop's reason; an error from closeespnow with
  // nothing running is a normal save.
  const Command& command = commands[0];
  struct Case { bool running; bool pending; const char* stop; bool fails; };
  const Case stops[] = {
    {true, false, "ESP-NOW deinitialized successfully", false},
    {true, false, "Error: ESP-NOW shutdown did not finish: the TX dispatcher did not stop. Run 'closeespnow' again.", true},
    {false, true, "ESP-NOW deinitialized successfully", false},
    {false, true, "Error: ESP-NOW shutdown did not finish: the TX dispatcher did not stop. Run 'closeespnow' again.", true},
    {false, false, "Error: ESP-NOW is not initialized", false},
  };
  for (const Case& stop : stops) {
    reset(command, true, "stop outcome", stop.stop);
    espNowRunning = stop.running;
    espNowStopPending = stop.pending;
    stopReply = stop.stop;
    const std::string reply = command.handler("off");
    check(!gSettings.espnowEnabled && persistCalls == 1, "the setting is stored as off");
    if (!ENABLE_ESPNOW) {
      check(executed.empty() && reply == command.setOff, "without ESP-NOW compiled there is nothing to stop");
      continue;
    }
    check(executed == std::vector<std::string>{"closeespnow"}, "closeespnow runs");
    if (stop.fails) {
      check(reply == "Error: espnowenabled saved as 0, but ESP-NOW did not stop. ESP-NOW shutdown did not "
                     "finish: the TX dispatcher did not stop. Run 'closeespnow' again.",
            "a failed stop is not reported as stopped");
    } else {
      check(reply == command.setOff, "the reply for a stop that worked or had nothing to do changed");
    }
  }
}

int main() {
  static_assert(std::size(commands) == 5, "the five commands that had their own parser");
  acceptedWords();
  refusals();
  noReplyBuffer();
  espNowStopFailed();
  std::printf("settings on/off commands (ESP-NOW %s): %d cases, %d checks, %d failures\n",
              ENABLE_ESPNOW ? "compiled" : "not compiled", cases, checks, failures);
  return failures ? 1 : 0;
}
