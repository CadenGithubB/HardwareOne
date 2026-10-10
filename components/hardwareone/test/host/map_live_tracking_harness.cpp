#include <Arduino.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

void webBatchTestStringAppend() {}
#define EXT_RAM_BSS_ATTR
#define INFO_MAPSF(...) do {} while (0)
#define ERROR_MAPSF(...) do {} while (0)
static std::atomic<unsigned> checks{0};
static void check(bool ok, const char* message) {
  ++checks;
  if (!ok) throw std::runtime_error(message);
}
static void boundary(const char* name);
class MapRenderer;
// INSERT_TYPES
static bool gCLIValidateOnly, debugAvailable = true, allocationOk = true;
static const char* failedAllocationName;
static bool gSensorLoggingRunning;
static String gSensorLogPath;
static struct {
  bool sensorLogEnabled = true, healthLoggingEnabled = false;
  bool gpsAutoStart = false, sensorLogAutoStart = false;
} gSettings;
static std::mutex sSensorLogSessionMux;
static uint32_t sSensorLogSessionId;
static thread_local unsigned criticalDepth, dataLockDepth;
static std::atomic<unsigned> allocations{0}, fsOperations{0};
static std::atomic<size_t> lastSnapshotBytes{0};
static std::atomic<unsigned long> now{1000};
static size_t testStrlcpy(char* target, const char* source, size_t size) {
  const size_t length = std::strlen(source);
  if (size) { const size_t copied = std::min(length, size - 1); std::memcpy(target, source, copied); target[copied] = '\0'; }
  return length;
}
#define strlcpy testStrlcpy
static void portENTER_CRITICAL(std::mutex* mux) { mux->lock(); ++criticalDepth; }
static void portEXIT_CRITICAL(std::mutex* mux) { --criticalDepth; mux->unlock(); }
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
struct StaticSemaphore_t { std::mutex mutex; bool data = false; };
using SemaphoreHandle_t = StaticSemaphore_t*;
using TickType_t = unsigned;
static constexpr int pdTRUE = 1;
static constexpr TickType_t portMAX_DELAY = ~TickType_t(0);
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) { return storage; }
static int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait) {
  check(mutex != nullptr, "owner never takes an absent mutex");
  if (wait == 0) return mutex->mutex.try_lock() ? pdTRUE : 0;
  check(dataLockDepth == 0, "owner does not recursively acquire the data mutex");
  mutex->mutex.lock(); mutex->data = true; ++dataLockDepth; return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t mutex) {
  if (mutex->data) { check(dataLockDepth > 0, "data lock balanced"); --dataLockDepth; }
  mutex->mutex.unlock();
}
static std::vector<std::string> actions, messages;
static unsigned stopEvents;
enum { SYSEVT_SENSOR_STOPPED = 1 };
static void systemEventPost(int event, const char*) {
  check(criticalDepth == 0 && event == SYSEVT_SENSOR_STOPPED,
        "stop side effects occur after logger critical section");
  boundary("event"); ++stopEvents;
}
static void broadcastOutput(const char* message) { messages.emplace_back(message); }
static char debugBuffer[1024];
static bool ensureDebugBuffer() { return debugAvailable; }
static char* getDebugBuffer() { return debugBuffer; }
static void cliHint(const char*) {}
static unsigned long millis() { return now.load(); }
enum class AllocPref { PreferPSRAM };
static void* ps_alloc(size_t size, AllocPref, const char* name) {
  ++allocations;
  if (std::strcmp(name, "gps.snapshot") == 0) lastSnapshotBytes = size;
  return allocationOk && (!failedAllocationName || std::strcmp(failedAllocationName, name) != 0)
      ? std::malloc(size) : nullptr;
}
using AuthContext = const char*;
static AuthContext currentAuthContext() { return "owner"; }
static std::recursive_mutex fsMutex;
struct FsLockGuard {
  explicit FsLockGuard(const char*) { boundary("filesystem lock"); fsMutex.lock(); ++fsOperations; }
  ~FsLockGuard() { fsMutex.unlock(); }
};
static void fsLock(const char*) { boundary("filesystem lock"); fsMutex.lock(); ++fsOperations; }
static void fsUnlock() { fsMutex.unlock(); }
static bool fileExists = true, fileOpen = true;
static std::vector<std::string> inputLines{"1000,37.000000,-122.000000", "2000,37.001000,-122.000000"};
static std::string written;
struct File {
  size_t at = 0;
  bool writable = false;
  explicit operator bool() const { return fileOpen; }
  bool available() const { return at < inputLines.size(); }
  String readStringUntil(char) { boundary("file read"); return inputLines[at++]; }
  size_t print(const char* text) { boundary("file write"); written += text; return std::strlen(text); }
  size_t write(const uint8_t* bytes, size_t count) {
    boundary("file write"); written.append(reinterpret_cast<const char*>(bytes), count); return count;
  }
  void close() { boundary("file close"); }
};
namespace VFS {
static bool existsGuarded(const char*, const char*) { boundary("file exists"); ++fsOperations; return fileExists; }
static File openGuarded(const char*, const char* mode, const char*, bool = false) {
  boundary("file open"); ++fsOperations; if (mode[0] == 'w') written.clear(); return {0, mode[0] == 'w'};
}
static bool removeGuarded(const char*, const char*) { boundary("file remove"); ++fsOperations; return true; }
static bool mkdirGuarded(const char*, const char*) { boundary("mkdir"); ++fsOperations; return true; }
static AuthContext systemAuth(const char*) { return "system"; }
}
namespace Clock { static bool isValidEpoch(time_t) { return false; } }
struct LoadedMap {
  struct { int32_t minLat = 36000000, minLon = -123000000, maxLat = 38000000, maxLon = -121000000; } header;
};
static bool mapAvailable;
static LoadedMap loadedMap;
struct MapCacheGuard {
  explicit MapCacheGuard(const char*) {
    check(dataLockDepth == 0, "map cache lock is acquired before track data lock");
  }
};
class MapCore {
public:
  static bool hasValidMap() { return mapAvailable; }
  static const LoadedMap& getCurrentMap() { return loadedMap; }
  static void geoToScreen(int32_t lat, int32_t lon, int32_t centerLat, int32_t centerLon,
                         int32_t, int32_t, int width, int height, int16_t& x, int16_t& y) {
    x = width / 2 + (lon - centerLon) / 1000;
    y = height / 2 + (lat - centerLat) / 1000;
  }
};
enum { LINE_DOTTED, LINE_SOLID, MAP_SHADE_MAX };
struct MapFeatureStyle { int pattern; int width; int shade; bool overlay; };
class MapRenderer {
public:
  unsigned lines = 0;
  int getWidth() const { return 128; }
  int getHeight() const { return 64; }
  void drawLine(int16_t, int16_t, int16_t, int16_t, const MapFeatureStyle&) { boundary("render"); ++lines; }
};
static const char* gpsReply = "[GPS] Start queued; initialization is pending";
static const char* formatReply = "Log format set to TRACK";
static const char* startReply = "SUCCESS: Sensor logging started";
static bool loseLoggerAfterStart;
#if ENABLE_GPS_SENSOR
static const char* borrowed(const char* text) {
  if (!text) return nullptr;
  std::snprintf(debugBuffer, sizeof(debugBuffer), "%s", text); return debugBuffer;
}
static const char* cmd_gpsstart(const String&) {
  boundary("GPS request"); actions.emplace_back("gps"); return borrowed(gpsReply);
}
static const char* cmd_sensorlog(const String& command);
#endif

// INSERT_PRODUCTION

static std::function<void(const char*)> boundaryHook;
static thread_local bool insideHook;
static void boundary(const char* name) {
  check(dataLockDepth == 0, "owner releases track data mutex before filesystem, command, event or render work");
  if (!insideHook && boundaryHook) {
    insideHook = true;
    boundaryHook(name);
    insideHook = false;
  }
}
static GPSTrackSnapshot capture(int maxPoints = MAX_TRACK_POINTS) {
  GPSTrackSnapshot value;
  check(GPSTrackManager::readSnapshot(value, maxPoints), "snapshot succeeds");
  return value;
}
static bool load(const char* path, String& why, bool rejectOutOfBounds = false) {
  GPSTrackSnapshot result;
  return GPSTrackManager::loadTrack(path, why, result, rejectOutOfBounds);
}

static void snapshotTests();
#if ENABLE_GPS_SENSOR
static const char* cmd_sensorlog(const String& command) {
  boundary("logger request"); actions.emplace_back(command.c_str());
  if (command == "format track") return borrowed(formatReply);
  check(command == "start /logging_captures/tracks/live.csv 1000",
        "Map requests literal existing capture target at one second");
  if (startReply && std::strncmp(startReply, "SUCCESS:", 8) == 0) {
    gSensorLogPath = "/logging_captures/tracks/live.csv";
    sensorLogPublishStarted();
    if (loseLoggerAfterStart) sensorLogMarkStopped();
  }
  return borrowed(startReply);
}
#endif
static bool error(const char* result) {
  return result && std::string(result).rfind("Error", 0) == 0;
}
static bool success(const char* result) {
  return result && std::string(result).rfind("SUCCESS:", 0) == 0;
}
static void reset() {
  sensorLogMarkStopped();
  GPSTrackManager::stopLiveTracking();
  check(GPSTrackManager::clearTrack(), "idle track cleared for test");
  gSettings = {}; sSensorLogSessionId = 0;
  gCLIValidateOnly = false; debugAvailable = allocationOk = true;
  allocations = fsOperations = stopEvents = 0; now = 1000;
  actions.clear(); messages.clear();
  gpsReply = "[GPS] Start queued; initialization is pending";
  formatReply = "Log format set to TRACK"; startReply = "SUCCESS: Sensor logging started";
  loseLoggerAfterStart = false;
  fileExists = fileOpen = true; mapAvailable = false; failedAllocationName = nullptr;
  inputLines = {"1000,37.000000,-122.000000", "2000,37.001000,-122.000000"};
  boundaryHook = {};
}
static void imported() {
  String why;
  check(load("/logs/imported.csv", why), "offline imported track loads");
  check(GPSTrackManager::readStatus().pointCount == 2 && GPSTrackManager::readStatus().stats.totalDistanceM > 100,
        "imported fixture has points and prior statistics");
}
static void unchangedPreferences(bool gps, bool logger) {
  check(gSettings.gpsAutoStart == gps && gSettings.sensorLogAutoStart == logger,
        "Map live operations preserve existing boot preferences");
}

int main() {
  reset(); imported();
  for (const char* invalid : {"live", "live start extra", "live stop extra", "live unknown", "live \"start"}) {
    check(error(cmd_gpstrack(invalid)), "malformed live command rejected");
    check(actions.empty() && GPSTrackManager::readStatus().pointCount == 2,
          "malformed command has no owner effects");
  }
  gCLIValidateOnly = true;
  check(std::string(cmd_gpstrack("live start")) == "VALID" && actions.empty(),
        "validation-only does not initialize services");
  gCLIValidateOnly = false;
#if ENABLE_GPS_SENSOR
  for (unsigned guard = 0; guard < 4; ++guard) {
    reset(); imported(); const auto before = GPSTrackManager::readStatus();
    const unsigned allocated = allocations;
    if (guard == 0) gSettings.sensorLogEnabled = false;
    if (guard == 1) sensorLogPublishStarted();
    if (guard == 2) gSettings.healthLoggingEnabled = true;
    if (guard == 3) debugAvailable = false;
    check(error(cmd_gpstrack("live start")), "live startup guard refuses request");
    check(actions.empty() && allocations == allocated && GPSTrackManager::readStatus().pointCount == before.pointCount &&
          GPSTrackManager::readStatus().pointCount == 2, "preflight preserves imported track/services");
  }
  reset(); allocationOk = false;
  check(error(cmd_gpstrack("live start")) && actions.empty() && !GPSTrackManager::isLiveTracking(),
        "allocation failure happens before GPS/logger effects");
  for (bool loaded : {false, true}) for (unsigned failure = 0; failure < 8; ++failure) {
    reset(); if (loaded) imported();
    const auto before = GPSTrackManager::readStatus();
    const auto count = GPSTrackManager::readStatus().pointCount;
    if (failure == 0) gpsReply = "Error: GPS queue unavailable";
    if (failure == 1) gpsReply = nullptr;
    if (failure == 2) formatReply = "Error: Logger format refused";
    if (failure == 3) formatReply = nullptr;
    if (failure == 4) startReply = "Error: Capture file open failed";
    if (failure == 5) startReply = nullptr;
    if (failure == 6) loseLoggerAfterStart = true;
    if (failure == 7) startReply = "Queued: startup still pending";
    check(error(cmd_gpstrack("live start")), "startup failure propagated");
    check(!GPSTrackManager::isLiveTracking() && GPSTrackManager::readStatus().pointCount == before.pointCount &&
          GPSTrackManager::readStatus().pointCount == count, "failed start preserves imported track");
    check(actions.size() == (failure < 2 ? 1u : failure < 4 ? 2u : 3u),
          "startup stops immediately at failing step");
    unchangedPreferences(false, false);
  }
  for (bool gpsAuto : {false, true}) for (bool logAuto : {false, true}) {
    reset(); imported(); gSettings.gpsAutoStart = gpsAuto; gSettings.sensorLogAutoStart = logAuto;
    const unsigned allocated = allocations;
    const std::string reply = cmd_gpstrack("live start");
    check(success(reply.c_str()) && reply.find(gpsReply) != std::string::npos,
          "successful startup preserves actual borrowed GPS request status");
    check(GPSTrackManager::isLiveTracking() && allocations == allocated,
          "live startup reuses existing point storage");
    check(GPSTrackManager::readStatus().pointCount == 0 && !GPSTrackManager::readStatus().bounds.valid &&
          !GPSTrackManager::readStatus().stats.valid && GPSTrackManager::readStatus().stats.totalDistanceM == 0 &&
          GPSTrackManager::readStatus().stats.durationSec == 0 && GPSTrackManager::readStatus().stats.avgSpeedMps == 0 &&
          GPSTrackManager::readStatus().lastUpdateMs == 0, "new live capture resets all prior statistics");
    check(actions == std::vector<std::string>({"gps", "format track",
          "start /logging_captures/tracks/live.csv 1000"}), "single owner startup has no redundant settings calls");
    check(std::string(cmd_gpstrack("status")).find("waiting for GPS fix") != std::string::npos,
          "live status before first indoor fix remains active");
    check(success(cmd_gpstrack("live start")) && actions.size() == 3,
          "repeated start preserves active capture");
    String why;
    check(!GPSTrackManager::clearTrack() && !load("/logs/another.csv", why),
          "clear/load cannot replace an active live track");
    check(error(cmd_gpstrack("clear")), "CLI reports live clear refusal");
    check(GPSTrackManager::appendPoint(37, -122), "live point admitted");
    now += 1000;
    check(GPSTrackManager::appendPoint(37.001f, -122), "second live point admitted");
    check(success(cmd_gpstrack("live toggle")) && !gSensorLoggingRunning &&
          !GPSTrackManager::isLiveTracking() && GPSTrackManager::readStatus().pointCount == 2,
          "toggle stops owned capture and preserves points");
    const unsigned stops = stopEvents;
    check(success(cmd_gpstrack("live stop")) && stopEvents == stops,
          "repeated stop has no duplicate logger event");
    unchangedPreferences(gpsAuto, logAuto);
  }
  reset(); check(success(cmd_gpstrack("live toggle")), "toggle starts inactive capture");
  const auto owned = sensorLogCurrentSessionId();
  sensorLogMarkStopped(); sensorLogPublishStarted();  // CLI/Health restarts the SAME path.
  const auto replacement = sensorLogCurrentSessionId();
  check(replacement != owned && !GPSTrackManager::isLiveTracking(),
        "replacement logger invalidates stale Map ownership even with same filename");
  check(!GPSTrackManager::appendPoint(38, -123), "stale live capture no longer appends");
  check(success(cmd_gpstrack("live stop")) && sensorLogCurrentSessionId() == replacement,
        "stopping stale Map live state cannot stop unrelated replacement logger");
  reset(); check(success(cmd_gpstrack("live start")), "first capture starts before external stop");
  check(GPSTrackManager::appendPoint(37, -122), "old capture contains points before restart");
  sensorLogMarkStopped();
  check(success(cmd_gpstrack("live start")) && GPSTrackManager::isLiveTracking() &&
        GPSTrackManager::readStatus().pointCount == 0,
        "external stop followed by Map restart resets before publishing live state");
#else
  const auto before = GPSTrackManager::readStatus();
  const unsigned allocated = allocations;
  check(error(cmd_gpstrack("live start")) && error(cmd_gpstrack("live toggle")),
        "GPS-disabled build honestly refuses live startup");
  check(actions.empty() && allocations == allocated && GPSTrackManager::readStatus().pointCount == before.pointCount,
        "GPS-disabled live requests preserve offline track with no effects");
  sensorLogPublishStarted();
  const auto unrelated = sensorLogCurrentSessionId();
  check(success(cmd_gpstrack("live stop")) && sensorLogCurrentSessionId() == unrelated,
        "GPS-disabled stop does not affect unrelated logger");
  unchangedPreferences(false, false);
#endif
  snapshotTests();
  reset();
  std::printf("Map live tracking (GPS=%d): %u production-path assertions passed\n",
              ENABLE_GPS_SENSOR, checks.load());
}

static void snapshotTests() {
  reset(); imported();
  auto retained = capture();
  check(retained.pointCount == 2 && retained.status.pointCount == 2 &&
        std::string(retained.status.filename) == "/logs/imported.csv" &&
        retained.status.bounds.valid && retained.status.stats.valid &&
        retained.status.validation == TRACK_NO_MAP_LOADED,
        "one snapshot owns coherent imported metadata and points");
  const float initialLat = retained.points.get()[0].lat;
  check(GPSTrackManager::clearTrack(), "clear succeeds with retained reader snapshot");
  check(retained.points.get()[0].lat == initialLat && retained.pointCount == 2 &&
        retained.status.stats.totalDistanceM > 100 && retained.status.bounds.valid,
        "retained snapshot remains valid after owner clear and buffer release");
  auto empty = capture();
  check(empty.status.pointCount == 0 && empty.pointCount == 0 && !empty.points &&
        !empty.status.bounds.valid && !empty.status.stats.valid && empty.status.filename[0] == '\0',
        "empty snapshot contains no stale metadata or points");
  imported();
  auto tail = capture(1);
  check(tail.pointCount == 1 && tail.status.pointCount == 2 &&
        tail.points.get()[0].lat == retained.points.get()[1].lat,
        "bounded snapshot returns newest points with coherent total metadata");
  const unsigned beforeMetadata = allocations;
  auto metadata = capture(0);
  check(!metadata.points && metadata.pointCount == 0 && metadata.status.pointCount == 2 &&
        allocations == beforeMetadata, "metadata-only snapshot does not allocate points");
  allocationOk = false;
  check(!GPSTrackManager::readSnapshot(tail) && !tail.points && tail.pointCount == 0 &&
        tail.status.pointCount == 0, "allocation failure resets reused snapshot and reports failure");
  allocationOk = true;
  check(!GPSTrackManager::readSnapshot(metadata, -1) && metadata.status.pointCount == 0 &&
        !metadata.points && metadata.pointCount == 0, "negative copy bound fails and empties output");
  for (unsigned failure = 0; failure < 5; ++failure) {
    auto before = capture();
    if (failure == 0) fileExists = false;
    if (failure == 1) fileOpen = false;
    if (failure == 2) allocationOk = false;
    if (failure == 3) inputLines = {"# nothing valid", "gps: no_fix"};
    if (failure == 4) failedAllocationName = "gps.snapshot";
    String why;
    GPSTrackSnapshot rejected;
    check(!GPSTrackManager::loadTrack("/logs/failure.csv", why, rejected) && why.length() > 0,
          "failed load returns explicit error");
    fileExists = fileOpen = allocationOk = true; failedAllocationName = nullptr;
    inputLines = {"1000,37.000000,-122.000000", "2000,37.001000,-122.000000"};
    const auto after = capture();
    check(after.status.pointCount == before.status.pointCount &&
          std::string(after.status.filename) == before.status.filename &&
          after.points.get()[1].lat == before.points.get()[1].lat,
          "failed load leaves complete previous generation untouched");
  }
  GPSTrackStatus loadedStatus;
  const unsigned beforeLoad = allocations;
  failedAllocationName = "gps.snapshot";
  String metadataError;
  check(GPSTrackManager::loadTrack("/logs/imported.csv", metadataError, loadedStatus) &&
        loadedStatus.pointCount == 2 && loadedStatus.stats.valid && allocations == beforeLoad + 1,
        "metadata-only load publishes without unnecessary point snapshot allocation");
  failedAllocationName = nullptr;
  mapAvailable = true;
  auto visible = GPSTrackManager::readStatus();
  check(visible.validation == TRACK_VALID && visible.coveragePercent == 100,
        "status validates copied points against current map bounds");
  inputLines = {"1000,20.0,20.0", "2000,20.1,20.0"};
  String why;
  GPSTrackSnapshot outOfBounds;
  check(!GPSTrackManager::loadTrack("/logs/outside.csv", why, outOfBounds, true),
        "owner rejects disallowed out-of-bounds generation before replacing track");
  check(std::string(GPSTrackManager::readStatus().filename) == "/logs/imported.csv",
        "rejected bounds preserve prior track without consumer-side clear");
  check(GPSTrackManager::loadTrack("/logs/outside.csv", why, outOfBounds),
        "web-style load may publish outside track with explicit validation result");
  check(outOfBounds.status.validation == TRACK_OUT_OF_BOUNDS &&
        outOfBounds.status.coveragePercent == 0 && outOfBounds.points.get()[0].lat == 20,
        "load reply carries the exact accepted generation and its validation");
  const String firstMessage = GPSTrackManager::getValidationMessage(TRACK_VALID, 100);
  const String secondMessage = GPSTrackManager::getValidationMessage(TRACK_PARTIAL, 60);
  check(firstMessage.startsWith("Track valid") && secondMessage.startsWith("Warning") &&
        firstMessage != secondMessage, "validation messages have independent caller-owned lifetime");

  reset(); imported();
  bool readDuringLoad = false;
  boundaryHook = [&](const char* name) {
    if (std::string(name) != "file read") return;
    auto during = capture();
    check(std::string(during.status.filename) == "/logs/imported.csv" && during.pointCount == 2,
          "readers retain previous complete generation during file parsing");
    check(!GPSTrackManager::clearTrack(), "competing clear refuses while load owns lifecycle operation");
    readDuringLoad = true;
  };
  GPSTrackSnapshot loaded;
  check(GPSTrackManager::loadTrack("/logs/replaced.csv", why, loaded) && readDuringLoad,
        "load releases data mutex through parser and publishes at completion");
  boundaryHook = {};
  check(loaded.status.pointCount == 2 && std::string(loaded.status.filename) == "/logs/replaced.csv",
        "load result identifies committed generation");
  check(GPSTrackManager::clearTrack() && loaded.points.get()[0].lat == 37,
        "load response survives immediate subsequent clear");

  imported();
  bool renderMutation = false;
  boundaryHook = [&](const char* name) {
    if (std::string(name) != "render" || renderMutation) return;
    renderMutation = true;
    check(GPSTrackManager::clearTrack(), "render callback can clear owner without deadlocking");
  };
  MapRenderer renderer;
  GPSTrackManager::renderTrack(&renderer, 37, -122, 1, 1);
  boundaryHook = {};
  check(renderMutation && renderer.lines >= 7 && !GPSTrackManager::hasTrack(),
        "render completes from owned snapshot after track is freed by another action");
  imported();
  bool saveMutation = false;
  boundaryHook = [&](const char* name) {
    if (std::string(name) != "file write" || saveMutation) return;
    saveMutation = true;
    check(GPSTrackManager::clearTrack(), "save callback can clear owner without deadlocking");
  };
  char path[256]; int savedPoints = -1;
  check(GPSTrackManager::saveTrack(path, sizeof(path), &savedPoints), "snapshot saves through existing VFS boundary");
  boundaryHook = {};
  check(saveMutation && savedPoints == 2 && written.find("37.000000,-122.000000") != std::string::npos &&
        written.find("37.000999,-122.000000") != std::string::npos && !GPSTrackManager::hasTrack(),
        "save writes original generation and reports original count after concurrent clear");

  imported();
  fileOpen = false; savedPoints = -1;
  check(!GPSTrackManager::saveTrack(path, sizeof(path), &savedPoints) && savedPoints == 0,
        "save-open failure never reports a successful point count");
  fileOpen = true;

  reset();
  inputLines.clear();
  for (int point = 0; point <= MAX_TRACK_POINTS; ++point) {
    char line[80];
    std::snprintf(line, sizeof(line), "%d,%.6f,-122.000000", point * 1000, 10.0 + point / 1000.0);
    inputLines.emplace_back(line);
  }
  GPSTrackSnapshot capacityLoad;
  check(GPSTrackManager::loadTrack("/logs/capacity.csv", why, capacityLoad),
        "maximum-capacity track loads from one-extra-point fixture");
  const float lastCapacityLat = static_cast<float>(10.0 + (MAX_TRACK_POINTS - 1) / 1000.0);
  check(capacityLoad.pointCount == MAX_TRACK_POINTS && capacityLoad.status.pointCount == MAX_TRACK_POINTS &&
        capacityLoad.points.get()[0].lat == 10 &&
        capacityLoad.points.get()[MAX_TRACK_POINTS - 1].lat == lastCapacityLat &&
        capacityLoad.status.bounds.maxLat == lastCapacityLat &&
        lastSnapshotBytes == MAX_TRACK_POINTS * sizeof(GPSTrackPoint),
        "load caps points and allocates an exact-size complete snapshot");
  auto capacityFull = capture();
  check(capacityFull.pointCount == MAX_TRACK_POINTS &&
        lastSnapshotBytes == MAX_TRACK_POINTS * sizeof(GPSTrackPoint),
        "full read snapshot preserves maximum capacity and allocation length");
  auto capacityTail = capture(500);
  check(capacityTail.pointCount == 500 && capacityTail.status.pointCount == MAX_TRACK_POINTS &&
        lastSnapshotBytes == 500 * sizeof(GPSTrackPoint) &&
        capacityTail.points.get()[0].lat == static_cast<float>(10.0 + (MAX_TRACK_POINTS - 500) / 1000.0) &&
        capacityTail.points.get()[499].lat == lastCapacityLat &&
        capacityTail.status.bounds.minLat == 10 && capacityTail.status.bounds.maxLat == lastCapacityLat &&
        capacityTail.status.stats.totalDistanceM == capacityFull.status.stats.totalDistanceM,
        "500-point tail uses bounded allocation with complete-track metadata");
  check(GPSTrackManager::clearTrack() && capacityLoad.points.get()[MAX_TRACK_POINTS - 1].lat == lastCapacityLat &&
        capacityFull.points.get()[0].lat == 10 && capacityFull.pointCount == MAX_TRACK_POINTS &&
        capacityTail.points.get()[499].lat == lastCapacityLat &&
        std::string(capacityTail.status.filename) == "/logs/capacity.csv",
        "all maximum-capacity reader copies survive clear and owner buffer release");

  reset();
  std::atomic<bool> writerDone{false};
  std::atomic<unsigned> reads{0}, nonemptyReads{0};
  std::thread reader([&] {
    do {
      const auto view = capture(16);
      check(view.pointCount == view.status.pointCount && (view.pointCount == 0 || view.pointCount == 2),
            "concurrent replace reader receives complete point count");
      if (view.pointCount) {
        const bool firstGeneration = std::string(view.status.filename) == "/logs/concurrent-a.csv";
        const float firstLat = firstGeneration ? 37 : 38;
        const float firstLon = firstGeneration ? -122 : -123;
        check(view.points && view.points.get()[0].lat == firstLat && view.points.get()[1].lat > firstLat &&
              view.points.get()[0].lon == firstLon && view.status.bounds.minLat == firstLat &&
              view.status.bounds.minLon == firstLon && view.status.bounds.valid && view.status.stats.valid &&
              view.status.stats.totalDistanceM > 100 &&
              (firstGeneration || std::string(view.status.filename) == "/logs/concurrent-b.csv"),
              "concurrent replace reader gets points, bounds, stats and name from one generation");
        ++nonemptyReads;
      } else {
        check(!view.points && !view.status.bounds.valid && !view.status.stats.valid &&
              !view.status.filename[0], "concurrent clear never leaves stale metadata");
      }
      ++reads;
    } while (!writerDone);
  });
  for (unsigned pass = 0; pass < 150; ++pass) {
    GPSTrackSnapshot loadedPass;
    const bool firstGeneration = pass % 2 == 0;
    inputLines = firstGeneration ?
        std::vector<std::string>{"1000,37.000000,-122.000000", "2000,37.001000,-122.000000"} :
        std::vector<std::string>{"1000,38.000000,-123.000000", "2000,38.001000,-123.000000"};
    check(GPSTrackManager::loadTrack(firstGeneration ? "/logs/concurrent-a.csv" : "/logs/concurrent-b.csv",
                                   why, loadedPass), "concurrent load publishes");
    if (pass == 0) while (nonemptyReads == 0) std::this_thread::yield();
    check(GPSTrackManager::clearTrack(), "concurrent clear publishes");
  }
  writerDone = true; reader.join();
  check(reads > 0 && nonemptyReads > 0, "concurrent reader exercised replacing and clearing owner buffers");
#if ENABLE_GPS_SENSOR
  reset(); imported();
  auto beforeStart = capture();
  bool startupRead = false;
  boundaryHook = [&](const char* name) {
    if (std::string(name) != "logger request") return;
    auto during = capture();
    check(during.pointCount == 2 && std::string(during.status.filename) == "/logs/imported.csv" &&
          !during.status.live, "startup commands leave prior generation fully readable until publish");
    check(error(GPSTrackManager::stopLiveTracking()) && !GPSTrackManager::clearTrack(),
          "reentrant lifecycle mutations refuse during startup without deadlock");
    startupRead = true;
  };
  check(success(cmd_gpstrack("live start")), "live snapshot test starts capture");
  boundaryHook = {};
  check(startupRead && beforeStart.pointCount == 2 && beforeStart.points.get()[0].lat == 37 &&
        GPSTrackManager::readStatus().pointCount == 0,
        "startup publication resets owner while retained imported snapshot remains valid");
  writerDone = false; reads = 0;
  std::thread liveReader([&] {
    do {
      const auto view = capture(16);
      check(view.status.live && std::string(view.status.filename) == "[LIVE]" &&
            view.pointCount == std::min(view.status.pointCount, 16),
            "live reader gets coherent identity and bounded tail");
      if (view.pointCount) {
        const auto& last = view.points.get()[view.pointCount - 1];
        check(last.lat == view.status.bounds.maxLat && last.timestamp == view.status.lastUpdateMs,
              "live metadata and newest point describe same append");
        if (view.status.pointCount > 1) {
          check(view.status.stats.valid && view.status.stats.totalDistanceM > 0 &&
                view.status.stats.durationSec == (last.timestamp - 1000) / 1000.0f,
                "live statistics agree with captured point generation");
        }
      }
      ++reads;
    } while (!writerDone);
  });
  for (unsigned point = 0; point < 200; ++point) {
    now = 1000 + point * 1000;
    check(GPSTrackManager::appendPoint(37 + point * 0.001f, -122), "live writer appends unique point");
  }
  writerDone = true; liveReader.join();
  check(reads > 0, "concurrent reader exercised live appends");
  const auto liveView = capture();
  check(success(cmd_gpstrack("live stop")) && GPSTrackManager::clearTrack() &&
        liveView.pointCount == 200 && liveView.status.live && liveView.points.get()[199].lat > 37.19f,
        "live snapshot survives stop, clear and owner buffer release");
#endif
}
