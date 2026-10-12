// Production GPS handlers and logger startup helpers are inserted by the runner.
#include <Arduino.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

void webBatchTestStringAppend() {}
#define EXT_RAM_BSS_ATTR
#define CAPTURE_SENSORLOG_DEFAULT "/logging_captures/sensors/sensors.txt"
#define CAPTURE_HEALTHLOG_DEFAULT "/logging_captures/sensors/health.csv"
bool gCLIValidateOnly = false;

// INSERT_TYPES

static struct {
  bool sensorLogEnabled, sensorLogAutoStart, healthLoggingEnabled, gpsAutoStart;
  int sensorLogMask, sensorLogFormat, sensorLogIntervalMs;
  int sensorLogMaxSize, sensorLogMaxRotations;
  String sensorLogPath;
} gSettings;
static bool gSensorLoggingRunning;
static String gSensorLogPath;
static uint32_t gSensorLogIntervalMs;
static uint8_t gSensorLogMask, gSensorLogMaxRotations;
static SensorLogFormat gSensorLogFormat;
static size_t gSensorLogMaxSize;
static unsigned persistCalls;
static char debugBuffer[1024];
static std::vector<std::string> actions, hints;
static std::vector<const void*> changedFields;
static std::vector<bool> persistSawRunning;
bool requestSettingsPersist() {
  ++persistCalls;
  persistSawRunning.push_back(gSensorLoggingRunning);
  return true;
}
void notifySettingChanged(const void* field) { changedFields.push_back(field); }
struct SettingEntry;
void finishSettingChange(const void* field, const SettingEntry*) {
  (void)requestSettingsPersist();
  notifySettingChanged(field);
}
void cliHint(const char* text) { hints.emplace_back(text); }
#if TEST_GPS
static const char* borrowed(const char* text) {
  if (!text) return nullptr;
  std::snprintf(debugBuffer, sizeof(debugBuffer), "%s", text);
  return debugBuffer;
}

enum { I2C_DEVICE_GPS = 4 };
static bool gGpsRunning;
static const char* queueReply;
static const char* formatReply;
static const char* intervalReply;
static const char* startupReply;
static const char* autostartReply;
static unsigned queuedRequests, configuredStarts;
static bool startSawAutoStart;
const char* cmd_sensorstart_queued(int device, const char* name, const char* origin) {
  if (device != I2C_DEVICE_GPS || std::strcmp(name, "GPS") ||
      std::strcmp(origin, "opengps@enqueue")) std::abort();
  ++queuedRequests;
  actions.emplace_back("gps request");
  return borrowed(queueReply);
}
static bool succeeded(const char* response) {
  return response && std::strncmp(response, "Error", 5) != 0;
}
const char* cmd_sensorlog(const String& input) {
  actions.emplace_back(input.c_str());
  if (input == "format track") {
    if (succeeded(formatReply)) {
      gSensorLogFormat = SENSOR_LOG_TRACK;
      gSettings.sensorLogFormat = SENSOR_LOG_TRACK;
      gSensorLogMask = LOG_GPS;
      gSettings.sensorLogMask = LOG_GPS;
    }
    return borrowed(formatReply);
  }
  if (input.startsWith("interval ")) {
    if (succeeded(intervalReply)) {
      gSensorLogIntervalMs = static_cast<uint32_t>(input.substring(9).toInt());
      gSettings.sensorLogIntervalMs = static_cast<int>(gSensorLogIntervalMs);
    }
    return borrowed(intervalReply);
  }
  if (input == "autostart on") {
    if (succeeded(autostartReply)) gSettings.sensorLogAutoStart = true;
    return borrowed(autostartReply);
  }
  std::abort();
}
const char* sensorLogStartConfiguredSession() {
  ++configuredStarts;
  actions.emplace_back("logger start");
  startSawAutoStart = gSettings.gpsAutoStart || gSettings.sensorLogAutoStart;
  if (startupReply && std::strncmp(startupReply, "SUCCESS:", 8) == 0) {
    gSensorLoggingRunning = true;
    gSensorLogPath = "/logs/a quoted \"trip\"/track.track";
  }
  return borrowed(startupReply);
}
#else
using portMUX_TYPE = int;
static portMUX_TYPE sSensorLogSessionMux;
static uint32_t sSensorLogSessionId;
[[maybe_unused]] static bool gSensorLogTransient;
static void sensorLogRestoreSettings();
static void portENTER_CRITICAL(portMUX_TYPE*) {}
static void portEXIT_CRITICAL(portMUX_TYPE*) {}
static bool debugAvailable, keyAvailable, headerWritable, fileOpenable;
static bool wantsSealing, pathExists, sdAvailable;
static unsigned freeHeap;
static uint64_t flashFree, sdFree;
static bool gSensorLogSealed;
static uint32_t gSensorLogLastWrite;
static size_t gApproxSizeBytes;
static unsigned resolvedCalls, shapedCalls, fileCloses, fileRemoves;
static std::string resolvedPath, shapedInput, classifiedPath;
static std::vector<std::string> outputs, logEvents;
static std::vector<int> systemEvents;
enum { SYSEVT_SENSOR_START_FAILED = 1, SYSEVT_SENSOR_STARTED = 2, SYSEVT_SENSOR_STOPPED = 3 };
bool ensureDebugBuffer() { return debugAvailable; }
char* getDebugBuffer() { return debugBuffer; }
void broadcastOutput(const String& text) { outputs.emplace_back(text.c_str()); }
void broadcastPrintf(const char* format, ...) {
  char text[1024];
  va_list args;
  va_start(args, format);
  std::vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  outputs.emplace_back(text);
}
#define BROADCAST_PRINTF(...) broadcastPrintf(__VA_ARGS__)
void cliHintf(const char*, ...) {}
void logSystemEvent(const char*, const char* format, ...) {
  char text[1024];
  va_list args;
  va_start(args, format);
  std::vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  logEvents.emplace_back(text);
}
void systemEventPost(int event, const char*) { systemEvents.push_back(event); }
unsigned long millis() { return 1234; }
struct File {
  bool prepared = true;
  explicit operator bool() const { return fileOpenable && prepared; }
  void close() { ++fileCloses; }
};
struct FsLockGuard { explicit FsLockGuard(const char*) {} };
namespace VFS {
enum Storage { INTERNAL, SDCARD };
const char* systemAuth(const char* tag) { return tag; }
bool existsGuarded(const String&, const char*) { return pathExists; }
void mkdirGuarded(const String&, const char*) { pathExists = true; }
File openGuarded(const String&, const char*, const char*, bool) { return {}; }
void removeGuarded(const String&, const char*) { ++fileRemoves; }
bool getStats(Storage storage, uint64_t& total, uint64_t& used, uint64_t& free) {
  total = 1024 * 1024 * 32;
  free = storage == INTERNAL ? flashFree : sdFree;
  used = total - free;
  return true;
}
bool isSDAvailable() { return sdAvailable; }
}
struct { unsigned getFreeHeap() { return freeHeap; } } ESP;
String resolveSessionTarget(const String& path) {
  ++resolvedCalls;
  resolvedPath = path.c_str();
  return path;
}
bool sessionWantsSealing(uint8_t) { return wantsSealing; }
bool captureCryptoEnsureKey() { return keyAvailable; }
bool writeHeaderChecked(File&) { return headerWritable; }
File openSensorLogForAppend(const String&) { return {headerWritable}; }
void sensorLogClassifyActivePath(const char* path) { classifiedPath = path; }
size_t sensorLogResolvedSize(const String&) { return 42; }
String stripSessionShaping(const String& path) { return path; }
String shapeSessionPath(const String& path) {
  ++shapedCalls;
  shapedInput = path.c_str();
  return path + ".shaped";
}
#endif

// INSERT_PRODUCTION

static int checks, failures;
static std::string context;
static void check(bool ok, const char* message) {
  ++checks;
  if (ok) return;
  ++failures;
  if (failures < 50) std::fprintf(stderr, "%s: %s\n", context.c_str(), message);
}
static bool error(const std::string& text) { return text.rfind("Error", 0) == 0; }
static void reset() {
  gSettings = {true, false, false, false, LOG_GPS, SENSOR_LOG_TRACK,
               1000, 256000, 3, String("/logs/saved track.track")};
  gSensorLogMask = LOG_TOF;
  gSensorLogFormat = SENSOR_LOG_CSV;
  gSensorLogIntervalMs = 5000;
  gSensorLogMaxSize = 10240;
  gSensorLogMaxRotations = 1;
  gSensorLogPath = "/logs/previous.csv";
  gSensorLoggingRunning = gCLIValidateOnly = false;
  persistCalls = 0;
  actions.clear();
  hints.clear();
  changedFields.clear();
  persistSawRunning.clear();
  std::memset(debugBuffer, 0, sizeof(debugBuffer));
#if TEST_GPS
  queueReply = "[GPS] Start queued; initialization is pending";
  formatReply = "Log format set to track";
  intervalReply = "Logging interval set to 1000ms";
  startupReply = "SUCCESS: Sensor logging started";
  autostartReply = "Sensor logging auto-start enabled";
  queuedRequests = configuredStarts = 0;
  startSawAutoStart = false;
  gGpsRunning = false;
#else
  sSensorLogSessionId = 0;
  debugAvailable = keyAvailable = headerWritable = fileOpenable = pathExists = true;
  wantsSealing = sdAvailable = gSensorLogSealed = false;
  freeHeap = 65536;
  flashFree = sdFree = 16 * 1024 * 1024;
  gSensorLogLastWrite = gApproxSizeBytes = 0;
  resolvedCalls = shapedCalls = fileCloses = fileRemoves = 0;
  resolvedPath.clear();
  shapedInput.clear();
  classifiedPath.clear();
  outputs.clear();
  logEvents.clear();
  systemEvents.clear();
#endif
}

#if TEST_GPS
static void untouched() {
  check(queuedRequests == 0 && configuredStarts == 0 && actions.empty(), "preflight requested a service");
  check(gSensorLogMask == LOG_TOF && gSensorLogFormat == SENSOR_LOG_CSV &&
        gSensorLogIntervalMs == 5000, "preflight changed live logger parameters");
  check(gSettings.sensorLogMask == LOG_GPS && gSettings.sensorLogFormat == SENSOR_LOG_TRACK &&
        gSettings.sensorLogIntervalMs == 1000, "preflight changed saved logger parameters");
  check(!gSettings.gpsAutoStart && !gSettings.sensorLogAutoStart && persistCalls == 0,
        "preflight enabled or persisted auto-start");
}
static void invalidAndGuards() {
  for (const char* arg : {"abc", "99", "3600001", "-100", "0", "1000ms", "1000 2000", "1000\t500", "1e3",
                         "100.0", "\"1000\"", "99999999999999999999999999999999999999"}) {
    context = std::string("invalid interval: ") + arg;
    reset();
    check(error(cmd_gpslog(arg)), "invalid interval accepted");
    untouched();
  }
  for (int guard = 0; guard < 3; ++guard) {
    context = "startup preflight " + std::to_string(guard);
    reset();
    if (guard == 0) gSettings.sensorLogEnabled = false;
    if (guard == 1) gSensorLoggingRunning = true;
    if (guard == 2) gSettings.healthLoggingEnabled = true;
    const std::string result = cmd_gpslog("");
    check(error(result), "policy conflict accepted");
    check(result.find(guard == 0 ? "disabled" : guard == 1 ? "already running" : "Health") != std::string::npos,
          "policy conflict lost useful reason");
    untouched();
    check(gSensorLoggingRunning == (guard == 1), "preflight stopped/started existing logger");
    check(gSettings.healthLoggingEnabled == (guard == 2), "preflight changed health policy");
  }
  context = "validation-only";
  reset();
  gCLIValidateOnly = true;
  check(std::string(cmd_gpslog("nonsense")) == "VALID", "validation-only response changed");
  untouched();
}
static void acceptedStarts() {
  for (const char* arg : {"", " \t ", "100", "1000", "3600000", " 500 \r\n", "+200", "000300"}) {
    for (bool alreadyRunning : {false, true}) {
      context = std::string("accepted interval ") + arg + (alreadyRunning ? " ready" : " pending");
      reset();
      gGpsRunning = alreadyRunning;
      queueReply = alreadyRunning ? "[GPS] Already running" : "[GPS] Start queued; initialization is pending";
      const std::string queueText = queueReply;
      const std::string result = cmd_gpslog(arg);
      String trimmed(arg); trimmed.trim();
      const unsigned expected = trimmed.length() ? static_cast<unsigned>(trimmed.toInt()) : 1000;
      check(result.rfind("[gpslog] Logger active", 0) == 0, "successful logger not reported active");
      check(result.find(queueText) != std::string::npos, "borrowed GPS outcome overwritten or invented readiness");
      check(gGpsRunning == alreadyRunning, "queued GPS was presented by changing readiness state");
      check(result.find("interval=" + std::to_string(expected) + "ms") != std::string::npos, "interval response incorrect");
      check(result.find(gSensorLogPath.c_str()) != std::string::npos, "actual logger path omitted");
      check(queuedRequests == 1 && configuredStarts == 1, "queue/start adapter not called exactly once");
      check(actions == std::vector<std::string>({"gps request", "format track", "interval " + std::to_string(expected),
                                               "logger start", "autostart on"}), "operation order changed");
      check(gSensorLogMask == LOG_GPS && gSensorLogFormat == SENSOR_LOG_TRACK &&
            gSensorLogIntervalMs == expected, "owner commands did not receive track parameters");
      check(!startSawAutoStart && gSettings.gpsAutoStart && gSettings.sensorLogAutoStart,
            "auto-start not deferred until success");
      check(persistCalls == 1 && persistSawRunning[0], "GPS auto-start saved before logger was running");
    }
  }
}
static void failedSteps() {
  for (int step = 0; step < 4; ++step) {
    for (const char* failure : {static_cast<const char*>(nullptr), "Error: injected owner failure"}) {
      for (bool existingAutoStart : {false, true}) {
        context = "failed step " + std::to_string(step);
        reset();
        gSettings.gpsAutoStart = gSettings.sensorLogAutoStart = existingAutoStart;
        if (step == 0) queueReply = failure;
        if (step == 1) formatReply = failure;
        if (step == 2) intervalReply = failure;
        if (step == 3) startupReply = failure;
        const std::string result = cmd_gpslog("500");
        check(error(result), "owner error returned success/active");
        if (failure) check(result == failure, "owner error text overwritten");
        check(actions.size() == static_cast<size_t>(step + 1), "execution continued after failed step");
        check(!gSensorLoggingRunning, "failed start reported logger running");
        check(gSettings.gpsAutoStart == existingAutoStart && gSettings.sensorLogAutoStart == existingAutoStart,
              "failure changed pre-existing auto-start preferences");
        check(persistCalls == 0, "failed setup newly persisted GPS auto-start");
      }
    }
  }
  context = "non-error does not confirm logger start";
  reset();
  startupReply = "start requested";
  check(error(cmd_gpslog("")), "unconfirmed logger start reported active");
  check(!gSettings.gpsAutoStart && !gSettings.sensorLogAutoStart, "unconfirmed start enabled auto-start");
  context = "late autostart error retains actual running state";
  reset();
  autostartReply = "Error: autostart refused";
  check(std::string(cmd_gpslog("")) == autostartReply, "late error hidden behind active banner");
  check(gSensorLoggingRunning && gSettings.gpsAutoStart && !gSettings.sensorLogAutoStart,
        "late error fabricated rollback of completed operations");
}
#else
static void manualAndBootPaths() {
  for (const char* path : {"", "relative.track", "/logs/simple.track", "/logs/a space/track.track", "/logs/a \"quoted\" track.track"}) {
    for (bool boot : {false, true}) {
      context = std::string(boot ? "boot literal path: " : "manual literal path: ") + path;
      reset();
      gSettings.sensorLogPath = path;
      if (boot) { gSettings.sensorLogAutoStart = true; sensorLogAutoStart(); }
      else check(std::string(sensorLogStartConfiguredSession()).rfind("SUCCESS:", 0) == 0, "configured start failed");
      const std::string expected = path[0] == '/' ? path : CAPTURE_SENSORLOG_DEFAULT;
      check(shapedCalls == 1 && shapedInput == expected, "configured/default path not shaped exactly once");
      check(resolvedCalls == 1 && resolvedPath == expected + ".shaped", "literal path changed through CLI tokenization");
      check(gSensorLoggingRunning && gSensorLogPath == (expected + ".shaped").c_str(), "session path not published");
      check(gSensorLogMask == LOG_GPS && gSensorLogFormat == SENSOR_LOG_TRACK &&
            gSensorLogIntervalMs == 1000 && gSensorLogMaxSize == 256000 && gSensorLogMaxRotations == 3,
            "saved parameters not restored");
      check(gSensorLogLastWrite == 1234 && gApproxSizeBytes == 42 && classifiedPath == resolvedPath,
            "typed start lost session accounting");
      check(systemEvents == std::vector<int>({SYSEVT_SENSOR_STARTED}), "successful start event missing");
      check(!gSettings.gpsAutoStart && gSettings.sensorLogAutoStart == boot, "manual start changed boot enable policy");
    }
  }
  for (bool disabled : {false, true}) {
    context = "manual preflight preserves divergent state";
    reset();
    gSettings.sensorLogEnabled = !disabled;
    gSensorLoggingRunning = !disabled;
    check(error(sensorLogStartConfiguredSession()), "disabled/running manual start accepted");
    check(gSensorLogMask == LOG_TOF && gSensorLogFormat == SENSOR_LOG_CSV &&
          gSensorLogIntervalMs == 5000 && shapedCalls == 0 && resolvedCalls == 0 && persistCalls == 0,
          "manual preflight changed existing session parameters");
  }
  context = "boot disabled by flags";
  reset();
  sensorLogAutoStart();
  check(shapedCalls == 0 && gSensorLogMask == LOG_TOF && persistCalls == 0, "boot flag guard changed state");
  context = "boot already running";
  reset();
  gSensorLoggingRunning = gSettings.sensorLogAutoStart = true;
  sensorLogAutoStart();
  check(shapedCalls == 0 && gSensorLogMask == LOG_TOF && persistCalls == 0, "boot replaced existing session");
  context = "boot no sensors";
  reset();
  gSettings.sensorLogAutoStart = true;
  gSettings.sensorLogMask = gSensorLogMask = 0;
  sensorLogAutoStart();
  check(shapedCalls == 0 && !gSensorLoggingRunning && persistCalls == 0, "boot zero-selection guard changed");
}
static void legacyRestoreBounds() {
  for (int interval : {-1, 0, 99, 100, 3600000, 3600001, std::numeric_limits<int>::max()}) {
    for (bool boot : {false, true}) {
      context = "saved interval " + std::to_string(interval);
      reset();
      gSettings.sensorLogIntervalMs = interval;
      if (boot) { gSettings.sensorLogAutoStart = true; sensorLogAutoStart(); }
      else (void)sensorLogStartConfiguredSession();
      const unsigned expected = interval < 100 ? 5000 : interval > 3600000 ? 3600000 : static_cast<unsigned>(interval);
      check(gSensorLoggingRunning && gSensorLogIntervalMs == expected &&
            gSettings.sensorLogIntervalMs == static_cast<int>(expected), "legacy startup interval limits changed");
    }
  }
  for (int size : {10239, 10240, 10485760, 10485761}) {
    context = "saved maximum size " + std::to_string(size);
    reset();
    gSettings.sensorLogMaxSize = size;
    (void)sensorLogStartConfiguredSession();
    check(gSensorLogMaxSize == static_cast<size_t>(size >= 10240 && size <= 10485760 ? size : 10240),
          "maximum size restoration bounds changed");
  }
  for (int rotations : {-1, 0, 9, 10}) {
    context = "saved rotations " + std::to_string(rotations);
    reset();
    gSettings.sensorLogMaxRotations = rotations;
    (void)sensorLogStartConfiguredSession();
    check(gSensorLogMaxRotations == (rotations >= 0 && rotations <= 9 ? rotations : 1),
          "rotation restoration bounds changed");
  }
  for (int mask : {-1, 0, 1, 255, 256}) {
    context = "legacy saved mask restoration";
    reset();
    gSettings.sensorLogMask = mask;
    (void)sensorLogStartConfiguredSession();
    check(gSensorLogMask == (mask > 0 ? static_cast<uint8_t>(mask) : LOG_TOF),
          "saved mask legacy positive-only restoration changed");
  }
}
static void healthBootPolicy() {
  for (SensorLogFormat format : {SENSOR_LOG_TEXT, SENSOR_LOG_CSV, SENSOR_LOG_TRACK}) {
    for (const char* path : {"/logs/track.track", "/my health folder/vitals.csv"}) {
      for (bool boot : {false, true}) {
        context = "health boot/manual policy";
        reset();
        gSettings.healthLoggingEnabled = true;
        gSettings.sensorLogPath = path;
        gSettings.sensorLogFormat = format;
        if (boot) sensorLogAutoStart();
        else (void)sensorLogStartConfiguredSession();
        check(gSensorLoggingRunning, "health policy path did not start");
        check(gSensorLogMask == (boot ? LOG_GPS | LOG_R1 : LOG_GPS), "manual start applied boot R1 policy");
        check(gSensorLogFormat == (boot ? SENSOR_LOG_CSV : format), "health boot CSV/manual format policy changed");
        const std::string expected = boot && std::string(path).find("health") == std::string::npos ? CAPTURE_HEALTHLOG_DEFAULT : path;
        check(shapedInput == expected, "manual start applied health path repair or boot lost repair");
        check(gSettings.healthLoggingEnabled && !gSettings.sensorLogAutoStart,
              "startup changed boot policy preference");
      }
    }
  }
}
static void admissionFailuresAndAdapter() {
  for (int failure = 0; failure < 6; ++failure) {
    for (bool boot : {false, true}) {
      context = "owner admission failure " + std::to_string(failure);
      reset();
      if (failure == 0) debugAvailable = false;
      if (failure == 1) { wantsSealing = true; keyAvailable = false; }
      if (failure == 2) freeHeap = 8191;
      if (failure == 3) flashFree = 0;
      if (failure == 4) headerWritable = false;
      if (failure == 5) fileOpenable = false;
      if (boot) { gSettings.sensorLogAutoStart = true; sensorLogAutoStart(); }
      else check(error(sensorLogStartConfiguredSession()), "admission error hidden");
      check(!gSensorLoggingRunning && gSensorLogPath == "/logs/previous.csv", "failed start published live session");
      check(sensorLogCurrentSessionId() == 0 && sSensorLogSessionId == 0,
            "failed start consumed a logger session identity");
      check(persistCalls == 0, "failed admission persisted session parameters");
      if (boot) check(!logEvents.empty() && logEvents.back().find("FAILED") != std::string::npos, "boot failed start logged OK");
    }
  }
  for (const char* interval : {"", " 99", " 3600001", " -1", " bad", " 777"}) {
    context = "explicit command adapter";
    reset();
    const String command = String("start \"/logs/literal path.csv\"") + interval;
    check(std::string(commandStart(command)).rfind("SUCCESS:", 0) == 0, "command start adapter failed");
    check(resolvedPath == "/logs/literal path.csv", "command adapter changed quoted path");
    const unsigned expected = !std::strcmp(interval, " 99") ? 100 :
        (!std::strcmp(interval, " 3600001") || !std::strcmp(interval, " -1")) ? 3600000 :
        !std::strcmp(interval, " 777") ? 777 : 5000;
    check(gSensorLogIntervalMs == expected, "explicit command parsing/clamping changed");
  }
  context = "explicit command error precedence";
  reset();
  gSettings.sensorLogEnabled = false;
  check(std::string(commandStart("start")).find("disabled") != std::string::npos, "missing args bypassed master guard");
  reset();
  gSensorLoggingRunning = true;
  check(std::string(commandStart("start")).find("already running") != std::string::npos, "missing args bypassed running guard");
  reset();
  check(std::string(commandStart("start")).find("Usage:") != std::string::npos, "missing path lost usage error");
  check(error(commandStart("start relative.csv")) && resolvedCalls == 0 && persistCalls == 0,
        "relative path reached filesystem or changed persistence");
}
static void sessionOwnership() {
  context = "session identity and guarded stop";
  reset();
  check(sensorLogCurrentSessionId() == 0 && !sensorLogStopSession(0),
        "idle session exposes or accepts an owner token");
  check(!error(commandStart("start /logs/reused.csv 1000")), "first owned start failed");
  const uint32_t first = sensorLogCurrentSessionId();
  check(first != 0, "successful start has no identity");
  check(!sensorLogStopSession(first + 1) && gSensorLoggingRunning,
        "wrong token stopped active logger");
  check(sensorLogStopSession(first) && sensorLogCurrentSessionId() == 0,
        "matching token did not stop logger");
  const size_t eventsAfterStop = systemEvents.size();
  check(!sensorLogStopSession(first) && systemEvents.size() == eventsAfterStop,
        "repeated stop posted a duplicate event");
  check(!error(commandStart("start /logs/reused.csv 1000")), "replacement start failed");
  const uint32_t replacement = sensorLogCurrentSessionId();
  check(replacement != 0 && replacement != first, "same path reused previous identity");
  check(!sensorLogStopSession(first) && sensorLogCurrentSessionId() == replacement,
        "old consumer stopped unrelated same-path replacement");
  gSensorLogPath = "/logs/rolled.csv";
  check(sensorLogCurrentSessionId() == replacement,
        "rolling a file changed its session owner identity");
  check(sensorLogStopSession(replacement), "replacement owner could not stop");
  sSensorLogSessionId = UINT32_MAX;
  check(!error(commandStart("start /logs/reused.csv 1000")) && sensorLogCurrentSessionId() == 1,
        "session identity wrap published the zero sentinel");
}
#if HAVE_LIVE_TRACK_START
static void configuredSession() {
  gSettings.sensorLogMask = LOG_TOF | LOG_IMU;
  gSettings.sensorLogFormat = SENSOR_LOG_CSV;
  gSettings.sensorLogIntervalMs = 5000;
  gSettings.sensorLogPath = "/logging_captures/sensors/sensors.csv";
  gSensorLogMask = LOG_TOF | LOG_IMU;
  gSensorLogFormat = SENSOR_LOG_CSV;
  gSensorLogIntervalMs = 5000;
}
static void savedSessionUntouched() {
  check(gSettings.sensorLogMask == (LOG_TOF | LOG_IMU) && gSettings.sensorLogFormat == SENSOR_LOG_CSV &&
        gSettings.sensorLogIntervalMs == 5000 && gSettings.sensorLogPath == "/logging_captures/sensors/sensors.csv",
        "live track changed the saved last-used session");
  check(persistCalls == 0 && changedFields.empty(), "live track requested settings persistence");
}
static void liveSelectionRestored() {
  check(gSensorLogFormat == SENSOR_LOG_CSV && gSensorLogMask == (LOG_TOF | LOG_IMU) && gSensorLogIntervalMs == 5000,
        "ending the live track did not restore the saved selection");
}
static void liveTrackTransientSession() {
  context = "live track start leaves the saved session untouched";
  reset(); configuredSession();
  const std::string started = sensorLogStartLiveTrack("/logging_captures/tracks/live.csv", 1000);
  check(started.rfind("SUCCESS:", 0) == 0, "live track start refused");
  check(gSensorLoggingRunning && gSensorLogPath == "/logging_captures/tracks/live.csv" &&
        resolvedPath == "/logging_captures/tracks/live.csv" && shapedCalls == 0,
        "live track did not use the literal path");
  check(gSensorLogFormat == SENSOR_LOG_TRACK && gSensorLogMask == LOG_GPS && gSensorLogIntervalMs == 1000,
        "live capture did not switch the live values to a GPS-only TRACK session");
  savedSessionUntouched();
  const uint32_t session = sensorLogCurrentSessionId();
  check(session != 0 && sensorLogStopSession(session), "owner stop of the live session failed");
  liveSelectionRestored();
  savedSessionUntouched();

  context = "an external stop also restores the saved selection";
  reset(); configuredSession();
  check(!error(sensorLogStartLiveTrack("/logging_captures/tracks/live.csv", 1000)), "second live start refused");
  check(sensorLogMarkStopped(), "unconditional owner stop failed");
  liveSelectionRestored();
  savedSessionUntouched();

  context = "a refused live start changes nothing";
  reset(); configuredSession();
  fileOpenable = false;
  check(error(sensorLogStartLiveTrack("/logging_captures/tracks/live.csv", 1000)) && !gSensorLoggingRunning,
        "refused live start reported success");
  liveSelectionRestored();
  savedSessionUntouched();
  reset(); configuredSession();
  check(error(sensorLogStartLiveTrack("relative.csv", 1000)) && resolvedCalls == 0, "relative live path accepted");
  liveSelectionRestored();

  context = "typed starts still persist, and their stop restores nothing";
  reset(); configuredSession();
  check(!error(commandStart("start /logs/typed.csv 700")), "typed start refused");
  check(gSettings.sensorLogPath == "/logs/typed.csv" && gSettings.sensorLogIntervalMs == 700 &&
        gSettings.sensorLogMask == (LOG_TOF | LOG_IMU) && persistCalls == 2, "typed start no longer persists its session");
  gSensorLogFormat = SENSOR_LOG_TEXT;  // a later live change that no stop may undo
  check(sensorLogMarkStopped() && gSensorLogFormat == SENSOR_LOG_TEXT && gSensorLogIntervalMs == 700,
        "stopping a persisted session restored saved values");
}
#endif
#endif

int main() {
  const auto run = [](const char* name, void (*test)()) {
    int beforeChecks = checks, beforeFailures = failures;
    test();
    std::printf("  %s: %d checks, %d failures\n", name, checks - beforeChecks, failures - beforeFailures);
  };
#if TEST_GPS
  run("GPS preflight and validation", invalidAndGuards);
  run("GPS queued/ready startup and borrowed results", acceptedStarts);
  run("GPS owner failures and existing preferences", failedSteps);
#else
  run("configured/boot literal paths and preflight", manualAndBootPaths);
  run("legacy saved-parameter restoration", legacyRestoreBounds);
  run("manual versus boot health policy", healthBootPolicy);
  run("typed admission and command adapter", admissionFailuresAndAdapter);
  run("logger session ownership", sessionOwnership);
#if HAVE_LIVE_TRACK_START
  run("transient live-track session", liveTrackTransientSession);
#endif
#endif
  std::printf("gpslog startup (%s): %d checks, %d failures\n", TEST_GPS ? "GPS" : "owner", checks, failures);
  return failures ? 1 : 0;
}
