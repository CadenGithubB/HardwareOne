#include <Arduino.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

void webBatchTestStringAppend() {}
#define EXT_RAM_BSS_ATTR
#define INFO_MAPSF(...) do {} while (0)
#define ERROR_MAPSF(...) do {} while (0)
static unsigned checks;
static void check(bool ok, const char* message) {
  ++checks;
  if (!ok) throw std::runtime_error(message);
}
class MapRenderer;
// INSERT_TYPES
static bool gCLIValidateOnly, debugAvailable = true, allocationOk = true;
static bool gSensorLoggingRunning;
static String gSensorLogPath;
static struct {
  bool sensorLogEnabled = true, healthLoggingEnabled = false;
  bool gpsAutoStart = false, sensorLogAutoStart = false;
} gSettings;
static std::mutex sSensorLogSessionMux;
static uint32_t sSensorLogSessionId;
static unsigned criticalDepth, allocations, fsOperations, now = 1000;
static bool observeNextSessionRead, observeRestartPublication;
static size_t publicationObservedStrlcpy(char* target, const char* source, size_t size) {
  // This boundary runs after adoption of the replacement logger ID, but before
  // the owner's final live publication. Observing only its earlier ID read
  // would miss a stale live flag becoming visible during the subsequent reset.
  if (observeRestartPublication && std::strcmp(source, "[LIVE]") == 0) {
    check(!GPSTrackManager::isLiveTracking(),
          "replacement identity stays hidden until final live publication");
  }
  const size_t length = std::strlen(source);
  if (size) { const size_t copied = std::min(length, size - 1); std::memcpy(target, source, copied); target[copied] = '\0'; }
  return length;
}
#define strlcpy publicationObservedStrlcpy
static void portENTER_CRITICAL(std::mutex* mux) {
  if (observeNextSessionRead) {
    observeNextSessionRead = false;
    check(!GPSTrackManager::isLiveTracking(),
          "old live flag cannot publish replacement ownership before reset");
  }
  mux->lock(); ++criticalDepth;
}
static void portEXIT_CRITICAL(std::mutex* mux) { --criticalDepth; mux->unlock(); }
static std::vector<std::string> actions, messages;
static unsigned stopEvents;
enum { SYSEVT_SENSOR_STOPPED = 1 };
static void systemEventPost(int event, const char*) {
  check(criticalDepth == 0 && event == SYSEVT_SENSOR_STOPPED,
        "stop side effects occur after owner critical section");
  ++stopEvents;
}
static void broadcastOutput(const char* message) { messages.emplace_back(message); }
static char debugBuffer[1024];
static bool ensureDebugBuffer() { return debugAvailable; }
static char* getDebugBuffer() { return debugBuffer; }
static void cliHint(const char*) {}
static unsigned long millis() { return now; }
enum class AllocPref { PreferPSRAM };
static void* ps_alloc(size_t size, AllocPref, const char*) {
  ++allocations;
  return allocationOk ? std::malloc(size) : nullptr;
}
static const char* currentAuthContext() { return "owner"; }
struct FsLockGuard { explicit FsLockGuard(const char*) { ++fsOperations; } };
static void fsLock(const char*) { ++fsOperations; }
static void fsUnlock() {}
struct File {
  size_t at = 0;
  explicit operator bool() const { return true; }
  bool available() const { return at < 2; }
  String readStringUntil(char) {
    return at++ == 0 ? "1000,37.000000,-122.000000" : "2000,37.001000,-122.000000";
  }
  void close() {}
};
namespace VFS {
static bool existsGuarded(const char*, const char*) { ++fsOperations; return true; }
static File openGuarded(const char*, const char*, const char*) { ++fsOperations; return {}; }
static bool removeGuarded(const char*, const char*) { ++fsOperations; return true; }
}
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
  actions.emplace_back("gps"); return borrowed(gpsReply);
}
static const char* cmd_sensorlog(const String& command);
#endif

// INSERT_PRODUCTION

TrackValidation GPSTrackManager::validateTrack(float& coverage) {
  coverage = 0; return hasTrack() ? TRACK_NO_MAP_LOADED : TRACK_EMPTY;
}
#if ENABLE_GPS_SENSOR
static const char* cmd_sensorlog(const String& command) {
  actions.emplace_back(command.c_str());
  if (command == "format track") return borrowed(formatReply);
  check(command == "start /logging_captures/tracks/live.csv 1000",
        "Map requests literal existing capture target at one second");
  if (startReply && std::strncmp(startReply, "SUCCESS:", 8) == 0) {
    gSensorLogPath = "/logging_captures/tracks/live.csv";
    sensorLogPublishStarted();
    if (loseLoggerAfterStart) sensorLogMarkStopped();
    observeNextSessionRead = observeRestartPublication;
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
  observeNextSessionRead = observeRestartPublication = false;
}
static void imported() {
  String why;
  check(GPSTrackManager::loadTrack("/logs/imported.csv", why), "offline imported track loads");
  check(GPSTrackManager::getPointCount() == 2 && GPSTrackManager::getStats().totalDistanceM > 100,
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
    check(actions.empty() && GPSTrackManager::getPointCount() == 2,
          "malformed command has no owner effects");
  }
  gCLIValidateOnly = true;
  check(std::string(cmd_gpstrack("live start")) == "VALID" && actions.empty(),
        "validation-only does not initialize services");
  gCLIValidateOnly = false;
#if ENABLE_GPS_SENSOR
  for (unsigned guard = 0; guard < 4; ++guard) {
    reset(); imported(); const auto* points = GPSTrackManager::getPoints();
    const unsigned allocated = allocations;
    if (guard == 0) gSettings.sensorLogEnabled = false;
    if (guard == 1) sensorLogPublishStarted();
    if (guard == 2) gSettings.healthLoggingEnabled = true;
    if (guard == 3) debugAvailable = false;
    check(error(cmd_gpstrack("live start")), "live startup guard refuses request");
    check(actions.empty() && allocations == allocated && GPSTrackManager::getPoints() == points &&
          GPSTrackManager::getPointCount() == 2, "preflight preserves imported track/services");
  }
  reset(); allocationOk = false;
  check(error(cmd_gpstrack("live start")) && actions.empty() && !GPSTrackManager::isLiveTracking(),
        "allocation failure happens before GPS/logger effects");
  for (bool loaded : {false, true}) for (unsigned failure = 0; failure < 8; ++failure) {
    reset(); if (loaded) imported();
    const auto* points = GPSTrackManager::getPoints();
    const auto count = GPSTrackManager::getPointCount();
    if (failure == 0) gpsReply = "Error: GPS queue unavailable";
    if (failure == 1) gpsReply = nullptr;
    if (failure == 2) formatReply = "Error: Logger format refused";
    if (failure == 3) formatReply = nullptr;
    if (failure == 4) startReply = "Error: Capture file open failed";
    if (failure == 5) startReply = nullptr;
    if (failure == 6) loseLoggerAfterStart = true;
    if (failure == 7) startReply = "Queued: startup still pending";
    check(error(cmd_gpstrack("live start")), "startup failure propagated");
    check(!GPSTrackManager::isLiveTracking() && GPSTrackManager::getPoints() == points &&
          GPSTrackManager::getPointCount() == count, "failed start preserves imported track");
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
    check(GPSTrackManager::getPointCount() == 0 && !GPSTrackManager::getBounds().valid &&
          !GPSTrackManager::getStats().valid && GPSTrackManager::getStats().totalDistanceM == 0 &&
          GPSTrackManager::getStats().durationSec == 0 && GPSTrackManager::getStats().avgSpeedMps == 0 &&
          GPSTrackManager::getLastUpdateTime() == 0, "new live capture resets all prior statistics");
    check(actions == std::vector<std::string>({"gps", "format track",
          "start /logging_captures/tracks/live.csv 1000"}), "single owner startup has no redundant settings calls");
    check(std::string(cmd_gpstrack("status")).find("waiting for GPS fix") != std::string::npos,
          "live status before first indoor fix remains active");
    check(success(cmd_gpstrack("live start")) && actions.size() == 3,
          "repeated start preserves active capture");
    String why;
    check(!GPSTrackManager::clearTrack() && !GPSTrackManager::loadTrack("/logs/another.csv", why),
          "clear/load cannot replace an active live track");
    check(error(cmd_gpstrack("clear")), "CLI reports live clear refusal");
    check(GPSTrackManager::appendPoint(37, -122), "live point admitted");
    now += 1000;
    check(GPSTrackManager::appendPoint(37.001f, -122), "second live point admitted");
    check(success(cmd_gpstrack("live toggle")) && !gSensorLoggingRunning &&
          !GPSTrackManager::isLiveTracking() && GPSTrackManager::getPointCount() == 2,
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
  observeRestartPublication = true;
  check(success(cmd_gpstrack("live start")) && GPSTrackManager::isLiveTracking() &&
        GPSTrackManager::getPointCount() == 0,
        "external stop followed by Map restart resets before publishing live state");
#else
  const auto* points = GPSTrackManager::getPoints();
  const unsigned allocated = allocations;
  check(error(cmd_gpstrack("live start")) && error(cmd_gpstrack("live toggle")),
        "GPS-disabled build honestly refuses live startup");
  check(actions.empty() && allocations == allocated && GPSTrackManager::getPoints() == points,
        "GPS-disabled live requests preserve offline track with no effects");
  sensorLogPublishStarted();
  const auto unrelated = sensorLogCurrentSessionId();
  check(success(cmd_gpstrack("live stop")) && sensorLogCurrentSessionId() == unrelated,
        "GPS-disabled stop does not affect unrelated logger");
  unchangedPreferences(false, false);
#endif
  reset();
  std::printf("Map live tracking (GPS=%d): %u production-path assertions passed\n",
              ENABLE_GPS_SENSOR, checks);
}
