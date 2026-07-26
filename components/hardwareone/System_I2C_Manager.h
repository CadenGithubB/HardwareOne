/**
 * I2C Device Manager - Unified I2C subsystem controller
 * Single entry point for all I2C operations, device lifecycle, and bus management
 * 
 * Now includes I2CDevice class (merged from System_I2C_Device.h)
 */

#ifndef SYSTEM_I2C_MANAGER_H
#define SYSTEM_I2C_MANAGER_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <Wire.h>
#include <type_traits>  // std::is_invocable_v for dual-bus helper dispatch

#include "System_BuildConfig.h"
#include "System_Debug.h"
// broadcastOutput provided by System_Debug.h

// ============================================================================
// I2C Error Classification (merged from System_I2C_Device.h)
// ============================================================================

enum class I2CErrorType : uint8_t {
  NONE = 0,
  NACK,           // Device not responding
  TIMEOUT,        // Bus hung or device too slow
  BUS_ERROR,      // Arbitration lost / SDA/SCL stuck
  BUFFER_OVERFLOW // ESP-IDF internal buffer issue
};

I2CErrorType classifyI2CError(uint8_t espError);

// ============================================================================
// I2CDevice Class (merged from System_I2C_Device.h)
// ============================================================================

// Forward declaration of manager for transaction template
class I2CDeviceManager;

class I2CDevice {
public:
  // Device state
  uint8_t address;
  uint8_t bus;            // 0 = primary (Wire1, I2C1), 1 = secondary (Wire, I2C2)
                          // Defaults to 0 so legacy single-bus callers work unchanged.
  const char* name;
  uint32_t clockHz;
  uint32_t baseTimeoutMs;
  uint32_t adaptiveTimeoutMs;
  
  // Health tracking
  struct Health {
    uint8_t consecutiveErrors;
    uint16_t totalErrors;
    bool degraded;
    uint32_t lastErrorTime;
    uint32_t lastSuccessTime;
    uint32_t registrationTime;
    
    // Error classification
    uint8_t nackCount;
    uint8_t timeoutCount;
    uint8_t busErrorCount;
    I2CErrorType lastErrorType;
  } health;
  
  // Transaction modes
  enum class Mode {
    STANDARD,      // Normal with health tracking
    NACK_TOLERANT, // Don't track NACKs (FM Radio)
    PERFORMANCE    // Track duration for slow sensors
  };
  
  // Constructor
  I2CDevice();
  // Initialize a device. The `busIdx` parameter is optional with default 0
  // so legacy registration sites (which don't know about multi-bus) keep
  // compiling — they get the primary bus (Wire1 / I2C1), matching the
  // single-bus behavior that existed before the dual-bus refactor.
  void init(uint8_t addr, const char* deviceName, uint32_t clock, uint32_t timeout, uint8_t busIdx = 0);
  
  // Transaction interface - unified entry point
  template<typename Func>
  auto transaction(Func&& operation, Mode mode = Mode::STANDARD) -> decltype(operation());
  
  // Health management
  void recordSuccess();
  void recordError(I2CErrorType errorType, uint8_t espError);
  bool isDegraded() const;
  void attemptRecovery();
  void resetGracePeriod();
  
  // Getters
  uint32_t getAdaptiveTimeout() const { return adaptiveTimeoutMs; }
  const Health& getHealth() const { return health; }
  bool isInitialized() const { return address != 0; }
};

// ============================================================================
// I2C Bus Metrics - Global bus performance tracking
// ============================================================================

struct I2CBusMetrics {
  uint32_t totalTransactions;
  uint32_t mutexTimeouts;
  uint32_t mutexContentions;
  uint32_t avgWaitTimeUs;
  uint32_t maxWaitTimeUs;
  uint32_t lastResetMs;
  
  // Bandwidth monitoring
  uint32_t totalBytesTransferred;
  uint32_t avgTransactionDurationUs;
  uint32_t maxTransactionDurationUs;
  
  // Duration histogram
  uint32_t txDuration_0_100us;
  uint32_t txDuration_100_500us;
  uint32_t txDuration_500_2000us;
  uint32_t txDuration_2000plus_us;
};

// ============================================================================
// I2C Device Lifecycle Management
// ============================================================================

enum I2CDeviceType {
  I2C_DEVICE_THERMAL = 0,
  I2C_DEVICE_TOF = 1,
  I2C_DEVICE_IMU = 2,
  I2C_DEVICE_INPUT = 3,
  I2C_DEVICE_GPS = 4,
  I2C_DEVICE_FMRADIO = 5,
  I2C_DEVICE_APDS = 6,
  I2C_DEVICE_RTC = 7,
  I2C_DEVICE_PRESENCE = 8
};

struct I2CDeviceStartRequest {
  I2CDeviceType device;
  unsigned long queuedAt;
};

// ============================================================================
// I2C Device Manager - Singleton
// ============================================================================

class I2CDeviceManager {
public:
  // Device registry (public for direct access by commands)
  static const int MAX_DEVICES = 16;
  I2CDevice devices[MAX_DEVICES];

  // Number of physical I2C buses we can manage. Bus 0 = Wire1 (primary STEMMA
  // QT / "I2C1" in the UI), bus 1 = Wire (secondary STEMMA QT / "I2C2").
  // Bumping this would require allocating more TwoWire instances; the ESP32-S3
  // only exposes two I2C peripherals so 2 is the practical cap.
  static const uint8_t NUM_BUSES = 2;

private:
  int deviceCount;

  // ---- Per-bus state -------------------------------------------------------
  // All hot-path I2C state is indexed by bus id (0 or 1). Bus 0 is always
  // assumed initialized when gI2CBusRunning is true; bus 1 only when
  // gSettings.i2c2Enabled and its pins are valid.
  SemaphoreHandle_t busMutexes[NUM_BUSES];   // one mutex per bus → true parallel ops
  TwoWire*          wires[NUM_BUSES];        // pointers into Arduino's Wire1/Wire
  uint32_t          currentClockHz[NUM_BUSES];
  uint32_t          defaultClockHz[NUM_BUSES];
  bool              busInitialized[NUM_BUSES];
  I2CBusMetrics     busMetrics[NUM_BUSES];

  // Clock stack for nested transactions — also per-bus, since two tasks on
  // different buses can be mid-transaction simultaneously.
  static const int CLOCK_STACK_MAX = 8;
  uint32_t clockStacks[NUM_BUSES][CLOCK_STACK_MAX];
  int      clockStackDepths[NUM_BUSES];

  // Shared (cross-bus) state — device registry mutex, request queue, etc.
  SemaphoreHandle_t managerMutex;
  I2CDeviceStartRequest deviceQueue[8];
  int queueHead;
  int queueTail;
  SemaphoreHandle_t queueMutex;
  volatile bool pollingPaused;

  // Singleton instance
  static I2CDeviceManager* instance;

  // Private constructor (singleton)
  I2CDeviceManager();

  // Internal helpers — all per-bus.
  bool clockStackPush(uint8_t bus, uint32_t hz);
  void clockStackPop(uint8_t bus);
  uint32_t clockStackTopOrDefault(uint8_t bus);
  void setBusClock(uint8_t bus, uint32_t hz);
  void updateMetrics(uint8_t bus, uint32_t waitUs, uint32_t txDurationUs, uint32_t clockHz);
  void updateHistogram(uint8_t bus, uint32_t txDurationUs);

public:
  // Singleton access
  static I2CDeviceManager* getInstance();
  static void initialize();

  // Device registration.
  // The `busIdx` parameter defaults to 0 so callers from the legacy single-bus
  // codepath (no bus knowledge) implicitly register on the primary bus.
  I2CDevice* registerDevice(uint8_t addr, const char* name, uint32_t clockHz = 100000, uint32_t timeoutMs = 200, uint8_t busIdx = 0);
  // Lookup by (address, bus). Same address on different buses → two distinct
  // devices (e.g., two DS3231s, one per bus). Default bus=0 preserves legacy
  // single-bus lookup semantics.
  I2CDevice* getDevice(uint8_t addr, uint8_t busIdx = 0);
  I2CDevice* getDeviceByName(const char* name);
  int getDeviceCount() const { return deviceCount; }
  int getActiveDeviceCount() const {
    int count = 0;
    for (int i = 0; i < deviceCount; i++) {
      if (devices[i].health.lastSuccessTime > 0) count++;
    }
    return count;
  }

  // Bus operations
  // initBuses() initializes bus 0 unconditionally (assuming gI2CBusRunning)
  // and bus 1 only when gSettings.i2c2Enabled is set + its pins are valid.
  void initBuses();
  // Initialize a specific bus. Idempotent — safe to call repeatedly; later
  // calls only re-apply the clock + pins on the existing TwoWire.
  void initBus(uint8_t busIdx, int sdaPin, int sclPin, uint32_t defaultHz);
  // Bus recovery — targets a specific bus's mutex + pins + Wire instance.
  // performBusRecovery() defaults to bus 0 for legacy callers; the bus-aware
  // overload is used when the failing device is on bus 1.
  void performBusRecovery(uint8_t busIdx = 0);
  void checkBusRecoveryNeeded();  // Event-driven recovery check (called when device degrades)
  void discoverDevices();

  // I2C device lifecycle
  bool enqueueDeviceStart(I2CDeviceType sensor);
  bool dequeueDeviceStart(I2CDeviceStartRequest* req);
  bool isInQueue(I2CDeviceType sensor);
  int getQueuePosition(I2CDeviceType sensor);
  int getQueueDepth();
  void pausePolling();
  void resumePolling();
  bool isPollingPaused() const { return pollingPaused; }

  // Metrics access — per-bus.
  const I2CBusMetrics& getMetrics(uint8_t busIdx = 0) const { return busMetrics[busIdx < NUM_BUSES ? busIdx : 0]; }
  void resetMetrics();

  // Transaction execution (called by I2CDevice). Picks the right mutex /
  // wire / clock stack from `device->bus`, runs the operation under the
  // bus's mutex with the device's clock applied, and tracks per-bus
  // metrics + health. The lambda is nullary; sensors that need to talk
  // to a specific Wire instance capture the TwoWire* externally (use
  // getWire(busIdx) at init time). This keeps the API shape identical to
  // the pre-dual-bus version — no migration cost for lambdas that stay
  // on bus 0 (their `Wire1` references already match bus 0's wire).
  template<typename Func>
  auto executeTransaction(I2CDevice* device, Func&& operation,
                         I2CDevice::Mode mode) -> decltype(operation());

  // Mutex access for external use (legacy compatibility during migration).
  // The default bus=0 keeps every existing getBusMutex() call working.
  SemaphoreHandle_t getBusMutex(uint8_t busIdx = 0) { return busMutexes[busIdx < NUM_BUSES ? busIdx : 0]; }

  // Wire accessor — returns the Arduino TwoWire pointer for a given bus, or
  // nullptr if the bus isn't initialized. Used by the dual-bus helpers and
  // by sensor drivers that need a TwoWire* to hand to a library constructor
  // (e.g., Adafruit_BNO055(addr, id, getWire(myBus))).
  TwoWire* getWire(uint8_t busIdx) {
    return (busIdx < NUM_BUSES && busInitialized[busIdx]) ? wires[busIdx] : nullptr;
  }
  bool isBusInitialized(uint8_t busIdx) const {
    return (busIdx < NUM_BUSES) && busInitialized[busIdx];
  }
};

// Global accessor
inline I2CDeviceManager* i2c() {
  return I2CDeviceManager::getInstance();
}

// ============================================================================
// Template Implementations (must be in header)
// ============================================================================

template<typename Func>
auto I2CDeviceManager::executeTransaction(I2CDevice* device, Func&& operation,
                                          I2CDevice::Mode mode)
    -> decltype(operation()) {
  using ReturnType = decltype(operation());

  if (!device) {
    DEBUG_I2CF("[TX] ABORT: device=null");
    return ReturnType();
  }

  // Resolve the bus this device lives on. Bounds-clamp for safety so an
  // uninitialized device with bus==255 doesn't index out of bounds.
  uint8_t bus = (device->bus < NUM_BUSES) ? device->bus : 0;
  SemaphoreHandle_t mutex = busMutexes[bus];
  if (!mutex || !busInitialized[bus] || !wires[bus]) {
    DEBUG_I2CF("[TX] ABORT: bus %u not initialized (device 0x%02X %s)",
               bus, device->address, device->name);
    return ReturnType();
  }

  // Check if device is degraded (allow recovery after timeout)
  if (device->isDegraded()) {
    DEBUG_I2CF("[TX] SKIP 0x%02X (%s) bus=%u: device degraded",
               device->address, device->name, bus);
    return ReturnType();
  }

  // Track transaction start
  uint32_t startUs = micros();
  busMetrics[bus].totalTransactions++;

  // Acquire this bus's mutex with the device's adaptive timeout. Other tasks
  // talking to devices on the OTHER bus are not blocked — that's the whole
  // point of per-bus mutexes.
  BaseType_t acquired = xSemaphoreTake(mutex, pdMS_TO_TICKS(device->adaptiveTimeoutMs));
  uint32_t waitUs = micros() - startUs;

  if (acquired != pdTRUE) {
    busMetrics[bus].mutexTimeouts++;
    DEBUG_I2CF("[TX] MUTEX_TIMEOUT 0x%02X (%s) bus=%u waited=%luus",
               device->address, device->name, bus, (unsigned long)waitUs);
    return ReturnType();
  }
  (void)waitUs;  // referenced again below via updateMetrics

  // Push clock to this bus's stack
  if (!clockStackPush(bus, device->clockHz)) {
    DEBUG_I2CF("[TX] CLOCK_STACK_OVERFLOW 0x%02X (%s) bus=%u",
               device->address, device->name, bus);
    xSemaphoreGive(mutex);
    return ReturnType();
  }

  // Set the bus's clock to this device's rate
  setBusClock(bus, device->clockHz);

  // Execute operation and track duration. The lambda is nullary; if it needs
  // to operate on a specific TwoWire instance other than Wire1, the lambda
  // body should capture the TwoWire* externally (e.g., via getWire(myBus)
  // cached at sensor init).
  uint32_t txStartUs = micros();

  if constexpr (std::is_same<ReturnType, void>::value) {
    operation();
    uint32_t txDurationUs = micros() - txStartUs;

    // Restore clock from the stack (nested-transaction safe)
    clockStackPop(bus);
    setBusClock(bus, clockStackTopOrDefault(bus));
    xSemaphoreGive(mutex);

    updateMetrics(bus, waitUs, txDurationUs, device->clockHz);

    if (mode != I2CDevice::Mode::NACK_TOLERANT) {
      device->recordSuccess();
    }
    return;
  } else {
    ReturnType result = operation();
    uint32_t txDurationUs = micros() - txStartUs;

    clockStackPop(bus);
    setBusClock(bus, clockStackTopOrDefault(bus));
    xSemaphoreGive(mutex);

    updateMetrics(bus, waitUs, txDurationUs, device->clockHz);

    if (mode != I2CDevice::Mode::NACK_TOLERANT) {
      if constexpr (std::is_same<ReturnType, bool>::value) {
        if (result) {
          device->recordSuccess();
        } else {
          device->recordError(I2CErrorType::NACK, 0x02);
        }
      } else {
        device->recordSuccess();
      }
    }
    (void)txDurationUs;  // suppress unused warning when DEBUG_I2CF is compiled out

    return result;
  }
}

// ============================================================================
// I2CDevice Transaction Template Implementation
// ============================================================================

// Global accessor for I2CDevice::transaction to call manager
inline I2CDeviceManager* I2CDeviceManager_getInstance() {
  return I2CDeviceManager::getInstance();
}

template<typename Func>
auto I2CDevice::transaction(Func&& operation, Mode mode) -> decltype(operation()) {
  I2CDeviceManager* mgr = I2CDeviceManager_getInstance();
  if (!mgr) return decltype(operation())();
  return mgr->executeTransaction(this, std::forward<Func>(operation), mode);
}

#endif // SYSTEM_I2C_MANAGER_H
