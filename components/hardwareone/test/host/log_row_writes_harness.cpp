// Production append operations with controlled I/O, clock and crypto boundaries.
#include <Print.h>
#include "BatteryPolicy.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

// INSERT_TYPES

static size_t checks = 0, cases = 0;
static void check(bool value, const char* what) {
  ++checks;
  if (!value) { std::cerr << "FAIL: " << what << '\n'; std::exit(1); }
}
static std::string text(const String& value) { return value.c_str(); }
static std::map<std::string, std::string> disk;
static std::vector<std::string> operations, messages;
static int lockDepth = 0, liveFiles = 0, writeCalls = 0, failWrite = 0, reads = 0;
static size_t shortCount = 0;
static bool failAppendOpen = false, failReadOpen = false, reportWriteError = false;
static bool failHeaderOpen = false, failReplace = false, failRemove = false, headerFlushError = false;
static size_t headerSizeOffset = 0;
static int appendOpens = 0, failAppendOpenCall = 0;
static bool overflow = false, sealFailure = false, allocFailure = false, clockSynced = false;
static unsigned long nowMs = 10000;
static time_t fixedEpoch = 1800000000;
static unsigned long millis() { return nowMs; }
static time_t controlledTime(time_t* output) { if (output) *output = fixedEpoch; return fixedEpoch; }
#define time controlledTime
static void io(const std::string& operation) {
  check(lockDepth == 1, "I/O held under filesystem lock");
  operations.push_back(operation);
}
using TickType_t = unsigned;
static constexpr TickType_t portMAX_DELAY = ~TickType_t(0);
static constexpr int gFsMutex = 1, pdTRUE = 1;
static bool isHeldByCurrentTask(int) { return lockDepth == 1; }
static int xSemaphoreTake(int, TickType_t) { check(lockDepth == 0, "acquire exactly one owner lock"); ++lockDepth; return pdTRUE; }
static void xSemaphoreGive(int) { check(lockDepth == 1 && liveFiles == 0, "close files before unlock"); --lockDepth; }
// INSERT_LOCKS
static constexpr int DEBUG_STORAGE = 1, DEBUG_LOGGER = 2;
static bool isDebugFlagSet(int) { return true; }
static void logMessage(const char* format, ...) {
  char value[512]; va_list args; va_start(args, format); vsnprintf(value, sizeof(value), format, args); va_end(args);
  messages.emplace_back(value);
}
#define DEBUG_LOGGERF(...) logMessage(__VA_ARGS__)
#define DEBUGF_BROADCAST(flag, ...) logMessage(__VA_ARGS__)
static void logSystemEvent(const char*, const char* format, const char* path) { logMessage(format, path); }

namespace fs {
class File : public Print {
  struct Handle { std::string path, mode; bool closed = false; };
  std::shared_ptr<Handle> handle;
public:
  File() = default;
  File(std::string path, std::string mode) : handle(std::make_shared<Handle>()) {
    handle->path = std::move(path); handle->mode = std::move(mode); ++liveFiles;
  }
  explicit operator bool() const { return handle && !handle->closed; }
  bool isDirectory() const { return false; }
  void injectWriteError() { setWriteError(); }
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* bytes, size_t count) override {
    check(bool(*this) && (handle->mode == "a" || handle->mode == "w"), "write uses borrowed writable file");
    io("write:" + std::to_string(count)); ++writeCalls;
    const size_t accepted = writeCalls == failWrite ? std::min(count, shortCount) : count;
    if (accepted) disk.at(handle->path).append(reinterpret_cast<const char*>(bytes), accepted);
    if (reportWriteError) setWriteError();
    return accepted;
  }
  size_t size() const { check(bool(*this), "size uses open file"); io("size"); return disk.at(handle->path).size() + (handle->mode == "w" ? headerSizeOffset : 0); }
  void flush() override { check(bool(*this), "flush uses open file"); io("flush"); if (headerFlushError && handle->mode == "w") setWriteError(); }
  void close() { check(bool(*this), "close file once"); io("close"); handle->closed = true; --liveFiles; }
};
}
using fs::File;
namespace VFS {
struct Context { std::string tag; };
static Context systemAuth(const char* tag) { return {tag}; }
static bool resolveOverflowPath(const char* path, size_t need, char* output, size_t capacity) {
  io("resolve:" + std::to_string(need)); snprintf(output, capacity, "%s%s", overflow ? "/sd" : "", path); return overflow;
}
static File openGuarded(const String& path, const char* mode, const Context& ctx, bool create = false) {
  io("open:" + text(path) + ":" + mode + ":" + ctx.tag);
  check(ctx.tag.rfind("senlog.", 0) == 0 || ctx.tag == "batlog", "trusted owner context retained");
  if (strcmp(mode, "r") == 0) {
    ++reads; check(!create, "size lookup must not create");
    if (failReadOpen || !disk.count(text(path))) return {};
  } else {
    check(create && (strcmp(mode, "a") == 0 || strcmp(mode, "w") == 0), "output opens create with explicit mode");
    if (strcmp(mode, "w") == 0) {
      if (failHeaderOpen) return {};
      disk[text(path)].clear();
    } else {
      ++appendOpens;
      if (failAppendOpen || appendOpens == failAppendOpenCall) return {};
      disk.try_emplace(text(path));
    }
  }
  return File(text(path), mode);
}
static bool existsGuarded(const String& path, const Context&) { io("exists:" + text(path)); return disk.count(text(path)); }
static bool removeGuarded(const String& path, const Context&) { io("remove:" + text(path)); check(liveFiles == 0, "close before removal"); return !failRemove && disk.erase(text(path)) != 0; }
static bool renameGuarded(const String& from, const String& to, const Context&) {
  io("rename:" + text(from) + ":" + text(to)); check(liveFiles == 0, "close before rotation");
  if (!disk.count(text(from))) return false;
  disk[text(to)] = std::move(disk.at(text(from))); disk.erase(text(from)); return true;
}
static bool replaceFileGuarded(const String& from, const String& to, const Context& ctx) {
  io("replace:" + text(from) + ":" + text(to));
  if (failReplace) return false;
  return renameGuarded(from, to, ctx);
}
}
namespace Clock { static bool isSynced() { return clockSynced; } }
static SensorLogFormat gSensorLogFormat = SENSOR_LOG_TEXT;
static uint8_t gSensorLogMask = LOG_TOF;
static SensorLogPathMode gSensorLogPathMode = SensorLogPathMode::MANUAL;
static String gSensorLogPath = "/sensor.log";
static char gSensorLogDayStr[11] = "2026-10-11";
static bool gSensorLogSealed = false;
static char* gSealBuf = nullptr;
static size_t gApproxSizeBytes = 0, gSensorLogMaxSize = 100000;
static int gSensorLogMaxRotations = 2;
static unsigned long gSensorLogLastWrite = 25, lastTruncateMs = 0;
static const unsigned long truncateCooldownMs = 5000;
static uint32_t log_writes = 0, log_open_fail = 0, log_trunc = 0;
enum class AllocPref { PreferPSRAM };
static char sealStorage[CAPCRYPT_MAX_SEALED];
static void* ps_alloc(size_t count, AllocPref, const char* tag) {
  check(count == sizeof(sealStorage) && std::string(tag) == "sensor.log.seal", "seal keeps original buffer policy");
  return allocFailure ? nullptr : sealStorage;
}
static int captureCryptoSealLine(const char* line, size_t length, char* out, size_t capacity) {
  if (sealFailure) return -1;
  std::string sealed = "SEALED[" + std::string(line, length) + "]";
  check(sealed.size() + 1 <= capacity, "fixture fits crypto output buffer");
  memcpy(out, sealed.c_str(), sealed.size() + 1); return static_cast<int>(sealed.size());
}
static String stripSessionShaping(const String& value) { return value; }
static String shapeSessionPath(const String&) { return "/next-day.csv"; }
static String resolveSessionTarget(const String& value) { return value; }
static void sensorLogClassifyActivePath(const char*) { gSensorLogPathMode = SensorLogPathMode::DAY; }
static size_t sensorLogResolvedSize(const String& path) { return disk[text(path)].size(); }
static bool sessionWantsSealing(uint8_t) { return gSensorLogSealed; }
struct { bool batteryLogEnabled = true; uint32_t batteryLogIntervalMs = 60000; } gSettings;
uint32_t gBootCounter = 7;
static BatteryState batteryState;
static BatteryState getBatterySnapshot() { return batteryState; }

// INSERT_PRODUCTION

static std::string sensorPath() { return overflow ? "/sd/sensor.log" : "/sensor.log"; }
static bool containsMessage(const std::string& value) {
  return std::any_of(messages.begin(), messages.end(), [&](const std::string& m) { return m.find(value) != std::string::npos; });
}
static void released() { check(lockDepth == 0 && liveFiles == 0, "call releases every file and lock"); }
static void reset(bool onSd = false) {
  released(); ++cases; disk.clear(); operations.clear(); messages.clear(); writeCalls = failWrite = reads = 0; shortCount = 0;
  failAppendOpen = failReadOpen = reportWriteError = sealFailure = allocFailure = clockSynced = false;
  failHeaderOpen = failReplace = failRemove = headerFlushError = false;
  headerSizeOffset = 0; appendOpens = failAppendOpenCall = 0;
  overflow = onSd; nowMs = 10000; gSensorLogFormat = SENSOR_LOG_TEXT; gSensorLogMask = LOG_TOF;
  gSensorLogPathMode = SensorLogPathMode::MANUAL; gSensorLogPath = "/sensor.log";
  gSensorLogSealed = false; gSealBuf = nullptr; gApproxSizeBytes = 0; gSensorLogMaxSize = 100000;
  gSensorLogMaxRotations = 2; gSensorLogLastWrite = 25; lastTruncateMs = 0; log_writes = log_open_fail = log_trunc = 0;
  gSettings.batteryLogEnabled = true; gSettings.batteryLogIntervalMs = 60000; batteryState = {};
}
static void transcript(const char* name) {
  released(); std::cout << name << ':' << overflow << ':' << log_writes << ':' << gSensorLogLastWrite << ':' << log_trunc << '\n';
  for (const auto& file : disk) std::cout << std::quoted(file.first) << '=' << std::quoted(file.second) << '\n';
}
static void sensorNormal(bool onSd) {
  for (auto format : {SENSOR_LOG_TEXT, SENSOR_LOG_CSV, SENSOR_LOG_TRACK}) {
    for (bool sealed : {false, true}) {
      reset(onSd); gSensorLogFormat = format; gSensorLogMask = LOG_GPS | LOG_R1; gSensorLogSealed = sealed;
      sensorAppend("123,body"); released();
      std::string expected = sealed ? CAPCRYPT_MAGIC_LINE "\n" : "";
      if (format == SENSOR_LOG_CSV) expected += text(buildCsvHeader(LOG_GPS | LOG_R1)) + "\n";
      if (format == SENSOR_LOG_TRACK) expected += kTrackHeader;
      expected += sealed ? "SEALED[123,body]\n" : "123,body\n";
      check(disk.at(sensorPath()) == expected, "format header and plain/sealed row exact");
      check(log_writes == 1 && gSensorLogLastWrite == nowMs && reads == 0, "success counters and fast path");
      sensorAppend("next"); check(log_writes == 2, "second row counted");
      check(disk.at(sensorPath()) == expected + (sealed ? "SEALED[next]\n" : "next\n"), "existing header not repeated");
      transcript("sensor-format");
    }
  }
  for (int rotations : {0, 1, 2, 3}) {
    reset(onSd); gSensorLogMaxSize = 5; gSensorLogMaxRotations = rotations;
    disk[sensorPath()] = "old\n"; gApproxSizeBytes = 4;
    for (int i = 1; i <= rotations; ++i) disk[sensorPath() + "." + std::to_string(i)] = "prior" + std::to_string(i);
    sensorAppend("abc"); released(); check(!disk.count(sensorPath()) && log_trunc == 1 && gApproxSizeBytes == 0, "rotation threshold and counter");
    if (rotations) check(disk.at(sensorPath() + ".1") == "old\nabc\n", "rotate completed base to first generation");
    transcript("sensor-rotate");
  }
  reset(onSd); disk[sensorPath()] = "old\n"; gApproxSizeBytes = 4; gSensorLogMaxSize = 8;
  sensorAppend("abc"); check(log_trunc == 0 && gApproxSizeBytes == 8, "exact cap does not rotate"); transcript("sensor-exact-cap");
  reset(onSd); gSensorLogMaxSize = 3; lastTruncateMs = nowMs - 100; sensorAppend("abc");
  check(log_trunc == 0, "rotation cooldown retained"); transcript("sensor-cooldown");
  reset(onSd); sensorAppend(nullptr); sensorAppend(""); check(disk.empty() && operations.empty(), "empty/null built rows skip all I/O"); transcript("sensor-empty");
  reset(onSd); gSensorLogSealed = true; sealFailure = true; sensorAppend("private");
  check(disk.at(sensorPath()) == CAPCRYPT_MAGIC_LINE "\n" && log_writes == 0, "seal failure never appends plaintext"); transcript("sensor-seal-failure");
  reset(onSd); gSensorLogSealed = true; allocFailure = true; sensorAppend("private");
  check(disk.at(sensorPath()) == CAPCRYPT_MAGIC_LINE "\n" && log_writes == 0, "allocation failure remains fail closed"); transcript("sensor-allocation-failure");
  reset(onSd); failAppendOpen = true; sensorAppend("body");
  check(disk.empty() && log_open_fail == 1 && log_writes == 0, "append open failure preserved"); transcript("sensor-open-failure");
  reset(onSd); gSensorLogFormat = SENSOR_LOG_CSV; failWrite = 1; shortCount = 3; sensorAppend("body");
  check(log_open_fail == 1 && log_writes == 0 && disk.at(sensorPath()) == (BASELINE ? "tim" : ""), "failed header remains unpublished"); released();
}
static void sensorFailures(bool onSd) {
  for (bool sealed : {false, true}) for (int failed : {1, 2}) {
    reset(onSd); disk[sensorPath()] = "prefix\n"; gApproxSizeBytes = 7; gSensorLogSealed = sealed;
    failWrite = failed; shortCount = failed == 1 ? 2 : 0;
    sensorAppend("body"); released();
    const size_t expected = failed == 1 ? 3 : sealed ? 12 : 4;
    check(writeCalls == 2, "body and LF both attempted on short writes");
    check(disk.at(sensorPath()).size() == 7 + expected, "actual partial bytes retained");
    check(log_writes == (LEGACY_ROWS ? 1U : 0U) && gSensorLogLastWrite == (LEGACY_ROWS ? nowMs : 25), "only complete rows update success count/time");
    check(gApproxSizeBytes == (LEGACY_ROWS ? 7 + (sealed ? 13 : 5) : 7 + expected), "size follows actual bytes on failure");
    check(reads == (LEGACY_ROWS ? 0 : 1), "only failure resynchronizes size");
    if (!LEGACY_ROWS) check(!containsMessage("wrote ") && containsMessage("failed"), "failed rows do not emit success diagnostics");
  }
  reset(onSd); disk[sensorPath()] = "prefix\n"; gApproxSizeBytes = 1; reportWriteError = true;
  sensorAppend("body"); released(); check(log_writes == (LEGACY_ROWS ? 1U : 0U), "Print error rejects exact-count false success");
  check(gApproxSizeBytes == (LEGACY_ROWS ? 6 : 12), "failure size query includes existing bytes");
  reset(onSd); disk[sensorPath()] = "prefix\n"; gApproxSizeBytes = 1; failWrite = 1; shortCount = 2; failReadOpen = true;
  sensorAppend("body"); released(); check(gApproxSizeBytes == (LEGACY_ROWS ? 6 : 4), "unavailable size retains accepted-byte estimate");
  reset(onSd); gSensorLogFormat = SENSOR_LOG_CSV; failWrite = 2; shortCount = 2;
  sensorAppend("body"); released();
  check(gApproxSizeBytes == (LEGACY_ROWS ? 5 : disk.at(sensorPath()).size()), "failed fresh CSV row resynchronizes including header bytes");
  check(disk.at(sensorPath()) == text(buildCsvHeader(LOG_TOF)) + "\nbo\n", "failure preserves format header and accepted row bytes");
  reset(onSd); disk[sensorPath()] = "prefix\n"; gApproxSizeBytes = 7; gSensorLogMaxSize = 8; failWrite = 1; shortCount = 2;
  sensorAppend("body"); released(); check(log_trunc == 1 && !disk.count(sensorPath()), "partial rows still permit size rotation");
  check(disk.at(sensorPath() + ".1") == "prefix\nbo\n", "rotation preserves exact accepted partial contents");
}
static const char* batteryHeader = "boot,uptime_ms,epoch_s,datetime,pct[%],voltage[V],crate[%/hr],status,charging,usb,event\n";
static void batteryNormal() {
  reset(); batteryLogAppend("powersave:enter"); released();
  check(disk.at(kBatteryLogPath).rfind(batteryHeader, 0) == 0, "fresh battery file has original schema header");
  check(disk.at(kBatteryLogPath).find(",,,,Unknown,,,powersave:enter\n") != std::string::npos, "unknown battery readings remain empty");
  transcript("battery-unknown");
  reset(); batteryState.percentageValid = batteryState.voltageValid = batteryState.rateValid = true;
  batteryState.percentage = 55.5f; batteryState.voltage = 3.75f; batteryState.cratePctPerHr = -2.25f;
  batteryState.chargingKnown = batteryState.usbKnown = true; batteryState.isCharging = false; batteryState.usbPresent = true;
  batteryState.status = BATTERY_MEDIUM; batteryState.statusEstimated = true;
  batteryLogAppend("cpufreq:80MHz"); released();
  check(disk.at(kBatteryLogPath).find(",55.5,3.750,-2.25,Medium (estimated),0,1,cpufreq:80MHz\n") != std::string::npos, "valid readings/status/event schema retained");
  batteryLogAppend(nullptr); check(disk.at(kBatteryLogPath).find(batteryHeader, strlen(batteryHeader)) == std::string::npos, "existing file gets no repeated header");
  transcript("battery-known");
  reset(); disk[kBatteryLogPath] = ""; batteryLogAppend(""); released();
  check((disk.at(kBatteryLogPath).find(batteryHeader) == std::string::npos) == bool(BASELINE), "existing empty battery files receive their header before rows");
  reset(); disk[kBatteryLogPath] = std::string(kBatteryLogMaxSize + 1, 'x'); disk["/battery.csv.1"] = "previous"; disk["/battery.csv.2"] = "oldest";
  batteryLogAppend("wake"); released(); check(!disk.count(kBatteryLogPath) && disk.at("/battery.csv.2") == "previous", "battery generations rotate identically"); transcript("battery-rotation");
  reset(); failAppendOpen = true; batteryLogAppend("event"); released(); check(disk.empty(), "battery open failure releases lock"); transcript("battery-open-failure");
  reset(); failWrite = 1; shortCount = 3; batteryLogAppend("event"); released();
  check(BASELINE ? disk.at(kBatteryLogPath).rfind("boo7,", 0) == 0 : disk.at(kBatteryLogPath).empty(), "failed battery header prevents row append");
}
static void batteryFailures() {
  for (int failed : {1, 2}) {
    reset(); disk[kBatteryLogPath] = "prefix\n"; failWrite = failed; shortCount = failed == 1 ? 2 : 0;
    batteryLogAppend("event"); released();
    check(writeCalls == 2, "battery attempts LF following failed body");
    if (!LEGACY_ROWS) check(containsMessage("failed"), "battery failed row has failure diagnostic");
  }
  reset(); disk[kBatteryLogPath] = "prefix\n"; reportWriteError = true; batteryLogAppend("event"); released();
  if (!LEGACY_ROWS) check(containsMessage("failed"), "battery Print error reported");
  reset(); disk[kBatteryLogPath] = std::string(kBatteryLogMaxSize, 'x'); failWrite = 1; shortCount = 2;
  batteryLogAppend("event"); released(); check(!disk.count(kBatteryLogPath) && disk.at("/battery.csv.1").size() == kBatteryLogMaxSize + 3, "partial battery rows still trigger rotation at actual size");
  reset(); fsLock("outer"); batteryLogAppend("nested");
  check(lockDepth == (LEGACY_ROWS ? 0 : 1) && liveFiles == 0, "battery guard preserves caller-owned lock");
  if (!LEGACY_ROWS) fsUnlock(); released();
}
static void batteryCadence() {
  reset(); gSettings.batteryLogEnabled = false; nowMs = 1000; batteryLogTick(); batteryLogEvent("off");
  check(disk.empty(), "disabled battery logger has no tick/event output");
  gSettings.batteryLogEnabled = true; gSettings.batteryLogIntervalMs = 1; failAppendOpen = true; batteryLogTick();
  failAppendOpen = false; nowMs = 5999; batteryLogTick(); check(disk.empty(), "failed tick retains cadence and five second floor");
  nowMs = 6000; batteryLogTick(); check(disk.count(kBatteryLogPath), "next interval retries after failed open");
  const size_t before = disk.at(kBatteryLogPath).size(); nowMs = 6001; batteryLogEvent("wake");
  check(disk.at(kBatteryLogPath).size() > before, "event bypasses periodic cadence");
  const size_t after = disk.at(kBatteryLogPath).size(); nowMs = 10999; batteryLogTick();
  check(disk.at(kBatteryLogPath).size() == after, "event does not advance periodic timestamp");
  nowMs = 11000; failWrite = writeCalls + 1; shortCount = 2; batteryLogTick();
  const size_t partial = disk.at(kBatteryLogPath).size(); nowMs = 15999; batteryLogTick();
  check(disk.at(kBatteryLogPath).size() == partial, "failed row still waits for next interval");
  nowMs = 16000; batteryLogTick(); check(disk.at(kBatteryLogPath).size() > partial, "row failure retries on next interval");
  transcript("battery-cadence");
#if !BASELINE
  disk.erase(kBatteryLogPath); nowMs = 21000; failWrite = writeCalls + 1; shortCount = 2;
  batteryLogTick(); const int calls = writeCalls;
  check(disk.at(kBatteryLogPath).empty(), "failed tick header leaves empty destination and no row");
  nowMs = 25999; batteryLogTick(); check(writeCalls == calls, "failed header still preserves five-second sampling floor");
  nowMs = 26000; failWrite = 0; batteryLogTick();
  check(disk.at(kBatteryLogPath).rfind(batteryHeader, 0) == 0, "next scheduled tick retries complete header before row");
  released();
#endif
}

static void headerCases() {
  // Existing nonempty legacy damage keeps the established append policy. This
  // change prevents new partial headers; it does not rewrite historical files.
  reset(); gSensorLogFormat = SENSOR_LOG_CSV; disk[sensorPath()] = "legacy-damaged-header\n";
  sensorAppend("body"); check(disk.at(sensorPath()) == "legacy-damaged-header\nbody\n", "existing nonempty sensor file is not repaired or rewritten");
  reset(); disk[kBatteryLogPath] = "legacy-damaged-header\n"; batteryLogAppend("event");
  check(disk.at(kBatteryLogPath).rfind("legacy-damaged-header\n", 0) == 0, "existing nonempty battery file is not repaired or rewritten");
#if !BASELINE
  for (bool sensor : {false, true}) for (bool onSd : {false, true}) {
    if (!sensor && onSd) continue; // Battery's existing owner has no overflow routing.
    for (unsigned fault = 0; fault < 8; ++fault) {
      reset(onSd); gSensorLogFormat = SENSOR_LOG_CSV;
      const std::string destination = sensor ? sensorPath() : kBatteryLogPath;
      const std::string temporary = destination + ".header.tmp";
      if (fault == 0) { failWrite = 1; shortCount = 0; }
      if (fault == 1) { failWrite = 1; shortCount = 3; }
      if (fault == 2) reportWriteError = true;
      if (fault == 3) headerFlushError = true;
      if (fault == 4) headerSizeOffset = 1;
      if (fault == 5) failHeaderOpen = true;
      if (fault == 6) failReplace = true;
      if (fault == 7) { failWrite = 1; shortCount = 3; failRemove = true; }
      if (sensor) sensorAppend("body"); else batteryLogAppend("event");
      released(); check(disk.at(destination).empty(), "failed fresh header never publishes partial destination");
      if (sensor) check(log_writes == 0 && gSensorLogLastWrite == 25, "failed sensor header never counts successful row");
      check(disk.count(temporary) == (fault == 7 ? 1U : 0U), "failed staging cleanup leaves only its reserved sibling");
      failWrite = 0; reportWriteError = headerFlushError = failHeaderOpen = failReplace = false;
      headerSizeOffset = 0; nowMs += 5000;
      if (sensor) sensorAppend("body"); else batteryLogAppend("event");
      released();
      if (fault == 7) {
        check(disk.at(destination).empty() && disk.at(temporary) == (sensor ? "tim" : "boo"), "retry refuses unresolved header sibling instead of appending");
        auto retained = disk; reset(onSd); disk = retained; gSensorLogFormat = SENSOR_LOG_CSV;
        if (sensor) sensorAppend("body"); else batteryLogAppend("event");
        check(disk == retained, "reboot-like runtime reset cannot bypass unresolved header sibling");
      } else {
        const std::string expected = sensor ? text(buildCsvHeader(LOG_TOF)) + "\nbody\n" : batteryHeader;
        check(disk.at(destination).rfind(expected, 0) == 0 && !disk.count(temporary), "retry publishes full header before next row");
      }
      released();
    }
    reset(onSd); gSensorLogFormat = SENSOR_LOG_CSV;
    const std::string destination = sensor ? sensorPath() : kBatteryLogPath;
    const std::string temporary = destination + ".header.tmp";
    disk[temporary] = "existing neighboring data";
    if (sensor) sensorAppend("body"); else batteryLogAppend("event");
    check(disk.at(destination).empty() && disk.at(temporary) == "existing neighboring data", "existing sibling is not overwritten or removed"); released();
    reset(onSd); gSensorLogFormat = SENSOR_LOG_CSV; failAppendOpenCall = 2;
    if (sensor) sensorAppend("body"); else batteryLogAppend("event");
    check(disk.at(destination) == (sensor ? text(buildCsvHeader(LOG_TOF)) + "\n" : batteryHeader), "failed append reopen retains complete header without rows");
    failAppendOpenCall = 0;
    if (sensor) sensorAppend("body"); else batteryLogAppend("event");
    check(disk.at(destination).size() > (sensor ? buildCsvHeader(LOG_TOF).length() + 1 : strlen(batteryHeader)), "next append reuses successfully published header"); released();
  }
  for (auto format : {SENSOR_LOG_TEXT, SENSOR_LOG_CSV, SENSOR_LOG_TRACK}) {
    for (int failure : {1, 2}) {
      if (format == SENSOR_LOG_TEXT && failure == 2) continue;
      reset(); gSensorLogSealed = true; gSensorLogFormat = format; failWrite = failure; shortCount = 2;
      sensorAppend("private body"); released();
      check(disk.at(sensorPath()).empty() && log_writes == 0 && !disk.count(sensorPath() + ".header.tmp"), "partial sealing magic/schema never becomes appendable");
      failWrite = 0; sensorAppend("private body");
      check(disk.at(sensorPath()).rfind(CAPCRYPT_MAGIC_LINE "\n", 0) == 0 && disk.at(sensorPath()).find("SEALED[private body]\n") != std::string::npos,
            "sealed retry publishes magic/schema before sealed row"); released();
    }
  }
#endif
}
#if !LEGACY_ROWS
static void helperCases() {
  for (size_t length : {size_t(0), size_t(1), size_t(512), size_t(2048)}) {
    for (int failure : {0, 1, 2, 3}) {
      reset(); disk["/sensor.log"] = ""; fsLock("helper"); File f("/sensor.log", "a");
      failWrite = failure < 3 ? failure : 0; reportWriteError = failure == 3; shortCount = 0;
      std::string body(length, 'x'); size_t accepted = 999;
      const bool ok = writeLineChecked(f, reinterpret_cast<const uint8_t*>(body.data()), body.size(), accepted);
      const size_t expected = (failure == 1 ? 0 : length) + (failure == 2 ? 0 : 1);
      check(accepted == expected && disk.at("/sensor.log").size() == expected, "helper reports actual accepted bytes");
      check(ok == (failure == 0 || (failure == 1 && length == 0)), "helper checks both counts and Print error");
      check(writeCalls == 2 && liveFiles == 1 && lockDepth == 1, "helper borrows file and does not alter lock/lifetime");
      check(operations.size() == 2, "helper performs exactly body and LF writes");
      f.close(); fsUnlock(); released();
    }
  }
  reset(); disk["/sensor.log"] = ""; fsLock("helper"); File f("/sensor.log", "a");
  const uint8_t binary[] = {'a', 0, 'b'}; size_t accepted = 999; f.injectWriteError();
  check(!writeLineChecked(f, binary, sizeof(binary), accepted) && accepted == 4, "helper retains existing Print error despite exact writes");
  check(disk.at("/sensor.log") == std::string("a\0b\n", 4), "helper uses explicit byte length, not C-string termination");
  check(f.getWriteError(), "borrowed file's error is not cleared"); f.close(); fsUnlock(); released();
}
#endif
int main() {
  tzset();
  for (bool onSd : {false, true}) { sensorNormal(onSd); sensorFailures(onSd); }
  batteryNormal(); batteryFailures(); batteryCadence(); headerCases();
#if !LEGACY_ROWS
  helperCases();
#endif
  released(); std::cerr << cases << " scenarios; " << checks << " assertions passed\n";
}
