#include <cassert>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

#define ENABLE_GPS_SENSOR 1
#define ENABLE_MAPS 1
#define ENABLE_OLED_DISPLAY 1
#define ESP_OK 0
#define LOG_GPS 1u
#define CACHE_MUTEX_TIMEOUT_MS 100
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define INFO_GPS_LIFECYCLEF(...) ((void)0)
#define DEBUG_GPS_LIFECYCLEF(...) ((void)0)
#define ERROR_GPSF(...) ((void)0)
using TickType_t = uint32_t;
struct Semaphore { bool held = false; } semaphore;
using SemaphoreHandle_t = Semaphore*;
static bool admit = true;
static bool memoryAvailable = true;
static bool createMutex = true;
static std::vector<uint32_t> waits;
static bool isHeldByCurrentTask(SemaphoreHandle_t m) { return m && m->held; }
static int xSemaphoreTake(SemaphoreHandle_t m, TickType_t ms) {
  waits.push_back(ms);
  if (!m || m->held || !admit) return 0;
  m->held = true;
  return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t m) { assert(m && m->held); m->held = false; }
static SemaphoreHandle_t xSemaphoreCreateMutex() { return createMutex ? &semaphore : nullptr; }
static bool checkMemoryAvailable(const char*, void*) { return memoryAvailable; }

// INSERT_TYPES
// INSERT_STATE

bool gGpsRunning = true;
bool gGpsConnected = true;
struct FakeGPS {
  float latitudeDegrees = 22.5f, longitudeDegrees = -93.25f, altitude = 18.75f;
  float speed = 7.5f, angle = 80.5f;
  bool fix = true;
  uint8_t fixquality = 1, satellites = 6, year = 25, month = 12, day = 31;
  uint8_t hour = 23, minute = 58, seconds = 59;
} driver;
static FakeGPS* gPA1010D = &driver;
static struct { bool mqttPublishGPS = true; } gSettings;
static std::string response;
static void sendJsonResponse(void*, const char* text, int len = -1) {
  assert(!semaphore.held);
  response.assign(text, len < 0 ? std::strlen(text) : static_cast<size_t>(len));
}
static int trackedSnprintf(char* dst, size_t cap, const char* format, ...) {
#if !TEST_BASELINE
  assert(!semaphore.held);  // formatting must follow the short copy-only hold
#endif
  va_list args;
  va_start(args, format);
  const int result = std::vsnprintf(dst, cap, format, args);
  va_end(args);
  return result;
}
#define snprintf trackedSnprintf
static int destroyed = 0, exited = 0, delays = 0;
static int trackPoints = 0, events = 0, redraws = 0;
[[maybe_unused]] static constexpr int SYSEVT_GPS_FIX = 1, SYSEVT_GPS_LOST = 2, OLED_GPS_DATA = 3;
[[maybe_unused]] static int currentOLEDMode = OLED_GPS_DATA;
struct GPSTrackManager {
  static bool isLiveTracking() { assert(!semaphore.held); return true; }
  static void appendPoint(float lat, float lon) {
    assert(!semaphore.held && lat == driver.latitudeDegrees && lon == driver.longitudeDegrees);
    ++trackPoints;
  }
};
static void systemEventPost(int, const char* = nullptr, const char* = nullptr) {
  assert(!semaphore.held); ++events;
}
static void oledMarkDirty() { assert(!semaphore.held); ++redraws; }
static bool stopAfterDelay = false;
static void* gGpsTaskHandle = nullptr;
struct StopPending {};
static void gpsDestroyObject() {
  assert(!semaphore.held);
  ++destroyed;
  gPA1010D = nullptr;
}
static void vTaskDelay(TickType_t ticks) {
  assert(ticks == 10 && !semaphore.held);
  ++delays;
  if (stopAfterDelay) throw StopPending{};
  admit = true;
}
#define SENSOR_TASK_EXIT(subsys, ...) do { ++exited; return; } while (false)

// INSERT_FUNCTIONS

static auto fields(const GPSCache& s) {
  return std::make_tuple(s.latitude, s.longitude, s.altitude, s.speed, s.angle,
                        s.hasFix, s.fixQuality, s.satellites, s.year, s.month, s.day,
                        s.hour, s.minute, s.second, s.dataValid, s.lastUpdate);
}
static void reset() {
  DATA = {};
  semaphore = {};
  MUTEX = &semaphore;
  DATA.latitude = -12.25f; DATA.longitude = 130.5f; DATA.altitude = 81.25f;
  DATA.speed = 12.5f; DATA.angle = 42.25f;
  DATA.hasFix = true; DATA.fixQuality = 2; DATA.satellites = 9;
  DATA.year = 2026; DATA.month = 10; DATA.day = 8;
  DATA.hour = 19; DATA.minute = 5; DATA.second = 7;
  DATA.dataValid = true; DATA.lastUpdate = 12345;
  gGpsRunning = true; gGpsConnected = true; driver = {}; gPA1010D = &driver;
  gSettings.mqttPublishGPS = true;
  admit = memoryAvailable = createMutex = true;
  waits.clear(); response.clear();
  destroyed = exited = delays = 0; stopAfterDelay = false;
  trackPoints = events = redraws = 0;
#if !TEST_BASELINE
  sGpsEvtHasFix = false; sGpsNoFixSinceMs = 0;
#endif
}
static void waited(uint32_t expected) {
  assert(waits.size() == 1 && waits[0] == expected && !semaphore.held);
  waits.clear();
}
static std::string json() {
  char out[512];
  const int n = gpsBuildDataJSON(out, sizeof(out));
  assert(n > 0 && n == static_cast<int>(std::strlen(out)));
  waited(50);
  return out;
}
static void emit(const std::string& value) { std::cout << value << '\n'; }
static void commonContracts() {
  const std::string fixed = "{\"valid\":true,\"connected\":true,\"ts\":12345,\"fix\":1,\"quality\":2,\"sats\":9,\"lat\":-12.250000,\"lon\":130.500000,\"alt\":81.25,\"speed\":12.50}";
  reset(); assert(json() == fixed); emit(fixed);
  // Every buffer capacity, including exactly the byte count without room for
  // NUL, exercises the actual builder and envelope without an overflow model.
  for (size_t cap = 0; cap <= fixed.size() + 2; ++cap) {
    std::vector<char> buf(cap + 2, 'Z');
    const int n = gpsBuildDataJSON(buf.data() + 1, cap);
    assert(buf.front() == 'Z' && buf.back() == 'Z');
    if (cap <= fixed.size()) assert(n == 0);
    else assert(n == static_cast<int>(fixed.size()) && std::string(buf.data() + 1) == fixed);
  }
  assert(gpsBuildDataJSON(nullptr, 20) == 0);
  reset(); DATA.hasFix = false;
  const std::string nofix = "{\"valid\":true,\"connected\":true,\"ts\":12345,\"fix\":0,\"quality\":0,\"sats\":9,\"lat\":0,\"lon\":0,\"alt\":0,\"speed\":0}";
  assert(json() == nofix); emit(nofix);
  reset(); admit = false;
  const std::string unavailable = "{\"valid\":false,\"connected\":true,\"ts\":0,\"fix\":0,\"quality\":0,\"sats\":0,\"lat\":0,\"lon\":0,\"alt\":0,\"speed\":0}";
  assert(json() == unavailable); emit(unavailable);

  for (int state = 0; state < 4; ++state) {
    reset();
    if (state == 1) DATA.hasFix = false;
    if (state == 2) DATA.dataValid = false;
    if (state == 3) admit = false;
    char out[256]{};
    gpsG2FormatValue(out, sizeof(out)); waited(5); emit(out);
    g2BuildGpsReadout(out, sizeof(out)); waited(5); emit(out);
    assert(webGps() == ESP_OK); waited(50); emit(response);
    const auto snap = loggingGps(LOG_GPS); waited(10);
    assert(snap.gpsFix == (state == 0));
    assert(snap.gpsHasTime == (state == 0));
    if (state == 0) {
      assert(snap.gpsLatitude == -12.25f && snap.gpsLongitude == 130.5f);
      assert(snap.gpsAltitude == 81.25f && snap.gpsSpeed == 12.5f);
      assert(snap.gpsSatellites == 9 && snap.gpsFixQuality == 2);
      assert(snap.gpsHour == 19 && snap.gpsMinute == 5 && snap.gpsSecond == 7);
    }
    // Legacy MQTT had no lock; the new timeout omission is tested separately.
    if (state != 3) {
      char mqtt[16384]{};
      const int n = mqttGps(mqtt);
      assert(n == static_cast<int>(std::strlen(mqtt)));
#if !TEST_BASELINE
      waited(10);
#endif
      const std::string expected = state == 1 ? ",\"gps\":{\"fix\":false,\"satellites\":9}" :
          ",\"gps\":{\"lat\":-12.250000,\"lon\":130.500000,\"alt\":81.2,\"speed\":12.5,\"satellites\":9}";
      assert(mqtt == expected); emit(mqtt);
    }
  }
  reset(); gGpsRunning = false;
  assert(webGps() == ESP_OK && response == "{\"error\":\"not_enabled\"}");
  assert(waits.empty()); emit(response);
  char mqtt[16384]{};
  assert(mqttGps(mqtt) == 0 && waits.empty());
  reset(); gSettings.mqttPublishGPS = false;
  assert(mqttGps(mqtt) == 0 && waits.empty());
  (void)loggingGps(0); assert(waits.empty());
}

#if !TEST_BASELINE
static void ownerContracts() {
  reset();
  GPSCache out{};
  assert(gpsCacheSnapshot(out)); waited(10);
  assert(fields(out) == fields(DATA));
  out.latitude = 99; assert(DATA.latitude == -12.25f);
  for (uint32_t timeout : {5, 10, 50}) {
    admit = true;
    assert(gpsCacheSnapshot(out, timeout)); waited(timeout);
    admit = false;
    assert(!gpsCacheSnapshot(out, timeout)); waited(timeout);
    assert(fields(out) == fields(GPSCache{}));
  }
  reset(); MUTEX = nullptr; out.hasFix = true;
  assert(!gpsCacheSnapshot(out) && fields(out) == fields(GPSCache{}));
  assert(waits.empty());
  char jsonOut[512];
  assert(gpsBuildDataJSON(jsonOut, sizeof(jsonOut)) > 0 && waits.empty());
  assert(std::string(jsonOut).find("\"valid\":false") != std::string::npos);
  reset(); admit = false;
  char mqtt[16384]{};
  assert(mqttGps(mqtt) == 0); waited(10);

  reset(); const auto old = fields(DATA); admit = false;
  assert(!gpsInvalidateCache()); waited(50);
  assert(fields(DATA) == old);
  admit = true;
  assert(gpsInvalidateCache()); waited(50);
  assert(!DATA.dataValid && !DATA.hasFix && !DATA.fixQuality && !DATA.satellites);
  assert(DATA.latitude == -12.25f && DATA.lastUpdate == 12345 && DATA.year == 2026);

  reset(); gGpsRunning = false; admit = false; stopAfterDelay = true;
  try { runStopBranch(); assert(false); } catch (const StopPending&) {}
  waited(50);
  assert(destroyed == 0 && exited == 0 && delays == 1 && gPA1010D);
  assert(DATA.hasFix && DATA.dataValid);  // failed take must not write unlocked
  admit = true; stopAfterDelay = false;
  runStopBranch(); waited(50);
  assert(destroyed == 1 && exited == 1 && !gPA1010D && !gGpsConnected);
  assert(!DATA.hasFix && !DATA.dataValid);
  reset(); gGpsRunning = false; admit = false;
  runStopBranch();  // The delay admits the next take inside the actual loop.
  assert((waits == std::vector<uint32_t>{50, 50}));
  assert(delays == 1 && destroyed == 1 && exited == 1 && !DATA.hasFix);

  reset(); gGpsRunning = false; gGpsTaskHandle = reinterpret_cast<void*>(1);
  const auto pending = fields(DATA);
  assert(!runStartReset() && fields(DATA) == pending && waits.empty());
  gGpsTaskHandle = nullptr;
  admit = false;
  assert(!runStartReset()); waited(100);
  assert(DATA.hasFix && DATA.year == 2026 && !gGpsRunning);
  admit = true;
  assert(runStartReset()); waited(100);
  assert(fields(DATA) == fields(GPSCache{}));
  reset(); gGpsRunning = false; MUTEX = nullptr; createMutex = false;
  assert(!runStartReset() && waits.empty());
  reset(); gGpsRunning = false; memoryAvailable = false;
  assert(!runStartReset() && waits.empty());

  reset(); const auto previous = fields(DATA);
  runPublication(false, 999);
  assert(waits.empty() && fields(DATA) == previous && !events && !redraws);
  runPublication(true, 54321); waited(50);
  assert(DATA.latitude == 22.5f && DATA.longitude == -93.25f && DATA.altitude == 18.75f);
  assert(DATA.speed == 7.5f && DATA.angle == 80.5f && DATA.hasFix);
  assert(DATA.fixQuality == 1 && DATA.satellites == 6 && DATA.dataValid);
  assert(DATA.year == 2025 && DATA.month == 12 && DATA.day == 31);
  assert(DATA.hour == 23 && DATA.minute == 58 && DATA.second == 59 && DATA.lastUpdate == 54321);
  assert(trackPoints == 1 && events == 1 && redraws == 1);
  reset(); const auto beforeFailure = fields(DATA); admit = false;
  runPublication(true, 54321); waited(50);
  assert(fields(DATA) == beforeFailure && trackPoints == 0);
  assert(events == 1 && redraws == 1);  // existing callbacks still follow parsed data
}
#endif

int main() {
  commonContracts();
#if !TEST_BASELINE
  ownerContracts();
#endif
}
