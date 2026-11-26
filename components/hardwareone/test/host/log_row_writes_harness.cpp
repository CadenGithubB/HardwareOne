// Production append operations with controlled I/O, clock and crypto boundaries.
#include <Print.h>
#include "System_BatteryPolicy.h"
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
static size_t cardKeepsBytes = SIZE_MAX;  // B11: what a card that loses a handle's buffer keeps of a file (File::write)
static int appendOpens = 0, failAppendOpenCall = 0;
static bool overflow = false, sealFailure = false, allocFailure = false, clockSynced = false;
static bool reserveRefused = false;  // G-39: resolveLogPath answers Refused
static unsigned reserveDrops = 0;
static unsigned long nowMs = 10000;
static time_t fixedEpoch = 1800000000;
static unsigned long millis() { return nowMs; }
static time_t controlledTime(time_t* output) { if (output) *output = fixedEpoch; return fixedEpoch; }
#define time controlledTime
static bool mutexExists();
static void io(const std::string& operation) {
  // Before the mutex exists (early boot) there is nothing to hold.
  check(lockDepth == 1 || !mutexExists(), "I/O held under filesystem lock");
  operations.push_back(operation);
}
using TickType_t = unsigned;
static constexpr TickType_t portMAX_DELAY = ~TickType_t(0);
static constexpr int pdTRUE = 1, pdFALSE = 0;
// The filesystem mutex: a handle that can be absent (before it is created) and
// can be held by another task. A wait without limit on a lock another task
// holds would hang the harness, so it is a failure here; a try-lock misses.
static int fsMutexStorage = 0;
static int* gFsMutex = &fsMutexStorage;
static bool heldElsewhere = false;
static unsigned blockingWaitsOnBusyLock = 0, tryLockMisses = 0;
static bool mutexExists() { return gFsMutex != nullptr; }
static bool isHeldByCurrentTask(int* mutex) { return mutex && lockDepth == 1; }
static bool isFsLockedByCurrentTask() { return isHeldByCurrentTask(gFsMutex); }
static int xSemaphoreTake(int*, TickType_t timeout) {
  check(lockDepth == 0, "acquire exactly one owner lock");
  if (heldElsewhere) {
    if (timeout == portMAX_DELAY) { ++blockingWaitsOnBusyLock; heldElsewhere = false; }  // "waited", then got it
    else { ++tryLockMisses; return pdFALSE; }
  }
  ++lockDepth; return pdTRUE;
}
static void xSemaphoreGive(int*) { check(lockDepth == 1 && liveFiles == 0, "close files before unlock"); --lockDepth; }
// INSERT_LOCKS
static constexpr int DEBUG_STORAGE = 1, DEBUG_LOGGER = 2;
static bool isDebugFlagSet(int) { return true; }
static void logMessage(const char* format, ...) {
  char value[512]; va_list args; va_start(args, format); vsnprintf(value, sizeof(value), format, args); va_end(args);
  messages.emplace_back(value);
}
#define DEBUG_LOGGERF(...) logMessage(__VA_ARGS__)
#define DEBUGF_QUEUE(flag, ...) logMessage(__VA_ARGS__)
static void logSystemEvent(const char*, const char* format, ...) {
  char line[256];
  va_list args; va_start(args, format); vsnprintf(line, sizeof(line), format, args); va_end(args);
  logMessage("%s", line);
}

namespace fs {
class File : public Print {
  struct Handle { std::string path, mode; size_t offset = 0; bool closed = false; };
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
    // "Write accepted, flush lost": a card pulled under the handle. write()
    // reports every byte and flush() nothing; the card file keeps at most
    // cardKeepsBytes. Only its size tells.
    size_t landed = accepted;
    if (cardKeepsBytes != SIZE_MAX && handle->path.rfind("/sd/", 0) == 0) {
      const size_t have = disk.at(handle->path).size();
      landed = cardKeepsBytes > have ? std::min(accepted, cardKeepsBytes - have) : 0;
    }
    if (landed) disk.at(handle->path).append(reinterpret_cast<const char*>(bytes), landed);
    if (reportWriteError) setWriteError();
    return accepted;
  }
  size_t size() const { check(bool(*this), "size uses open file"); io("size"); return disk.at(handle->path).size() + (handle->mode == "w" ? headerSizeOffset : 0); }
  // B11: SealedLog reads a card file's first bytes to judge whose it is, and
  // its last byte to see whether the last row was torn.
  size_t read(uint8_t* bytes, size_t count) {
    check(bool(*this) && handle->mode == "r", "the head is read from a file open for reading");
    io("read:" + std::to_string(count));
    const std::string& data = disk.at(handle->path);
    const size_t n = std::min(count, data.size() - std::min(handle->offset, data.size()));
    memcpy(bytes, data.data() + handle->offset, n);
    handle->offset += n;
    return n;
  }
  bool seek(size_t offset) {
    check(bool(*this) && handle->mode == "r", "seek uses a file open for reading");
    if (offset > disk.at(handle->path).size()) return false;
    handle->offset = offset; return true;
  }
  void flush() override { check(bool(*this), "flush uses open file"); io("flush"); if (headerFlushError && handle->mode == "w") setWriteError(); }
  void close() { check(bool(*this), "close file once"); io("close"); handle->closed = true; --liveFiles; }
};
}
using fs::File;
namespace VFS {
struct Context { std::string tag; };
namespace Scopes {
constexpr const char* CAPTURES    = "/logging_captures";
constexpr const char* CAPTURES_SD = "/sd/logging_captures";
}
static const char* captureScopeFor(const String& path) {
  return std::strncmp(path.c_str(), "/sd/", 4) == 0 ? Scopes::CAPTURES_SD : Scopes::CAPTURES;
}
static Context systemAuth(const char* tag) { return {tag}; }
static Context systemAuth(const char* scope, const char* tag) { (void)scope; return {tag}; }
static bool resolveOverflowPath(const char* path, size_t need, char* output, size_t capacity) {
  io("resolve:" + std::to_string(need)); snprintf(output, capacity, "%s%s", overflow ? "/sd" : "", path); return overflow;
}
enum class LogTier : uint8_t { Primary, Overflow, Refused };
static LogTier resolveLogPath(const char* path, size_t need, char* output, size_t capacity) {
  io("resolve:" + std::to_string(need));
  if (reserveRefused) { snprintf(output, capacity, "%s", path); return LogTier::Refused; }
  snprintf(output, capacity, "%s%s", overflow ? "/sd" : "", path);
  return overflow ? LogTier::Overflow : LogTier::Primary;
}
static void noteLogReserveDrop() { ++reserveDrops; }
// B11: a card that did not take a row is reported; log lines then stay off it.
static unsigned sdFailureNotes = 0;
static void noteLogOverflowSdFailure(const char*) { ++sdFailureNotes; overflow = false; }
// One log's card file failed: the real note asks the card's write probe and
// reports the card (as above) or holds only this log off it. Either way the
// log's next resolution is flash; the path it was told is kept for the cases.
static std::string cardFileFailurePath;
static void noteLogCardFileFailure(const char* path, const char*) { cardFileFailurePath = path; ++sdFailureNotes; overflow = false; }
// UP-16: the stored LittleFS free figure the battery log checks against the
// shared reserve; noteLittleFsBytesWritten records nothing here.
static size_t littleFsFree = SIZE_MAX;
[[maybe_unused]] static size_t peekLittleFsFree() { return littleFsFree; }
[[maybe_unused]] static size_t logReserveBytes() { return 100 * 1024; }
[[maybe_unused]] static void noteLittleFsBytesWritten(size_t) {}
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
  if (failReplace || !disk.count(text(from))) return false;
  // FAT cannot rename over an existing file; LittleFS can.
  if (text(to).rfind("/sd/", 0) == 0 && disk.count(text(to))) return false;
  disk[text(to)] = std::move(disk.at(text(from))); disk.erase(text(from)); return true;
}
// The real VFS::replaceFileGuarded is compiled here (INSERT_REPLACE) over these fakes.
using AuthContext = Context;
enum StorageType { INTERNAL, SDCARD };
static StorageType getStorageType(const String& path) { return path.startsWith("/sd/") ? SDCARD : INTERNAL; }
static bool guardedNormalize(const String& path, String& normalized, const char*, const Context&) {
  normalized = path; return path.startsWith("/") && path.indexOf("..") < 0;
}
// INSERT_REPLACE
}
// B11: a capture on the card is always sealed. The SealedLog double (the mark
// with this device's key id, a file that is not ours moved aside) runs over
// the fakes above. The cases written before B11 keep the card as an unsealed
// tier (cardSealing off: they exercise the row writer, the header staging and
// the rotation with plain bytes on either tier); the B11 cases switch it on,
// which is what the device does.
#define SEALED_LOG_HOST_OPEN
#define SEALED_LOG_HOST_MARK_API
#include "sealed_log_host.h"
static void cardSealing(bool on) { sealed_log_host::cardPrefix = on ? "/sd/" : "\x01no card sealing"; }
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
enum class AllocPref { PreferPsram };
static char sealStorage[CAPCRYPT_MAX_SEALED];
static char pendingStorage[1024];
static bool pendingAllocFailure = false;
static unsigned pendingAllocations = 0;
static void* ps_alloc(size_t count, AllocPref, const char* tag) {
  if (std::string(tag) == "sensor.log.pending") {
    // The waiting row's own copy: one PSRAM block, the size of the largest row buffer.
    check(count == sizeof(pendingStorage), "the waiting row has one fixed-size PSRAM buffer");
    ++pendingAllocations;
    return pendingAllocFailure ? nullptr : pendingStorage;
  }
  check(count == sizeof(sealStorage) && std::string(tag) == "sensor.log.seal", "seal keeps original buffer policy");
  return allocFailure ? nullptr : sealStorage;
}
static uint32_t sessionId = 1;
static uint32_t sensorLogCurrentSessionId() { return sessionId; }
static std::string sealedForPath;  // the path the last row was sealed for (its class binds the row)
static int captureCryptoSealLine(const char* path, const char* line, size_t length, char* out, size_t capacity) {
  check(path != nullptr, "a row is sealed for the path of its file");
  sealedForPath = path;
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

using AuthContext = VFS::AuthContext;
// INSERT_PRODUCTION

// The file the cases from before B11 write: the session's own path, on flash
// or (onSd) on the card. They used to reach the card through the overflow
// resolution; since B11 a card twin that fails falls back to flash, which the
// B11 cases cover (cardTwinCases), so these name the card file themselves and
// keep testing the row writer, the header staging and the rotation on it.
static bool legacyOnSd = false;
static std::string sensorPath() { return legacyOnSd ? "/sd/sensor.log" : "/sensor.log"; }
static bool containsMessage(const std::string& value) {
  return std::any_of(messages.begin(), messages.end(), [&](const std::string& m) { return m.find(value) != std::string::npos; });
}
static void released() { check(lockDepth == 0 && liveFiles == 0, "call releases every file and lock"); }
static void reset(bool onSd = false) {
  released(); ++cases; disk.clear(); operations.clear(); messages.clear(); writeCalls = failWrite = reads = 0; shortCount = 0;
  failAppendOpen = failReadOpen = reportWriteError = sealFailure = allocFailure = clockSynced = false;
  failHeaderOpen = failReplace = failRemove = headerFlushError = false;
  headerSizeOffset = 0; appendOpens = failAppendOpenCall = 0; reserveRefused = false; reserveDrops = 0; VFS::littleFsFree = SIZE_MAX;
  overflow = false; legacyOnSd = onSd; nowMs = 10000; gSensorLogFormat = SENSOR_LOG_TEXT; gSensorLogMask = LOG_TOF;
  gSensorLogPathMode = SensorLogPathMode::MANUAL; gSensorLogPath = onSd ? "/sd/sensor.log" : "/sensor.log";
  gSensorLogSealed = false; gSealBuf = nullptr; gApproxSizeBytes = 0; gSensorLogMaxSize = 100000;
  gSensorLogMaxRotations = 2; gSensorLogLastWrite = 25; lastTruncateMs = 0; log_writes = log_open_fail = log_trunc = 0;
  gSettings.batteryLogEnabled = true; gSettings.batteryLogIntervalMs = 60000; batteryState = {};
  heldElsewhere = false; blockingWaitsOnBusyLock = tryLockMisses = 0; gFsMutex = &fsMutexStorage;
  pendingAllocFailure = false; sessionId = 1;
  cardSealing(false); VFS::sdFailureNotes = 0; sealed_log_host::keyAvailable = true; sealed_log_host::movedAside = 0;
  cardKeepsBytes = SIZE_MAX;
#if TRY_LOCK_WRITERS
  sPendingRowHeld = false; sAppendLockMisses = sPendingRowsDropped = 0; sBatteryLogLockMisses = 0;
  sBatteryLogSampleWaiting = false;
#endif
}
static void transcript(const char* name) {
  released(); std::cout << name << ':' << legacyOnSd << ':' << log_writes << ':' << gSensorLogLastWrite << ':' << log_trunc << '\n';
  for (const auto& file : disk) std::cout << std::quoted(file.first) << '=' << std::quoted(file.second) << '\n';
}
static void sensorNormal(bool onSd) {
  for (auto format : {SENSOR_LOG_TEXT, SENSOR_LOG_CSV, SENSOR_LOG_TRACK}) {
    for (bool sealed : {false, true}) {
      reset(onSd); gSensorLogFormat = format; gSensorLogMask = LOG_GPS | LOG_R1; gSensorLogSealed = sealed;
      sensorAppend("123,body"); released();
      std::string expected = sealed ? sealed_log_host::mark() : "";   // the mark names the key (B11)
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
  check(disk.at(sensorPath()) == sealed_log_host::mark() && log_writes == 0, "seal failure never appends plaintext"); transcript("sensor-seal-failure");
  reset(onSd); gSensorLogSealed = true; allocFailure = true; sensorAppend("private");
  check(disk.at(sensorPath()) == sealed_log_host::mark() && log_writes == 0, "allocation failure remains fail closed"); transcript("sensor-allocation-failure");
  reset(onSd); failAppendOpen = true; sensorAppend("body");
  check(disk.empty() && log_open_fail == 1 && log_writes == 0, "append open failure preserved"); transcript("sensor-open-failure");
  reset(onSd); gSensorLogFormat = SENSOR_LOG_CSV; failWrite = 1; shortCount = 3; sensorAppend("body");
  check(log_open_fail == 1 && log_writes == 0 &&
        (BASELINE ? (disk.count(sensorPath()) && disk.at(sensorPath()) == "tim") : !disk.count(sensorPath())),
        "failed header remains unpublished and creates no destination"); released();
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
      released();
      // The sensor opener creates the destination only by promotion; the battery writer still probes with "a".
      check(sensor ? !disk.count(destination) : disk.at(destination).empty(), "failed fresh header never creates or publishes the destination");
      if (sensor) check(log_writes == 0 && gSensorLogLastWrite == 25, "failed sensor header never counts successful row");
      check(disk.count(temporary) == (fault == 7 ? 1U : 0U), "failed staging cleanup leaves only its reserved sibling");
      failWrite = 0; reportWriteError = headerFlushError = failHeaderOpen = failReplace = false;
      headerSizeOffset = 0; nowMs += 5000;
      if (sensor) sensorAppend("body"); else batteryLogAppend("event");
      released();
      {
        const std::string expected = sensor ? text(buildCsvHeader(LOG_TOF)) + "\nbody\n" : batteryHeader;
        check(disk.at(destination).rfind(expected, 0) == 0 && !disk.count(temporary),
              fault == 7 ? "retry replaces the leftover header sibling and publishes the header" : "retry publishes full header before next row");
      }
      released();
      if (fault == 7) {
        // Reboot-like: the destination is empty and only the staging sibling survived, so a fresh
        // runtime stages the header again instead of refusing the path forever.
        reset(onSd); gSensorLogFormat = SENSOR_LOG_CSV; disk[destination] = ""; disk[temporary] = sensor ? "tim" : "boo";
        if (sensor) sensorAppend("body"); else batteryLogAppend("event");
        const std::string expected = sensor ? text(buildCsvHeader(LOG_TOF)) + "\nbody\n" : batteryHeader;
        check(disk.at(destination).rfind(expected, 0) == 0 && !disk.count(temporary), "reboot-like runtime reset replaces the leftover header sibling");
        released();
      }
    }
    reset(onSd); gSensorLogFormat = SENSOR_LOG_CSV;
    const std::string destination = sensor ? sensorPath() : kBatteryLogPath;
    const std::string temporary = destination + ".header.tmp";
    disk[temporary] = "existing neighboring data";
    if (sensor) sensorAppend("body"); else batteryLogAppend("event");
    {
      const std::string expected = sensor ? text(buildCsvHeader(LOG_TOF)) + "\nbody\n" : batteryHeader;
      check(disk.at(destination).rfind(expected, 0) == 0 && !disk.count(temporary), "stale sibling beside an empty destination is replaced by a fresh header");
    }
    released();
    if (sensor) {
      // A new destination comes into being only through promotion, so the rename never meets an
      // existing file on either backend; an existing empty destination is removed first.
      const std::string expected = text(buildCsvHeader(LOG_TOF)) + "\nbody\n";
      const std::string replaceOp = "rename:" + temporary + ":" + destination;   // the real helper's promotion
      auto find = [&](const std::string& op) { return std::find(operations.begin(), operations.end(), op); };
      reset(onSd); gSensorLogFormat = SENSOR_LOG_CSV; sensorAppend("body"); released();
      check(find(replaceOp) != operations.end(), "fresh destination is promoted from its staging file");
      check(std::none_of(operations.begin(), find(replaceOp), [&](const std::string& op) { return op.rfind("open:" + destination + ":", 0) == 0; }),
            "fresh destination is not opened or created before promotion");
      check(disk.at(destination).rfind(expected, 0) == 0, "fresh destination receives header and row");
      reset(onSd); gSensorLogFormat = SENSOR_LOG_CSV; disk[destination] = ""; sensorAppend("body"); released();
      check(find("remove:" + destination) != operations.end() && find("remove:" + destination) < find(replaceOp),
            "empty existing destination is removed before promotion");
      check(disk.at(destination).rfind(expected, 0) == 0, "empty existing destination receives header and row");
      reset(onSd); gSensorLogFormat = SENSOR_LOG_CSV; disk[destination] = ""; failRemove = true; sensorAppend("body"); released();
      check(disk.at(destination).empty() && log_writes == 0 && log_open_fail == 1 && !disk.count(temporary),
            "failed removal of an empty destination refuses this append without staging");
      if (onSd) {
        // Against the real helper: the sensor logger never enters SD's backup branch, so no
        // .bak is created, and a foreign .bak beside an absent destination neither blocks nor is consumed.
        const std::string backup = destination + ".bak";
        reset(onSd); gSensorLogFormat = SENSOR_LOG_CSV; sensorAppend("body"); released();
        check(find("rename:" + temporary + ":" + destination) != operations.end() && find("rename:" + destination + ":" + backup) == operations.end() && !disk.count(backup),
              "fresh SD destination is promoted by one rename without a backup");
        reset(onSd); gSensorLogFormat = SENSOR_LOG_CSV; disk[backup] = "foreign"; sensorAppend("body"); released();
        check(disk.at(destination).rfind(expected, 0) == 0 && disk.at(backup) == "foreign", "foreign backup beside an absent destination neither blocks nor is consumed");
        reset(onSd); gSensorLogFormat = SENSOR_LOG_CSV; disk[destination] = ""; sensorAppend("body"); released();
        check(disk.at(destination).rfind(expected, 0) == 0 && !disk.count(backup), "empty SD destination is replaced without a backup");
      }
    }
    // The sensor opener's first "a" open is the reopen after promotion; the battery writer's is its probe.
    reset(onSd); gSensorLogFormat = SENSOR_LOG_CSV; failAppendOpenCall = sensor ? 1 : 2;
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
      check(!disk.count(sensorPath()) && log_writes == 0 && !disk.count(sensorPath() + ".header.tmp"), "partial sealing magic/schema never becomes appendable or creates the destination");
      failWrite = 0; sensorAppend("private body");
      check(disk.at(sensorPath()).rfind(sealed_log_host::mark(), 0) == 0 && disk.at(sensorPath()).find("SEALED[private body]\n") != std::string::npos,
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
#if !BASELINE && !LEGACY_ROWS
// B11: the card twin of the session's file. Whatever the capture mode says for
// flash, a capture on the card is sealed: the mark with this device's key id,
// the plain schema line, then sealed rows. A card file that is not this
// device's is moved aside; a card that does not take the row is reported and
// the row is written again, on flash.
// A flash-named session in the capture tree (where every sensor capture
// lives), the card taking log lines.
static void cardReset() {
  reset(false); overflow = true; cardSealing(true);
  gSensorLogPath = "/logging_captures/sensors/sensor.log";
  cardKeepsBytes = SIZE_MAX; VFS::cardFileFailurePath.clear(); sealedForPath.clear();
}
static void cardTwinCases() {
  const std::string card = "/sd/logging_captures/sensors/sensor.log", flash = "/logging_captures/sensors/sensor.log";
  for (auto format : {SENSOR_LOG_TEXT, SENSOR_LOG_CSV, SENSOR_LOG_TRACK}) {
    cardReset(); gSensorLogFormat = format; gSensorLogMask = LOG_GPS | LOG_R1;
    gSensorLogSealed = false;                     // capturecrypt off: flash would be plain
    sensorAppend("123,body"); released();
    std::string expected = sealed_log_host::mark();
    if (format == SENSOR_LOG_CSV) expected += text(buildCsvHeader(LOG_GPS | LOG_R1)) + "\n";
    if (format == SENSOR_LOG_TRACK) expected += kTrackHeader;
    check(disk.at(card) == expected + "SEALED[123,body]\n" && !disk.count(flash),
          "on the card: the mark, the plain schema, a sealed row; nothing on flash");
    check(sealedForPath == card, "the row is sealed for the card file's path (its class: the capture tree)");
    check(log_writes == 1 && VFS::sdFailureNotes == 0, "the row counts as written");
    sensorAppend("next");
    check(disk.at(card) == expected + "SEALED[123,body]\nSEALED[next]\n", "further rows are sealed under the one mark");
    // The same session on flash (no card): plain, as the mode says.
    reset(false); cardSealing(true); gSensorLogFormat = format; gSensorLogMask = LOG_GPS | LOG_R1;
    gSensorLogPath = flash.c_str();
    sensorAppend("123,body");
    check(disk.at(flash).find("#HW1ENC") == std::string::npos && disk.at(flash).find("123,body\n") != std::string::npos,
          "flash stays as the capture mode decides");
  }
  // A file on the card that is not this device's sealed file: moved aside
  // unchanged, and a new sealed file takes the name.
  for (const char* foreign : {"#HW1ENC v1 k1 id=fedcba9876543210\ntimestamp_ms\nENC1:AAAA\n", "timestamp_ms\n1,2,3\n",
                              "#HW1ENC v1 k1\ntimestamp_ms\nENC1:AAAA\n"}) {
    cardReset(); gSensorLogFormat = SENSOR_LOG_CSV; disk[card] = foreign;
    sensorAppend("body"); released();
    check(disk.count(card + ".foreign-1") && disk.at(card + ".foreign-1") == foreign, "the other file is moved aside unchanged");
    check(disk.at(card) == sealed_log_host::mark() + text(buildCsvHeader(LOG_TOF)) + "\nSEALED[body]\n" &&
          sealed_log_host::movedAside == 1, "a new sealed file takes the name");
  }
  // This device's own file from an earlier run takes the row.
  cardReset(); gSensorLogFormat = SENSOR_LOG_CSV;
  disk[card] = sealed_log_host::mark() + text(buildCsvHeader(LOG_TOF)) + "\nSEALED[old]\n";
  sensorAppend("body");
  check(disk.at(card) == sealed_log_host::mark() + text(buildCsvHeader(LOG_TOF)) + "\nSEALED[old]\nSEALED[body]\n" &&
        sealed_log_host::movedAside == 0, "our own file is appended to");
  // The card is pulled: the open fails. The card is reported and the row is
  // written once more, on flash, plain.
  cardReset(); failAppendOpenCall = 1;
  disk[card] = sealed_log_host::mark() + "SEALED[old]\n";
  sensorAppend("after the pull"); released();
  check(VFS::sdFailureNotes == 1 && disk.at(flash) == "after the pull\n" && disk.at(card) == sealed_log_host::mark() + "SEALED[old]\n",
        "an open that fails on the card sends the row to flash");
  check(log_writes == 1, "the row is written once");
  // The card is pulled mid-row: a short write. Same fallback; the card holds
  // at most the torn sealed row, never plain text.
  cardReset(); disk[card] = sealed_log_host::mark() + "SEALED[old]\n";
  failWrite = 1; shortCount = 4;
  sensorAppend("torn row"); released();
  check(VFS::sdFailureNotes == 1 && disk.at(flash) == "torn row\n", "a short write on the card sends the row to flash");
  check(disk.at(card) == sealed_log_host::mark() + "SEALED[old]\nSEAL\n" || disk.at(card) == sealed_log_host::mark() + "SEALED[old]\nSEAL",
        "the card keeps the rows it had and at most a torn sealed row");
  check(disk.at(card).find("torn row") == std::string::npos, "no plain text of the row on the card");
  // Review of 2026-11-22. "Write accepted, flush lost": the card is pulled
  // under the handle; write() and flush() report nothing. The row counts only
  // when the flushed file has grown by it, so it is written again, on flash.
  cardReset(); disk[card] = sealed_log_host::mark() + "SEALED[old]\n";
  cardKeepsBytes = disk.at(card).size();
  sensorAppend("lost at the flush"); released();
  check(VFS::sdFailureNotes == 1 && VFS::cardFileFailurePath == flash && disk.at(flash) == "lost at the flush\n" &&
        disk.at(card) == sealed_log_host::mark() + "SEALED[old]\n", "a row the card lost is noticed and lands on flash");
  check(log_writes == 1, "and it is counted once");
  // A torn last row (a pull, a power cut): the next row is not glued to it.
  cardReset(); disk[card] = sealed_log_host::mark() + "SEALED[old]\nSEAL";
  sensorAppend("after the tear"); released();
  check(disk.at(card) == sealed_log_host::mark() + "SEALED[old]\nSEAL\nSEALED[after the tear]\n" && VFS::sdFailureNotes == 0,
        "the torn row gets its line end before the next row");
  // Without the device key a card file is not judged: one that is another
  // device's (or plain) is not moved aside. Before the fix it was.
  for (const char* theirs : {"#HW1ENC v1 k1 id=fedcba9876543210\nENC1:AAAA\n", "timestamp_ms\n1,2,3\n"}) {
    cardReset(); sealed_log_host::keyAvailable = false; disk[card] = theirs;
    sensorAppend("no key"); released();
    check(disk.at(card) == theirs && !disk.count(card + ".foreign-1") && sealed_log_host::movedAside == 0,
          "without the key nothing on the card is moved aside");
    check(disk.at(flash) == "no key\n", "and the row lands on flash");
  }
  // No device key: no card file is started (the header's mark fails), and the
  // row goes to flash.
  cardReset(); sealed_log_host::keyAvailable = false;
  sensorAppend("no key"); released();
  check(!disk.count(card) && !disk.count(card + ".header.tmp") && disk.at(flash) == "no key\n" && VFS::sdFailureNotes == 1,
        "without the key nothing is created on the card and the row lands on flash");
  // A session whose own path is on the card has no flash twin: a failure there
  // is a failed row, and the card is not reported on its account.
  reset(false); cardSealing(true); gSensorLogPath = "/sd/own.log"; failAppendOpenCall = 1;
  sensorAppend("body"); released();
  check(VFS::sdFailureNotes == 0 && log_open_fail == 1 && log_writes == 0 && !disk.count("/own.log"),
        "a card-named session does not fall back");
  failAppendOpenCall = 0; sensorAppend("body");
  check(disk.at("/sd/own.log") == sealed_log_host::mark() + "SEALED[body]\n", "and is sealed like every capture on the card");
}
#endif
#if !BASELINE
// G-39: with the overflow latch set, no writable card and LittleFS below the
// 100 KB floor, a sensor row must not be appended to flash (it would spend the
// reserve settings saves depend on); it is dropped and counted, and the
// session writes again once the resolution clears.
static void reserveRefusedCases() {
  for (auto format : {SENSOR_LOG_TEXT, SENSOR_LOG_CSV}) {
    reset(); gSensorLogFormat = format; disk[sensorPath()] = "prefix\n"; gApproxSizeBytes = 7;
    reserveRefused = true;
    sensorAppend("body"); released();
    check(disk.at(sensorPath()) == "prefix\n" && log_writes == 0 && reserveDrops == 1,
          "Refused resolution drops the row and leaves the file untouched");
    check(std::none_of(operations.begin(), operations.end(), [](const std::string& op) { return op.rfind("open:", 0) == 0; }),
          "Refused resolution opens nothing");
    check(gApproxSizeBytes == 7 && log_open_fail == 0, "a refused row is neither a write nor an open failure");
    reserveRefused = false; sensorAppend("body"); released();
    check(disk.at(sensorPath()) == "prefix\nbody\n" && log_writes == 1 && reserveDrops == 1,
          "writing resumes once the resolution clears");
  }
  reset(); gSensorLogFormat = SENSOR_LOG_CSV; reserveRefused = true; sensorAppend("body"); released();
  check(disk.empty() && reserveDrops == 1, "a refused fresh CSV session creates no file or header");
  // UP-16: the battery log has no SD mirror; below the shared reserve its row
  // is skipped and counted, and nothing is opened. At the reserve it writes.
  reset(); disk[kBatteryLogPath] = "prefix\n"; VFS::littleFsFree = 100 * 1024 - 1;
  batteryLogAppend("event"); released();
  check(disk.at(kBatteryLogPath) == "prefix\n" && reserveDrops == 1 && sBatteryLogReserveSkips == 1,
        "a battery row below the reserve is skipped and counted");
  check(std::none_of(operations.begin(), operations.end(), [](const std::string& op) { return op.rfind("open:", 0) == 0; }),
        "a skipped battery row opens nothing");
  VFS::littleFsFree = 100 * 1024; batteryLogAppend("event"); released();
  check(disk.at(kBatteryLogPath).size() > 7 && reserveDrops == 1, "a battery row at the reserve is written");
}
#endif

#if TRY_LOCK_WRITERS
// S-5: the loop's writers never wait for the filesystem lock.
static void busyLockCases() {
  for (bool onSd : {false, true}) {
    // Held by another task: the append returns at once, opens and writes
    // nothing, and the row waits.
    reset(onSd); disk[sensorPath()] = "prefix\n"; gApproxSizeBytes = 7;
    heldElsewhere = true;
    sensorAppend("row-1"); released();
    check(operations.empty() && disk.at(sensorPath()) == "prefix\n" && log_writes == 0, "a busy lock writes nothing");
    check(blockingWaitsOnBusyLock == 0 && tryLockMisses == 1, "the append tries the lock and does not wait");
    check(sPendingRowHeld && sAppendLockMisses == 1 && sPendingRowsDropped == 0, "the row is kept and the miss counted");
    // Every later lap tries again and still does not wait.
    for (int lap = 0; lap < 3; ++lap) sensorRetry();
    released();
    check(operations.empty() && sPendingRowHeld && tryLockMisses == 4 && blockingWaitsOnBusyLock == 0,
          "every lap tries again without waiting");
    check(sAppendLockMisses == 1, "the waiting row counts once, not once a lap");
    // Released: the same row is written once, and only once.
    heldElsewhere = false;
    sensorRetry(); released();
    check(disk.at(sensorPath()) == "prefix\nrow-1\n" && log_writes == 1 && !sPendingRowHeld, "the kept row is written after release");
    sensorRetry(); sensorRetry(); released();
    check(disk.at(sensorPath()) == "prefix\nrow-1\n" && log_writes == 1, "the kept row is written exactly once");
    check(sAppendLockMisses == 1 && sPendingRowsDropped == 0, "no further misses or drops");
    check(pendingAllocations >= 1, "the waiting row lives in its own buffer");

    // The row buffer of the builders is reused by the next sample: the kept
    // row is a copy.
    reset(onSd); heldElsewhere = true;
    char shared[32] = "first";
    sensorAppend(shared);
    std::snprintf(shared, sizeof(shared), "overwritten");
    heldElsewhere = false; sensorRetry(); released();
    check(disk.at(sensorPath()) == "first\n", "the kept row does not alias the builders' buffer");

    // Still busy when the next sample is ready: the older row gives way and
    // is counted; the newer one is written after release.
    reset(onSd); heldElsewhere = true;
    sensorAppend("older"); sensorAppend("newer"); released();
    check(sPendingRowHeld && sPendingRowsDropped == 1 && sAppendLockMisses == 2, "one row waits; the older one is dropped and counted");
    heldElsewhere = false; sensorRetry(); released();
    check(disk.at(sensorPath()) == "newer\n" && log_writes == 1, "the newest row is the one written");

    // A waiting row goes first when the lock is free again at the next sample.
    reset(onSd); heldElsewhere = true; sensorAppend("one");
    heldElsewhere = false; sensorAppend("two"); released();
    check(disk.at(sensorPath()) == "one\ntwo\n" && log_writes == 2 && sPendingRowsDropped == 0, "rows keep their order");

    // A row never reaches a later session, and a session that ended drops it.
    reset(onSd); heldElsewhere = true; sensorAppend("old session");
    heldElsewhere = false; sessionId = 2; sensorRetry(); released();
    check(disk.empty() && !sPendingRowHeld && sPendingRowsDropped == 1, "a row of an earlier session is not written into a later one");
    reset(onSd); heldElsewhere = true; sensorAppend("stopped");
    heldElsewhere = false; sessionId = 0; sensorRetry(); released();
    check(disk.empty() && !sPendingRowHeld && sPendingRowsDropped == 1, "a stopped session's waiting row is dropped");

    // No memory for the copy, or a row too long to keep: dropped and counted.
    reset(onSd); heldElsewhere = true; sPendingRow = nullptr; pendingAllocFailure = true;
    sensorAppend("no memory"); released();
    check(!sPendingRowHeld && sPendingRowsDropped == 1 && operations.empty(), "a row that cannot be kept is counted as dropped");
    pendingAllocFailure = false; sPendingRow = nullptr;
    const std::string longRow(sizeof(pendingStorage), 'x');
    sensorAppend(longRow.c_str()); released();
    check(!sPendingRowHeld && sPendingRowsDropped == 2, "a row longer than the buffer is not cut");

    // This task's own hold and a device without the mutex are not "busy".
    reset(onSd); fsLock("outer"); sensorAppend("nested");
    check(lockDepth == 1 && liveFiles == 0 && disk.at(sensorPath()) == "nested\n" && !sPendingRowHeld, "reentry writes under the caller's lock");
    fsUnlock(); released();
    reset(onSd); gFsMutex = nullptr; sensorAppend("no mutex");
    check(disk.at(sensorPath()) == "no mutex\n" && !sPendingRowHeld && sAppendLockMisses == 0, "no mutex yet: the row is written");
    gFsMutex = &fsMutexStorage;
  }

  // Battery: the periodic sample is deferred without moving its cadence.
  reset(); gSettings.batteryLogIntervalMs = 5000; nowMs = 100000;
  batteryLogTick(); released();
  const size_t first = disk.at(kBatteryLogPath).size();
  nowMs = 105000; heldElsewhere = true;
  batteryLogTick(); released();
  check(disk.at(kBatteryLogPath).size() == first && blockingWaitsOnBusyLock == 0 && sBatteryLogLockMisses == 1,
        "a busy lock defers the periodic battery sample");
  const unsigned triesBefore = tryLockMisses;
  nowMs = 105001; batteryLogTick(); nowMs = 105002; batteryLogTick(); released();
  check(disk.at(kBatteryLogPath).size() == first && tryLockMisses == triesBefore + 2, "it is due again on every lap while busy");
  check(sBatteryLogLockMisses == 1, "the waiting sample counts once, not once a lap");
  heldElsewhere = false; nowMs = 105003; batteryLogTick(); released();
  const size_t second = disk.at(kBatteryLogPath).size();
  check(second > first && disk.at(kBatteryLogPath).find(",105003,") != std::string::npos,
        "the deferred sample is taken once the lock is free, with the reading of that moment");
  nowMs = 105004; batteryLogTick(); released();
  check(disk.at(kBatteryLogPath).size() == second, "and the cadence starts from that sample");
  // The next sample that has to wait is counted as its own.
  nowMs = 111000; heldElsewhere = true; batteryLogTick(); batteryLogTick(); released();
  check(sBatteryLogLockMisses == 2, "a later deferred sample counts again");
  heldElsewhere = false; batteryLogTick(); released();
  // An event keeps the blocking lock: it waits and is written.
  heldElsewhere = true; batteryLogEvent("sleep:deep"); released();
  check(blockingWaitsOnBusyLock == 1 && disk.at(kBatteryLogPath).find(",sleep:deep\n") != std::string::npos,
        "a power event is never deferred");
  // Reentry and a missing mutex still sample.
  reset(); nowMs = 200000; fsLock("outer"); batteryLogTick();
  check(lockDepth == 1 && disk.count(kBatteryLogPath) && sBatteryLogLockMisses == 0, "the tick under the caller's lock samples");
  fsUnlock(); released();
  reset(); nowMs = 300000; gFsMutex = nullptr; batteryLogTick();
  check(disk.count(kBatteryLogPath) && sBatteryLogLockMisses == 0, "no mutex yet: the battery sample is written");
  gFsMutex = &fsMutexStorage;
}
#endif

int main() {
  tzset();
#if TRY_LOCK_WRITERS
  busyLockCases();
#endif
#if !BASELINE
  reserveRefusedCases();
#endif
#if !BASELINE && !LEGACY_ROWS
  cardTwinCases();
#endif
  for (bool onSd : {false, true}) { sensorNormal(onSd); sensorFailures(onSd); }
  batteryNormal(); batteryFailures(); batteryCadence(); headerCases();
#if !LEGACY_ROWS
  helperCases();
#endif
  released(); std::cerr << cases << " scenarios; " << checks << " assertions passed\n";
}
