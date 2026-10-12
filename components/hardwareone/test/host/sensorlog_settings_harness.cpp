// Production owner transitions, generic setting mutation and parser are inserted
// by test_sensorlog_settings.py. Persistence/events/startup are observations.
#include <Arduino.h>
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

void webBatchTestStringAppend() {}
#define EXT_RAM_BSS_ATTR
bool gCLIValidateOnly = false;

// INSERT_TYPES

static uint8_t gSensorLogMask;
static SensorLogFormat gSensorLogFormat;
static bool gSensorLoggingRunning;
static struct {
  int sensorLogMask;
  int sensorLogFormat;
  bool sensorLogEnabled;
  bool healthLoggingEnabled;
  bool sensorLogAutoStart;
  String sensorLogPath;
} gSettings;
static bool lookupAvailable = true, bufferAvailable = true;
static unsigned persistCalls, disableCalls;
static char debugBuffer[1024];
struct Event { int type; std::string label; std::string value; };
static std::vector<std::string> broadcasts, hints;
static std::vector<Event> events;
static std::vector<const void*> changedFields;
struct PersistState { uint8_t mask; SensorLogFormat format; };
static std::vector<PersistState> persistedLiveStates;
enum { SYSEVT_SETTING_CHANGED = 1, SYSEVT_SENSOR_STOPPED = 2 };
bool requestSettingsPersist() {
  ++persistCalls;
  persistedLiveStates.push_back({gSensorLogMask, gSensorLogFormat});
  return true;
}
void notifySettingChanged(const void* field) { changedFields.push_back(field); }
void finishSettingChange(const void* field, const SettingEntry*) {
  (void)requestSettingsPersist();
  notifySettingChanged(field);
}
void applyMasterEnableDisable(const char*) { ++disableCalls; }
void cliHint(const char* value) { hints.emplace_back(value); }
void broadcastPrintf(const char* format, ...) {
  char text[256];
  va_list args;
  va_start(args, format);
  std::vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  broadcasts.emplace_back(text);
}
#define BROADCAST_PRINTF(...) broadcastPrintf(__VA_ARGS__)
void systemEventPost(int type, const char* label, const char* value = "") {
  events.push_back({type, label, value});
}
char* getDebugBuffer() { return debugBuffer; }
bool ensureDebugBuffer() { return bufferAvailable; }
static const SettingEntry entries[] = {
// INSERT_ENTRIES
};
const SettingEntry* findSettingByCmdKey(const char* key) {
  if (lookupAvailable)
    for (const auto& entry : entries)
      if (std::strcmp(entry.cmdKey, key) == 0) return &entry;
  return nullptr;
}

#if ENABLE_R1_HEALTH
using portMUX_TYPE = int;
static portMUX_TYPE sSensorLogSessionMux;
static uint32_t sSensorLogSessionId;
static void portENTER_CRITICAL(portMUX_TYPE*) {}
static void portEXIT_CRITICAL(portMUX_TYPE*) {}
// Health restarts stop a persisted session, never a transient live track, so
// the saved-selection restore must not run on these paths.
[[maybe_unused]] static bool gSensorLogTransient = false;
static unsigned settingsRestores = 0;
[[maybe_unused]] static void sensorLogRestoreSettings() { ++settingsRestores; }
#define CAPTURE_HEALTHLOG_DEFAULT "/logging_captures/sensors/health.csv"
static String gSensorLogPath;
static uint32_t gSensorLogIntervalMs;
enum { HT_MINE_IDLE = 0, HT_MINE_WAIT = 1 };
static uint8_t sHtMineState;
static uint32_t sHtMineLastBurstMs, sHtMineLastReseekMs;
static bool sHtPageRefreshPending;
static std::vector<std::string> shapedPaths, starts;
static std::vector<uint32_t> startIntervals;
static bool startSawRunning;
static uint8_t startMask;
static SensorLogFormat startFormat;
static const char* startResult;
String shapeSessionPath(String path) {
  shapedPaths.emplace_back(path.c_str());
  return String("/shaped folder/session.csv");
}
static const char* sensorLogStart(String path, uint32_t interval) {
  starts.emplace_back(path.c_str());
  startIntervals.push_back(interval);
  startSawRunning = gSensorLoggingRunning;
  startMask = gSensorLogMask;
  startFormat = gSensorLogFormat;
  if (startResult && std::strncmp(startResult, "SUCCESS", 7) == 0) {
    gSensorLoggingRunning = true;
    gSensorLogPath = "/shaped folder/session.csv";
  }
  return startResult;
}
#endif

// INSERT_PRODUCTION

static int checks, failures;
static std::string context;
static void check(bool condition, const char* message) {
  ++checks;
  if (condition) return;
  ++failures;
  if (failures <= 40) std::fprintf(stderr, "%s: %s\n", context.c_str(), message);
}
static void reset(uint8_t mask = LOG_TOF, SensorLogFormat format = SENSOR_LOG_TEXT,
                  bool running = false) {
  gSensorLogMask = mask;
  gSensorLogFormat = format;
  gSensorLoggingRunning = running;
  gSettings.sensorLogMask = mask;
  gSettings.sensorLogFormat = format;
  gSettings.sensorLogEnabled = true;
  gSettings.healthLoggingEnabled = false;
  gSettings.sensorLogAutoStart = false;
  gSettings.sensorLogPath = "/logs/old.txt";
  gCLIValidateOnly = false;
  lookupAvailable = bufferAvailable = true;
  persistCalls = disableCalls = 0;
  changedFields.clear();
  persistedLiveStates.clear();
  broadcasts.clear();
  events.clear();
  hints.clear();
#if ENABLE_R1_HEALTH
  gSensorLogPath = "/logs/active.csv";
  gSensorLogIntervalMs = 7000;
  sHtMineState = HT_MINE_WAIT;
  sHtMineLastBurstMs = 42;
  sHtMineLastReseekMs = 43;
  sHtPageRefreshPending = true;
  shapedPaths.clear();
  starts.clear();
  startIntervals.clear();
  startSawRunning = true;
  startMask = 0;
  startFormat = SENSOR_LOG_TRACK;
  startResult = "SUCCESS: started";
#endif
}
static void noEffects(uint8_t liveMask, int savedMask, SensorLogFormat liveFormat,
                      int savedFormat, bool running) {
  check(sensorLogSelectedMask() == liveMask && gSettings.sensorLogMask == savedMask,
        "read/error changed live or saved mask");
  check(sensorLogCurrentFormat() == liveFormat && gSettings.sensorLogFormat == savedFormat,
        "read/error changed live or saved format");
  check(gSensorLoggingRunning == running, "read/error changed session state");
  check(persistCalls == 0 && broadcasts.empty() && events.empty() && changedFields.empty(),
        "read/error produced persistence or setting events");
  check(disableCalls == 0, "unrelated subsystem stop");
}
static void genericMutation(const char* key, const char* label, int value,
                            unsigned expectedPersists = 1) {
  (void)label;  // The shared formatter is exercised by settings_mutation.
  check(persistCalls == expectedPersists, "incorrect persistence count");
  check(broadcasts.size() == 1 && broadcasts[0] ==
        std::string(key) + " set to " + std::to_string(value), "generic broadcast changed");
  check(events.empty() && changedFields.size() == expectedPersists,
        "generic mutation bypassed/duplicated shared finalization");
  check(disableCalls == 0 && hints.empty(), "unrelated mutation effect");
}

static void queriesAndErrors() {
  for (bool running : {false, true}) {
    for (SensorLogFormat format : {SENSOR_LOG_TEXT, SENSOR_LOG_CSV, SENSOR_LOG_TRACK}) {
      for (const char* query : {"", " \t\r\n "}) {
        context = "query preserves divergent saved/live state";
        reset(LOG_TOF, format, running);
        gSettings.sensorLogMask = LOG_GPS;
        gSettings.sensorLogFormat = SENSOR_LOG_TRACK;
        check(std::string(cmd_sensorlogmask(query)) == "sensorLogMask = 32", "mask query output changed");
        noEffects(LOG_TOF, LOG_GPS, format, SENSOR_LOG_TRACK, running);
        check(std::string(cmd_sensorlogformat(query)) == "sensorLogFormat = 2", "format query output changed");
        noEffects(LOG_TOF, LOG_GPS, format, SENSOR_LOG_TRACK, running);
        check(hints.empty(), "query emitted CSV refusal hint");
      }
    }
  }
  for (const char* invalid : {"-1", "256", " 999 "}) {
    context = std::string("invalid mask ") + invalid;
    reset(LOG_TOF, SENSOR_LOG_TEXT, true);
    gSettings.sensorLogMask = LOG_GPS;
    check(std::string(cmd_sensorlogmask(invalid)) == "Error: sensorLogMask must be 0..255", "mask range error changed");
    noEffects(LOG_TOF, LOG_GPS, SENSOR_LOG_TEXT, SENSOR_LOG_TEXT, true);
  }
  for (const char* invalid : {"-1", "3", " 999 "}) {
    context = std::string("invalid format ") + invalid;
    reset(LOG_TOF, SENSOR_LOG_CSV, true);
    gSettings.sensorLogFormat = SENSOR_LOG_TRACK;
    check(std::string(cmd_sensorlogformat(invalid)) == "Error: sensorLogFormat must be 0..2", "format range error changed");
    noEffects(LOG_TOF, LOG_TOF, SENSOR_LOG_CSV, SENSOR_LOG_TRACK, true);
  }
  for (bool mask : {false, true}) {
    context = "validation and missing lookup";
    reset();
    lookupAvailable = false;
    const auto command = mask ? cmd_sensorlogmask : cmd_sensorlogformat;
    check(std::string(command("2")) == "Error: setting not found for this command", "missing entry not reported");
    noEffects(LOG_TOF, LOG_TOF, SENSOR_LOG_TEXT, SENSOR_LOG_TEXT, false);
    gCLIValidateOnly = true;
    check(std::string(command("nonsense")) == "VALID", "validation no longer short circuits");
    noEffects(LOG_TOF, LOG_TOF, SENSOR_LOG_TEXT, SENSOR_LOG_TEXT, false);
  }
}

static void numericMaskPolicy() {
  const char* refusal = "Error: can't change the sensor selection while a CSV session is running (rows would stop matching the file's header)";
  for (const char* request : {"0", "2", "255", "-1", "256", "bad"}) {
    context = std::string("running CSV mask ") + request;
    reset(LOG_TOF, SENSOR_LOG_CSV, true);
    gSettings.sensorLogMask = LOG_GPS;
    check(std::string(cmd_sensorlogmask(request)) == refusal, "numeric mask bypassed shared CSV guard");
    noEffects(LOG_TOF, LOG_GPS, SENSOR_LOG_CSV, SENSOR_LOG_CSV, true);
    check(hints.size() == 1 && hints[0].find("healthlogging") != std::string::npos, "CSV hint missing");
  }
  for (unsigned mask = 0; mask <= 255; ++mask) {
    for (SensorLogFormat format : {SENSOR_LOG_TEXT, SENSOR_LOG_CSV, SENSOR_LOG_TRACK}) {
      for (bool running : {false, true}) {
        if (running && format == SENSOR_LOG_CSV) continue;
        context = "numeric mask=" + std::to_string(mask);
        reset(LOG_TOF, format, running);
        check(std::string(cmd_sensorlogmask(String(std::to_string(mask)))) == "[Settings] Configuration updated", "mask response changed");
        check(sensorLogSelectedMask() == mask && gSettings.sensorLogMask == (int)mask, "mask not applied in owner");
        check(gSensorLogFormat == format && gSensorLoggingRunning == running, "mask changed session/format");
        genericMutation("sensorLogMask", "Sensors to log", mask, mask == LOG_TOF ? 0 : 1);
      }
    }
  }
  // The generic editor now rejects incomplete integer tokens instead of
  // accepting atoi's numeric prefixes and nonnumeric zero.
  for (const char* request : {"garbage", "0xFF", "32junk"}) {
    reset();
    context = std::string("generic grammar ") + request;
    check(std::string(cmd_sensorlogmask(request)).rfind("Error:", 0) == 0, "malformed mask accepted");
    noEffects(LOG_TOF, LOG_TOF, SENSOR_LOG_TEXT, SENSOR_LOG_TEXT, false);
  }
  reset();
  check(std::string(cmd_sensorlogmask(" +32 ")) == "[Settings] Configuration updated", "trimmed signed decimal rejected");
  genericMutation("sensorLogMask", "Sensors to log", 32);
}

static void formatPaths() {
  for (SensorLogFormat initial : {SENSOR_LOG_TEXT, SENSOR_LOG_CSV, SENSOR_LOG_TRACK}) {
    for (bool running : {false, true}) {
      for (int format = 0; format <= 2; ++format) {
        context = "numeric format=" + std::to_string(format);
        reset(LOG_TOF | LOG_R1, initial, running);
        check(std::string(cmd_sensorlogformat(String(std::to_string(format)))) == "[Settings] Configuration updated", "format response changed");
        check(sensorLogCurrentFormat() == format && gSettings.sensorLogFormat == format, "numeric format not applied");
        const uint8_t expectedMask = format == SENSOR_LOG_TRACK ? LOG_GPS : LOG_TOF | LOG_R1;
        check(sensorLogSelectedMask() == expectedMask && gSettings.sensorLogMask == expectedMask, "numeric TRACK did not apply GPS-only policy");
        check(gSensorLoggingRunning == running, "format unexpectedly restarted/stopped session");
        const bool formatChanged = format != initial;
        const unsigned expectedPersists = (formatChanged ? 1 : 0) + (format == SENSOR_LOG_TRACK ? 1 : 0);
        genericMutation("sensorLogFormat", "Format", format, expectedPersists);
        if (formatChanged) check(!persistedLiveStates.empty() &&
              persistedLiveStates[0].mask == (LOG_TOF | LOG_R1) && persistedLiveStates[0].format == initial,
              "changed numeric editor lost persist-before-live-apply ordering");
        if (format == SENSOR_LOG_TRACK) {
          check(persistedLiveStates.size() == expectedPersists && persistedLiveStates.back().mask == LOG_GPS &&
                persistedLiveStates.back().format == SENSOR_LOG_TRACK,
                "TRACK dependent persistence observed mismatched live values");
          check(changedFields.back() == &gSettings.sensorLogMask, "TRACK notified wrong dependent setting");
        }
        const uint8_t numericMask = sensorLogSelectedMask();
        const SensorLogFormat numericFormat = sensorLogCurrentFormat();
        reset(LOG_TOF | LOG_R1, initial, running);
        const char* requests[] = {"format TEXT", "format cSv", "format track"};
        check(std::string(textFormat(requests[format])).rfind("Log format set to ", 0) == 0, "text format response changed");
        check(sensorLogSelectedMask() == numericMask && sensorLogCurrentFormat() == numericFormat, "numeric and text format policies diverge");
        check(gSettings.sensorLogMask == numericMask && gSettings.sensorLogFormat == numericFormat, "text format persistence diverges");
        check(gSensorLoggingRunning == running, "text format session behavior changed");
        for (const auto& state : persistedLiveStates) {
          check(state.mask == numericMask && state.format == numericFormat,
                "text format persistence ran before coupled live values were published");
        }
      }
    }
  }
  context = "TRACK repeated with already GPS-only mask";
  reset(LOG_GPS, SENSOR_LOG_TRACK, true);
  check(std::string(cmd_sensorlogformat("2")) == "[Settings] Configuration updated", "same format rejected");
  genericMutation("sensorLogFormat", "Format", 2, 0);
  check(changedFields.empty(), "unchanged dependent mask persisted again");
  context = "text format query/error";
  reset(LOG_R1, SENSOR_LOG_CSV, true);
  gSettings.sensorLogFormat = SENSOR_LOG_TRACK;
  check(std::string(textFormat("format")).rfind("Current format: csv", 0) == 0, "text format query changed");
  noEffects(LOG_R1, LOG_R1, SENSOR_LOG_CSV, SENSOR_LOG_TRACK, true);
  check(std::string(textFormat("format invalid")) == "Error: Format must be 'text', 'csv', or 'track'", "text format error changed");
  noEffects(LOG_R1, LOG_R1, SENSOR_LOG_CSV, SENSOR_LOG_TRACK, true);
}

#if ENABLE_R1_HEALTH
static void healthTransitions() {
  for (SensorLogFormat format : {SENSOR_LOG_TEXT, SENSOR_LOG_CSV, SENSOR_LOG_TRACK}) {
    for (bool running : {false, true}) {
      for (bool hadR1 : {false, true}) {
        context = "health on format=" + std::to_string(format) + (hadR1 ? " R1" : " noR1");
        reset(LOG_TOF | (hadR1 ? LOG_R1 : 0), format, running);
        const std::string result = healthLoggingSet(true);
        check(result.rfind("SUCCESS: Health logging ON", 0) == 0, "health on failed");
        check(gSettings.healthLoggingEnabled && gSettings.sensorLogAutoStart && gSensorLoggingRunning, "health start flags changed");
        check(sensorLogSelectedMask() == (LOG_TOF | LOG_R1) && gSettings.sensorLogMask == (LOG_TOF | LOG_R1), "health lost unrelated sensors");
        check(sensorLogCurrentFormat() == SENSOR_LOG_CSV && gSettings.sensorLogFormat == SENSOR_LOG_CSV, "health format coercion changed");
        check(gSettings.sensorLogPath == CAPTURE_HEALTHLOG_DEFAULT, "health capture default changed");
        check(sHtMineState == HT_MINE_IDLE && sHtMineLastBurstMs == 0 && sHtMineLastReseekMs == 0, "health mine reset changed");
        const bool noRestart = running && hadR1 && format == SENSOR_LOG_CSV;
        check(starts.size() == (noRestart ? 0u : 1u), "health restart decision changed");
        if (!noRestart) {
          check(!startSawRunning && startMask == (LOG_TOF | LOG_R1) && startFormat == SENSOR_LOG_CSV, "restart started before state transition");
          check(starts[0] == "/shaped folder/session.csv" && startIntervals[0] == 7000, "restart literal path or interval changed");
          check(shapedPaths.size() == 1 && shapedPaths[0] == CAPTURE_HEALTHLOG_DEFAULT, "restart skipped shared path shaping");
        }
      }
    }
  }
  for (bool others : {false, true}) {
    for (bool running : {false, true}) {
      context = "health off";
      reset(LOG_R1 | (others ? LOG_TOF : 0), SENSOR_LOG_CSV, running);
      gSettings.healthLoggingEnabled = true;
      gSettings.sensorLogAutoStart = true;
      const std::string result = healthLoggingSet(false);
      check(result.rfind("SUCCESS: Health logging OFF", 0) == 0, "health off failed");
      check(!gSettings.healthLoggingEnabled && sensorLogSelectedMask() == (others ? LOG_TOF : 0), "health off selection/flag wrong");
      check(gSettings.sensorLogMask == sensorLogSelectedMask(), "health off selection not persisted");
      check(sHtMineState == HT_MINE_IDLE && !sHtPageRefreshPending, "health off pending work not cleared");
      check(starts.size() == (running && others ? 1u : 0u), "health off restart decision changed");
      check(gSensorLoggingRunning == (running && others), "health off session state wrong");
      check(gSettings.sensorLogAutoStart == (!running || others), "health off autostart behavior changed");
      check(events.size() == (running && !others ? 1u : 0u), "health stop event changed");
      if (!events.empty()) check(events[0].type == SYSEVT_SENSOR_STOPPED && events[0].label == "Logging", "health stopped event wrong");
    }
  }
  for (bool on : {false, true}) {
    for (const char* failure : {static_cast<const char*>(nullptr), "Error: start failed"}) {
      context = "health restart failure";
      reset(LOG_TOF | LOG_R1, SENSOR_LOG_TEXT, true);
      gSettings.healthLoggingEnabled = true;
      startResult = failure;
      const std::string result = healthLoggingSet(on);
      check(!gSensorLoggingRunning && starts.size() == 1, "failed restart retained running state");
      check(result == (failure ? failure : on ? "Error: Health logging failed to start sensorlog" : "SUCCESS: Health logging OFF"), "restart failure response changed");
      check(gSettings.healthLoggingEnabled == on && sensorLogSelectedMask() == (on ? LOG_TOF | LOG_R1 : LOG_TOF), "restart failure unexpectedly rolled back settings");
    }
  }
  context = "health startup boundaries";
  reset();
  gSettings.sensorLogEnabled = false;
  check(std::string(healthLoggingSet(true)).find("sensor logging is disabled") != std::string::npos, "disabled logger accepted health start");
  noEffects(LOG_TOF, LOG_TOF, SENSOR_LOG_TEXT, SENSOR_LOG_TEXT, false);
  reset();
  bufferAvailable = false;
  check(std::string(healthLoggingSet(true)) == "Error: Debug buffer unavailable", "buffer error changed");
  noEffects(LOG_TOF, LOG_TOF, SENSOR_LOG_TEXT, SENSOR_LOG_TEXT, false);
  reset();
  gSensorLogIntervalMs = 99;
  gSettings.sensorLogPath = "relative-health.csv";
  check(std::string(healthLoggingSet(true)).rfind("SUCCESS", 0) == 0, "fallback start failed");
  check(starts[0] == "/shaped folder/session.csv" && startIntervals[0] == 5000 && shapedPaths[0] == CAPTURE_HEALTHLOG_DEFAULT, "invalid path/interval fallback changed");
  reset();
  gSettings.sensorLogPath = "/custom/health.csv";
  check(std::string(healthLoggingSet(true)).rfind("SUCCESS", 0) == 0, "custom health start failed");
  check(shapedPaths[0] == "/custom/health.csv" && gSettings.sensorLogPath == "/custom/health.csv", "custom health path lost");
  for (uint32_t interval : {0u, 99u, 100u, 3600000u, 3600001u, UINT32_MAX}) {
    context = "health startup interval bounds";
    reset();
    gSensorLogIntervalMs = interval;
    check(std::string(healthLoggingSet(true)).rfind("SUCCESS", 0) == 0, "bounded health start failed");
    const uint32_t expected = interval < 100 ? 5000 : interval > 3600000 ? 3600000 : interval;
    check(startIntervals[0] == expected, "health restart lost fallback or upper interval clamp");
  }
}
#endif

int main() {
  const auto run = [](const char* name, void (*test)()) {
    const int previousChecks = checks, previousFailures = failures;
    test();
    std::printf("  %s: %d checks, %d failures\n", name,
                checks - previousChecks, failures - previousFailures);
  };
  run("queries/errors preserve live state", queriesAndErrors);
  run("numeric mask policy", numericMaskPolicy);
  run("format entry point agreement", formatPaths);
#if ENABLE_R1_HEALTH
  run("health session transitions", healthTransitions);
  run("health restarts keep the saved selection", [] {
    check(settingsRestores == 0, "a health restart ran the transient-session restore");
  });
#endif
  std::printf("sensorlog settings (health=%d): %d checks, %d failures\n",
              ENABLE_R1_HEALTH, checks, failures);
  return failures ? 1 : 0;
}
