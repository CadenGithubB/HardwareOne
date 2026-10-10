#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

using String = std::string;
using TaskHandle_t = void*;
using SemaphoreHandle_t = void*;
constexpr unsigned MIN_RESTART_DELAY_MS = 2000;
constexpr int DEBUG_ALWAYS = 1, DEBUG_RTC = 2, ANO_AXIS_VERTICAL = 0;
constexpr int I2C_ADDR_TOF = 0x29, I2C_ADDR_GPS = 0x10, I2C_ADDR_APDS = 0x39;
constexpr int PMTK_SET_NMEA_OUTPUT_RMCGGA = 1, PMTK_SET_NMEA_UPDATE_1HZ = 2, PGCMD_ANTENNA = 3;
#define pdMS_TO_TICKS(ms) (ms)
#define ENABLE_ESPNOW 0
template <typename... Args> void logBoundary(Args...) {}
// INSERT_LOG_BOUNDARIES

// INSERT_PRODUCTION_TYPES

enum class Handshake { Success, Failure, Timeout, TaskStopped };
struct Scenario {
  bool memory = true, mutex = true, cache = true, init = true, createTask = true, transaction = true;
  Handshake handshake = Handshake::Success;
  unsigned effects = 0, initCalls = 0, creates = 0, destroyed = 0, releases = 0, hardwareStops = 0;
  unsigned cacheInvalidations = 0, statuses = 0, now = 10000, delays = 0;
} scenario;
int taskToken, mutexToken;
bool checkMemoryAvailable(const char*, String*) { ++scenario.effects; return scenario.memory; }
SemaphoreHandle_t xSemaphoreCreateMutex() { ++scenario.effects; return scenario.mutex ? &mutexToken : nullptr; }
struct SensorCacheGuard {
  bool held;
  SensorCacheGuard(SemaphoreHandle_t mutex, unsigned, const char*) : held(mutex && scenario.cache) { ++scenario.effects; }
};
unsigned long millis() { return scenario.now; }
void sensorStatusBumpWith(const char*) { ++scenario.statuses; }
void gamepadLogHeap(const char*) {}
void anoLogHeap(const char*) {}
struct { unsigned getFreeHeap() { return 1000000; } } ESP;
struct { int tofBus = 0, gpsBus = 0, apdsBus = 0; } gSettings;

struct FakeDriver {
  ~FakeDriver() { ++scenario.destroyed; }
  int VL53L4CX_StopMeasurement() { ++scenario.hardwareStops; return 0; }
  void enableColor(bool enabled) { if (!enabled) ++scenario.hardwareStops; }
};
FakeDriver *gVL53L4CX = nullptr, *gBNO055 = nullptr, *gMLX90640 = nullptr, *gAPDS9960 = nullptr;
template <typename T> void ps_delete(T* value) { delete value; }
struct TwoWire {} wire;
struct BusManager { TwoWire* getWire(uint8_t) { return &wire; } } bus;
BusManager* i2c() { return &bus; }
template <typename F> auto i2cDeviceTransaction(uint8_t, uint8_t, unsigned, unsigned, F op) -> decltype(op()) {
  ++scenario.effects;
  return scenario.transaction ? op() : decltype(op()){};
}
template <typename F> void i2cDeviceTransactionVoid(uint8_t, uint8_t, unsigned, unsigned, F op) {
  ++scenario.effects;
  if (scenario.transaction) op();
}
enum class AllocPref { PreferPSRAM };
void* ps_alloc(size_t bytes, AllocPref, const char*) { ++scenario.effects; return std::malloc(bytes); }
struct Adafruit_GPS {
  explicit Adafruit_GPS(TwoWire*) {}
  bool begin(int) { ++scenario.initCalls; return scenario.init; }
  void sendCommand(int) { ++scenario.effects; }
  ~Adafruit_GPS() { ++scenario.destroyed; }
} *gPA1010D = nullptr;
void gpsDestroyObject() {
  if (gPA1010D) { gPA1010D->~Adafruit_GPS(); std::free(gPA1010D); gPA1010D = nullptr; ++scenario.releases; }
}

#define DECLARE_OWNER(Name) \
  bool g##Name##Running = false, g##Name##Connected = false; \
  TaskHandle_t g##Name##TaskHandle = nullptr
DECLARE_OWNER(Tof);
DECLARE_OWNER(Imu);
DECLARE_OWNER(Thermal);
DECLARE_OWNER(Gps);
DECLARE_OWNER(Apds);
DECLARE_OWNER(Presence);
DECLARE_OWNER(Rtc);
DECLARE_OWNER(FmRadio);
DECLARE_OWNER(Input);
bool gApdsColorRunning = false, gApdsProximityRunning = false, gApdsGestureRunning = false;
bool gAnoEncoderEnabled = false, gAnoEncoderConnected = false;
bool gRadioInitialized = false;
bool gImuInitRequested = false, gImuInitDone = false, gImuInitResult = false;
bool gThermalInitRequested = false, gThermalInitDone = false, gThermalInitResult = false;
bool fmRadioInitRequested = false, fmRadioInitDone = false, fmRadioInitResult = false;
bool gThermalPendingFirstFrame = false;
unsigned long gThermalArmAtMs = 0;
unsigned long gTofLastStopTime = 0, gImuLastStopTime = 0, gThermalLastStopTime = 0;
SemaphoreHandle_t sTofCacheMutex = nullptr, sImuCacheMutex = nullptr, sThermalCacheMutex = nullptr;
SemaphoreHandle_t sGpsCacheMutex = nullptr, sApdsCacheMutex = nullptr, sPresenceCacheMutex = nullptr;
SemaphoreHandle_t sRtcCacheMutex = nullptr, sAnoCacheMutex = nullptr;
TofCache sTofCache{};
ImuCache sImuCache{};
GPSCache sGpsCache{};
APDSCache sApdsCache{};
PresenceCache sPresenceCache{};
RTCCache sRtcCache{};
AnoEncoderCache sAnoEncoderCache{};

bool initialize(bool& connected, FakeDriver** driver = nullptr) {
  ++scenario.initCalls;
  if (!scenario.init) return false;
  connected = true;
  if (driver && !*driver) *driver = new FakeDriver;
  return true;
}
bool tofInit() { return initialize(gTofConnected, &gVL53L4CX); }
bool apdsInit() { return initialize(gApdsConnected, &gAPDS9960); }
bool presenceInit() { return initialize(gPresenceConnected); }
bool rtcInit() { return initialize(gRtcConnected); }
bool gamepadInit() { return initialize(gInputConnected); }
bool anoEncoderInit() { const bool ok = initialize(gAnoEncoderConnected); gInputConnected = ok; return ok; }
bool thermalResetCacheForStart() { ++scenario.effects; return scenario.cache; }
bool inputCacheInit() { ++scenario.effects; return scenario.mutex; }
bool inputCacheReset(int, int, int, bool) { ++scenario.effects; return scenario.cache; }
bool inputCacheInvalidate() { ++scenario.cacheInvalidations; return true; }
bool invalidateAnoCache() { ++scenario.cacheInvalidations; return true; }
void fmRadioDeinit() { ++scenario.hardwareStops; gRadioInitialized = gFmRadioConnected = gFmRadioRunning = false; }
bool createTask(bool running, TaskHandle_t& handle) {
  ++scenario.creates;
  assert(running && handle == nullptr);
  if (!scenario.createTask) return false;
  handle = &taskToken;
  return true;
}
#define CREATE_OWNER(Function, Name) bool create##Function##Task() { return createTask(g##Name##Running, g##Name##TaskHandle); }
CREATE_OWNER(ToF, Tof)
CREATE_OWNER(IMU, Imu)
CREATE_OWNER(Thermal, Thermal)
CREATE_OWNER(GPS, Gps)
CREATE_OWNER(APDS, Apds)
CREATE_OWNER(Presence, Presence)
CREATE_OWNER(RTC, Rtc)
CREATE_OWNER(FMRadio, FmRadio)
CREATE_OWNER(Input, Input)

void completeHandshake(bool requested, bool& running, bool& connected, bool& done, bool& result) {
  if (!requested || scenario.handshake == Handshake::Timeout) return;
  if (scenario.handshake == Handshake::TaskStopped) { running = false; return; }
  done = true;
  result = scenario.handshake == Handshake::Success;
  connected = result;
}
void delay(unsigned ms) {
  scenario.now += ms;
  ++scenario.delays;
  completeHandshake(gImuInitRequested, gImuRunning, gImuConnected, gImuInitDone, gImuInitResult);
  completeHandshake(gThermalInitRequested, gThermalRunning, gThermalConnected, gThermalInitDone, gThermalInitResult);
  completeHandshake(fmRadioInitRequested, gFmRadioRunning, gFmRadioConnected, fmRadioInitDone, fmRadioInitResult);
}

// INSERT_PRODUCTION_STARTERS

struct Owner {
  const char* name;
  bool (*start)();
  bool& running;
  bool& connected;
  TaskHandle_t& handle;
  bool deferred;
};
std::array<Owner, 10> owners = {{
  {"ToF", tofStartInternal, gTofRunning, gTofConnected, gTofTaskHandle, false},
  {"IMU", imuStartInternal, gImuRunning, gImuConnected, gImuTaskHandle, true},
  {"Thermal", thermalStartInternal, gThermalRunning, gThermalConnected, gThermalTaskHandle, true},
  {"GPS", gpsStartInternal, gGpsRunning, gGpsConnected, gGpsTaskHandle, false},
  {"APDS", apdsStartInternal, gApdsRunning, gApdsConnected, gApdsTaskHandle, false},
  {"Presence", presenceStartInternal, gPresenceRunning, gPresenceConnected, gPresenceTaskHandle, false},
  {"RTC", rtcStartInternal, gRtcRunning, gRtcConnected, gRtcTaskHandle, false},
  {"FM", fmRadioStartInternal, gFmRadioRunning, gFmRadioConnected, gFmRadioTaskHandle, true},
  {"Gamepad", gamepadStartInternal, gInputRunning, gInputConnected, gInputTaskHandle, false},
  {"ANO", anoStartInternal, gInputRunning, gInputConnected, gInputTaskHandle, false},
}};

void reset() {
  delete gVL53L4CX; delete gBNO055; delete gMLX90640; delete gAPDS9960;
  gVL53L4CX = gBNO055 = gMLX90640 = gAPDS9960 = nullptr;
  gpsDestroyObject();
  for (auto& owner : owners) { owner.running = false; owner.connected = false; owner.handle = nullptr; }
  gApdsColorRunning = gApdsProximityRunning = gApdsGestureRunning = false;
  gAnoEncoderEnabled = gAnoEncoderConnected = gRadioInitialized = false;
  gImuInitRequested = gImuInitDone = gImuInitResult = false;
  gThermalInitRequested = gThermalInitDone = gThermalInitResult = false;
  fmRadioInitRequested = fmRadioInitDone = fmRadioInitResult = false;
  gThermalPendingFirstFrame = false; gThermalArmAtMs = 0;
  gTofLastStopTime = gImuLastStopTime = gThermalLastStopTime = 0;
  sTofCacheMutex = sImuCacheMutex = sThermalCacheMutex = sGpsCacheMutex = &mutexToken;
  sApdsCacheMutex = sPresenceCacheMutex = sRtcCacheMutex = sAnoCacheMutex = &mutexToken;
  sTofCache = {}; sImuCache = {}; sGpsCache = {}; sApdsCache = {};
  sPresenceCache = {}; sRtcCache = {}; sAnoEncoderCache = {};
  scenario = {};
}

void testEveryOwner() {
  for (auto& owner : owners) {
    reset();
    owner.handle = &taskToken;
    owner.connected = true;
    sTofCache.tofDataValid = sImuCache.imuDataValid = sGpsCache.dataValid = true;
    sApdsCache.apdsDataValid = sPresenceCache.dataValid = sRtcCache.dataValid = sAnoEncoderCache.dataValid = true;
    assert(!owner.start());
    assert(!owner.running && owner.connected && owner.handle == &taskToken);
    assert(scenario.effects == 0 && scenario.initCalls == 0 && scenario.creates == 0);
    assert(scenario.destroyed == 0 && scenario.hardwareStops == 0 && scenario.cacheInvalidations == 0);
    assert(sTofCache.tofDataValid && sImuCache.imuDataValid && sGpsCache.dataValid);
    assert(sApdsCache.apdsDataValid && sPresenceCache.dataValid && sRtcCache.dataValid && sAnoEncoderCache.dataValid);

    reset();
    owner.running = true; owner.connected = true; owner.handle = &taskToken;
    gApdsColorRunning = true;
    assert(owner.start());
    assert(scenario.effects == 0 && scenario.creates == 0 && scenario.initCalls == 0);

    reset();
    scenario.memory = false;
    assert(!owner.start());
    assert(!owner.running && owner.handle == nullptr && scenario.creates == 0 && scenario.initCalls == 0);

    reset();
    scenario.createTask = false;
    // Exercise release of a retained object when task allocation fails too.
    if (std::strcmp(owner.name, "IMU") == 0) gBNO055 = new FakeDriver;
    if (std::strcmp(owner.name, "Thermal") == 0) gMLX90640 = new FakeDriver;
    assert(!owner.start());
    assert(!owner.running && !owner.connected && owner.handle == nullptr && scenario.creates == 1);
    assert(!gAnoEncoderEnabled && !gAnoEncoderConnected);
    assert(!gApdsColorRunning && !gApdsProximityRunning && !gApdsGestureRunning);
    assert(!gImuInitRequested && !gThermalInitRequested && !fmRadioInitRequested);
    assert(!gThermalPendingFirstFrame && gThermalArmAtMs == 0);
    assert(gVL53L4CX == nullptr && gBNO055 == nullptr && gMLX90640 == nullptr && gAPDS9960 == nullptr && gPA1010D == nullptr);
    if (std::strcmp(owner.name, "ToF") == 0 || std::strcmp(owner.name, "APDS") == 0) {
      assert(scenario.destroyed == 1 && scenario.hardwareStops == 1);
    }
    if (std::strcmp(owner.name, "IMU") == 0 || std::strcmp(owner.name, "Thermal") == 0) {
      assert(scenario.destroyed == 1);
    }
    if (std::strcmp(owner.name, "GPS") == 0) assert(scenario.destroyed == 1 && scenario.releases == 1);
    if (std::strcmp(owner.name, "FM") == 0) assert(scenario.hardwareStops == 1);
    if (std::strcmp(owner.name, "Gamepad") == 0) assert(scenario.cacheInvalidations == 1);
    if (std::strcmp(owner.name, "ANO") == 0) assert(scenario.cacheInvalidations == 2);

    reset();
    if (!owner.deferred) {
      scenario.init = false;
      assert(!owner.start());
      assert(!owner.running && !owner.connected && owner.handle == nullptr && scenario.creates == 0);
    }

    reset();
    assert(owner.start());
    assert(owner.running && owner.connected && owner.handle == &taskToken && scenario.creates == 1);
    if (std::strcmp(owner.name, "ANO") == 0) assert(gAnoEncoderEnabled && gAnoEncoderConnected);
    std::printf("%s: cleanup guard, idempotence, memory/init/task failure and start passed\n", owner.name);
  }
}

void testDeferredHandshakes() {
  for (auto& owner : owners) {
    if (!owner.deferred) continue;
    for (auto outcome : {Handshake::Failure, Handshake::Timeout, Handshake::TaskStopped}) {
      reset();
      scenario.handshake = outcome;
      assert(!owner.start());
      assert(!owner.running && owner.handle == &taskToken && scenario.creates == 1);
      assert(scenario.now <= 13000);
      if (outcome == Handshake::Timeout) assert(scenario.now == 13000);
      // A timed-out task may still own an in-flight init. No next start can
      // reset its handshake or resources before it acknowledges cleanup.
      const unsigned effects = scenario.effects, creates = scenario.creates, delays = scenario.delays;
      assert(!owner.start());
      assert(scenario.effects == effects && scenario.creates == creates && scenario.delays == delays);
    }
  }
}

int main() {
  testEveryOwner();
  testDeferredHandshakes();
  reset();
  std::puts("Sensor start outcome tests passed (complete production owner functions)");
}
