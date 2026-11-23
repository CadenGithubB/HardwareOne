#include <array>
#include <atomic>
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
#include "System_SensorRestartCore.h"  // the real backoff policy
#define EXT_RAM_BSS_ATTR
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
#define ENABLE_ENCODER 1

struct FakeSemaphore { bool available = true, held = false; };
static constexpr TickType_t portMAX_DELAY = UINT32_MAX;
using TaskHandle_t = void*;
using UBaseType_t = uint32_t;
static constexpr uint32_t SENSOR_QUEUE_STACK_WORDS = 4096;
TwoWire Wire, Wire1;
bool gI2cBusRunning = true;
static uint32_t now = 100;
static std::function<void()> afterUnlock, duringDelay, duringInit, duringProbe;
static unsigned takes, gives, probeCalls, pauseCalls, resumeCalls, pauseDepth, taskDeletes;
static unsigned fmDeinitCalls, rtcStopCalls;
static bool debugEnabled, bufferAvailable, fmHardwareReady;
[[maybe_unused]] static bool& gRadioInitialized = fmHardwareReady;  // FM owner: initialized without a task
static bool gRtcConnected;                         // RTC owner: connected without a task
static unsigned managerInits;
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
  // The one saved switch per device (start at boot), at the firmware's defaults.
  bool thermalAutoStart = false, tofAutoStart = false, imuAutoStart = false, inputAutoStart = false;
  bool gpsAutoStart = false, fmRadioAutoStart = false, apdsAutoStart = false, rtcAutoStart = true;
  bool presenceAutoStart = false;
  bool i2cEnabled = true;
} gSettings;
// HAL_Motion: the address the IMU opens (its imuAddress setting).
static uint8_t imuDeviceAddress() { return 0x28; }
// The OLED menu's input auto-start (OLED_Utils.cpp tryAutoStartInputForMenu):
// its connected flag and the RAM-flush resolve of the shared input switch.
static bool gInputConnected = false;
enum { RF_INPUT = 3 };
static bool ramFlushResolve(int, bool intent) { return intent; }
// The encoder's saved settings of every instance (i2csensor_encoder.h): its
// start-at-boot switches live here, not in gSettings.
static EncoderInstanceSettings gEncoderInst[kEncoderMaxInstances];
static bool& autoStartSetting(I2cDeviceType sensor) {
  switch (sensor) {
    case I2C_DEVICE_THERMAL: return gSettings.thermalAutoStart;
    case I2C_DEVICE_TOF: return gSettings.tofAutoStart;
    case I2C_DEVICE_IMU: return gSettings.imuAutoStart;
    case I2C_DEVICE_INPUT: return gSettings.inputAutoStart;
    case I2C_DEVICE_GPS: return gSettings.gpsAutoStart;
    case I2C_DEVICE_FMRADIO: return gSettings.fmRadioAutoStart;
    case I2C_DEVICE_APDS: return gSettings.apdsAutoStart;
    case I2C_DEVICE_RTC: return gSettings.rtcAutoStart;
    case I2C_DEVICE_ENCODER: return gEncoderInst[0].autoStart;
    default: return gSettings.presenceAutoStart;
  }
}
// First-time setup state (System_FirstTimeSetup.h).
enum FirstTimeSetupState { SETUP_NOT_NEEDED = 0, SETUP_REQUIRED = 1, SETUP_IN_PROGRESS = 2, SETUP_COMPLETE = 3 };
static volatile FirstTimeSetupState gFirstTimeSetupState = SETUP_NOT_NEEDED;
// RAM-flush boundary: ids numbered as the device types; the resume's device
// list can override instance 1's start-at-boot intent (here: the cases only
// set instance 1), and "failed, not switched off" marks are kept.
using RamFlushFeatureId = int;
static std::array<int, I2C_DEVICE_COUNT> flushOverride;  // -1 = no overlay, else 0/1
static std::array<bool, I2C_DEVICE_COUNT> flushFailedMark;
static RamFlushFeatureId ramFlushIdForDevice(I2cDeviceType sensor) { return static_cast<int>(sensor); }
static bool ramFlushResolveDevice(I2cDeviceType sensor, uint8_t n, bool intent) {
  if (n != 1 || flushOverride[sensor] < 0) return intent;
  return flushOverride[sensor] != 0;
}
static void ramFlushMarkAutostartFailed(RamFlushFeatureId id) { flushFailedMark[id] = true; }
static void ramFlushClearAutostartFailed(RamFlushFeatureId id) { flushFailedMark[id] = false; }
// The per-instance marks (System_RamFlush.h): instance 1 is its feature's mark.
static std::array<bool, kI2cSlotCount> flushFailedSlot;
static void ramFlushMarkAutostartFailed(I2cDeviceType sensor, uint8_t n) {
  if (n == 1) ramFlushMarkAutostartFailed(ramFlushIdForDevice(sensor));
  else flushFailedSlot[i2cSlot(sensor, n)] = true;
}
static void ramFlushClearAutostartFailed(I2cDeviceType sensor, uint8_t n) {
  if (n == 1) ramFlushClearAutostartFailed(ramFlushIdForDevice(sensor));
  else flushFailedSlot[i2cSlot(sensor, n)] = false;
}

enum { DEBUG_MEMORY, SYSEVT_SENSOR_STARTED, SYSEVT_SENSOR_START_FAILED, SYSEVT_SENSOR_STOPPED };
// Numbered as the I2C device types here, so a notice names its device. RTC
// and presence are remote types too (their start/stop is told, the APDS's not).
enum { REMOTE_SENSOR_THERMAL, REMOTE_SENSOR_TOF, REMOTE_SENSOR_IMU,
       REMOTE_SENSOR_INPUT, REMOTE_SENSOR_GPS, REMOTE_SENSOR_FMRADIO,
       REMOTE_SENSOR_RTC = 7, REMOTE_SENSOR_PRESENCE = 8 };
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
I2cDeviceManager* I2cDeviceManager::instance = nullptr;
void I2cDeviceManager::initialize() { ++managerInits; }  // Allocation is a controlled platform boundary.

// INSERT_MANAGER_DEFINITIONS

static bool ensureDebugBuffer() { return bufferAvailable; }
static char* getDebugBuffer() { return debugBuffer; }
static uint8_t i2cProbeAddress(uint8_t addr, uint32_t clock, uint32_t wait, uint8_t bus) {
  assert(clock == 100000 && wait == 50); ++probeCalls; probeAddress = addr; probeBus = bus;
  if (duringProbe) std::exchange(duringProbe, {})();
  return probeResult;
}
static void sensorStatusBumpWith(const char* cause) { statuses.emplace_back(cause); }
static std::vector<std::string> hints;
static void cliHint(const char* text) { hints.emplace_back(text); }
struct String {};  // Command handlers only pass it through.
// The single-instance argument check of the open/close commands
// (System_I2C.cpp, test_i2c_instance_core): the handlers here get no argument.
static const char* i2cOpenCloseArgError(I2cDeviceType, const String&, const char*) { return nullptr; }
#define RETURN_VALID_IF_VALIDATE_CSTR() do {} while (0)
[[maybe_unused]] static void sensorPresenceChanged(int sensor, bool online) { meshStatuses.emplace_back(sensor, online); }
static void systemEventPost(int event, const char* name) { events.emplace_back(event, name); }
static void systemEventPost(int event, const char* name, const char*) { events.emplace_back(event, name); }
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
#define ERROR_I2CF(...) debugLog(__VA_ARGS__)
#define DEBUG_DISPLAYF(...) debugLog(__VA_ARGS__)
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
static bool startOwner(I2cDeviceType sensor) {
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
// The encoder board has no task: its owner reports running, and its stop
// darkens the pixels and returns (the worker publishes the stop at once).
// Instance 1 is the type's slot in the arrays above; instances n >= 2 keep
// their own state below (index n).
static unsigned encoderStopCalls;
static std::array<bool, kEncoderMaxInstances + 1> encoderExtraRunning;
static std::array<unsigned, kEncoderMaxInstances + 1> encoderExtraStarts, encoderExtraStops;
static bool encoderRunning(uint8_t n = 1) {
  return n == 1 ? running[I2C_DEVICE_ENCODER] : encoderExtraRunning[n];
}
static bool encoderAutoStartOf(uint8_t n) { return gEncoderInst[n - 1].autoStart; }
static bool* encoderAutoStartSetting(uint8_t n) { return &gEncoderInst[n - 1].autoStart; }
static uint8_t encoderInstanceAddress(uint8_t n) {
  return n == 1 ? 0x49 : static_cast<uint8_t>(gEncoderInst[n - 1].address);
}
static uint8_t encoderInstanceBus(uint8_t n) { return static_cast<uint8_t>(gEncoderInst[n - 1].bus); }
static bool encoderStartInternal(uint8_t n = 1) {
  if (n != 1) {
    assert(pauseDepth > 0); ++encoderExtraStarts[n];
    encoderExtraRunning[n] = true;
    return true;
  }
  const bool started = startOwner(I2C_DEVICE_ENCODER);
  handles[I2C_DEVICE_ENCODER] = nullptr;  // no task
  return started;
}
static void encoderStop(uint8_t n = 1) {
  assert(pauseDepth > 0); ++encoderStopCalls;
  if (n == 1) running[I2C_DEVICE_ENCODER] = false;
  else { ++encoderExtraStops[n]; encoderExtraRunning[n] = false; }
}
static void fmRadioDeinit() {
  assert(pauseDepth > 0 && gFmRadioTaskHandle == nullptr);
  ++fmDeinitCalls; fmHardwareReady = false; gFmRadioRunning = false;
}
static void rtcStop() {
  assert(pauseDepth > 0); ++rtcStopCalls; gRtcRunning = false;
  if (!gRtcTaskHandle) gRtcConnected = false;
}

// INSERT_CONTROLLER_DEFINITIONS

struct Fixture {
  I2cDeviceManager manager;
  FakeSemaphore mutex;
  Fixture() {
    now += 10000;  // Prior worker static timestamps remain valid between cases.
    manager.queueMutex = &mutex; I2cDeviceManager::instance = &manager;
    gI2cBusRunning = bufferAvailable = true; debugEnabled = false; gSettings = {};
    for (auto& instance : gEncoderInst) instance = {};
    encoderExtraRunning.fill(false); encoderExtraStarts.fill(0); encoderExtraStops.fill(0);
    running.fill(false); handles.fill(nullptr); stopTimes.fill(0); starts.fill(0);
    startResult.fill(true); runningAfterStart.fill(true);
    gApdsColorRunning = gApdsProximityRunning = gApdsGestureRunning = false;
    std::memset(sSensorStopAwaitingCleanup, 0, sizeof(sSensorStopAwaitingCleanup));
    takes = gives = probeCalls = pauseCalls = resumeCalls = pauseDepth = taskDeletes = 0;
    fmDeinitCalls = rtcStopCalls = encoderStopCalls = 0; fmHardwareReady = false;
    hints.clear(); gRtcConnected = false; managerInits = 0;
    gFirstTimeSetupState = SETUP_NOT_NEEDED; flushOverride.fill(-1); flushFailedMark.fill(false);
    for (int i = 0; i < I2C_DEVICE_COUNT; ++i) {
      sSensorRestart[i] = {}; sSensorRestartNoted[i] = false;
      sSensorRestartByHand[i] = kRestartHandNone; sSensorRestartPending[i] = false;
    }
    sSensorRestartLastTickMs = now - 1000;  // The first worker pass of a case ticks.
#if HAS_START_OUTSTANDING
    for (auto& outstanding : sSensorStartOutstanding) outstanding = false;
#endif
    probeResult = probeAddress = probeBus = 0;
    afterUnlock = duringDelay = duringInit = duringProbe = {};
    statuses.clear(); broadcasts.clear(); events.clear(); logs.clear(); meshStatuses.clear(); graceResets.clear();
  }
  ~Fixture() { assert(!mutex.held && takes == gives && pauseCalls == resumeCalls); I2cDeviceManager::instance = nullptr; }
  std::string open(I2cDeviceType sensor = I2C_DEVICE_GPS, const char* hint = "test hint") {
#if HAS_START_HINT
    return cmd_sensorstart_queued(sensor, deviceTypeDisplayName(sensor), "test@enqueue", hint);
#else
    (void)hint;
    return cmd_sensorstart_queued(sensor, deviceTypeDisplayName(sensor), "test@enqueue");
#endif
  }
  void worker() {
    const unsigned beforeDepth = pauseDepth;
    try { sensorQueueProcessorTask(nullptr); } catch (const TaskIdle&) {}
    assert(pauseCalls == resumeCalls && pauseDepth == beforeDepth);
    assert(!mutex.held && takes == gives);
  }
  std::string close(I2cDeviceType sensor = I2C_DEVICE_GPS) {
    return cmd_sensorstop_queued(sensor, deviceTypeDisplayName(sensor));
  }
  // Time passes, then the worker runs (its restart tick first, the queue after).
  void after(uint32_t ms) { now += ms; worker(); worker(); }
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
  { Fixture f; gI2cBusRunning = false; SensorStartRefusal why = SensorStartRefusal::None;
    assert(!requestSensorStart(I2C_DEVICE_GPS, "menu@auto", &why) && why == SensorStartRefusal::BusDisabled);
    assert(probeCalls == 0 && statuses.empty() && contains(sensorStartRefusalText(why), "disabled")); }
  { Fixture f; probeResult = 2; SensorStartRefusal why = SensorStartRefusal::None;
    assert(!requestSensorStart(I2C_DEVICE_GPS, "menu@auto", &why) && why == SensorStartRefusal::NoAck);
    assert(probeCalls == 1 && f.manager.getQueueDepth() == 0 && statuses.empty()); }
  { Fixture f; gI2cBusRunning = false; assert(contains(f.open(), "disabled")); assert(probeCalls == 0 && f.manager.getQueueDepth() == 0); }
  { Fixture f; I2cDeviceManager::instance = nullptr; assert(contains(f.open(), "queue is unavailable")); assert(probeCalls == 0); }
  { Fixture f; handles[I2C_DEVICE_GPS] = &f; assert(contains(f.open(), "cleanup is still pending")); assert(probeCalls == 0); }
  { Fixture f; running[I2C_DEVICE_GPS] = true; assert(contains(f.open(), "already running")); assert(probeCalls == 0); }
  { Fixture f; assert(enqueueDeviceStart(I2C_DEVICE_GPS)); assert(contains(f.open(), "already queued")); assert(probeCalls == 0); }
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
      assert(enqueueDeviceStart(sensor));
    }
    // Admission keeps one start per instance slot and the ring is one longer
    // than the slot count, so admission alone cannot fill it; fill the rest by
    // hand to keep the refusal covered.
    while ((f.manager.queueTail + 1) % I2cDeviceManager::DEVICE_QUEUE_SIZE != f.manager.queueHead) {
      f.manager.deviceQueue[f.manager.queueTail] = {I2C_DEVICE_COUNT, now, I2cDeviceRequestAction::Start, 0};
      f.manager.queueTail = (f.manager.queueTail + 1) % I2cDeviceManager::DEVICE_QUEUE_SIZE;
    }
    assert(contains(f.open(), "Could not queue") && probeCalls == 1 && statuses.empty()); }
}

// A start command's next-step hint follows only an accepted start.
static void hintCases() {
  { Fixture f; assert(contains(f.open(), "queued for start"));
    assert((hints == std::vector<std::string>{"test hint"})); }
  { Fixture f; assert(contains(f.open(I2C_DEVICE_GPS, nullptr), "queued for start") && hints.empty()); }
  { Fixture f; gI2cBusRunning = false; assert(contains(f.open(), "disabled") && hints.empty()); }
  { Fixture f; running[I2C_DEVICE_GPS] = true; assert(contains(f.open(), "already running") && hints.empty()); }
  { Fixture f; probeResult = 2; assert(contains(f.open(), "No acknowledgement") && hints.empty()); }
  { Fixture f; handles[I2C_DEVICE_GPS] = &f; assert(contains(f.open(), "cleanup is still pending") && hints.empty()); }
  // The production GPS command: a refused open prints no "opens in the
  // background" hint; an accepted one prints exactly one.
  { Fixture f; probeResult = 2;
    assert(contains(cmd_gpsstart(String{}), "No acknowledgement") && hints.empty()); }
  { Fixture f; assert(contains(cmd_gpsstart(String{}), "queued for start") && hints.size() == 1);
    assert(contains(hints[0], "gpsread")); }
}

static void workerOutcomeCases() {
  for (int index = 0; index < I2C_DEVICE_COUNT; ++index) {
    Fixture f; const auto sensor = static_cast<I2cDeviceType>(index);
    debugEnabled = true; pauseDepth = 1;  // An unrelated pause survives the batch.
    assert(enqueueDeviceStart(sensor)); f.worker();
    assert(starts[sensor] == 1 && running[sensor]);
    assert(eventCount(SYSEVT_SENSOR_STARTED) == 1 && eventCount(SYSEVT_SENSOR_START_FAILED) == 0);
    assert(pauseCalls == 1 && pauseDepth == 1 && graceResets.size() == 1);
#if ENABLE_ESPNOW
    // The APDS and the encoder board have no remote sensor type.
    assert(meshStatuses.size() ==
           static_cast<size_t>(sensor != I2C_DEVICE_APDS && sensor != I2C_DEVICE_ENCODER));
    if (!meshStatuses.empty()) assert(meshStatuses[0].first == index && meshStatuses[0].second);
#else
    assert(meshStatuses.empty());
#endif
  }
  { Fixture f; startResult[I2C_DEVICE_GPS] = false;
    assert(enqueueDeviceStart(I2C_DEVICE_GPS)); f.worker();
    assert(running[I2C_DEVICE_GPS] && eventCount(SYSEVT_SENSOR_START_FAILED) == 1);
    assert(eventCount(SYSEVT_SENSOR_STARTED) == 0 && meshStatuses.empty()); } // Return value, not stale flag.
  { Fixture f; runningAfterStart[I2C_DEVICE_FMRADIO] = false;
    assert(enqueueDeviceStart(I2C_DEVICE_FMRADIO)); f.worker();
    assert(eventCount(SYSEVT_SENSOR_STARTED) == 1 && starts[I2C_DEVICE_FMRADIO] == 1); } // Owner acceptance contract.
  { Fixture f; running[I2C_DEVICE_GPS] = true;
    assert(enqueueDeviceStart(I2C_DEVICE_GPS)); f.worker();
    assert(starts[I2C_DEVICE_GPS] == 0 && events.empty() && graceResets.empty()); }
  { Fixture f; handles[I2C_DEVICE_GPS] = &f;
    assert(enqueueDeviceStart(I2C_DEVICE_GPS)); f.worker();
    assert(starts[I2C_DEVICE_GPS] == 0 && eventCount(SYSEVT_SENSOR_START_FAILED) == 1);
    assert(broadcasts.size() == 1 && graceResets.empty()); }
  { Fixture f; I2cDeviceManager::instance = nullptr; f.worker(); assert(taskDeletes == 1 && pauseCalls == 0); }
}

static void closeOrderingCases() {
  // Cancel before dequeue: actual manager removes pending starts.
  { Fixture f; assert(enqueueDeviceStart(I2C_DEVICE_GPS));
    assert(requestSensorStop(I2C_DEVICE_GPS)); assert(starts[I2C_DEVICE_GPS] == 0);
    f.worker(); assert(starts[I2C_DEVICE_GPS] == 0 && eventCount(SYSEVT_SENSOR_STOPPED) == 1);
    assert(eventCount(SYSEVT_SENSOR_STARTED) == 0 && !running[I2C_DEVICE_GPS]); }
  // Inject after the real queue advances its head, before dequeue returns.
  { Fixture f; assert(enqueueDeviceStart(I2C_DEVICE_GPS));
    afterUnlock = [&] { assert(f.manager.queueHead != f.manager.queueTail || f.manager.queueHead != 0);
      assert(requestSensorStop(I2C_DEVICE_GPS)); };
    f.worker(); assert(starts[I2C_DEVICE_GPS] == 0 && eventCount(SYSEVT_SENSOR_STARTED) == 0);
    assert(eventCount(SYSEVT_SENSOR_STOPPED) == 1); }
  // Close while waiting for a previous device's initialization delay.
  { Fixture f; assert(enqueueDeviceStart(I2C_DEVICE_THERMAL));
    assert(enqueueDeviceStart(I2C_DEVICE_GPS));
    duringDelay = [] { assert(starts[I2C_DEVICE_THERMAL] == 1 && starts[I2C_DEVICE_GPS] == 0);
      assert(requestSensorStop(I2C_DEVICE_GPS)); };
    f.worker(); assert(!duringDelay && starts[I2C_DEVICE_GPS] == 0);
    assert(eventCount(SYSEVT_SENSOR_STARTED) == 1 && eventCount(SYSEVT_SENSOR_STOPPED) == 1); }
  // Close during owner initialization only queues a request. The returned
  // successful start must not publish a stale online event; cleanup waits.
  { Fixture f; assert(enqueueDeviceStart(I2C_DEVICE_GPS));
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
    assert(enqueueDeviceStart(I2C_DEVICE_GPS));
    duringInit = [] { assert(requestSensorStop(I2C_DEVICE_GPS)); };
    f.worker(); assert(eventCount(SYSEVT_SENSOR_START_FAILED) == 0 && eventCount(SYSEVT_SENSOR_STOPPED) == 1); }
}

static void cleanupCases() {
  for (int index = 0; index < I2C_DEVICE_COUNT; ++index) {
    if (index == I2C_DEVICE_ENCODER) continue;  // no task: its own case below
    Fixture f; const auto sensor = static_cast<I2cDeviceType>(index);
    running[sensor] = true; handles[sensor] = &f;
    if (sensor == I2C_DEVICE_APDS) gApdsColorRunning = gApdsProximityRunning = gApdsGestureRunning = true;
    assert(requestSensorStop(sensor)); assert(running[sensor]); f.worker();
    assert(!running[sensor] && handles[sensor] && stopTimes[sensor] == now);
    assert(events.empty() && meshStatuses.empty() && sSensorStopAwaitingCleanup[sensor]);
    assert(fmDeinitCalls == 0);  // A live FM task must perform its own teardown.
    assert(rtcStopCalls == static_cast<unsigned>(sensor == I2C_DEVICE_RTC));
    if (sensor == I2C_DEVICE_APDS) assert(!gApdsColorRunning && !gApdsProximityRunning && !gApdsGestureRunning);
    handles[sensor] = nullptr;
    assert(enqueueDeviceStart(sensor)); f.worker();
    assert(events.size() == 2 && events[0].first == SYSEVT_SENSOR_STOPPED && events[1].first == SYSEVT_SENSOR_STARTED);
    assert(starts[sensor] == 1 && !sSensorStopAwaitingCleanup[sensor]);
#if ENABLE_ESPNOW
    // The stop and the new start are both told, for every remote type.
    if (sensor == I2C_DEVICE_APDS) assert(meshStatuses.empty());
    else assert((meshStatuses == std::vector<std::pair<int, bool>>{{index, false}, {index, true}}));
#endif
  }
  // The bus-running flag is fixed at boot. With the bus disabled a stop is
  // refused like a start: nothing is queued and no manager is allocated.
  { Fixture f; gI2cBusRunning = false;
    assert(!requestSensorStop(I2C_DEVICE_GPS) && !f.manager.hasPendingDeviceStop(I2C_DEVICE_GPS));
    I2cDeviceManager::instance = nullptr;
    assert(!requestSensorStop(I2C_DEVICE_GPS) && managerInits == 0); }
  // Stopping an idle sensor (nothing running, starting or queued) publishes
  // nothing: no cleanup latch, no status bump, no stop event.
  for (int index = 0; index < I2C_DEVICE_COUNT; ++index) {
    Fixture f; const auto sensor = static_cast<I2cDeviceType>(index);
    assert(requestSensorStop(sensor)); f.worker(); f.worker();
    assert(!sSensorStopAwaitingCleanup[sensor] && statuses.empty() && events.empty() && meshStatuses.empty());
  }
  // The encoder board has no task: its owner stops at once, so the stop
  // is published on the same worker pass and a new open needs no cleanup wait.
  { Fixture f; const auto sensor = I2C_DEVICE_ENCODER;
    running[sensor] = true;
    assert(requestSensorStop(sensor)); f.worker();
    assert(!running[sensor] && encoderStopCalls == 1);
    assert(eventCount(SYSEVT_SENSOR_STOPPED) == 1 && !sSensorStopAwaitingCleanup[sensor]);
    assert(enqueueDeviceStart(sensor)); f.worker();
    assert(starts[sensor] == 1 && running[sensor] && eventCount(SYSEVT_SENSOR_STARTED) == 1); }
  // Connected without a task still counts (RTC; FM tune/boot is below).
  { Fixture f; gRtcConnected = true;
    assert(requestSensorStop(I2C_DEVICE_RTC)); f.worker();
    assert(rtcStopCalls == 1 && !gRtcConnected && eventCount(SYSEVT_SENSOR_STOPPED) == 1); }
  // An admitted start that the stop cancels is closed with the stop event.
  { Fixture f; assert(contains(f.open(), "queued for start"));
    assert(requestSensorStop(I2C_DEVICE_GPS)); f.worker();
    assert(starts[I2C_DEVICE_GPS] == 0 && eventCount(SYSEVT_SENSOR_STOPPED) == 1); }
  // A settled start does not linger: after a failed start, stopping the idle
  // sensor publishes nothing.
  { Fixture f; startResult[I2C_DEVICE_GPS] = false; runningAfterStart[I2C_DEVICE_GPS] = false;
    assert(contains(f.open(), "queued for start")); f.worker();
    assert(eventCount(SYSEVT_SENSOR_START_FAILED) == 1);
    events.clear(); statuses.clear();
    assert(requestSensorStop(I2C_DEVICE_GPS)); f.worker(); f.worker();
    assert(events.empty() && statuses.empty() && !sSensorStopAwaitingCleanup[I2C_DEVICE_GPS]); }
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

// No saved setting refuses an open (owner decision 2026-11-21): with every
// start-at-boot switch off, each sensor still opens by hand and from the menu,
// inside first-time setup and outside it.
static void noGateCases() {
  for (int d = 0; d < I2C_DEVICE_COUNT; ++d) {
    const auto sensor = static_cast<I2cDeviceType>(d);
    { Fixture f; autoStartSetting(sensor) = false;
      assert(contains(f.open(sensor), "queued for start"));
      f.worker(); assert(starts[sensor] == 1); }
    { Fixture f; autoStartSetting(sensor) = false;
      SensorStartRefusal why = SensorStartRefusal::NotQueued;
      assert(requestSensorStart(sensor, "menu@auto", &why) && why == SensorStartRefusal::None); }
  }
  for (auto state : {SETUP_NOT_NEEDED, SETUP_REQUIRED, SETUP_IN_PROGRESS, SETUP_COMPLETE}) {
    Fixture f; gSettings.inputAutoStart = false; gFirstTimeSetupState = state;
    assert(contains(cmd_sensorstart_queued(I2C_DEVICE_INPUT, "Input", "input@setup", nullptr), "queued for start"));
    f.worker(); assert(starts[I2C_DEVICE_INPUT] == 1);
  }
}

// UP-05: a sensor that switched itself off is started again by the worker.
static unsigned logCount(const char* token) {
  unsigned count = 0; for (const auto& line : logs) if (contains(line, token)) ++count; return count;
}
// What an auto-disable site does: report, then clear its running flag (the
// task then exits and clears its handle).
static void autoDisable(I2cDeviceType sensor) {
  noteSensorAutoDisabled(sensor); running[sensor] = false; handles[sensor] = nullptr;
}
static void restartCases() {
  using sensor_restart::Phase;
  const auto gps = I2C_DEVICE_GPS;
  // Not set to start by itself: it stays off, silently, as before this change.
  { Fixture f; running[gps] = true; autoDisable(gps); f.after(0); f.after(3600000);
    assert(starts[gps] == 0 && logs.empty() && events.empty() && probeCalls == 0);
    assert(sSensorRestart[gps].phase == Phase::Idle && !flushFailedMark[gps]); }
  // The RAM-flush resume decides start-at-boot, here as at boot.
  { Fixture f; gSettings.gpsAutoStart = true; flushOverride[gps] = 0;
    running[gps] = true; autoDisable(gps); f.after(0); f.after(3600000);
    assert(starts[gps] == 0 && logs.empty()); }
  // A sensor that stopped without the report (stack bailout, failed init,
  // close) is never restarted.
  { Fixture f; gSettings.gpsAutoStart = true; running[gps] = false; f.after(0); f.after(3600000);
    assert(starts[gps] == 0 && logs.empty() && probeCalls == 0); }

  // The plain case: wait 30 s, start, run cleanly for five minutes.
  { Fixture f; gSettings.gpsAutoStart = true; running[gps] = true; autoDisable(gps);
    f.after(0);
    assert(sSensorRestart[gps].phase == Phase::Waiting && sSensorRestartPending[gps] && flushFailedMark[gps]);
    assert((logs == std::vector<std::string>{"GPS automatic restart: next try in 30 s"}));
    assert(events.empty() && probeCalls == 0);
    f.after(29000); assert(starts[gps] == 0 && probeCalls == 0);
    f.after(1000);
    assert(starts[gps] == 1 && running[gps] && probeCalls == 1);
    assert(eventCount(SYSEVT_SENSOR_STARTED) == 1 && eventCount(SYSEVT_SENSOR_START_FAILED) == 0);
    assert(statuses.size() == 1 && statuses[0] == "restart@enqueue");
    assert(sSensorRestart[gps].phase == Phase::Proving && !sSensorRestartPending[gps] && !flushFailedMark[gps]);
    assert(logs.size() == 2 && logs[1] == "GPS online");
    f.after(299000); assert(sSensorRestart[gps].phase == Phase::Proving);
    f.after(1000); assert(sSensorRestart[gps].phase == Phase::Idle && sSensorRestart[gps].rung == 0);
    // The next failure starts at the first rung again.
    autoDisable(gps); f.after(1000);
    assert(logs.back() == "GPS automatic restart: next try in 30 s"); }

  // Failing again before the clean run: the next wait is one rung longer, up
  // to the last rung, which repeats.
  { Fixture f; gSettings.gpsAutoStart = true; running[gps] = true; autoDisable(gps); f.after(0);
    const uint32_t waits[] = {30000, 120000, 600000, 1800000, 1800000, 1800000};
    for (size_t i = 0; i < sizeof(waits) / sizeof(waits[0]); ++i) {
      f.after(waits[i] - 1000); assert(starts[gps] == i);
      f.after(1000); assert(starts[gps] == i + 1 && running[gps]);
      f.after(20000); autoDisable(gps); f.after(1000);
      assert(sSensorRestart[gps].phase == Phase::Waiting && flushFailedMark[gps]);
    }
    assert(eventCount(SYSEVT_SENSOR_START_FAILED) == 0 && eventCount(SYSEVT_SENSOR_STARTED) == 6);
    assert(logCount("restart: next try in 30 s") == 1 && logCount("restart: next try in 120 s") == 1);
    assert(logCount("restart: next try in 600 s") == 1 && logCount("restart: next try in 1800 s") == 4); }

  // An absent device: one address probe and one log line per try. No driver
  // init, no event (so no toast), and the ladder climbs.
  { Fixture f; gSettings.gpsAutoStart = true; running[gps] = true; autoDisable(gps); f.after(0);
    probeResult = 2;
    const uint32_t waits[] = {30000, 120000, 600000, 1800000, 1800000};
    for (size_t i = 0; i < sizeof(waits) / sizeof(waits[0]); ++i) {
      f.after(waits[i] - 1000); assert(probeCalls == i);
      f.after(1000); assert(probeCalls == i + 1);
    }
    assert(starts[gps] == 0 && events.empty() && statuses.empty() && graceResets.empty());
    assert(logs.size() == 6 && logCount("no acknowledgement from device on I2C bus, next try in") == 5);
    assert(contains(logs[1], "next try in 120 s") && contains(logs[5], "next try in 1800 s"));
    assert(flushFailedMark[gps] && sSensorRestartPending[gps]);
    // It answers again: started, announced once.
    probeResult = 0; f.after(1800000);
    assert(starts[gps] == 1 && eventCount(SYSEVT_SENSOR_STARTED) == 1 && !flushFailedMark[gps]); }

  // The device answers but its init fails: the start-failed event (a toast)
  // is posted for the first failure of the episode only; later ones are a log line.
  { Fixture f; gSettings.gpsAutoStart = true; running[gps] = true; autoDisable(gps); f.after(0);
    startResult[gps] = false; runningAfterStart[gps] = false;
    f.after(30000);
    assert(starts[gps] == 1 && eventCount(SYSEVT_SENSOR_START_FAILED) == 1 && logs.back() == "GPS start FAILED");
    assert(sSensorRestart[gps].phase == Phase::Waiting && sSensorRestartPending[gps]);
    f.after(120000);
    assert(starts[gps] == 2 && eventCount(SYSEVT_SENSOR_START_FAILED) == 1);
    assert(logs.back() == "GPS automatic restart failed again, next try in 600 s");
    f.after(600000);
    assert(starts[gps] == 3 && eventCount(SYSEVT_SENSOR_START_FAILED) == 1);
    assert(logs.back() == "GPS automatic restart failed again, next try in 1800 s");
    // It comes back: the normal started event, once.
    startResult[gps] = true; runningAfterStart[gps] = true; f.after(1800000);
    assert(starts[gps] == 4 && eventCount(SYSEVT_SENSOR_STARTED) == 1 && meshStatuses.size() == ENABLE_ESPNOW); }

  // A busy bus is not an absent device: the try is repeated on the next
  // ticks, and only five in a row use the rung.
  { Fixture f; gSettings.gpsAutoStart = true; running[gps] = true; autoDisable(gps); f.after(0);
    probeResult = 4; f.after(30000);
    for (int i = 0; i < 3; ++i) f.after(1000);
    assert(probeCalls == 4 && logs.size() == 1 && sSensorRestart[gps].rung == 0);
    probeResult = 0; f.after(1000); assert(starts[gps] == 1 && probeCalls == 5); }
  { Fixture f; gSettings.gpsAutoStart = true; running[gps] = true; autoDisable(gps); f.after(0);
    probeResult = 4; f.after(30000);
    for (int i = 0; i < 4; ++i) f.after(1000);
    assert(probeCalls == 5 && sSensorRestart[gps].rung == 1);
    assert(logs.back() == "GPS automatic restart: I2C probe could not complete, next try in 120 s");
    f.after(60000); assert(probeCalls == 5); }

  // A close by hand while a try waits: its own reply, and the sensor stays off.
  { Fixture f; gSettings.gpsAutoStart = true; running[gps] = true; autoDisable(gps); f.after(0);
    assert(f.close() == "[GPS] Automatic restart cancelled; the sensor stays off until 'opengps'");
    f.after(1000);
    assert(sSensorRestart[gps].phase == Phase::Idle && !sSensorRestartPending[gps] && !flushFailedMark[gps]);
    assert(logs.back() == "GPS automatic restart ended: opened or closed by hand");
    f.after(3600000); assert(starts[gps] == 0 && probeCalls == 0 && events.empty());
    // With nothing waiting, the not-running reply is unchanged.
    assert(f.close() == "Error: GPS sensor is not running. Start it with 'opengps'."); }
  { Fixture f; assert(f.close() == "Error: GPS sensor is not running. Start it with 'opengps'."); }
  // The same without the reply buffer.
  { Fixture f; gSettings.gpsAutoStart = true; running[gps] = true; autoDisable(gps); f.after(0);
    bufferAvailable = false; assert(contains(f.close(), "Automatic restart cancelled")); }
  // A close of the restarted, running sensor ends the episode too.
  { Fixture f; gSettings.gpsAutoStart = true; running[gps] = true; autoDisable(gps); f.after(0);
    f.after(30000); assert(running[gps] && sSensorRestart[gps].phase == Phase::Proving);
    assert(contains(f.close(), "Stop requested")); f.after(1000);
    assert(sSensorRestart[gps].phase == Phase::Idle && !running[gps]); }
  // A close while the automatic start is still queued removes it.
  { Fixture f; gSettings.gpsAutoStart = true; running[gps] = true; autoDisable(gps); f.after(0);
    now += 30000; f.worker();  // The tick queued the start; the queue has not run yet.
    assert(sSensorRestart[gps].phase == Phase::Starting && f.manager.getQueueDepth() == 1);
    assert(contains(f.close(), "Stop requested")); f.after(1000);
    assert(starts[gps] == 0 && sSensorRestart[gps].phase == Phase::Idle && !sSensorRestartPending[gps]);
    f.after(3600000); assert(starts[gps] == 0); }

  // `<x>autostart 0` while a try waits: no hook is needed, the worker reads
  // it when it looks.
  {
    Fixture f; gSettings.gpsAutoStart = true; running[gps] = true; autoDisable(gps); f.after(0);
    f.after(10000);
    gSettings.gpsAutoStart = false;
    f.after(1000);
    assert(sSensorRestart[gps].phase == Phase::Idle && !sSensorRestartPending[gps] && !flushFailedMark[gps]);
    assert(logs.back() == "GPS automatic restart ended: auto-start off");
    gSettings.gpsAutoStart = true;
    f.after(3600000); assert(starts[gps] == 0 && probeCalls == 0);
  }

  // An open by hand while a try waits starts it at once and ends the episode.
  { Fixture f; gSettings.gpsAutoStart = true; running[gps] = true; autoDisable(gps); f.after(0);
    f.after(5000); assert(contains(f.open(), "queued for start")); f.after(1000);
    assert(starts[gps] == 1 && running[gps] && sSensorRestart[gps].phase == Phase::Idle);
    assert(!sSensorRestartPending[gps] && !flushFailedMark[gps]);
    f.after(3600000); assert(starts[gps] == 1 && probeCalls == 1); }
  // A refused open by hand (device still absent) leaves the automatic tries alone.
  { Fixture f; gSettings.gpsAutoStart = true; running[gps] = true; autoDisable(gps); f.after(0);
    probeResult = 2; assert(contains(f.open(), "No acknowledgement")); f.after(1000);
    assert(sSensorRestart[gps].phase == Phase::Waiting && sSensorRestartPending[gps]);
    probeResult = 0; f.after(29000); assert(starts[gps] == 1); }

  // An open by hand, then the sensor fails again before the worker's next
  // look (its tick runs once a second, and only with an empty queue): the
  // report is about the sensor that open started, so the restart follows.
  { Fixture f; gSettings.gpsAutoStart = true; f.after(0);
    now += 300; assert(contains(f.open(), "queued for start")); f.worker();
    assert(starts[gps] == 1 && running[gps] && sSensorRestartByHand[gps] == kRestartHandOpened);
    now += 500; autoDisable(gps); f.after(200);
    assert(sSensorRestart[gps].phase == Phase::Waiting && sSensorRestartPending[gps] && flushFailedMark[gps]);
    assert(logs.back() == "GPS automatic restart: next try in 30 s");
    f.after(30000); assert(starts[gps] == 2 && running[gps]); }
  // The same while a try was waiting: the old episode ends, a new one begins
  // at the first rung.
  { Fixture f; gSettings.gpsAutoStart = true; running[gps] = true; autoDisable(gps); f.after(0);
    f.after(30000); f.after(20000); autoDisable(gps); f.after(1000);
    assert(sSensorRestart[gps].phase == Phase::Waiting && sSensorRestart[gps].rung == 1 && starts[gps] == 1);
    now += 300; assert(contains(f.open(), "queued for start")); f.worker();
    assert(starts[gps] == 2 && running[gps] && sSensorRestartByHand[gps] == kRestartHandOpened);
    now += 500; autoDisable(gps); f.after(200);
    assert(sSensorRestart[gps].phase == Phase::Waiting && sSensorRestart[gps].rung == 0);
    assert(sSensorRestartPending[gps] && flushFailedMark[gps]);
    assert(logCount("ended: opened or closed by hand") == 1);
    assert(logs.back() == "GPS automatic restart: next try in 30 s");
    f.after(30000); assert(starts[gps] == 3 && running[gps]); }
  // A report from before the open is void: the user's start supersedes it.
  { Fixture f; gSettings.gpsAutoStart = true; running[gps] = true; f.after(0);
    now += 300; autoDisable(gps); assert(contains(f.open(), "queued for start")); f.worker();
    assert(starts[gps] == 1 && running[gps]);
    f.after(1000);
    assert(sSensorRestart[gps].phase == Phase::Idle && !sSensorRestartPending[gps] && !flushFailedMark[gps]);
    assert(logCount("automatic restart") == 0);
    f.after(3600000); assert(starts[gps] == 1 && probeCalls == 1); }
  // A close by hand wins over a report the worker has not looked at yet.
  { Fixture f; gSettings.gpsAutoStart = true; running[gps] = true; handles[gps] = &f; f.after(0);
    now += 300; noteSensorAutoDisabled(gps); assert(contains(f.close(), "Stop requested"));
    f.after(700); handles[gps] = nullptr; f.after(1000);
    assert(!running[gps] && sSensorRestart[gps].phase == Phase::Idle && !sSensorRestartPending[gps]);
    assert(logCount("automatic restart") == 0 && !flushFailedMark[gps]);
    f.after(3600000); assert(starts[gps] == 0 && probeCalls == 0); }
  // A close that lands while the worker makes its try (after it looked for a
  // close, before the start is queued) finds nothing to stop and says the
  // restart is cancelled. The start queued a moment later is dropped, not run.
  { Fixture f; gSettings.gpsAutoStart = true; running[gps] = true; autoDisable(gps); f.after(0);
    std::string reply;
    duringProbe = [&] { reply = f.close(); };
    now += 30000; f.worker();
    assert(reply == "[GPS] Automatic restart cancelled; the sensor stays off until 'opengps'");
    assert(sSensorRestart[gps].phase == Phase::Starting && f.manager.getQueueDepth() == 1);
    f.worker();
    assert(starts[gps] == 0 && !running[gps] && f.manager.getQueueDepth() == 0);
    f.after(1000);
    assert(sSensorRestart[gps].phase == Phase::Idle && !sSensorRestartPending[gps] && !flushFailedMark[gps]);
    assert(events.empty() && logs.back() == "GPS automatic restart ended: opened or closed by hand");
    assert(f.close() == "Error: GPS sensor is not running. Start it with 'opengps'.");
    f.after(3600000); assert(starts[gps] == 0 && probeCalls == 1); }

  // The input drivers report and then queue their own stop while the running
  // flag is still set: the report is kept, the stop is published, and the
  // restart follows. requestSensorStop() itself must not cancel.
  { Fixture f; gSettings.inputAutoStart = true; const auto input = I2C_DEVICE_INPUT;
    running[input] = true; handles[input] = &f;
    noteSensorAutoDisabled(input); assert(requestSensorStop(input));
    noteSensorAutoDisabled(input);  // A second failed read before the stop is applied.
    f.after(0);
    assert(!running[input] && sSensorRestart[input].phase == Phase::Waiting);
    handles[input] = nullptr; f.after(1000);
    assert(eventCount(SYSEVT_SENSOR_STOPPED) == 1 && logCount("automatic restart: next try in 30 s") == 1);
    f.after(30000);
    assert(starts[input] == 1 && running[input] && eventCount(SYSEVT_SENSOR_STARTED) == 1); }
  // The old task has not finished cleaning up when the try is due: no rung is
  // used and no line is written; it starts as soon as the task is gone.
  { Fixture f; gSettings.gpsAutoStart = true; running[gps] = true; handles[gps] = &f;
    noteSensorAutoDisabled(gps); running[gps] = false; f.after(0);
    f.after(30000); f.after(5000);
    assert(starts[gps] == 0 && probeCalls == 0 && logs.size() == 1 && sSensorRestart[gps].rung == 0);
    handles[gps] = nullptr; f.after(1000); assert(starts[gps] == 1); }
  // Started by someone else while a try waits (the OLED menu's input
  // auto-start calls requestSensorStart directly): watched like our own.
  { Fixture f; gSettings.inputAutoStart = true; const auto input = I2C_DEVICE_INPUT;
    running[input] = true; autoDisable(input); f.after(0);
    SensorStartRefusal why = SensorStartRefusal::None;
    assert(requestSensorStart(input, "input@menu", &why)); f.after(1000);
    assert(starts[input] == 1 && sSensorRestart[input].phase == Phase::Waiting);
    f.after(29000);
    assert(starts[input] == 1 && sSensorRestart[input].phase == Phase::Proving && !flushFailedMark[input]); }
  // Every device type reads its own two switches (the table's rows).
  for (int index = 0; index < I2C_DEVICE_COUNT; ++index) {
    const auto sensor = static_cast<I2cDeviceType>(index);
    { Fixture f; gSettings.rtcAutoStart = false; autoStartSetting(sensor) = true;
      running[sensor] = true; autoDisable(sensor); f.after(0); f.after(30000);
      assert(starts[sensor] == 1 && running[sensor]);
      for (int other = 0; other < I2C_DEVICE_COUNT; ++other) {
        if (other != index) assert(sSensorRestart[other].phase == Phase::Idle);
      } }
    { Fixture f; gSettings.rtcAutoStart = false;
      running[sensor] = true; autoDisable(sensor); f.after(0); f.after(30000);
      assert(starts[sensor] == 0); }
  }
  // Two sensors fail independently.
  { Fixture f; gSettings.gpsAutoStart = gSettings.imuAutoStart = true;
    running[gps] = running[I2C_DEVICE_IMU] = true;
    autoDisable(gps); f.after(0); f.after(20000); autoDisable(I2C_DEVICE_IMU); f.after(1000);
    f.after(10000); assert(starts[gps] == 1 && starts[I2C_DEVICE_IMU] == 0);
    f.after(20000); assert(starts[I2C_DEVICE_IMU] == 1); }
  // The tick runs once a second however often the idle worker loops.
  { Fixture f; gSettings.gpsAutoStart = true; running[gps] = true; f.after(0);
    autoDisable(gps); now += 500; f.worker();
    assert(sSensorRestart[gps].phase == Phase::Idle);  // Not looked at yet.
    now += 500; f.worker(); assert(sSensorRestart[gps].phase == Phase::Waiting); }
  // millis() rollover during a wait.
  { Fixture f; gSettings.gpsAutoStart = true; running[gps] = true;
    now = 0xFFFFFFFFu - 10000u; sSensorRestartLastTickMs = now - 1000;
    autoDisable(gps); f.after(0); f.after(20000); assert(starts[gps] == 0);
    f.after(10000); assert(starts[gps] == 1); now = 100; }
}

#ifdef HW1_HOST_I2C_TEST_ENCODER_MAX
// Several instances (System_I2CInstanceCore.h), built with the encoder at two
// instances: each instance has its own queue entry, stop, start mark, cleanup
// latch, driver state, start-at-boot switch and restart ladder.
static void instanceCases() {
  using sensor_restart::Phase;
  const auto enc = I2C_DEVICE_ENCODER;
  const uint8_t slot2 = i2cSlot(enc, 2);
  assert(slot2 == I2C_DEVICE_COUNT);
  auto resetSecond = [&] {
    sSensorRestart[slot2] = {}; sSensorRestartNoted[slot2] = false;
    sSensorRestartByHand[slot2] = kRestartHandNone; sSensorRestartPending[slot2] = false;
    sensorStartOutstanding(slot2) = false; sensorStopAwaitingCleanup(slot2) = false;
    flushFailedSlot.fill(false);
  };
  // An instance the type does not have is refused everywhere.
  { Fixture f; resetSecond();
    assert(!enqueueDeviceStart(enc, 3) && !requestSensorStop(enc, 3) && !f.manager.hasPendingDeviceStop(enc, 3));
    assert(f.manager.getQueueDepth() == 0); }
  // Two instances queue independently; closing instance 2 keeps instance 1's start.
  { Fixture f; resetSecond();
    assert(enqueueDeviceStart(enc) && enqueueDeviceStart(enc, 2));
    assert(f.manager.getQueueDepth() == 2 && getQueuePosition(enc) == 1 && getQueuePosition(enc, 2) == 2);
    assert(contains(cmd_sensorstop_queued(enc, deviceTypeDisplayName(enc, 2), 2),
                    "[Encoder 2] Stop requested"));
    assert(isInQueue(enc) && !isInQueue(enc, 2) && f.manager.hasPendingDeviceStop(enc, 2));
    assert(!f.manager.hasPendingDeviceStop(enc));
    f.worker();
    assert(starts[enc] == 1 && running[enc] && !sensorStartOutstanding(slot2));
    assert(eventCount(SYSEVT_SENSOR_STOPPED) == 1 && eventCount(SYSEVT_SENSOR_STARTED) == 1);
    for (const auto& event : events) {
      assert(event.second == (event.first == SYSEVT_SENSOR_STOPPED ? "Encoder 2" : "Encoder"));
    } }
  // Instance 2 starts through its own driver state, under its own name, and
  // instance 1 stays as it was.
  { Fixture f; resetSecond(); assert(enqueueDeviceStart(enc, 2)); f.worker();
    assert(encoderExtraStarts[2] == 1 && encoderExtraRunning[2] && starts[enc] == 0 && !running[enc]);
    assert(!sensorStartOutstanding(slot2) && eventCount(SYSEVT_SENSOR_STARTED) == 1);
    assert(events.back().second == "Encoder 2");
    // Running: a second open of instance 2 is refused, one of instance 1 is not.
    SensorStartRefusal why = SensorStartRefusal::None;
    assert(!requestSensorStart(enc, "test@enqueue", &why, 2) && why == SensorStartRefusal::AlreadyRunning);
    assert(requestSensorStart(enc, "test@enqueue", &why, 1));
    f.worker(); assert(starts[enc] == 1 && running[enc]);
    // Closing instance 2 stops only its board.
    assert(contains(cmd_sensorstop_queued(enc, deviceTypeDisplayName(enc, 2), 2),
                    "[Encoder 2] Stop requested"));
    f.worker();
    assert(encoderExtraStops[2] == 1 && !encoderExtraRunning[2] && running[enc]);
    assert(eventCount(SYSEVT_SENSOR_STOPPED) == 1 && events.back().second == "Encoder 2"); }
  // Closing an idle instance 2 names its own open command.
  { Fixture f; resetSecond();
    assert(contains(cmd_sensorstop_queued(enc, deviceTypeDisplayName(enc, 2), 2),
                    "Start it with 'openencoder 2'")); }
  // A restart report of instance 2 never touches instance 1's ladder, and
  // with its own start-at-boot switch off instance 2 stays off.
  { Fixture f; resetSecond(); gEncoderInst[0].autoStart = true; running[enc] = true;
    noteSensorAutoDisabled(enc, 2); f.after(0); f.after(3600000);
    assert(sSensorRestart[enc].phase == Phase::Idle && sSensorRestart[slot2].phase == Phase::Idle);
    assert(starts[enc] == 0 && !flushFailedMark[enc] && !flushFailedSlot[slot2] && logs.empty());
    autoDisable(enc); f.after(1000);  // the tick runs once a second
    assert(sSensorRestart[enc].phase == Phase::Waiting && sSensorRestart[slot2].phase == Phase::Idle);
    assert(sSensorRestartPending[enc] && !sSensorRestartPending[slot2]);
    assert(flushFailedMark[enc] && !flushFailedSlot[slot2]);
    // Closing instance 2 by hand does not cancel instance 1's waiting try.
    cmd_sensorstop_queued(enc, deviceTypeDisplayName(enc, 2), 2);
    f.after(30000); assert(starts[enc] == 1 && running[enc]); }
  // With its own switch on, instance 2 is started again by its own ladder;
  // instance 1 (switch off) is left alone.
  { Fixture f; resetSecond(); gEncoderInst[1].autoStart = true; encoderExtraRunning[2] = true;
    noteSensorAutoDisabled(enc, 2); encoderExtraRunning[2] = false; f.after(1000);
    assert(sSensorRestart[slot2].phase == Phase::Waiting && sSensorRestart[enc].phase == Phase::Idle);
    assert(flushFailedSlot[slot2] && !flushFailedMark[enc]);
    f.after(30000);
    assert(encoderExtraStarts[2] == 1 && encoderExtraRunning[2] && starts[enc] == 0); }
  // Instance 2 climbs its own ladder: failing again before its clean run
  // waits one rung longer, five clean minutes end the episode at the first
  // rung, and instance 1's ladder never moves.
  { Fixture f; resetSecond(); gEncoderInst[1].autoStart = true;
    auto secondFails = [&] {
      encoderExtraRunning[2] = true; noteSensorAutoDisabled(enc, 2); encoderExtraRunning[2] = false;
    };
    secondFails(); f.after(1000);
    assert(sSensorRestart[slot2].phase == Phase::Waiting && sSensorRestart[slot2].rung == 0);
    f.after(29000); assert(encoderExtraStarts[2] == 0);
    f.after(1000);
    assert(encoderExtraStarts[2] == 1 && sSensorRestart[slot2].phase == Phase::Proving);
    assert(!sSensorRestartPending[slot2] && !flushFailedSlot[slot2]);
    f.after(20000); secondFails(); f.after(1000);
    assert(sSensorRestart[slot2].phase == Phase::Waiting && sSensorRestartPending[slot2] && flushFailedSlot[slot2]);
    assert(logCount("next try in 30 s") == 1 && logCount("next try in 120 s") == 1);
    f.after(119000); assert(encoderExtraStarts[2] == 1);
    f.after(1000); assert(encoderExtraStarts[2] == 2 && encoderExtraRunning[2]);
    f.after(299000); assert(sSensorRestart[slot2].phase == Phase::Proving);
    f.after(1000);
    assert(sSensorRestart[slot2].phase == Phase::Idle && sSensorRestart[slot2].rung == 0);
    assert(sSensorRestart[enc].phase == Phase::Idle && !sSensorRestartPending[enc]);
    assert(starts[enc] == 0 && !flushFailedMark[enc]); }
}
#endif

// Boot autostart queues the input, then the OLED menu is entered before the
// worker ran: the menu finds the start outstanding and asks for nothing, so
// there is one start and no start-failed event. Once it runs, a menu entry
// asks for nothing either.
static void bootThenMenuCases() {
  const auto input = I2C_DEVICE_INPUT;
  { Fixture f; gSettings.inputAutoStart = true; gInputConnected = false;
    assert(autoStartEnqueue(input, 1));
    assert(i2cSensorStartOutstanding(input, 1) && f.manager.getQueueDepth() == 1);
    tryAutoStartInputForMenu();
    tryAutoStartInputForMenu();
    assert(f.manager.getQueueDepth() == 1 && statuses.empty());
    f.worker();
    assert(starts[input] == 1 && running[input] && !i2cSensorStartOutstanding(input, 1));
    gInputConnected = true;
    tryAutoStartInputForMenu();
    f.worker();
    assert(starts[input] == 1 && f.manager.getQueueDepth() == 0);
    assert(eventCount(SYSEVT_SENSOR_STARTED) == 1 && eventCount(SYSEVT_SENSOR_START_FAILED) == 0);
    gInputConnected = false; }
  // With no boot start, the menu entry is the one start.
  { Fixture f; gSettings.inputAutoStart = true; gInputConnected = false;
    tryAutoStartInputForMenu();
    assert(f.manager.getQueueDepth() == 1 && statuses.size() == 1 && statuses.back() == "input@menu");
    tryAutoStartInputForMenu();
    assert(f.manager.getQueueDepth() == 1);
    f.worker();
    assert(starts[input] == 1 && eventCount(SYSEVT_SENSOR_START_FAILED) == 0); }
  // The switch off: neither path starts it.
  { Fixture f; gSettings.inputAutoStart = false;
    tryAutoStartInputForMenu(); f.worker();
    assert(starts[input] == 0 && f.manager.getQueueDepth() == 0); }
  gSettings.inputAutoStart = false;
}

int main() {
  bootThenMenuCases();
  admissionCases(); hintCases(); workerOutcomeCases(); closeOrderingCases(); cleanupCases();
  noGateCases(); restartCases();
#ifdef HW1_HOST_I2C_TEST_ENCODER_MAX
  instanceCases();
#endif
  std::printf("Sensor lifecycle requests passed (production controller + manager; mesh=%d, encoder "
              "instances=%d)\n", ENABLE_ESPNOW, int(kI2cEncoderMaxInstances));
}
