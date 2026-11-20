// The runner inserts the production status struct, its reader and formatters,
// the `sensorlog status` branch, the CSV header and row builders, the text row
// builder and the GPS log snapshot branch. Logger globals, the debug buffer,
// the clock and the GPS cache read are host doubles.
#include <Arduino.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

void webBatchTestStringAppend() {}

// INSERT_TYPES

// Logger state (file statics in System_SensorLogging.cpp).
static uint8_t gSensorLogMask;
static bool gSensorLoggingRunning;
static SensorLogFormat gSensorLogFormat;
static String gSensorLogPath = "/logs/test.txt";
static uint32_t gSensorLogIntervalMs = 5000;
static size_t gSensorLogMaxSize = 256000;
static uint8_t gSensorLogMaxRotations = 3;
static unsigned long gSensorLogLastWrite = 1000;
static size_t gApproxSizeBytes = 4321;
// Rows that waited for the filesystem lock, and rows given up while waiting
// (the status text names them only when they are not zero).
static uint32_t sAppendLockMisses = 0, sPendingRowsDropped = 0;
static unsigned long nowMs = 6000;
static unsigned long millis() { return nowMs; }
static struct { bool sensorLogAutoStart; } gSettings;
static bool bufferAvailable = true;
static char debugBuffer[1024 + 8];
static bool ensureDebugBuffer() { return bufferAvailable; }
static char* getDebugBuffer() { return debugBuffer; }

// Row builder boundaries.
enum class AllocPref { PreferPsram };
static void* ps_alloc(size_t bytes, AllocPref, const char*) { return ::operator new(bytes); }
namespace Clock {
static bool isSynced() { return false; }
static int64_t epochMillis() { return 0; }
}
static void getTimestampPrefixMsCached(char* out, size_t outSize) { std::snprintf(out, outSize, "[T] | "); }
// The R1 wear word of the text row (G2 ring code); not under test here.
static const char* g2RingWearName(bool, uint8_t) { return "--"; }

// GPS snapshot boundary: 0 no data, 1 data without a fix, 2 fix, 3 the read fails.
static bool gGpsRunning = true, gGpsConnected = true;
static int gpsState = 0;
static bool gpsCacheSnapshot(GpsCache& out) {
  if (gpsState == 3) return false;
  out = GpsCache{};
  if (gpsState == 0) return true;
  out.dataValid = true;
  out.satellites = 9;
  out.fixQuality = 2;
  out.hour = 12; out.minute = 34; out.second = 56;
  out.latitude = 48.5f; out.longitude = 9.25f; out.altitude = 300.0f; out.speed = 1.5f;
  out.hasFix = (gpsState == 2);
  return true;
}

// INSERT_PRODUCTION

static int checks, failures;
static std::string context;
static void check(bool condition, const std::string& message) {
  ++checks;
  if (condition) return;
  ++failures;
  if (failures <= 30) std::fprintf(stderr, "%s: %s\n", context.c_str(), message.c_str());
}
static size_t commas(const std::string& text) {
  size_t count = 0;
  for (char c : text) count += (c == ',');
  return count;
}
static std::vector<std::string> fields(const std::string& row) {
  std::vector<std::string> out(1);
  for (char c : row) {
    if (c == ',') out.emplace_back();
    else out.back() += c;
  }
  return out;
}
static const std::array<const char*, 8> kNames = {
  "thermal", "tof", "imu", "gamepad", "apds", "gps", "presence", "r1"
};
static std::string spacedNames(uint8_t mask) {
  std::string out;
  for (size_t i = 0; i < kNames.size(); ++i) if (mask & (1u << i)) { out += kNames[i]; out += " "; }
  return out;
}
static std::string jsonNames(uint8_t mask) {
  std::string out;
  for (size_t i = 0; i < kNames.size(); ++i) {
    if (!(mask & (1u << i))) continue;
    if (!out.empty()) out += ",";
    out += std::string("\"") + kNames[i] + "\"";
  }
  return "[" + out + "]";
}
static void reset(uint8_t mask, bool running, SensorLogFormat format) {
  gSensorLogMask = mask;
  gSensorLoggingRunning = running;
  gSensorLogFormat = format;
  gSensorLogPath = "/logs/test.txt";
  gSensorLogIntervalMs = 5000;
  gSensorLogMaxSize = 256000;
  gSensorLogMaxRotations = 3;
  gSensorLogLastWrite = 1000;
  gApproxSizeBytes = 4321;
  nowMs = 6000;
  gSettings.sensorLogAutoStart = false;
  bufferAvailable = true;
  std::memset(debugBuffer, 0x7e, sizeof(debugBuffer));
}
static std::string command(const char* line) {
  const std::string result = sensorLogStatusCommand(String(line));
  // The reply buffer is 1024 bytes; the guard bytes behind it stay untouched.
  check(std::memcmp(debugBuffer + 1024, "\x7e\x7e\x7e\x7e\x7e\x7e\x7e\x7e", 8) == 0, "status wrote past the reply buffer");
  return result;
}
static const std::string kTooLarge = "{\"schema\":1,\"ok\":false,\"error\":\"response too large\"}";

static void statusForms() {
  context = "status text";
  reset(LOG_THERMAL | LOG_GPS, true, SENSOR_LOG_CSV);
  gSettings.sensorLogAutoStart = true;
  check(command("status") ==
        "Sensor logging ACTIVE\n  File: /logs/test.txt\n  Interval: 5000ms\n  Format: CSV\n  Max size: 256000 bytes\n"
        "  Rotations: 3\n  Sensors: thermal gps \n  Auto-start: ON\n  Last write: 5s ago",
        "active text changed");
  context = "status json";
  check(command("status json") ==
        "{\"schema\":1,\"running\":true,\"file\":\"/logs/test.txt\",\"intervalMs\":5000,\"format\":\"csv\","
        "\"maxSizeBytes\":256000,\"rotations\":3,\"sensors\":[\"thermal\",\"gps\"],\"autoStart\":true,"
        "\"lastWriteAgoSec\":5,\"approxSizeBytes\":4321}",
        "active json changed: " + command("status json"));
  check(command("STATUS JSON") == command("status json"), "the form word is case-insensitive");
  // Only the word right after 'status' selects the form.
  check(command("status text").rfind("Sensor logging ACTIVE\n", 0) == 0, "an unknown form word prints the text");
  check(command("status now json").rfind("Sensor logging ACTIVE\n", 0) == 0, "json is read by position");

  reset(0, false, SENSOR_LOG_TEXT);
  context = "inactive";
  check(command("status") ==
        "Sensor logging is INACTIVE\n  Format: TEXT\n  Max size: 256000 bytes\n  Rotations: 3\n  Sensors: (none)\n"
        "  Auto-start: OFF", "inactive text changed");
  check(command("status json") ==
        "{\"schema\":1,\"running\":false,\"intervalMs\":5000,\"format\":\"text\",\"maxSizeBytes\":256000,"
        "\"rotations\":3,\"sensors\":[],\"autoStart\":false}",
        "inactive json changed: " + command("status json"));

  // Rows that waited for the filesystem lock this boot add one text line, in
  // both states; the json form is the same with or without them.
  for (bool running : {true, false}) {
    reset(LOG_THERMAL, running, SENSOR_LOG_CSV);
    const std::string plain = command("status");
    const std::string plainJson = command("status json");
    sAppendLockMisses = 7; sPendingRowsDropped = 2;
    context = "deferred writes";
    check(command("status") == plain + "\n  Filesystem busy: 7 deferred write(s), 2 row(s) dropped",
          "the deferred-write line changed: " + command("status"));
    check(command("status json") == plainJson, "the json form does not carry the deferred-write counters");
    sAppendLockMisses = 0; sPendingRowsDropped = 1;
    check(command("status") == plain + "\n  Filesystem busy: 0 deferred write(s), 1 row(s) dropped",
          "a dropped row alone is shown");
    sAppendLockMisses = sPendingRowsDropped = 0;
    check(command("status") == plain, "no line when nothing waited");
  }

  // No reply buffer: each form answers in its own shape.
  bufferAvailable = false;
  check(command("status") == "Error: Debug buffer unavailable", "text buffer error changed");
  check(command("status json") == "{\"schema\":1,\"ok\":false,\"error\":\"buffer\"}", "json buffer error changed");
  bufferAvailable = true;

  // Both forms agree for every selection, format and running state.
  const char* textFormat[] = {"TEXT", "CSV", "TRACK"};
  const char* jsonFormat[] = {"text", "csv", "track"};
  for (unsigned mask = 0; mask <= 255; ++mask) {
    for (SensorLogFormat format : {SENSOR_LOG_TEXT, SENSOR_LOG_CSV, SENSOR_LOG_TRACK}) {
      for (bool running : {false, true}) {
        reset(static_cast<uint8_t>(mask), running, format);
        gSettings.sensorLogAutoStart = running;
        gSensorLogMaxRotations = static_cast<uint8_t>(mask % 10);
        context = "agreement mask=" + std::to_string(mask) + " format=" + std::to_string(format);
        const std::string text = command("status");
        const std::string json = command("status json");
        const std::string names = mask ? spacedNames(static_cast<uint8_t>(mask)) : "(none)";
        check(text.find("  Sensors: " + names + "\n") != std::string::npos, "text sensor names");
        check(json.find(",\"sensors\":" + jsonNames(static_cast<uint8_t>(mask)) + ",") != std::string::npos, "json sensor names");
        check(text.find(std::string("  Format: ") + textFormat[format] + "\n") != std::string::npos, "text format");
        check(json.find(std::string(",\"format\":\"") + jsonFormat[format] + "\",") != std::string::npos, "json format");
        check((text.rfind("Sensor logging ACTIVE\n", 0) == 0) == running, "text running");
        check((json.find("\"running\":true,") != std::string::npos) == running, "json running");
        check((text.find("  Auto-start: ON") != std::string::npos) == running, "text auto-start");
        check((json.find(",\"autoStart\":true") != std::string::npos) == running, "json auto-start");
        check(text.find("  Rotations: " + std::to_string(mask % 10) + "\n") != std::string::npos, "text rotations");
        check(json.find(",\"rotations\":" + std::to_string(mask % 10) + ",") != std::string::npos, "json rotations");
        // The file, the last write and the size appear only while running.
        check((json.find(",\"file\":\"/logs/test.txt\",") != std::string::npos) == running, "json file");
        check((json.find(",\"lastWriteAgoSec\":5,") != std::string::npos) == running, "json last write");
        check((text.find("  Last write: 5s ago") != std::string::npos) == running, "text last write");
        check(json.front() == '{' && json.back() == '}', "json is one object");
      }
    }
  }

  // A path JSON must escape stays one string value; the text prints it raw.
  reset(LOG_GPS, true, SENSOR_LOG_TRACK);
  context = "escaped path";
  gSensorLogPath = "/logs/a\"b\\c\n.txt";
  check(command("status json").find(",\"file\":\"/logs/a\\\"b\\\\c\\n.txt\",") != std::string::npos, "path is not escaped");
  check(command("status").find("  File: /logs/a\"b\\c\n.txt\n") != std::string::npos, "text path changed");

  // The json is never cut: it fits whole or the reply is the error object.
  context = "long path";
  gSensorLogPath = String(std::string("/") + std::string(600, 'p'));
  const std::string fitting = command("status json");
  check(fitting.back() == '}' && fitting.find(std::string(600, 'p') + "\",") != std::string::npos, "a 600-byte path fits whole");
  gSensorLogPath = String(std::string("/") + std::string(1100, 'p'));
  check(command("status json") == kTooLarge, "an oversized reply is the error object");
  check(command("status").size() == 1023, "the text is cut at the reply buffer, as before");
  bool edgeFound = false;
  for (size_t length = 700; length < 1100 && !edgeFound; ++length) {
    gSensorLogPath = String(std::string("/") + std::string(length, 'p'));
    if (command("status json") != kTooLarge) continue;
    gSensorLogPath = String(std::string("/") + std::string(length - 1, 'p'));
    const std::string last = command("status json");
    check(last.size() == 1023 && last.back() == '}', "the largest reply fills the buffer and still closes the object");
    edgeFound = true;
  }
  check(edgeFound, "no size edge found");
  // Escapes count toward the size: a path of quotes needs two bytes each.
  gSensorLogPath = String(std::string(600, '"'));
  check(command("status json") == kTooLarge, "escaped length is what must fit");
}

// A snapshot with every reading present and distinct values.
static SensorCacheSnapshot fullSnapshot() {
  SensorCacheSnapshot s{};
  s.thermalValid = true; s.thermalMin = 20; s.thermalMax = 30; s.thermalAvg = 25;
  s.tofValid = true;
  for (int i = 0; i < 4; ++i) { s.tof[i].valid = (i % 2) == 0; s.tof[i].distance_mm = 100 + i; s.tof[i].status = 5 + i; }
  s.yaw = 1.5f; s.pitch = 2.5f; s.roll = 3.5f; s.ax = 0.125f; s.ay = 0.25f; s.az = 0.5f;
  s.gx = 1.0f; s.gy = 2.0f; s.gz = 3.0f; s.imuTemp = 31.5f;
  s.inputValid = true; s.joyX = 11; s.joyY = -12; s.buttons = 0x30;
  s.apdsValid = true; s.apdsRed = 1; s.apdsGreen = 2; s.apdsBlue = 3; s.apdsClear = 4; s.apdsProximity = 5; s.apdsGesture = 6;
  s.gGpsRunning = s.gGpsConnected = true;
  s.gpsFix = true; s.gpsLatitude = 48.5f; s.gpsLongitude = 9.25f; s.gpsAltitude = 300.0f; s.gpsSpeed = 1.5f;
  s.gpsSatellites = 9; s.gpsFixQuality = 2;
  s.presenceAmbientTemp = 21.5f; s.presenceValue = 7; s.presenceDetected = true; s.motionValue = 8; s.motionDetected = false;
  s.r1Connected = true; s.r1HrValid = true; s.r1Hr = 61;
  return s;
}

static void csvRows() {
  // Every selection, with each of the four "valid" flags on or off: the row
  // has exactly the header's columns.
  for (unsigned mask = 0; mask <= 255; ++mask) {
    const std::string header = buildCsvHeader(static_cast<uint8_t>(mask)).c_str();
    for (unsigned valid = 0; valid < 16; ++valid) {
      gSensorLogMask = static_cast<uint8_t>(mask);
      SensorCacheSnapshot s = fullSnapshot();
      s.thermalValid = valid & 1; s.tofValid = valid & 2; s.inputValid = valid & 4; s.apdsValid = valid & 8;
      context = "csv mask=" + std::to_string(mask) + " valid=" + std::to_string(valid);
      const std::string row = csvRow(s);
      check(commas(row) == commas(header), "row has " + std::to_string(commas(row)) + " commas, header " +
                                           std::to_string(commas(header)) + ": " + row);
    }
  }

  // All readings present: the rows are what they were before the padding.
  gSensorLogMask = 0xFF;
  context = "csv all valid";
  nowMs = 6000;
  check(std::string(csvRow(fullSnapshot())) ==
        "6000,20,30,25,100,1,5,101,0,6,102,1,7,103,0,8,1.5,2.5,3.5,0.125,0.250,0.500,1.000,2.000,3.000,31.5,"
        "11,-12,48,1,2,3,4,5,6,1,48.500000,9.250000,300.0,1.5,9,2,21.5,7,1,8,0,1,61,,,,,",
        std::string("valid row changed: ") + csvRow(fullSnapshot()));

  // No reading from thermal, ToF, gamepad or APDS: their fields are empty and
  // every later column is still under its header name.
  context = "csv all invalid";
  SensorCacheSnapshot s = fullSnapshot();
  s.thermalValid = s.tofValid = s.inputValid = s.apdsValid = false;
  const std::string row = csvRow(s);
  check(row ==
        "6000,,,,,,,,,,,,,,,,1.5,2.5,3.5,0.125,0.250,0.500,1.000,2.000,3.000,31.5,"
        ",,,,,,,,,1,48.500000,9.250000,300.0,1.5,9,2,21.5,7,1,8,0,1,61,,,,,",
        "padded row changed: " + row);
  const std::vector<std::string> names = fields(buildCsvHeader(0xFF).c_str());
  const std::vector<std::string> values = fields(row);
  check(names.size() == values.size(), "field count");
  for (size_t i = 0; i < names.size() && i < values.size(); ++i) {
    const bool padded = names[i].rfind("thermal_", 0) == 0 || names[i].rfind("tof_", 0) == 0 ||
                        names[i].rfind("gamepad_", 0) == 0 || names[i].rfind("apds_", 0) == 0;
    if (padded) check(values[i].empty(), names[i] + " is not empty: " + values[i]);
    if (names[i] == "imu_yaw") check(values[i] == "1.5", "imu_yaw moved: " + values[i]);
    if (names[i] == "gps_lat") check(values[i] == "48.500000", "gps_lat moved: " + values[i]);
    if (names[i] == "gps_sats") check(values[i] == "9", "gps_sats moved: " + values[i]);
    if (names[i] == "presence_ambient") check(values[i] == "21.5", "presence_ambient moved: " + values[i]);
    if (names[i] == "r1_hr") check(values[i] == "61", "r1_hr moved: " + values[i]);
  }

  // Mixed: only APDS without a reading, GPS selected after it.
  context = "csv mixed";
  gSensorLogMask = LOG_THERMAL | LOG_APDS | LOG_GPS;
  s = fullSnapshot();
  s.apdsValid = false;
  check(std::string(csvRow(s)) == "6000,20,30,25,,,,,,,1,48.500000,9.250000,300.0,1.5,9,2",
        std::string("mixed row changed: ") + csvRow(s));
  // An unselected sensor adds nothing, with or without a reading.
  gSensorLogMask = LOG_GPS;
  check(std::string(csvRow(s)) == "6000,1,48.500000,9.250000,300.0,1.5,9,2", "unselected sensors add columns");
}

static void gpsWithoutFix() {
  context = "gps snapshot";
  gpsState = 0;  // no data yet
  SensorCacheSnapshot snap = loggingGps(LOG_GPS);
  check(!snap.gpsFix && snap.gpsSatellites == 0 && snap.gpsFixQuality == 0 && !snap.gpsHasTime, "no data must stay zero");
  gpsState = 3;  // the cache read fails
  snap = loggingGps(LOG_GPS);
  check(!snap.gpsFix && snap.gpsSatellites == 0 && snap.gpsFixQuality == 0, "a failed read must stay zero");
  gpsState = 1;  // data, no fix: satellites and quality only
  snap = loggingGps(LOG_GPS);
  check(!snap.gpsFix && !snap.gpsHasTime, "no fix is reported as a fix");
  check(snap.gpsSatellites == 9 && snap.gpsFixQuality == 2, "satellites and quality are dropped without a fix");
  check(snap.gpsLatitude == 0 && snap.gpsLongitude == 0 && snap.gpsAltitude == 0 && snap.gpsSpeed == 0 &&
        snap.gpsHour == 0 && snap.gpsMinute == 0 && snap.gpsSecond == 0, "position, speed and time need a fix");
  check(snap.gGpsRunning && snap.gGpsConnected, "running flags lost");
  // The rows that print them.
  gSensorLogMask = LOG_GPS;
  check(std::string(textRow(snap)) == "[T] | gps: no_fix sats=9 q=2", std::string("no-fix text row: ") + textRow(snap));
  nowMs = 6000;
  check(std::string(csvRow(snap)) == "6000,0,0.000000,0.000000,0.0,0.0,9,2", std::string("no-fix csv row: ") + csvRow(snap));
  gpsState = 2;  // fix: everything
  snap = loggingGps(LOG_GPS);
  check(snap.gpsFix && snap.gpsHasTime && snap.gpsSatellites == 9 && snap.gpsFixQuality == 2 &&
        snap.gpsLatitude == 48.5f && snap.gpsLongitude == 9.25f && snap.gpsAltitude == 300.0f && snap.gpsSpeed == 1.5f &&
        snap.gpsHour == 12 && snap.gpsMinute == 34 && snap.gpsSecond == 56, "fix snapshot changed");
  check(std::string(textRow(snap)) == "[T] | gps: lat=48.500000 lon=9.250000 alt=300.0m speed=1.5kn sats=9 q=2",
        std::string("fix text row: ") + textRow(snap));
  // GPS not selected: nothing is read.
  snap = loggingGps(LOG_THERMAL);
  check(!snap.gGpsRunning && snap.gpsSatellites == 0, "an unselected GPS was read");
}

int main() {
  statusForms();
  csvRows();
  gpsWithoutFix();
  if (failures) {
    std::fprintf(stderr, "sensorlog status rows: %d of %d checks failed\n", failures, checks);
    return 1;
  }
  std::printf("sensorlog status rows: %d checks passed (status forms, CSV columns, GPS without a fix)\n", checks);
  return 0;
}
