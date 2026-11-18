/**
 * System_I2C.h - Unified I2C System Interface
 * Clean interface that delegates to I2cDevice/Manager architecture
 *
 * This header provides: enqueueDeviceStart(), isInQueue(), getQueuePosition(),
 * requestSensorStop(), sensorStatusBumpWith(), drainDebugRing(),
 * gSensorPollingPaused, i2cPingAddress(), and all I2C helpers.
 * Include this header instead of using 'extern' declarations for these functions.
 */

#ifndef I2C_SYSTEM_H
#define I2C_SYSTEM_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <Wire.h>

#include "System_BuildConfig.h"
#include "System_I2C_Manager.h"
#include "System_Mutex.h"
#include "System_CommandRegistry.h"

// ============================================================================
// Sensor restart cooldown - shared across all I2C sensor drivers
// ============================================================================
#ifndef MIN_RESTART_DELAY_MS
#define MIN_RESTART_DELAY_MS 2000
#endif

// CACHE_MUTEX_TIMEOUT_MS lives in System_Mutex.h (single canonical home).
// System_Mutex.h is included above, so the constant is available to anyone
// who includes this header.

// ============================================================================
// Transaction Helpers — single-bus (legacy) + dual-bus (new) variants
// ============================================================================
// Two flavors of every transaction helper exist:
//
//   * Legacy 4-arg: `i2cDeviceTransactionVoid(addr, clk, lockWait, lambda)`
//     The lambda takes no arguments and references Wire1 directly. Defaults
//     to bus 0 (Wire1). Existing sensor drivers compile unchanged.
//
//   * Dual-bus 5-arg: `i2cDeviceTransactionVoid(bus, addr, clk, lockWait, lambda)`
//     The lambda CAN take a `TwoWire&` argument and use that for the
//     transmission, letting one driver work on either bus. The lambda can
//     also still be nullary — `executeTransaction` detects the arity at
//     compile time via `if constexpr` and dispatches accordingly. So a
//     migrated driver that always uses `w.foo()` works on either bus, and
//     an unmigrated driver that uses `Wire1.foo()` works ONLY when its
//     configured bus is 0 (the bus 0 wire IS Wire1).
//
// Both flavors share the underlying mgr->executeTransaction() machinery.

// Legacy single-bus helper — implicit bus 0 (Wire1). Lambda gets no args.
template<typename Func>
auto i2cDeviceTransaction(uint8_t address, uint32_t clockHz, uint32_t lockWaitMs, Func&& operation)
    -> decltype(operation()) {
  extern bool gI2cBusRunning;
  if (!gI2cBusRunning) return decltype(operation())();

  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  if (!mgr) return decltype(operation())();

  // getDevice / registerDevice default busIdx=0 — legacy lookups still hit
  // the primary bus's device table slot, unchanged from before.
  I2cDevice* dev = mgr->getDevice(address);
  if (!dev) dev = mgr->registerDevice(address, "Auto");
  if (!dev) return decltype(operation())();

  const I2cTransactionOptions options{clockHz, lockWaitMs};
  return dev->transaction(std::forward<Func>(operation), options, I2cDevice::Mode::STANDARD);
}

template<typename Func>
void i2cDeviceTransactionVoid(uint8_t address, uint32_t clockHz, uint32_t lockWaitMs, Func&& operation) {
  extern bool gI2cBusRunning;
  if (!gI2cBusRunning) return;

  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  if (!mgr) return;

  I2cDevice* dev = mgr->getDevice(address);
  if (!dev) dev = mgr->registerDevice(address, "Auto");
  if (!dev) return;

  const I2cTransactionOptions options{clockHz, lockWaitMs};
  dev->transaction(std::forward<Func>(operation), options, I2cDevice::Mode::STANDARD);
}

// New dual-bus helpers — explicit `bus` (0 or 1). Lambda is nullary, exactly
// like the legacy helpers. Sensors that need to talk to a specific TwoWire
// (e.g., one routed to bus 1) capture the TwoWire* externally at init time
// via `i2c()->getWire(myBus)` and reference it through the capture instead
// of using Wire1 directly. The manager routes mutex/clock/recovery to the
// device's bus internally so the rest of the transaction shape is unchanged.
template<typename Func>
auto i2cDeviceTransaction(uint8_t bus, uint8_t address, uint32_t clockHz,
                          uint32_t lockWaitMs, Func&& operation)
    -> decltype(operation()) {
  extern bool gI2cBusRunning;
  if (!gI2cBusRunning) return decltype(operation())();

  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  if (!mgr) return decltype(operation())();

  I2cDevice* dev = mgr->getDevice(address, bus);
  if (!dev) dev = mgr->registerDevice(address, "Auto", bus);
  if (!dev) return decltype(operation())();

  const I2cTransactionOptions options{clockHz, lockWaitMs};
  return dev->transaction(std::forward<Func>(operation), options, I2cDevice::Mode::STANDARD);
}

template<typename Func>
void i2cDeviceTransactionVoid(uint8_t bus, uint8_t address, uint32_t clockHz,
                              uint32_t lockWaitMs, Func&& operation) {
  extern bool gI2cBusRunning;
  if (!gI2cBusRunning) return;

  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  if (!mgr) return;

  I2cDevice* dev = mgr->getDevice(address, bus);
  if (!dev) dev = mgr->registerDevice(address, "Auto", bus);
  if (!dev) return;

  const I2cTransactionOptions options{clockHz, lockWaitMs};
  dev->transaction(std::forward<Func>(operation), options, I2cDevice::Mode::STANDARD);
}

template<typename Func>
auto i2cTaskWithStandardTimeout(uint8_t address, uint32_t clockHz, Func&& operation)
    -> decltype(operation()) {
  // Legacy function name: 1000ms is the bus-mutex acquisition budget only.
  return i2cDeviceTransaction(address, clockHz, 1000, std::forward<Func>(operation));
}

template<typename Func>
auto i2cTaskWithTimeout(uint8_t address, uint32_t clockHz, uint32_t lockWaitMs, Func&& operation)
    -> decltype(operation()) {
  return i2cDeviceTransaction(address, clockHz, lockWaitMs, std::forward<Func>(operation));
}

// Bus-aware overloads — same semantics, leading `bus` arg routes the
// transaction through the bus-aware i2cDeviceTransaction (per-bus mutex/
// clock). Used by sensors migrated to dual-bus.
template<typename Func>
auto i2cTaskWithStandardTimeout(uint8_t bus, uint8_t address, uint32_t clockHz, Func&& operation)
    -> decltype(operation()) {
  // Legacy function name: 1000ms is the bus-mutex acquisition budget only.
  return i2cDeviceTransaction(bus, address, clockHz, 1000, std::forward<Func>(operation));
}

template<typename Func>
auto i2cTaskWithTimeout(uint8_t bus, uint8_t address, uint32_t clockHz,
                        uint32_t lockWaitMs, Func&& operation)
    -> decltype(operation()) {
  return i2cDeviceTransaction(bus, address, clockHz, lockWaitMs, std::forward<Func>(operation));
}

// Primary discovery address; SSD1306 panels may instead answer at 0x3C.
// Live display transactions use gDisplayI2cAddress after probing.
#define I2C_ADDR_OLED 0x3D

template<typename Func>
void i2cTransactionNackTolerant(uint8_t address, uint32_t clockHz, uint32_t lockWaitMs, Func&& operation) {
  extern bool gI2cBusRunning;
  if (!gI2cBusRunning) return;

  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  if (!mgr) return;

  I2cDevice* dev = mgr->getDevice(address);
  if (!dev) dev = mgr->registerDevice(address, "Auto");
  if (!dev) return;

  const I2cTransactionOptions options{clockHz, lockWaitMs};
  dev->transaction(std::forward<Func>(operation), options, I2cDevice::Mode::NACK_TOLERANT);
}

// Dual-bus NACK-tolerant variant (FM radio path uses NACK-tolerant for
// register polling; same shape extended with a bus selector).
template<typename Func>
void i2cTransactionNackTolerant(uint8_t bus, uint8_t address, uint32_t clockHz,
                                uint32_t lockWaitMs, Func&& operation) {
  extern bool gI2cBusRunning;
  if (!gI2cBusRunning) return;

  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  if (!mgr) return;

  I2cDevice* dev = mgr->getDevice(address, bus);
  if (!dev) dev = mgr->registerDevice(address, "Auto", bus);
  if (!dev) return;

  const I2cTransactionOptions options{clockHz, lockWaitMs};
  dev->transaction(std::forward<Func>(operation), options, I2cDevice::Mode::NACK_TOLERANT);
}

// ============================================================================
// Task Stack Safety - Auto-disable sensor if stack usage exceeds threshold
// ============================================================================
// Call this periodically (e.g. every 10s) from any sensor task's watermark check.
// If remaining stack drops below 5% of total, the sensor is auto-disabled and
// the standard shutdown path at the top of the task loop handles cleanup.
//
// Usage:  if (checkTaskStackSafety("presence", stackWords, &presenceEnabled)) break;
//
// Returns true if the task should exit (enabled flag was set to false).
inline bool checkTaskStackSafety(const char* sensorName, uint32_t totalStackWords, volatile bool* enabledFlag) {
  UBaseType_t freeWords = uxTaskGetStackHighWaterMark(nullptr);
  uint32_t usedPercent = (totalStackWords > 0) ? ((totalStackWords - freeWords) * 100 / totalStackWords) : 0;
  if (freeWords < (totalStackWords / 20)) {  // Less than 5% remaining
    // CRITICAL: About to overflow - auto-disable to prevent crash
    extern void sensorStatusBumpWith(const char* reason);
    char reason[64];
    snprintf(reason, sizeof(reason), "%s@stack_overflow_bailout", sensorName);
    if (enabledFlag) *enabledFlag = false;
    sensorStatusBumpWith(reason);
    // Use ERROR level logging - always visible regardless of debug flags
    log_e("[STACK_SAFETY] %s task at %u%% stack usage (%u/%u words free) - AUTO-DISABLED",
          sensorName, usedPercent, (unsigned)freeWords, totalStackWords);
    return true;  // Caller should break/return to hit the shutdown path
  }
  return false;
}

// Ping/probe helpers - DO NOT auto-register devices during probe!
// These are used by i2cscan to check if devices exist, not to set them up
// Probe an address twice on a positive reply; never read or write payload data.
// Does NOT auto-register the device
// (probes are used by the scanner / wizards to check existence, not to set
// devices up for ongoing use). `bus` defaults to 0 so legacy callers work.
inline uint8_t i2cProbeAddress(uint8_t address, uint32_t clockHz, uint32_t lockWaitMs, uint8_t bus = 0) {
  extern bool gI2cBusRunning;
  if (!gI2cBusRunning) return 4;

  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  if (!mgr) return 4;
  return mgr->probeAddress(bus, address,
                           I2cTransactionOptions{clockHz, lockWaitMs});
}

inline bool i2cPingAddress(uint8_t address, uint32_t clockHz, uint32_t lockWaitMs, uint8_t bus = 0) {
  return (i2cProbeAddress(address, clockHz, lockWaitMs, bus) == 0);
}

// Queue a stop, cancelling pending starts. The worker applies it after any
// active initialization; the sensor task then owns resource cleanup.
bool requestSensorStop(I2cDeviceType device);

// Admission for a queued sensor start, shared by the start commands and the
// main loop's menu input auto-start. Formats nothing: the command wrapper
// renders the outcome into cmd_exec's debug buffer, so other tasks can call
// this without touching that buffer. True only when the start was queued and
// the status bumped; `why` names the refusal otherwise.
enum class SensorStartRefusal : uint8_t {
  None,
  BusDisabled,
  QueueUnavailable,
  StopPending,
  AlreadyRunning,
  AlreadyQueued,
  NoAck,
  ProbeFailed,
  NotQueued,
};
bool requestSensorStart(I2cDeviceType sensor, const char* eventTag, SensorStartRefusal* why);
const char* sensorStartRefusalText(SensorStartRefusal why);

// `hint` is a next-step line printed only when the start was queued.
const char* cmd_sensorstart_queued(I2cDeviceType sensor, const char* displayName,
                                   const char* eventTag, const char* hint = nullptr);

// Command wrapper for requestSensorStop(): the stop commands return its reply.
// `displayName` is the tag the reply carries in brackets ("ToF", "FM Radio").
const char* cmd_sensorstop_queued(I2cDeviceType sensor, const char* displayName);

// Display name for I2cDeviceType (e.g., I2C_DEVICE_IMU → "IMU")
const char* deviceTypeDisplayName(I2cDeviceType device);

// Queue functions
// Queues a start and marks it outstanding until the queue worker settles it
// (started, failed, or closed by a stop).
bool enqueueDeviceStart(I2cDeviceType device);

inline bool dequeueDeviceRequest(I2cDeviceRequest* req) {
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  return mgr ? mgr->dequeueDeviceRequest(req) : false;
}

inline bool isInQueue(I2cDeviceType device) {
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  return mgr ? mgr->isInQueue(device) : false;
}

inline int getQueuePosition(I2cDeviceType device) {
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  return mgr ? mgr->getQueuePosition(device) : -1;
}

inline int getQueueDepth() {
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  return mgr ? mgr->getQueueDepth() : 0;
}

// Health functions are explicitly keyed by (bus, address), just like the
// registry. Address-only lookup is ambiguous when identical devices exist on
// both buses and used to let a boot-created bus-0 ghost mask the real device.
inline bool i2cDeviceIsDegraded(uint8_t address, uint8_t bus) {
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  if (!mgr) return false;
  I2cDevice* dev = mgr->getDevice(address, bus);
  return dev ? dev->isDegraded() : false;
}

inline void i2cDeviceSuccess(uint8_t address, uint8_t bus) {
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  if (!mgr) return;
  I2cDevice* dev = mgr->getDevice(address, bus);
  if (dev) dev->recordSuccess();
}

inline void i2cDeviceError(uint8_t address, uint8_t bus) {
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  if (!mgr) return;
  I2cDevice* dev = mgr->getDevice(address, bus);
  if (dev) dev->recordError(I2cErrorType::NACK, 0x02);
}

// Check if sensor should auto-disable based on consecutive I2C failures
// Uses existing I2cDevice health tracking - no local counters needed in sensor tasks
inline bool i2cShouldAutoDisable(uint8_t address, uint8_t bus,
                                 uint8_t maxConsecutiveErrors = 5) {
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  if (!mgr) return false;
  I2cDevice* dev = mgr->getDevice(address, bus);
  if (!dev) return false;
  return dev->getHealth().consecutiveErrors >= maxConsecutiveErrors;
}

// Get current consecutive error count for a device (for logging)
inline uint8_t i2cGetConsecutiveErrors(uint8_t address, uint8_t bus) {
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  if (!mgr) return 0;
  I2cDevice* dev = mgr->getDevice(address, bus);
  return dev ? dev->getHealth().consecutiveErrors : 0;
}

// Recover a specific bus, or bus 0 when the caller has no bus context. True
// only when the recovery ran; a refused one left the bus untouched.
inline bool i2cBusRecovery(uint8_t busIdx = 0) {
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  if (!mgr) return false;
  return mgr->performBusRecovery(busIdx) == I2cBusRecovery::Done;
}

// ============================================================================
// I2C Device Addresses
// ============================================================================
#define I2C_ADDR_GPS         0x10
#define I2C_ADDR_FM_RADIO    0x11
#define I2C_ADDR_IMU         0x28
#define I2C_ADDR_TOF         0x29
#define I2C_ADDR_THERMAL     0x33
#define I2C_ADDR_FUEL_GAUGE  0x36  // MAX17048G (FeatherS3[D] on-board LiPo gauge)
#define I2C_ADDR_APDS        0x39
// I2C_ADDR_OLED 0x3D — defined above the template helpers; not redefined here
#define I2C_ADDR_PCA9685     0x40
// ANO seesaw breakout default is 0x49 per Adafruit; some units expose solder
// jumpers for alternates. Override at runtime via gSettings.anoEncoderI2cAddr
// if your hardware is rebound.
#define I2C_ADDR_ANO_ENCODER 0x49
#define I2C_ADDR_PRESENCE    0x5A
#define I2C_ADDR_GAMEPAD     0x50
#define I2C_ADDR_DS3231      0x68

// ============================================================================
// Global Flags and Configuration
// ============================================================================
extern bool gI2cBusRunning;

// The reference-counted "pause sensor polling" primitive (pollPause/pollResume/
// PollPauseGuard, the legacy sensorPolling* aliases, and the gSensorPollingPaused
// mirror) now lives in its own lightweight module so non-I2C subsystems can use
// it without dragging in the sensor database below. Re-exported here so every
// existing includer of System_I2C.h keeps getting the API unchanged.
#include "System_PollPause.h"

// ============================================================================
// I2C Sensor Database
// ============================================================================
struct I2cSensorEntry {
  uint8_t address;
  const char* name;
  const char* description;
  const char* manufacturer;
  bool multiAddress;
  uint8_t altAddress;
  size_t libraryHeapBytes;
  const char* libraryName;
  const char* headerGuard;
  const char* moduleName;
  uint32_t i2cClockHz;
  uint32_t i2cTimeoutMs;
  // What the row is, as values. Code branches on these, never on moduleName.
  bool compiled;                  // the row's driver is in this build (its ENABLE_ flag)
  int (*configuredBus)();         // reads the setting that routes the device to a bus
  uint8_t (*configuredAddress)(); // the address when it is a setting, else nullptr
  I2cDeviceType deviceType;       // the start/stop queue's id; I2C_DEVICE_COUNT when the queue does not manage it
};

// The module name of a queue-managed device (its feature id), from i2cSensors[].
const char* i2cModuleNameForDeviceType(I2cDeviceType type);

extern const I2cSensorEntry i2cSensors[];
extern const size_t i2cSensorsCount;

// ============================================================================
// Hardware detection — scan the I2C buses and diff against configured features
// ============================================================================
// detectHardware() is READ-ONLY: it probes the known device addresses (the
// collision-free set above), maps each hit to its feature (moduleName ==
// FeatureEntry.id), and classifies it against the current enable flags. It never
// mutates config — "missing" devices are reported, never auto-disabled. Reusable
// by the `detect` command, the dashboard, and the setup wizard's auto-config step.

enum class DetectStatus : uint8_t {
  PRESENT_ENABLED,   // on the bus AND its feature is enabled
  PRESENT_DISABLED,  // on the bus but the feature is off (attachable)
  MISSING,           // feature enabled but not found on any bus (report-only)
  PRESENT_INFRA,     // present, no toggleable feature (OLED, fuel gauge, PWM)
};

struct DetectedEntry {
  uint8_t      address;     // device address (primary)
  uint8_t      bus;         // bus found on (0..POLL_NUM_BUSES-1), or 0xFF if MISSING
  const char*  name;        // device name from the sensor DB
  const char*  moduleName;  // feature id, or nullptr for infrastructure
  uint16_t     heapCostKb;  // feature heap estimate (0 for infrastructure)
  DetectStatus status;
};

struct DetectionResult {
  static const uint8_t MAX = 24;
  DetectedEntry entries[MAX];
  uint8_t count            = 0;
  uint8_t nPresentEnabled  = 0;
  uint8_t nPresentDisabled = 0;
  uint8_t nMissing         = 0;
  uint8_t nInfra           = 0;
  bool    busDisabled      = false;  // true if the I2C bus is off (nothing scanned)
};

// Probe both buses (each quiesced during its scan) and fill `out`. Read-only.
void detectHardware(DetectionResult& out);

// CLI `detect` — runs detectHardware() and prints the diff. `detect apply`
// (admin) auto-enables cheap present-but-disabled devices. Read-only otherwise.
const char* cmd_detect(const String& argsInput);

// Auto-config: enable the cheap present-but-disabled devices a scan found (set
// the feature flag + persist). Heavy devices (camera/microphone/speech/gamepad)
// are LEFT OFF for an explicit offer; missing devices are untouched. Reused by
// `detect apply` and the setup wizard's Basic auto-config step.
struct ApplyResult { uint8_t enabled; uint8_t offered; bool rebootNeeded; };
ApplyResult applyDetectedHardware(const DetectionResult& r);

// ============================================================================
// Device Registry
// ============================================================================
#define MAX_CONNECTED_DEVICES 16

struct ConnectedDevice {
  uint8_t address;
  uint8_t bus;
  const char* name;
  const char* description;
  const char* manufacturer;
  bool isConnected;
  unsigned long lastSeen;
  unsigned long firstDiscovered;
};

// Devices found by the last bus scan (System_I2C.cpp).
extern ConnectedDevice connectedDevices[];
extern int connectedDeviceCount;

// ============================================================================
// Initialization Functions
// ============================================================================
void initI2cBuses();
void initI2cManager();
void initSensorQueue();

// ============================================================================
// Discovery and Helper Functions
// ============================================================================
void discoverI2cDevices();
bool isSensorConnected(const char* name);
// Address-table label only; driver initialization validates the actual hardware.
String identifySensor(uint8_t address);

// Shared sensor-reading envelope: every <x>BuildDataJSON opens its payload with
// this so all interfaces (BLE/web/OLED/serial/MQTT/ESP-NOW) parse ONE shape.
// Writes, WITHOUT a closing brace:
//   {"valid":<bool>,"connected":<bool>,"ts":<lastUpdateMs>
// The caller then appends its own value keys (each starting with a comma) and
// the closing '}'. ts = the cache's lastUpdate (millis; 0 = never sampled).
// There is deliberately NO "age" key: local consumers derive it (millis()-ts,
// same clock), and remote ones can't use a sender-relative age anyway — they
// stamp arrival time instead. Dropping it also keeps a reading smaller: one
// of up to 190 bytes travels in a single frame to a direct peer (118 through
// a relay; the largest reading is 472 bytes in four frames,
// System_ESPNow_SensorCore.h). Returns bytes written
// (always < bufSize, leaving room for the caller's suffix), or 0 on
// error/overflow — a 0 return means the caller must NOT append.
int sensorEnvelopeBegin(char* buf, size_t bufSize, bool valid, bool connected,
                        unsigned long lastUpdateMs);

// Degraded-mode replies for `<x>read json`, for the paths where the builder
// cannot run at all (scratch-buffer alloc failed, or the builder returned 0).
// These are envelope-SHAPED on purpose: they used to be bare `{"valid":false}`
// literals, so a client that read `.connected`/`.ts` got `undefined` from a
// failure reply but a real value from every success reply — the one place the
// "every reading is one shape" guarantee broke. ts:0 means "never sampled",
// exactly as sensorEnvelopeBegin emits it. Keep these in sync with the format
// string in sensorEnvelopeBegin (System_I2C.cpp).
#define SENSOR_JSON_UNAVAILABLE "{\"valid\":false,\"connected\":false,\"ts\":0}"
#define SENSOR_JSON_NOBUF       "{\"valid\":false,\"connected\":false,\"ts\":0,\"error\":\"buffer\"}"

// Fixed `kind` vocabulary for the discovery layer (`sensors json`). Constants
// rather than bare literals so the vocabulary is greppable and typo-proof — a
// misspelled literal at a call site would otherwise ship silently, since no
// firmware consumer validates this field.
//   scalar — one or a few named values (rtc, gps, apds, fmradio, input, presence)
//   vector — a structured multi-axis reading (imu, tof)
//   stream — a large continuous payload whose full form never enters the
//            envelope; MUST register a compact summary builder (thermal)
#define SENSOR_KIND_SCALAR "scalar"
#define SENSOR_KIND_VECTOR "vector"
#define SENSOR_KIND_STREAM "stream"

// ============================================================================
// Sensor Task Functions
// ============================================================================
void tofTask(void* parameter);
void imuTask(void* parameter);
void thermalTask(void* parameter);
void inputTask(void* parameter);
void apdsTask(void* parameter);
void gpsTask(void* parameter);
void fmRadioTask(void* parameter);
void presenceTask(void* parameter);
void rtcTask(void* parameter);
void sensorQueueProcessorTask(void* param);

// Queue processor task handle (defined in System_I2C.cpp)
extern TaskHandle_t queueProcessorTask;

// Sensor Auto-Start (call during boot after I2C init)
void processAutoStartSensors();

// ============================================================================
// Sensor Command Handlers
// ============================================================================
const char* cmd_thermalstart(const String& argsInput);
const char* cmd_thermalstop(const String& argsInput);
const char* cmd_tofstart(const String& argsInput);
const char* cmd_tofstop(const String& argsInput);
const char* cmd_tof(const String& argsInput);
const char* cmd_imustart(const String& argsInput);
const char* cmd_imustop(const String& argsInput);
const char* cmd_imu(const String& argsInput);
// cmd_openinput / cmd_closeinput live in HAL_Input.cpp (unified for either driver).
const char* cmd_gamepad(const String& argsInput);
const char* cmd_apdscolor(const String& argsInput);
const char* cmd_apdsproximity(const String& argsInput);
const char* cmd_apdsgesture(const String& argsInput);
const char* cmd_gpsstart(const String& argsInput);
const char* cmd_gpsstop(const String& argsInput);
const char* cmd_gps(const String& argsInput);
const char* cmd_imuactions(const String& argsInput);
const char* cmd_sensorlog(const String& argsInput);
const char* cmd_i2cscan(const String& argsInput);
const char* cmd_i2cstats(const String& argsInput);
const char* cmd_i2cmetrics(const String& argsInput);
const char* cmd_i2chealth(const String& argsInput);
const char* cmd_sensors(const String& argsInput);
const char* cmd_sensorinfo(const String& argsInput);
const char* cmd_devices(const String& argsInput);
const char* cmd_devicefile(const String& argsInput);

// Shared bus-routing helper for the per-device `<device>bus` commands. Each
// sensor module's own cmd_<sensor>bus calls this so the parse/range check/
// persist/report behaviour stays in one place (a bus that does not exist is
// refused). Does NOT contain
// RETURN_VALID_IF_VALIDATE_CSTR — that must stay at each cmd_* entry point,
// because it returns from its caller.
const char* i2cSetDeviceBusAndReport(int& target, const String& argsInput,
                                     const char* settingName);

// ============================================================================
// Bus State
// ============================================================================
// The state of one sensor (running, connected, task handle, stop time) is
// declared in that sensor's own header, for both the enabled and the
// disabled build.
extern uint32_t gWire1DefaultHz;

// ============================================================================
// Helper Functions
// ============================================================================
void drainDebugRing();
void i2cSetDefaultWire1Clock();
void sensorStatusBumpWith(const char* reason);
// Sensor status change counter and the reason of the last change
// (HardwareOne.cpp). Readers compare the counter to detect a change.
extern volatile unsigned long gSensorStatusSeq;
extern const char* gLastStatusCause;
const char* buildSensorStatusJson();

// ============================================================================
// Thermal Sensor Externs
// ============================================================================
#if ENABLE_THERMAL_SENSOR
extern volatile uint32_t thermalArmAtMs;
extern volatile bool thermalPendingFirstFrame;
#endif

// ============================================================================
// I2C Bus Configuration
// ============================================================================
// Note: I2C pins are now configurable via settings (gSettings.i2cSdaPin, gSettings.i2cSclPin)
// Defaults are board-specific - see System_BuildConfig.h for BOARD_NAME and pin mappings
#define I2C_WIRE1_DEFAULT_FREQ 100000

// Grace period reset (used during sensor re-init to clear stale error state)
void i2cResetGracePeriod(uint8_t address, uint8_t bus);

void buildDeviceRegistryJson(JsonDocument& doc);
void buildI2cDeviceListJson(JsonArray& arr, bool verbose = false);
int i2cCompiledSensorCount();
int i2cConnectedDeviceCount();
void sensorStatusBump();

#endif // I2C_SYSTEM_H
