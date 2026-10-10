// The runner inserts production types, parsing, settings and selection helpers.
// String storage/debug allocation/persistence/notifications are host boundaries.
#include <Arduino.h>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

void webBatchTestStringAppend() {}

// INSERT_TYPES

static uint8_t gSensorLogMask;
static bool gSensorLoggingRunning;
static SensorLogFormat gSensorLogFormat;
static String gSensorLogPath = "/logs/test.txt";
static uint32_t gSensorLogIntervalMs = 5000;
static size_t gSensorLogMaxSize = 256000;
static uint8_t gSensorLogMaxRotations = 3;
static unsigned long gSensorLogLastWrite = 1000;
static unsigned long millis() { return 6000; }
static struct { int sensorLogMask; int unrelated; bool sensorLogAutoStart; } gSettings;
static bool bufferAvailable;
static char debugBuffer[1024];
static std::vector<std::string> hints;
static int persistCalls, notifyCalls;
static const void* notifiedField;
static bool ensureDebugBuffer() { return bufferAvailable; }
static char* getDebugBuffer() { return debugBuffer; }
static void cliHint(const char* text) { hints.emplace_back(text); }
static bool requestSettingsPersist() { ++persistCalls; return true; }
static void notifySettingChanged(const void* field) {
  ++notifyCalls;
  notifiedField = field;
}
struct SettingEntry;
static void finishSettingChange(const void* field, const SettingEntry*) {
  (void)requestSettingsPersist();
  notifySettingChanged(field);
}

// INSERT_PRODUCTION

static int checks, failures;
static std::string context;
static void check(bool condition, const char* message) {
  ++checks;
  if (condition) return;
  ++failures;
  if (failures <= 30) std::fprintf(stderr, "%s: %s\n", context.c_str(), message);
}
static const std::array<const char*, 8> names = {
  "thermal", "tof", "imu", "gamepad", "apds", "gps", "presence", "r1"
};
static const std::array<const char*, 8> labels = {
  "Thermal", "ToF", "IMU", "Gamepad", "APDS", "GPS", "Presence", "R1 Health"
};
static void reset(uint8_t mask, bool running = false,
                  SensorLogFormat format = SENSOR_LOG_TEXT) {
  gSensorLogMask = mask;
  gSettings.sensorLogMask = mask;
  gSettings.unrelated = 54321;
  gSettings.sensorLogAutoStart = false;
  gSensorLoggingRunning = running;
  gSensorLogFormat = format;
  bufferAvailable = true;
  persistCalls = notifyCalls = 0;
  notifiedField = nullptr;
  hints.clear();
  std::memset(debugBuffer, 0, sizeof(debugBuffer));
}
static std::string select(const std::string& input) {
  context = input;
  return sensorLogSelectSensors(CommandArgs(String(input)));
}
static bool isError(const std::string& result) { return result.rfind("Error:", 0) == 0; }
static void unchanged(uint8_t mask, bool running, SensorLogFormat format) {
  check(gSensorLogMask == mask && sensorLogSelectedMask() == mask, "live selection changed");
  check(gSettings.sensorLogMask == mask, "saved selection changed");
  check(persistCalls == 0 && notifyCalls == 0, "unexpected persistence/notification");
  check(gSettings.unrelated == 54321, "unrelated setting changed");
  check(gSensorLoggingRunning == running && gSensorLogFormat == format, "session policy changed");
}
static void changed(uint8_t mask, int writes) {
  check(gSensorLogMask == mask && sensorLogSelectedMask() == mask, "wrong live selection");
  check(gSettings.sensorLogMask == mask, "wrong saved selection");
  check(persistCalls == writes && notifyCalls == writes, "wrong persistence/notification count");
  check(!writes || notifiedField == &gSettings.sensorLogMask, "wrong notified setting");
  check(gSettings.unrelated == 54321 && hints.empty(), "unrelated effects");
}
static std::string expectedNames(uint8_t mask) {
  std::string result;
  for (size_t i = 0; i < names.size(); ++i) {
    if (mask & (1u << i)) result += std::string(names[i]) + " ";
  }
  return result;
}

static void catalogAndQuery() {
  context = "shared catalog";
  check(kSensorLogSensorCount == names.size(), "catalog count changed");
  for (size_t i = 0; i < kSensorLogSensorCount; ++i) {
    check(kSensorLogSensors[i].bit == (1u << i), "catalog bit/order changed");
    check(std::strcmp(kSensorLogSensors[i].name, names[i]) == 0, "catalog name changed");
    check(std::strcmp(kSensorLogSensors[i].label, labels[i]) == 0, "catalog label changed");
  }
  for (unsigned mask = 0; mask <= 255; ++mask) {
    reset(static_cast<uint8_t>(mask), true, SENSOR_LOG_CSV);
    const std::string result = select("sensors");
    std::string expected = "Selected sensors:\n";
    for (size_t i = 0; i < names.size(); ++i)
      expected += std::string("  ") + ((mask & (1u << i)) ? "☑ " : "☐ ") + labels[i] + "\n";
    expected += "\nUsage: sensorlog sensors <thermal|tof|imu|gamepad|apds|gps|presence|r1|all|none>";
    check(result.rfind(expected, 0) == 0, "query selection/order/usage changed");
    check(result.find("sensorlog sensors toggle <sensor>") != std::string::npos, "toggle usage absent");
    unchanged(static_cast<uint8_t>(mask), true, SENSOR_LOG_CSV);
    check(hints.empty(), "read-only query produced CSV warning");
  }
}

static void replacementGrammar() {
  struct Example { const char* input; uint8_t mask; };
  const Example examples[] = {
    {"sensors thermal", LOG_THERMAL}, {"sensors ToF, IMU", LOG_TOF | LOG_IMU},
    {"sensors \t gps ,  PRESENCE \r\n", LOG_GPS | LOG_PRESENCE},
    {"sensors input", LOG_GAMEPAD}, {"sensors health", LOG_R1},
    {"sensors ring", LOG_R1}, {"sensors r1,health,ring", LOG_R1},
    {"sensors gamepad,INPUT", LOG_GAMEPAD},
    {"sensors thermal,tof,imu,gamepad,apds,gps,presence,r1", 255},
    {"sensors gps,", LOG_GPS}, {"sensors gps,   ", LOG_GPS},
  };
  for (const auto& example : examples) {
    for (uint8_t initial : {uint8_t(0), uint8_t(255), example.mask}) {
      reset(initial);
      check(select(example.input) == "Logging enabled for: " + expectedNames(example.mask), "replacement response mismatch");
      changed(example.mask, initial != example.mask);
    }
  }
  for (const char* input : {"sensors all", "sensors ALL", "sensors \t ALL \r\n"}) {
    reset(LOG_GPS);
    check(select(input) == "All sensors enabled for logging", "all response changed");
    changed(255, 1);
    check(select(input) == "All sensors enabled for logging", "repeat all response changed");
    changed(255, 1);
  }
  for (const char* input : {"sensors none", "sensors NONE", "sensors \t none \r\n"}) {
    reset(255);
    check(select(input) == "All sensors disabled for logging", "none response changed");
    changed(0, 1);
    check(select(input) == "All sensors disabled for logging", "repeat none response changed");
    changed(0, 1);
  }
}

static void statusFormatting() {
  for (unsigned mask = 0; mask <= 255; ++mask) {
    for (SensorLogFormat format : {SENSOR_LOG_TEXT, SENSOR_LOG_CSV, SENSOR_LOG_TRACK}) {
      for (bool running : {false, true}) {
        reset(static_cast<uint8_t>(mask), running, format);
        gSettings.sensorLogAutoStart = running;
        context = "status mask=" + std::to_string(mask) + " format=" + std::to_string(format);
        const std::string result = sensorLogStatusForTest();
        const std::string selection = mask ? expectedNames(static_cast<uint8_t>(mask)) : "(none)";
        check(result.find("  Sensors: " + selection + "\n") != std::string::npos, "status sensor names changed");
        const char* formatNames[] = {"TEXT", "CSV", "TRACK"};
        check(result.find(std::string("  Format: ") + formatNames[format] + "\n") != std::string::npos, "status format label changed");
        check(result.rfind(running ? "Sensor logging ACTIVE\n" : "Sensor logging is INACTIVE\n", 0) == 0, "status running label changed");
        check(result.find(running ? "  Auto-start: ON" : "  Auto-start: OFF") != std::string::npos, "status auto-start label changed");
        if (running) {
          check(result.find("  File: /logs/test.txt\n  Interval: 5000ms\n") != std::string::npos, "status path or interval changed");
          check(result.find("  Last write: 5s ago") != std::string::npos, "status elapsed time changed");
        }
        unchanged(static_cast<uint8_t>(mask), running, format);
      }
    }
  }
}

static void invalidIsTransactional() {
  const std::vector<std::string> invalid = {
    "sensors bogus", "sensors thermal,bogus", "sensors gps,imu,bogus",
    "sensors ,gps", "sensors gps,,imu", "sensors ,", "sensors gps all",
    "sensors gps,all", "sensors gps,none", "sensors all,gps", "sensors none,gps",
    "sensors \"gps\"", "sensors \"\"", "sensors gps," + std::string(2048, 'x'),
    "sensors toggle", "sensors toggle none", "sensors toggle all",
    "sensors toggle bogus", "sensors toggle gps imu", "sensors toggle gps,imu",
    "sensors toggle \"gps imu\"", "sensors toggle \"\"", "sensors toggle \"gps"
  };
  for (const auto& input : invalid) {
    for (uint8_t initial : {uint8_t(0), uint8_t(LOG_TOF | LOG_PRESENCE), uint8_t(255)}) {
      reset(initial);
      check(isError(select(input)), "invalid selection accepted");
      unchanged(initial, false, SENSOR_LOG_TEXT);
      check(hints.empty(), "invalid selection changed hint policy");
    }
  }
  reset(LOG_GPS);
  check(select("sensors thermal,BOGUS") == "Error: Unknown sensor 'bogus'", "invalid-name response changed");
  unchanged(LOG_GPS, false, SENSOR_LOG_TEXT);
}

static void togglesResolveAtExecution() {
  for (unsigned initial = 0; initial <= 255; ++initial) {
    for (size_t i = 0; i < names.size(); ++i) {
      reset(static_cast<uint8_t>(initial));
      const std::string request = std::string("sensors toggle ") + names[i];
      const uint8_t after = static_cast<uint8_t>(initial ^ (1u << i));
      check(select(request) == "Logging enabled for: " + expectedNames(after), "toggle response/mask mismatch");
      changed(after, 1);
      check(select(request) == "Logging enabled for: " + expectedNames(static_cast<uint8_t>(initial)), "repeat toggle did not restore selection");
      changed(static_cast<uint8_t>(initial), 2);
    }
  }
  // Both intentions are formed before either executes. A UI-side replacement
  // derived from the same initial mask would lose the first toggle here.
  for (size_t first = 0; first < names.size(); ++first) {
    for (size_t second = 0; second < names.size(); ++second) {
      const std::array<std::string, 2> pending = {
        std::string("sensors toggle ") + names[first],
        std::string("sensors toggle ") + names[second]
      };
      reset(LOG_GPS);
      for (const auto& request : pending) check(!isError(select(request)), "queued toggle rejected");
      changed(static_cast<uint8_t>(LOG_GPS ^ (1u << first) ^ (1u << second)), 2);
    }
  }
  for (const char* alias : {"INPUT", "HeAlTh", "ring"}) {
    reset(0);
    check(!isError(select(std::string("sensors ToGgLe \t") + alias + "\r\n")), "toggle alias/case rejected");
    changed(std::strcmp(alias, "INPUT") == 0 ? LOG_GAMEPAD : LOG_R1, 1);
  }
  // The requested bit is interpreted after a different command replaces the
  // set; a pending toggle must not restore its earlier displayed selection.
  const std::string pending = "sensors toggle tof";
  reset(LOG_GPS);
  check(!isError(select("sensors presence")), "intervening replacement failed");
  check(!isError(select(pending)), "toggle after intervening replacement failed");
  changed(LOG_PRESENCE | LOG_TOF, 2);
}

static void sessionGuardsAndAllocation() {
  const char* csvError = "Error: can't change the sensor selection while a CSV session is running (rows would stop matching the file's header)";
  const char* csvHint = "run 'sensorlog stop' first, then reselect sensors and restart — or use 'healthlogging' which restarts the session itself";
  for (const char* input : {"sensors all", "sensors none", "sensors gps", "sensors toggle gps", "sensors toggle", "sensors bogus"}) {
    reset(LOG_TOF | LOG_R1, true, SENSOR_LOG_CSV);
    check(select(input) == csvError, "running CSV refusal changed");
    unchanged(LOG_TOF | LOG_R1, true, SENSOR_LOG_CSV);
    check(hints.size() == 1 && hints[0] == csvHint, "running CSV hint changed");
  }
  for (SensorLogFormat format : {SENSOR_LOG_TEXT, SENSOR_LOG_CSV, SENSOR_LOG_TRACK}) {
    for (bool running : {false, true}) {
      if (running && format == SENSOR_LOG_CSV) continue;
      reset(LOG_GPS, running, format);
      check(!isError(select("sensors toggle gps")), "permitted toggle rejected");
      changed(0, 1);
      check(gSensorLoggingRunning == running && gSensorLogFormat == format, "toggle changed session state");
      check(!isError(select("sensors thermal,tof")), "permitted replacement rejected");
      changed(LOG_THERMAL | LOG_TOF, 2);
      check(gSensorLoggingRunning == running && gSensorLogFormat == format, "replacement changed session state");
    }
  }
  for (const char* input : {"sensors", "sensors all", "sensors none", "sensors gps", "sensors toggle gps"}) {
    reset(LOG_APDS);
    bufferAvailable = false;
    check(select(input) == "Error: Debug buffer unavailable", "debug allocation failure changed");
    unchanged(LOG_APDS, false, SENSOR_LOG_TEXT);
    check(hints.empty(), "debug failure emitted hint");
  }
}

int main() {
  const auto run = [](const char* name, void (*test)()) {
    const int previousChecks = checks, previousFailures = failures;
    test();
    std::printf("  %s: %d checks, %d failures\n", name,
                checks - previousChecks, failures - previousFailures);
  };
  run("catalog and query", catalogAndQuery);
  run("replacement grammar", replacementGrammar);
  run("status formatting", statusFormatting);
  run("invalid input leaves selection unchanged", invalidIsTransactional);
  run("toggles resolve at execution", togglesResolveAtExecution);
  run("session guards and allocation", sessionGuardsAndAllocation);
  std::printf("sensorlog selection: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
