// test_features_list_only.py inserts the shipping `features` command, its text
// views and the registry helpers it reads. Registry rows, the switch table and
// the live-state reader are fixtures. Nothing here defines setSetting,
// systemEventPost, writeSettingsJson or automationSetSystemEnabled: if the
// command called any of them again, this file would not compile.
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define EXT_RAM_BSS_ATTR
#define PSRAM_JSON_DOC(name) JsonDocument name
#define PSRAM_STATIC_BUF(name, capacity) static char name[capacity]; constexpr size_t name##_SIZE = capacity
void webBatchTestStringAppend() {}
bool gCliValidateOnly = false;
// INSERT_TYPES

// Every flag a row points at. The command must leave all of them as they were.
static struct Flags {
  bool runtimeAllowed, runtimeAuto, rebootAllowed, unavailable, automationAllowed,
       automationAuto, ledStartup;
  bool operator==(const Flags& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
} flags;

static bool compiledOut() { return false; }
static const FeatureEntry featureRegistry[] = {
  // Carries REQUIRES_REBOOT like wifi and oled, whose <x>enabled acts at once:
  // the flag alone must not add a reboot note (allowAtReboot does).
  {"runtime", "Runtime", FEATURE_CAT_SENSOR, 8,
   FEATURE_FLAG_RUNTIME_TOGGLE | FEATURE_FLAG_REQUIRES_REBOOT,
   &flags.runtimeAuto, nullptr, "runtime fixture"},
  {"reboot", "Reboot", FEATURE_CAT_CORE, 3,
   FEATURE_FLAG_RUNTIME_TOGGLE | FEATURE_FLAG_REQUIRES_REBOOT,
   &flags.rebootAllowed, nullptr, "reboot fixture"},
  {"unavailable", "Unavailable", FEATURE_CAT_SENSOR, 3, FEATURE_FLAG_RUNTIME_TOGGLE,
   &flags.unavailable, compiledOut, "compiled-out fixture"},
  {"fixed", "Fixed", FEATURE_CAT_SYSTEM, 1, FEATURE_FLAG_COMPILE_TIME,
   nullptr, nullptr, "compile-time fixture"},
  {"automation", "Automations", FEATURE_CAT_SYSTEM, 8, FEATURE_FLAG_RUNTIME_TOGGLE,
   &flags.automationAllowed, nullptr, "owner-applied fixture"},
  {"led", "LED", FEATURE_CAT_SYSTEM, 2, FEATURE_FLAG_RUNTIME_TOGGLE,
   &flags.ledStartup, nullptr, "start-up effect fixture"},
};
static constexpr size_t featureRegistryCount = sizeof(featureRegistry) / sizeof(featureRegistry[0]);
static constexpr size_t kFeaturesJsonBuf = 4096;

// Fixture live state: kLive* below; the shipping reader is ramFlushReadLive().
enum : uint8_t { kLiveOff = 0, kLiveOn = 1, kLiveNone = 0xFF };
static uint8_t runtimeLive = kLiveOn;
static FeatureSwitches switches[] = {
  {"runtime", &flags.runtimeAllowed, &flags.runtimeAuto, kLiveOn,
   "openruntime / closeruntime", "runtimeautostart <0|1>", "runtimeenabled <0|1>"},
  {"reboot", &flags.rebootAllowed, nullptr, kLiveOff, nullptr, nullptr, "rebootenabled <0|1>", true},
  {"unavailable", &flags.unavailable, nullptr, kLiveOff,
   "openunavailable / closeunavailable", nullptr, "unavailableenabled <0|1>"},
  {"fixed", nullptr, nullptr, kLiveNone, nullptr, nullptr, nullptr},
  {"automation", &flags.automationAllowed, &flags.automationAuto, kLiveOff,
   "automation system enable|disable", "automationautostart <0|1>",
   "automation system enable|disable"},
  {"led", nullptr, &flags.ledStartup, kLiveNone, nullptr, "ledstartupenabled <0|1>", nullptr},
};
static const FeatureSwitches* featureSwitchesFor(const FeatureEntry* f) {
  const size_t index = (size_t)(f - featureRegistry);
  return index < featureRegistryCount ? &switches[index] : nullptr;
}
static unsigned liveReads;
static int featureRunningNow(const FeatureSwitches* s) {
  ++liveReads;
  if (s == &switches[0]) return runtimeLive;
  return s->live == kLiveNone ? -1 : s->live;
}

static uint32_t hw1InternalFreeBytes() { return 65536; }
static unsigned trailers;
static void emitListingTrailer(const char*, const char*) { ++trailers; }
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
static bool error(const std::string& reply) { return reply.rfind("Error:", 0) == 0; }
static bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

static void setAll(bool value) {
  flags.runtimeAllowed = flags.runtimeAuto = flags.rebootAllowed = flags.unavailable =
    flags.automationAllowed = flags.automationAuto = flags.ledStartup = value;
}

// Runs one input and checks that it changed no flag.
static std::string run(const char* input) {
  const Flags before = flags;
  const std::string reply = cmd_features(input);
  check(flags == before, "features changed a setting");
  return reply;
}

static const char* const kRuntimeRefusal =
  "Error: features only lists. Switch runtime with: openruntime / closeruntime (now), "
  "runtimeautostart <0|1> (at boot), runtimeenabled <0|1> (allowed).";

static void refusals() {
  for (bool state : {false, true}) {
    setAll(state);
    for (const char* input : {"runtime on", "runtime off", "runtime true", "runtime false",
                              "runtime 1", "runtime 0", "runtime enable", "runtime disable",
                              "RUNTIME ON", "runtime nonsense"}) {
      context = std::string("refusal '") + input + "'";
      check(run(input) == kRuntimeRefusal, "two-argument form not refused with the row's commands");
    }
    context = "refusal per row";
    check(run("reboot on") == std::string("Error: features only lists. Switch reboot with: "
                                          "rebootenabled <0|1> (allowed, after a reboot)."),
          "reboot row refusal changed");
    check(run("automation off") == std::string("Error: features only lists. Switch automation with: "
                                               "automation system enable|disable (now and allowed), "
                                               "automationautostart <0|1> (at boot)."),
          "automation refusal does not name the shared command once");
    check(run("led on") == std::string("Error: features only lists. Switch led with: "
                                       "ledstartupenabled <0|1> (at boot)."),
          "led refusal changed");
    check(run("unavailable on") == std::string("Error: features only lists, and unavailable is not "
                                               "built into this firmware."),
          "not-built refusal changed");
    check(run("fixed on") == std::string("Error: features only lists, and fixed has no switch: it is "
                                         "fixed when the firmware is built."),
          "compile-time refusal changed");
    check(run("nosuch on") == std::string("Error: Unknown feature. Run 'features' to see list."),
          "unknown id refusal changed");
    for (const char* input : {"runtime on extra", "runtime \"on", "\"runtime"}) {
      context = std::string("usage '") + input + "'";
      check(run(input) == std::string("Error: Usage: features [<id> | json]. "
                                      "The features command only lists."), "usage refusal changed");
    }
  }
  // The validate pass refuses too, so a saved automation step is rejected when saved.
  context = "validate pass";
  gCliValidateOnly = true;
  check(run("runtime on") == kRuntimeRefusal, "validate pass accepted the two-argument form");
  for (const char* input : {"", "runtime", "json"}) {
    check(run(input) == std::string("VALID"), "validate pass changed for a query");
  }
  gCliValidateOnly = false;
  check(trailers == 0 && liveReads == 0, "a refusal or validate pass read live state or listed");
}

static void queries() {
  for (bool state : {false, true}) {
    setAll(state);
    for (const char* input : {"", "runtime", "RUNTIME", "reboot", "unavailable", "fixed",
                              "automation", "led", "json", "runtime json"}) {
      context = std::string("query '") + input + "' from " + (state ? "on" : "off");
      check(!error(run(input)), "query failed");
    }
    context = "unknown id";
    check(run("nosuch") == std::string("Error: Unknown feature. Run 'features' to see list."),
          "unknown id reply changed");
  }
}

static void listing() {
  context = "listing";
  setAll(false);
  flags.runtimeAllowed = true; flags.runtimeAuto = true; flags.ledStartup = true;
  runtimeLive = kLiveOn;
  trailers = 0;
  const std::string text = run("");
  check(trailers == 1, "listing trailer not emitted once");
  check(has(text, " id           heap   built run  boot allow\n"), "column header changed");
  check(has(text, "\n[Sensors]\n runtime      ~ 8KB  yes   yes  on   on\n"), "runtime row changed");
  check(has(text, " unavailable  ~ 0KB  no    -    -    -\n"), "not-built row changed");
  check(has(text, "\n[Core]\n reboot       ~ 3KB  yes   no   -    off\n"), "reboot row changed");
  check(has(text, "\n[System]\n fixed        ~ 1KB  yes   -    -    -\n"), "compile-time row changed");
  check(has(text, " automation   ~ 8KB  yes   no   off  off\n"), "automation row changed");
  check(has(text, " led          ~ 2KB  yes   -    on   -\n"), "led row changed");
  // Running now = runtime 8 + fixed 1 + led 2 (rows with no live state count as present).
  check(has(text, "Running now: ~11KB | Free: 64KB | All built in: ~22KB\n"), "totals changed");
  check(has(text, "'features <id>' names the commands that switch it."), "footer does not point at the detail");
  check(!has(text, "on|off") && !has(text, "<on"), "listing still offers the on/off form");
  runtimeLive = kLiveOff;
  check(has(run(""), " runtime      ~ 8KB  yes   no   on   on\n"), "run column ignores live state");
  runtimeLive = kLiveOn;
}

static void details() {
  setAll(false);
  flags.runtimeAllowed = true;
  context = "runtime detail";
  check(run("runtime") == std::string(
          "[runtime] Runtime\nCategory: Sensors\nHeap cost: ~8KB\nBuilt in: yes\n"
          "Running now: yes\n"
          "Start at boot: off - runtimeautostart <0|1>\n"
          "Allowed: on - runtimeenabled <0|1>\n"
          "Start/stop now: openruntime / closeruntime\n"
          "runtime fixture"), "runtime detail changed");
  context = "reboot detail";
  const std::string reboot = run("reboot");
  check(has(reboot, "Start at boot: -\n") && has(reboot, "Start/stop now: -\n") &&
        has(reboot, "Allowed: off - rebootenabled <0|1> (takes effect after a reboot)\n"),
        "reboot detail lost its reboot note or '-' lines");
  context = "fixed detail";
  check(run("fixed") == std::string(
          "[fixed] Fixed\nCategory: System\nHeap cost: ~1KB\nBuilt in: yes\n"
          "Switch: none, fixed when the firmware is built\ncompile-time fixture"),
        "compile-time detail changed");
  context = "not-built detail";
  check(run("unavailable") == std::string(
          "[unavailable] Unavailable\nCategory: Sensors\nHeap cost: ~0KB (~3KB if built in)\n"
          "Built in: no\ncompiled-out fixture"), "not-built detail changed");
  context = "led detail";
  const std::string led = run("led");
  check(has(led, "Running now: -\n") && has(led, "Allowed: -\n") &&
        has(led, "Start at boot: off - ledstartupenabled <0|1>\n"), "led detail changed");
}

// Schema 1 and its seven keys stay byte for byte: `enabled` is still the
// row's enabledSetting and `toggleable` is canToggleFeature.
static void json() {
  context = "json";
  setAll(false);
  flags.runtimeAuto = true;
  check(run("json") == std::string(
          "{\"schema\":1,\"features\":["
          "{\"id\":\"runtime\",\"name\":\"Runtime\",\"category\":\"Sensors\",\"heapKB\":8,"
          "\"compiled\":true,\"enabled\":true,\"toggleable\":true},"
          "{\"id\":\"reboot\",\"name\":\"Reboot\",\"category\":\"Core\",\"heapKB\":3,"
          "\"compiled\":true,\"enabled\":false,\"toggleable\":true},"
          "{\"id\":\"unavailable\",\"name\":\"Unavailable\",\"category\":\"Sensors\",\"heapKB\":0,"
          "\"compiled\":false,\"enabled\":false,\"toggleable\":false},"
          "{\"id\":\"fixed\",\"name\":\"Fixed\",\"category\":\"System\",\"heapKB\":1,"
          "\"compiled\":true,\"enabled\":true,\"toggleable\":false},"
          "{\"id\":\"automation\",\"name\":\"Automations\",\"category\":\"System\",\"heapKB\":8,"
          "\"compiled\":true,\"enabled\":false,\"toggleable\":true},"
          "{\"id\":\"led\",\"name\":\"LED\",\"category\":\"System\",\"heapKB\":2,"
          "\"compiled\":true,\"enabled\":false,\"toggleable\":true}]}"),
        "features json changed");
}

// The listing fails loudly instead of handing back a cut-off list.
static void appendBounds() {
  context = "append bounds";
  char small[8];
  size_t pos = 0;
  check(featuresAppend(small, sizeof(small), pos, "%s", "abc") && pos == 3, "short append failed");
  check(!featuresAppend(small, sizeof(small), pos, "%s", "defgh") && pos == sizeof(small),
        "overflow not reported");
  check(!featuresAppend(small, sizeof(small), pos, "%s", "x") && pos == sizeof(small),
        "append after overflow wrote past the end");
}

int main() {
  refusals(); queries(); listing(); details(); json(); appendBounds();
  std::printf("features list-only: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
