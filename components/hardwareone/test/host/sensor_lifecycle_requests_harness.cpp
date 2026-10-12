#include <array>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// Real queue state and methods; private access only supplies/reset platform state.
#define DEBUG_SYSTEM_H
#define SYSTEM_BUILDCONFIG_H
#define DEBUG_I2CF(...) do {} while (0)
#define private public
#include "System_I2C_Manager.h"
#undef private
#define ENABLE_GPIO_ENCODER 0
#define ENABLE_ANO_ENCODER 0
#define ENABLE_GAMEPAD_SENSOR 1
#define ENABLE_THERMAL_SENSOR 1
#define ENABLE_TOF_SENSOR 1
#define ENABLE_IMU_SENSOR 1
#define ENABLE_GPS_SENSOR 1
#define ENABLE_FM_RADIO 1
#define ENABLE_APDS_SENSOR 1
#define ENABLE_RTC_SENSOR 1
#define ENABLE_PRESENCE_SENSOR 1

struct FakeSemaphore { bool available = true, held = false; };
static constexpr TickType_t portMAX_DELAY = UINT32_MAX;
using TaskHandle_t = void*;
using UBaseType_t = uint32_t;
static constexpr uint32_t SENSOR_QUEUE_STACK_WORDS = 4096;
TwoWire Wire, Wire1;
bool gI2CBusRunning = true;
static uint32_t now = 100;
static std::function<void()> afterUnlock, duringDelay, duringInit, duringProbe;
static unsigned takes, gives, probeCalls, pauseCalls, resumeCalls, pauseDepth, taskDeletes;
static unsigned fmDeinitCalls, rtcStopCalls;
static bool debugEnabled, bufferAvailable, fmHardwareReady;
static uint8_t probeResult, probeAddress, probeBus;
static char debugBuffer[1024];
static std::array<bool, I2C_DEVICE_COUNT> running;
static std::array<TaskHandle_t, I2C_DEVICE_COUNT> handles;
static std::array<unsigned long, I2C_DEVICE_COUNT> stopTimes;
static std::array<unsigned, I2C_DEVICE_COUNT> starts;
static std::array<bool, I2C_DEVICE_COUNT> startResult, runningAfterStart;

static bool& gThermalRunning = running[I2C_DEVICE_THERMAL];
static bool& gTofRunning = running[I2C_DEVICE_TOF];
static bool& gImuRunning = running[I2C_DEVICE_IMU];
static bool& gInputRunning = running[I2C_DEVICE_INPUT];
static bool& gGpsRunning = running[I2C_DEVICE_GPS];
static bool& gFmRadioRunning = running[I2C_DEVICE_FMRADIO];
static bool& gApdsRunning = running[I2C_DEVICE_APDS];
static bool& gRtcRunning = running[I2C_DEVICE_RTC];
static bool& gPresenceRunning = running[I2C_DEVICE_PRESENCE];
static bool gApdsColorRunning, gApdsProximityRunning, gApdsGestureRunning;
static TaskHandle_t& gThermalTaskHandle = handles[I2C_DEVICE_THERMAL];
static TaskHandle_t& gTofTaskHandle = handles[I2C_DEVICE_TOF];
static TaskHandle_t& gImuTaskHandle = handles[I2C_DEVICE_IMU];
static TaskHandle_t& gInputTaskHandle = handles[I2C_DEVICE_INPUT];
static TaskHandle_t& gGpsTaskHandle = handles[I2C_DEVICE_GPS];
static TaskHandle_t& gFmRadioTaskHandle = handles[I2C_DEVICE_FMRADIO];
static TaskHandle_t& gApdsTaskHandle = handles[I2C_DEVICE_APDS];
static TaskHandle_t& gRtcTaskHandle = handles[I2C_DEVICE_RTC];
static TaskHandle_t& gPresenceTaskHandle = handles[I2C_DEVICE_PRESENCE];
static unsigned long& gThermalLastStopTime = stopTimes[I2C_DEVICE_THERMAL];
static unsigned long& gTofLastStopTime = stopTimes[I2C_DEVICE_TOF];
static unsigned long& gImuLastStopTime = stopTimes[I2C_DEVICE_IMU];
static unsigned long& gGamepadLastStopTime = stopTimes[I2C_DEVICE_INPUT];
static unsigned long& gGpsLastStopTime = stopTimes[I2C_DEVICE_GPS];
static unsigned long& gFmRadioLastStopTime = stopTimes[I2C_DEVICE_FMRADIO];
static unsigned long& gApdsLastStopTime = stopTimes[I2C_DEVICE_APDS];
static unsigned long& gRtcLastStopTime = stopTimes[I2C_DEVICE_RTC];
static unsigned long& gPresenceLastStopTime = stopTimes[I2C_DEVICE_PRESENCE];
static struct {
  int thermalBus = 0, tofBus = 0, imuBus = 0, inputBus = 0, gpsBus = 0;
  int fmRadioBus = 0, apdsBus = 0, rtcBus = 0, presenceBus = 0;
} gSettings;

enum { DEBUG_MEMORY, SYSEVT_SENSOR_STARTED, SYSEVT_SENSOR_START_FAILED, SYSEVT_SENSOR_STOPPED };
enum { REMOTE_SENSOR_THERMAL, REMOTE_SENSOR_TOF, REMOTE_SENSOR_IMU,
       REMOTE_SENSOR_INPUT, REMOTE_SENSOR_GPS, REMOTE_SENSOR_FMRADIO };
static std::vector<std::string> statuses, broadcasts, logs;
static std::vector<std::pair<int, std::string>> events;
static std::vector<std::pair<int, bool>> meshStatuses;
static std::vector<std::pair<uint8_t, uint8_t>> graceResets;

BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait) {
  assert(mutex && !mutex->held && wait == portMAX_DELAY); ++takes;
  mutex->held = mutex->available; return mutex->held ? pdTRUE : pdFALSE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex) {
  assert(mutex && mutex->held); mutex->held = false; ++gives;
  if (afterUnlock) std::exchange(afterUnlock, {})();
  return pdTRUE;
}
uint32_t millis() { return now; }
I2CDeviceManager* I2CDeviceManager::instance = nullptr;
void I2CDeviceManager::initialize() {}  // Allocation is a controlled platform boundary.

// INSERT_MANAGER_DEFINITIONS

static bool ensureDebugBuffer() { return bufferAvailable; }
static char* getDebugBuffer() { return debugBuffer; }
static uint8_t i2cProbeAddress(uint8_t addr, uint32_t clock, uint32_t wait, uint8_t bus) {
  assert(clock == 100000 && wait == 50); ++probeCalls; probeAddress = addr; probeBus = bus;
  if (duringProbe) std::exchange(duringProbe, {})();
  return probeResult;
}
static void sensorStatusBumpWith(const char* cause) { statuses.emplace_back(cause); }
[[maybe_unused]] static void broadcastSensorStatus(int sensor, bool online) { meshStatuses.emplace_back(sensor, online); }
static void systemEventPost(int event, const char* name) { events.emplace_back(event, name); }
static std::string format(const char* fmt, va_list args) {
  char buffer[1024]; const int n = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
  assert(n >= 0 && n < static_cast<int>(sizeof(buffer))); return buffer;
}
static void broadcastPrintf(const char* fmt, ...) {
  va_list args; va_start(args, fmt); broadcasts.push_back(format(fmt, args)); va_end(args);
}
static void logSystemEvent(const char*, const char* fmt, ...) {
  va_list args; va_start(args, fmt); logs.push_back(format(fmt, args)); va_end(args);
}
template<class... Args> static void debugLog(const char*, Args...) {}
#define DEBUG_CLIF(...) debugLog(__VA_ARGS__)
#define INFO_I2C_AUTOSTARTF(...) debugLog(__VA_ARGS__)
#define DEBUG_MEMORY_STACKF(...) debugLog(__VA_ARGS__)
#define INFO_MEMORYF(...) debugLog(__VA_ARGS__)
#define BROADCAST_PRINTF(...) broadcastPrintf(__VA_ARGS__)
static bool isDebugFlagSet(int) { return debugEnabled; }
static UBaseType_t uxTaskGetStackHighWaterMark(void*) { return 2048; }
static struct { size_t getFreeHeap() { return 16000; } size_t getMinFreeHeap() { return 8000; } } ESP;
static void pollPause() { ++pauseCalls; ++pauseDepth; }
static void pollResume() { assert(pauseDepth > 0); ++resumeCalls; --pauseDepth; }
struct TaskIdle {};
static void vTaskDelay(TickType_t duration) {
  if (pauseCalls == resumeCalls) { assert(duration == 100); throw TaskIdle{}; }
  assert(pauseDepth > 0); now += duration;
  if (duringDelay) std::exchange(duringDelay, {})();
}
static void vTaskDelete(void*) { ++taskDeletes; }
static void i2cResetGracePeriod(uint8_t address, uint8_t bus) { graceResets.emplace_back(address, bus); }
static bool startOwner(I2CDeviceType sensor) {
  assert(pauseDepth > 0); ++starts[sensor];
  running[sensor] = runningAfterStart[sensor];
  if (running[sensor]) handles[sensor] = &handles[sensor];
  if (duringInit) std::exchange(duringInit, {})();
  return startResult[sensor];
}
static bool thermalStartInternal() { return startOwner(I2C_DEVICE_THERMAL); }
static bool tofStartInternal() { return startOwner(I2C_DEVICE_TOF); }
static bool imuStartInternal() { return startOwner(I2C_DEVICE_IMU); }
static bool inputStartInternal() { return startOwner(I2C_DEVICE_INPUT); }
static bool gpsStartInternal() { return startOwner(I2C_DEVICE_GPS); }
static bool fmRadioStartInternal() { return startOwner(I2C_DEVICE_FMRADIO); }
static bool apdsStartInternal() { return startOwner(I2C_DEVICE_APDS); }
static bool rtcStartInternal() { return startOwner(I2C_DEVICE_RTC); }
static bool presenceStartInternal() { return startOwner(I2C_DEVICE_PRESENCE); }
static void fmRadioDeinit() {
  assert(pauseDepth > 0 && gFmRadioTaskHandle == nullptr);
  ++fmDeinitCalls; fmHardwareReady = false; gFmRadioRunning = false;
}
static void rtcStop() {
  assert(pauseDepth > 0); ++rtcStopCalls; gRtcRunning = false;
}

// INSERT_CONTROLLER_DEFINITIONS

struct Fixture {
  I2CDeviceManager manager;
  FakeSemaphore mutex;
  Fixture() {
    now += 10000;  // Prior worker static timestamps remain valid between cases.
    manager.queueMutex = &mutex; I2CDeviceManager::instance = &manager;
    gI2CBusRunning = bufferAvailable = true; debugEnabled = false; gSettings = {};
    running.fill(false); handles.fill(nullptr); stopTimes.fill(0); starts.fill(0);
    startResult.fill(true); runningAfterStart.fill(true);
    gApdsColorRunning = gApdsProximityRunning = gApdsGestureRunning = false;
    std::memset(sSensorStopAwaitingCleanup, 0, sizeof(sSensorStopAwaitingCleanup));
    takes = gives = probeCalls = pauseCalls = resumeCalls = pauseDepth = taskDeletes = 0;
    fmDeinitCalls = rtcStopCalls = 0; fmHardwareReady = false;
    probeResult = probeAddress = probeBus = 0;
    afterUnlock = duringDelay = duringInit = duringProbe = {};
    statuses.clear(); broadcasts.clear(); events.clear(); logs.clear(); meshStatuses.clear(); graceResets.clear();
  }
  ~Fixture() { assert(!mutex.held && takes == gives && pauseCalls == resumeCalls); I2CDeviceManager::instance = nullptr; }
  std::string open(I2CDeviceType sensor = I2C_DEVICE_GPS) {
    return cmd_sensorstart_queued(sensor, deviceTypeDisplayName(sensor), "test@enqueue");
  }
  void worker() {
    const unsigned beforeDepth = pauseDepth;
    try { sensorQueueProcessorTask(nullptr); } catch (const TaskIdle&) {}
    assert(pauseCalls == resumeCalls && pauseDepth == beforeDepth);
    assert(!mutex.held && takes == gives);
  }
};
static bool contains(const std::string& text, const char* token) { return text.find(token) != std::string::npos; }
static unsigned eventCount(int type) {
  unsigned count = 0; for (const auto& event : events) if (event.first == type) ++count; return count;
}

static void admissionCases() {
  { Fixture f; bufferAvailable = false; assert(contains(f.open(), "buffer unavailable")); assert(probeCalls == 0); }
  // The admission core never touches the command reply buffer, so the main
  // loop (menu input auto-start) can call it beside a running command.
  { Fixture f; bufferAvailable = false; debugBuffer[0] = '\0';
    SensorStartRefusal why = SensorStartRefusal::NotQueued;
    assert(requestSensorStart(I2C_DEVICE_GPS, "menu@auto", &why) && why == SensorStartRefusal::None);
    assert(statuses.size() == 1 && statuses.back() == "menu@auto" && f.manager.getQueueDepth() == 1);
    assert(debugBuffer[0] == '\0'); }
  { Fixture f; gI2CBusRunning = false; SensorStartRefusal why = SensorStartRefusal::None;
    assert(!requestSensorStart(I2C_DEVICE_GPS, "menu@auto", &why) && why == SensorStartRefusal::BusDisabled);
    assert(probeCalls == 0 && statuses.empty() && contains(sensorStartRefusalText(why), "disabled")); }
  { Fixture f; probeResult = 2; SensorStartRefusal why = SensorStartRefusal::None;
    assert(!requestSensorStart(I2C_DEVICE_GPS, "menu@auto", &why) && why == SensorStartRefusal::NoAck);
    assert(probeCalls == 1 && f.manager.getQueueDepth() == 0 && statuses.empty()); }
  { Fixture f; gI2CBusRunning = false; assert(contains(f.open(), "disabled")); assert(probeCalls == 0 && f.manager.getQueueDepth() == 0); }
  { Fixture f; I2CDeviceManager::instance = nullptr; assert(contains(f.open(), "queue is unavailable")); assert(probeCalls == 0); }
  { Fixture f; handles[I2C_DEVICE_GPS] = &f; assert(contains(f.open(), "cleanup is still pending")); assert(probeCalls == 0); }
  { Fixture f; running[I2C_DEVICE_GPS] = true; assert(contains(f.open(), "already running")); assert(probeCalls == 0); }
  { Fixture f; assert(f.manager.enqueueDeviceStart(I2C_DEVICE_GPS)); assert(contains(f.open(), "already queued")); assert(probeCalls == 0); }
  { Fixture f; assert(requestSensorStop(I2C_DEVICE_GPS)); assert(contains(f.open(), "Stop/cleanup")); assert(probeCalls == 0); }
  for (uint8_t result : {2, 4, 5}) {
    Fixture f; probeResult = result;
    const std::string message = f.open();
    assert(contains(message, result == 2 ? "No acknowledgement" : "could not complete"));
    assert(probeCalls == 1 && f.manager.getQueueDepth() == 0 && statuses.empty());
  }
  { Fixture f; gSettings.gpsBus = 1; assert(contains(f.open(), "queued for start"));
    assert(probeAddress == I2C_ADDR_GPS && probeBus == 1 && statuses.size() == 1); }
  { Fixture f; duringProbe = [] { assert(requestSensorStop(I2C_DEVICE_GPS)); };
    assert(contains(f.open(), "Could not queue")); assert(f.manager.getQueueDepth() == 0 && statuses.empty()); }
  { Fixture f; f.manager.queueMutex = nullptr; assert(!requestSensorStop(I2C_DEVICE_GPS));
    assert(contains(f.open(), "Could not queue")); assert(statuses.empty()); }
  { Fixture f;
    for (auto sensor : {I2C_DEVICE_THERMAL, I2C_DEVICE_TOF, I2C_DEVICE_IMU, I2C_DEVICE_INPUT,
                        I2C_DEVICE_FMRADIO, I2C_DEVICE_APDS, I2C_DEVICE_RTC}) {
      assert(f.manager.enqueueDeviceStart(sensor));
    }
    assert(contains(f.open(), "Could not queue") && probeCalls == 1 && statuses.empty()); }
}

static void workerOutcomeCases() {
  for (int index = 0; index < I2C_DEVICE_COUNT; ++index) {
    Fixture f; const auto sensor = static_cast<I2CDeviceType>(index);
    debugEnabled = true; pauseDepth = 1;  // An unrelated pause survives the batch.
    assert(f.manager.enqueueDeviceStart(sensor)); f.worker();
    assert(starts[sensor] == 1 && running[sensor]);
    assert(eventCount(SYSEVT_SENSOR_STARTED) == 1 && eventCount(SYSEVT_SENSOR_START_FAILED) == 0);
    assert(pauseCalls == 1 && pauseDepth == 1 && graceResets.size() == 1);
#if ENABLE_ESPNOW
    assert(meshStatuses.size() == static_cast<size_t>(index <= I2C_DEVICE_FMRADIO));
    if (!meshStatuses.empty()) assert(meshStatuses[0].first == index && meshStatuses[0].second);
#else
    assert(meshStatuses.empty());
#endif
  }
  { Fixture f; startResult[I2C_DEVICE_GPS] = false;
    assert(f.manager.enqueueDeviceStart(I2C_DEVICE_GPS)); f.worker();
    assert(running[I2C_DEVICE_GPS] && eventCount(SYSEVT_SENSOR_START_FAILED) == 1);
    assert(eventCount(SYSEVT_SENSOR_STARTED) == 0 && meshStatuses.empty()); } // Return value, not stale flag.
  { Fixture f; runningAfterStart[I2C_DEVICE_FMRADIO] = false;
    assert(f.manager.enqueueDeviceStart(I2C_DEVICE_FMRADIO)); f.worker();
    assert(eventCount(SYSEVT_SENSOR_STARTED) == 1 && starts[I2C_DEVICE_FMRADIO] == 1); } // Owner acceptance contract.
  { Fixture f; running[I2C_DEVICE_GPS] = true;
    assert(f.manager.enqueueDeviceStart(I2C_DEVICE_GPS)); f.worker();
    assert(starts[I2C_DEVICE_GPS] == 0 && events.empty() && graceResets.empty()); }
  { Fixture f; handles[I2C_DEVICE_GPS] = &f;
    assert(f.manager.enqueueDeviceStart(I2C_DEVICE_GPS)); f.worker();
    assert(starts[I2C_DEVICE_GPS] == 0 && eventCount(SYSEVT_SENSOR_START_FAILED) == 1);
    assert(broadcasts.size() == 1 && graceResets.empty()); }
  { Fixture f; I2CDeviceManager::instance = nullptr; f.worker(); assert(taskDeletes == 1 && pauseCalls == 0); }
}

static void closeOrderingCases() {
  // Cancel before dequeue: actual manager removes pending starts.
  { Fixture f; assert(f.manager.enqueueDeviceStart(I2C_DEVICE_GPS));
    assert(requestSensorStop(I2C_DEVICE_GPS)); assert(starts[I2C_DEVICE_GPS] == 0);
    f.worker(); assert(starts[I2C_DEVICE_GPS] == 0 && eventCount(SYSEVT_SENSOR_STOPPED) == 1);
    assert(eventCount(SYSEVT_SENSOR_STARTED) == 0 && !running[I2C_DEVICE_GPS]); }
  // Inject after the real queue advances its head, before dequeue returns.
  { Fixture f; assert(f.manager.enqueueDeviceStart(I2C_DEVICE_GPS));
    afterUnlock = [&] { assert(f.manager.queueHead != f.manager.queueTail || f.manager.queueHead != 0);
      assert(requestSensorStop(I2C_DEVICE_GPS)); };
    f.worker(); assert(starts[I2C_DEVICE_GPS] == 0 && eventCount(SYSEVT_SENSOR_STARTED) == 0);
    assert(eventCount(SYSEVT_SENSOR_STOPPED) == 1); }
  // Close while waiting for a previous device's initialization delay.
  { Fixture f; assert(f.manager.enqueueDeviceStart(I2C_DEVICE_THERMAL));
    assert(f.manager.enqueueDeviceStart(I2C_DEVICE_GPS));
    duringDelay = [] { assert(starts[I2C_DEVICE_THERMAL] == 1 && starts[I2C_DEVICE_GPS] == 0);
      assert(requestSensorStop(I2C_DEVICE_GPS)); };
    f.worker(); assert(!duringDelay && starts[I2C_DEVICE_GPS] == 0);
    assert(eventCount(SYSEVT_SENSOR_STARTED) == 1 && eventCount(SYSEVT_SENSOR_STOPPED) == 1); }
  // Close during owner initialization only queues a request. The returned
  // successful start must not publish a stale online event; cleanup waits.
  { Fixture f; assert(f.manager.enqueueDeviceStart(I2C_DEVICE_GPS));
    duringInit = [] { assert(gGpsRunning && gGpsTaskHandle);
      assert(requestSensorStop(I2C_DEVICE_GPS)); assert(gGpsRunning && gGpsTaskHandle); };
    f.worker(); assert(starts[I2C_DEVICE_GPS] == 1 && !gGpsRunning && gGpsTaskHandle);
    assert(events.empty() && meshStatuses.empty() && sSensorStopAwaitingCleanup[I2C_DEVICE_GPS]);
    assert(contains(f.open(), "cleanup is still pending"));
    gGpsTaskHandle = nullptr; f.worker();
    assert(eventCount(SYSEVT_SENSOR_STOPPED) == 1 && !sSensorStopAwaitingCleanup[I2C_DEVICE_GPS]);
#if ENABLE_ESPNOW
    assert((meshStatuses == std::vector<std::pair<int, bool>>{{REMOTE_SENSOR_GPS, false}}));
#endif
    f.worker(); assert(eventCount(SYSEVT_SENSOR_STOPPED) == 1); }
  // An init failure canceled by close likewise produces no stale start result.
  { Fixture f; startResult[I2C_DEVICE_GPS] = false; runningAfterStart[I2C_DEVICE_GPS] = false;
    assert(f.manager.enqueueDeviceStart(I2C_DEVICE_GPS));
    duringInit = [] { assert(requestSensorStop(I2C_DEVICE_GPS)); };
    f.worker(); assert(eventCount(SYSEVT_SENSOR_START_FAILED) == 0 && eventCount(SYSEVT_SENSOR_STOPPED) == 1); }
}

static void cleanupCases() {
  for (int index = 0; index < I2C_DEVICE_COUNT; ++index) {
    Fixture f; const auto sensor = static_cast<I2CDeviceType>(index);
    running[sensor] = true; handles[sensor] = &f;
    if (sensor == I2C_DEVICE_APDS) gApdsColorRunning = gApdsProximityRunning = gApdsGestureRunning = true;
    assert(requestSensorStop(sensor)); assert(running[sensor]); f.worker();
    assert(!running[sensor] && handles[sensor] && stopTimes[sensor] == now);
    assert(events.empty() && meshStatuses.empty() && sSensorStopAwaitingCleanup[sensor]);
    assert(fmDeinitCalls == 0);  // A live FM task must perform its own teardown.
    assert(rtcStopCalls == static_cast<unsigned>(sensor == I2C_DEVICE_RTC));
    if (sensor == I2C_DEVICE_APDS) assert(!gApdsColorRunning && !gApdsProximityRunning && !gApdsGestureRunning);
    handles[sensor] = nullptr;
    assert(f.manager.enqueueDeviceStart(sensor)); f.worker();
    assert(events.size() == 2 && events[0].first == SYSEVT_SENSOR_STOPPED && events[1].first == SYSEVT_SENSOR_STARTED);
    assert(starts[sensor] == 1 && !sSensorStopAwaitingCleanup[sensor]);
  }
  { Fixture f; running[I2C_DEVICE_GPS] = true; handles[I2C_DEVICE_GPS] = &f;
    gI2CBusRunning = false; assert(requestSensorStop(I2C_DEVICE_GPS)); f.worker();
    assert(!gGpsRunning && gGpsTaskHandle && events.empty());
    gGpsTaskHandle = nullptr; f.worker(); assert(eventCount(SYSEVT_SENSOR_STOPPED) == 1); }
  // Legacy tune/boot may initialize FM without a polling task. Stop still
  // performs owner teardown on the worker before claiming cleanup complete.
  { Fixture f; fmHardwareReady = true;
    assert(requestSensorStop(I2C_DEVICE_FMRADIO));
    assert(fmHardwareReady && fmDeinitCalls == 0 && events.empty());
    f.worker(); assert(!fmHardwareReady && fmDeinitCalls == 1 && !gFmRadioTaskHandle);
    assert(eventCount(SYSEVT_SENSOR_STOPPED) == 1 && !sSensorStopAwaitingCleanup[I2C_DEVICE_FMRADIO]); }
  { Fixture f; gRtcRunning = true;
    assert(requestSensorStop(I2C_DEVICE_RTC)); assert(rtcStopCalls == 0 && gRtcRunning);
    f.worker(); assert(rtcStopCalls == 1 && !gRtcRunning);
    assert(eventCount(SYSEVT_SENSOR_STOPPED) == 1 && !sSensorStopAwaitingCleanup[I2C_DEVICE_RTC]); }
}

int main() {
  admissionCases(); workerOutcomeCases(); closeOrderingCases(); cleanupCases();
  std::printf("Sensor lifecycle requests passed (production controller + manager; mesh=%d)\n", ENABLE_ESPNOW);
}
