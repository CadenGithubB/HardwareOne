/**
 * I2C Device Manager - Unified I2C subsystem controller
 * Single entry point for all I2C operations, device lifecycle, and bus management
 * 
 * Now includes I2cDevice class (merged from System_I2C_Device.h)
 */

#ifndef SYSTEM_I2C_MANAGER_H
#define SYSTEM_I2C_MANAGER_H

#include <Arduino.h>
#include <atomic>
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

enum class I2cErrorType : uint8_t {
  NONE = 0,
  NACK,           // Device not responding
  TIMEOUT,        // Bus hung or device too slow
  BUS_ERROR,      // Arbitration lost / SDA/SCL stuck
  BUFFER_OVERFLOW // ESP-IDF internal buffer issue
};

I2cErrorType classifyI2cError(uint8_t espError);

// Execution policy belongs to an individual transaction, not to the device
// registry.  A single device can legitimately use a conservative clock during
// initialization and a different clock / bus-lock budget while polling.
//
// lockWaitMs is only the maximum wait to acquire the per-bus mutex.  It is not
// a Wire I/O deadline; TwoWire's hardware timeout is configured separately by
// initBus().  Keeping those two concepts separate prevents an I/O timeout from
// silently changing how long unrelated tasks queue for a busy bus.
struct I2cTransactionOptions {
  uint32_t clockHz;
  uint32_t lockWaitMs;
};

// ============================================================================
// I2cDevice Class (merged from System_I2C_Device.h)
// ============================================================================

// Forward declaration of manager for transaction template
class I2cDeviceManager;

class I2cDevice {
public:
  // Device state
  uint8_t address;
  uint8_t bus;            // 0 = primary (Wire1, I2C1), 1 = secondary (Wire, I2C2)
                          // Defaults to 0 so legacy single-bus callers work unchanged.
  const char* name;
  
  // Health tracking
  struct Health {
    uint8_t consecutiveErrors;
    uint16_t totalErrors;
    bool degraded;
    bool reportedLost;      // sensor_unresponsive posted, sensor_recovered not yet
    uint32_t lastErrorTime;
    uint32_t lastSuccessTime;
    uint32_t registrationTime;
    
    // Error classification
    uint8_t nackCount;
    uint8_t timeoutCount;
    uint8_t busErrorCount;
    I2cErrorType lastErrorType;
  } health;
  
  // Transaction modes
  enum class Mode {
    STANDARD,      // Normal with health tracking
    NACK_TOLERANT, // Don't track NACKs (FM Radio)
    PERFORMANCE    // Track duration for slow sensors
  };
  
  // Constructor
  I2cDevice();
  // Initialize registry identity. Timing is deliberately absent: it is
  // supplied through I2cTransactionOptions for every operation.
  void init(uint8_t addr, const char* deviceName, uint8_t busIdx = 0);
  
  // Transaction interface - unified entry point
  template<typename Func>
  auto transaction(Func&& operation, const I2cTransactionOptions& options,
                   Mode mode = Mode::STANDARD) -> decltype(operation());
  
  // Health management
  void recordSuccess();
  void recordError(I2cErrorType errorType, uint8_t espError);
  bool isDegraded() const;
  void attemptRecovery();
  void resetGracePeriod();
  
  // Getters
  const Health& getHealth() const { return health; }
  bool isInitialized() const { return address != 0; }
};

// ============================================================================
// I2C Bus Metrics - Global bus performance tracking
// ============================================================================

struct I2cBusMetrics {
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

enum I2cDeviceType : int {
  I2C_DEVICE_THERMAL = 0,
  I2C_DEVICE_TOF = 1,
  I2C_DEVICE_IMU = 2,
  I2C_DEVICE_INPUT = 3,
  I2C_DEVICE_GPS = 4,
  I2C_DEVICE_FMRADIO = 5,
  I2C_DEVICE_APDS = 6,
  I2C_DEVICE_RTC = 7,
  I2C_DEVICE_PRESENCE = 8,
  I2C_DEVICE_ENCODER = 9,  // Adafruit 5752/4991 encoder board pixels: no task, ticked by the main loop
  I2C_DEVICE_COUNT
};

enum class I2cDeviceRequestAction { Start, Stop };

// How performBusRecovery() ended. Everything but Done left the bus untouched.
enum class I2cBusRecovery : uint8_t {
  Done,            // bus torn down, clocked free and begun again
  NotInitialized,  // the bus was never brought up
  PinsInvalid,     // the SDA/SCL pin settings do not name a usable pair
  Busy,            // the bus mutex was not free within 2 s
  InUse,           // a client (the camera) holds a lease on the bus
};

struct I2cDeviceRequest {
  I2cDeviceType device;
  unsigned long queuedAt;
  I2cDeviceRequestAction action;
};

// ============================================================================
// I2C Device Manager - Singleton
// ============================================================================

class I2cDeviceManager {
public:
  // Device registry (public for direct access by commands)
  static const int MAX_DEVICES = 16;
  I2cDevice devices[MAX_DEVICES];

  // Number of physical I2C buses we can manage. Bus 0 = Wire1 (primary STEMMA
  // QT / "I2C1" in the UI), bus 1 = Wire (secondary STEMMA QT / "I2C2").
  // Bumping this would require allocating more TwoWire instances; the ESP32-S3
  // only exposes two I2C peripherals so 2 is the practical cap.
  static const uint8_t NUM_BUSES = 2;

private:
  int deviceCount;

  // ---- Per-bus state -------------------------------------------------------
  // All hot-path I2C state is indexed by bus id (0 or 1). Bus 0 is always
  // assumed initialized when gI2cBusRunning is true; bus 1 only when
  // gSettings.i2c2Enabled and its pins are valid.
  SemaphoreHandle_t busMutexes[NUM_BUSES];   // one mutex per bus → true parallel ops
  TwoWire*          wires[NUM_BUSES];        // pointers into Arduino's Wire1/Wire
  uint32_t          currentClockHz[NUM_BUSES];
  uint32_t          defaultClockHz[NUM_BUSES];
  bool              busInitialized[NUM_BUSES];
  std::atomic<unsigned> busClients[NUM_BUSES]{};
  I2cBusMetrics     busMetrics[NUM_BUSES];
  uint32_t          lastRecoveryMs[NUM_BUSES];  // throttles error-driven recovery

  // Per-bus clock restoration bookkeeping. Bus mutexes are intentionally
  // non-recursive: a transaction lambda must not enter another manager
  // transaction on the same bus. Separate state still lets the two physical
  // buses operate simultaneously.
  static const int CLOCK_STACK_MAX = 8;
  uint32_t clockStacks[NUM_BUSES][CLOCK_STACK_MAX];
  int      clockStackDepths[NUM_BUSES];

  // Shared (cross-bus) state — device registry mutex, request queue, etc.
  SemaphoreHandle_t managerMutex;
  static const int DEVICE_QUEUE_SIZE = 8;
  I2cDeviceRequest deviceQueue[DEVICE_QUEUE_SIZE];
  int queueHead;
  int queueTail;
  // Stops cannot be refused because the start ring is full. Each device has
  // one pending stop, which cancels its queued starts under the same mutex.
  bool pendingDeviceStops[I2C_DEVICE_COUNT];
  unsigned long deviceStopQueuedAt[I2C_DEVICE_COUNT];
  SemaphoreHandle_t queueMutex;
  volatile bool pollingPaused;

  // Singleton instance
  static I2cDeviceManager* instance;

  // Private constructor (singleton)
  I2cDeviceManager();

  // Internal helpers — all per-bus.
  bool clockStackPush(uint8_t bus, uint32_t hz);
  void clockStackPop(uint8_t bus);
  uint32_t clockStackTopOrDefault(uint8_t bus);
  void setBusClock(uint8_t bus, uint32_t hz);
  void updateMetrics(uint8_t bus, uint32_t waitUs, uint32_t txDurationUs, uint32_t clockHz);
  void updateHistogram(uint8_t bus, uint32_t txDurationUs);

  // Zero-length address probe used to classify a failed transaction. Must be
  // called with the bus mutex already held (executeTransaction does). Returns
  // a TwoWire::endTransmission() status for classifyI2cError().
  uint8_t probeDeviceStatus(uint8_t bus, uint8_t addr);

public:
  // Singleton access
  static I2cDeviceManager* getInstance();
  static void initialize();
  // Inspect already-created buses without allocating the manager during early
  // settings application or another operation that does not start I2C.
  static I2cDeviceManager* existingInstance() { return instance; }

  // Device registration records identity and health only. Clock rate and
  // mutex-wait policy are per-transaction options and never live here.
  // `busIdx` defaults to bus 0 for legacy callers.
  I2cDevice* registerDevice(uint8_t addr, const char* name, uint8_t busIdx = 0);
  // Lookup by (address, bus). Same address on different buses → two distinct
  // devices (e.g., two DS3231s, one per bus). Default bus=0 preserves legacy
  // single-bus lookup semantics.
  I2cDevice* getDevice(uint8_t addr, uint8_t busIdx = 0);
  // Address-only lookup across every bus, lowest bus first. Retained for CLI
  // commands whose syntax genuinely supplies only an address (i2crecover).
  // Runtime health and driver paths must use getDevice(addr, bus). If the same
  // address exists on both buses this returns bus 0 deterministically.
  I2cDevice* getDeviceAnyBus(uint8_t addr);
  I2cDevice* getDeviceByName(const char* name);
  int getDeviceCount() const { return deviceCount; }
  int getActiveDeviceCount() const {
    int count = 0;
    for (int i = 0; i < deviceCount; i++) {
      if (devices[i].health.lastSuccessTime > 0) count++;
    }
    return count;
  }

  // Bus operations
  // initBuses() initializes bus 0 unconditionally (assuming gI2cBusRunning)
  // and bus 1 only when gSettings.i2c2Enabled is set + its pins are valid.
  void initBuses();
  // Initialize a specific bus. Idempotent — safe to call repeatedly; later
  // calls only re-apply the clock + pins on the existing TwoWire.
  void initBus(uint8_t busIdx, int sdaPin, int sclPin, uint32_t defaultHz);
  // Lifetime lease for clients borrowing the underlying bus. Transactions still
  // use the ordinary device mutex/clock path; a lease blocks bus recreation.
  bool retainBus(uint8_t busIdx);
  void releaseBus(uint8_t busIdx);
  bool busHasClients(uint8_t busIdx) const {
    return busIdx < NUM_BUSES && busClients[busIdx].load() != 0;
  }
  // Bus recovery — targets a specific bus's mutex + pins + Wire instance.
  // performBusRecovery() defaults to bus 0 for legacy callers; the bus-aware
  // overload is used when the failing device is on bus 1.
  I2cBusRecovery performBusRecovery(uint8_t busIdx = 0);
  void checkBusRecoveryNeeded();  // Event-driven recovery check (called when device degrades)
  // True while a bus is inside its cooldown after an error-driven recovery.
  // Guards the automatic (BUS_ERROR) path only — explicit operator-initiated
  // recovery via i2creset deliberately ignores this.
  bool busRecoveryThrottled(uint8_t busIdx) const;
  void discoverDevices();

  // Address-only discovery; a positive result requires two ACKs. Uses the mutex and
  // clock cache but deliberately do not create device-health records or alter
  // the normal device-transaction metrics.
  uint8_t probeAddress(uint8_t busIdx, uint8_t address,
                       const I2cTransactionOptions& options);
  // Identity read for discovery: write `command`, wait `settleUs`, read
  // `replyLen` bytes. Same mutex/clock handling and no health record as
  // probeAddress(). Used only where a board names itself by a register a scan
  // may select (the seesaw product id, System_SeesawIdentity.h). Returns 0 on
  // success, else an endTransmission() status (4 = bus unavailable/busy or a
  // short reply).
  uint8_t identityRead(uint8_t busIdx, uint8_t address, const uint8_t* command,
                       size_t commandLen, uint32_t settleUs, uint8_t* reply,
                       size_t replyLen, const I2cTransactionOptions& options);

  // I2C device lifecycle. The existing sensor worker is the sole consumer:
  // it applies a dequeued stop before dispatching another start. Pending stops
  // cancel queued starts, take priority and remain visible during initialization.
  bool enqueueDeviceStart(I2cDeviceType sensor);
  bool enqueueDeviceStop(I2cDeviceType sensor);
  bool dequeueDeviceRequest(I2cDeviceRequest* req);
  bool hasPendingDeviceStop(I2cDeviceType sensor);
  // Queue status describes pending starts only; stops take dispatch priority.
  bool isInQueue(I2cDeviceType sensor);
  int getQueuePosition(I2cDeviceType sensor);
  int getQueueDepth();
  void pausePolling();
  void resumePolling();
  bool isPollingPaused() const { return pollingPaused; }

  // Metrics access — per-bus.
  const I2cBusMetrics& getMetrics(uint8_t busIdx = 0) const { return busMetrics[busIdx < NUM_BUSES ? busIdx : 0]; }
  void resetMetrics();

  // Transaction execution (called by I2cDevice). Picks the right mutex /
  // wire / clock stack from `device->bus`, runs the operation under the
  // bus's mutex with this call's clock and lock-wait policy, and tracks
  // per-bus metrics + health. The lambda is nullary; sensors that need to talk
  // to a specific Wire instance capture the TwoWire* externally (use
  // getWire(busIdx) at init time). This keeps the API shape identical to
  // the pre-dual-bus version — no migration cost for lambdas that stay
  // on bus 0 (their `Wire1` references already match bus 0's wire).
  template<typename Func>
  auto executeTransaction(I2cDevice* device, Func&& operation,
                         const I2cTransactionOptions& options,
                         I2cDevice::Mode mode) -> decltype(operation());

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

// Write `command`, wait `settleUs`, read `replyLen` (at most 32) bytes, on a
// bus the caller already holds (inside a transaction, or identityRead()).
// Returns 0 on success, else an endTransmission() status (4 = a short reply).
uint8_t i2cWriteThenReadHeld(TwoWire* wire, uint8_t address, const uint8_t* command,
                             size_t commandLen, uint32_t settleUs, uint8_t* reply,
                             size_t replyLen);

// Global accessor
inline I2cDeviceManager* i2c() {
  return I2cDeviceManager::getInstance();
}

// ============================================================================
// Template Implementations (must be in header)
// ============================================================================

template<typename Func>
auto I2cDeviceManager::executeTransaction(I2cDevice* device, Func&& operation,
                                          const I2cTransactionOptions& options,
                                          I2cDevice::Mode mode)
    -> decltype(operation()) {
  using ReturnType = decltype(operation());

  if (!device) {
    DEBUG_I2CF("[TX] ABORT: device=null");
    return ReturnType();
  }

  // Resolve the bus this device lives on. Bounds-clamp for safety so an
  // uninitialized device with bus==255 doesn't index out of bounds.
  uint8_t bus = (device->bus < NUM_BUSES) ? device->bus : 0;
  const uint32_t transactionClockHz = options.clockHz > 0 ? options.clockHz : 100000;
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

  // Acquire this bus's mutex using this call's explicit queueing budget.
  // Other tasks talking to devices on the OTHER bus are not blocked.
  BaseType_t acquired = xSemaphoreTake(mutex, pdMS_TO_TICKS(options.lockWaitMs));
  uint32_t waitUs = micros() - startUs;

  if (acquired != pdTRUE) {
    busMetrics[bus].mutexTimeouts++;
    DEBUG_I2CF("[TX] MUTEX_TIMEOUT 0x%02X (%s) bus=%u budget=%lums waited=%luus",
               device->address, device->name, bus,
               (unsigned long)options.lockWaitMs, (unsigned long)waitUs);
    return ReturnType();
  }
  (void)waitUs;  // referenced again below via updateMetrics

  // Push clock to this bus's stack
  if (!clockStackPush(bus, transactionClockHz)) {
    DEBUG_I2CF("[TX] CLOCK_STACK_OVERFLOW 0x%02X (%s) bus=%u",
               device->address, device->name, bus);
    xSemaphoreGive(mutex);
    return ReturnType();
  }

  // Set the bus's clock to this device's rate
  setBusClock(bus, transactionClockHz);

  // Execute operation and track duration. The lambda is nullary; if it needs
  // to operate on a specific TwoWire instance other than Wire1, the lambda
  // body should capture the TwoWire* externally (e.g., via getWire(myBus)
  // cached at sensor init).
  uint32_t txStartUs = micros();

  if constexpr (std::is_same<ReturnType, void>::value) {
    operation();
    uint32_t txDurationUs = micros() - txStartUs;

    // Restore the bus default before releasing its non-recursive mutex.
    clockStackPop(bus);
    setBusClock(bus, clockStackTopOrDefault(bus));
    xSemaphoreGive(mutex);

    updateMetrics(bus, waitUs, txDurationUs, transactionClockHz);

    if (mode != I2cDevice::Mode::NACK_TOLERANT) {
      device->recordSuccess();
    }
    return;
  } else {
    ReturnType result = operation();
    uint32_t txDurationUs = micros() - txStartUs;

    // Classify a failure while we still hold the bus, but record it after we
    // let go. The probe needs the bus; recordError must NOT run under the
    // mutex because its BUS_ERROR branch calls performBusRecovery(), which
    // takes this same mutex — classifying and recording together would
    // self-deadlock. So: probe here, stash, release, then record.
    //
    // The lambda only hands back a bool, so the underlying Wire status never
    // escapes it. Rather than rewrite every sensor driver to plumb the status
    // out, the manager asks the bus directly: a zero-length address probe
    // distinguishes the cases that actually drive different responses —
    // device silent (NACK), bus timing out (TIMEOUT), driver-level fault
    // (BUS_ERROR). This runs only on the failure path, which is rare by
    // definition, and it deliberately bypasses the metrics counters since it
    // is diagnostic traffic rather than real work.
    I2cErrorType errType = I2cErrorType::NONE;
    uint8_t wireStatus = 0;
    if constexpr (std::is_same<ReturnType, bool>::value) {
      if (!result && mode != I2cDevice::Mode::NACK_TOLERANT) {
        wireStatus = probeDeviceStatus(bus, device->address);
        errType = classifyI2cError(wireStatus);
        if (errType == I2cErrorType::NONE) {
          // The device ACKs its address, so it is present and the bus is
          // healthy — the operation failed for a reason neither can report
          // (bad register, short read, sensor-level rejection). Fall back to
          // NACK, matching the pre-classification behavior, so that repeated
          // failures still accumulate toward degraded instead of being
          // recorded as a non-event.
          errType = I2cErrorType::NACK;
          wireStatus = 2;
        }
      }
    }

    clockStackPop(bus);
    setBusClock(bus, clockStackTopOrDefault(bus));
    xSemaphoreGive(mutex);

    updateMetrics(bus, waitUs, txDurationUs, transactionClockHz);

    if (mode != I2cDevice::Mode::NACK_TOLERANT) {
      if constexpr (std::is_same<ReturnType, bool>::value) {
        if (result) {
          device->recordSuccess();
        } else {
          device->recordError(errType, wireStatus);
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
// I2cDevice Transaction Template Implementation
// ============================================================================

// Global accessor for I2cDevice::transaction to call manager
inline I2cDeviceManager* I2cDeviceManager_getInstance() {
  return I2cDeviceManager::getInstance();
}

template<typename Func>
auto I2cDevice::transaction(Func&& operation, const I2cTransactionOptions& options,
                            Mode mode) -> decltype(operation()) {
  I2cDeviceManager* mgr = I2cDeviceManager_getInstance();
  if (!mgr) return decltype(operation())();
  return mgr->executeTransaction(this, std::forward<Func>(operation), options, mode);
}

#endif // SYSTEM_I2C_MANAGER_H
