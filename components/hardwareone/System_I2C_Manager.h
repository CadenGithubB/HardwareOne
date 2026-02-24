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

#include "System_BuildConfig.h"
#include "System_Debug.h"

// Forward declarations
void broadcastOutput(const char* s);
void broadcastOutput(const String& s);

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
  void init(uint8_t addr, const char* deviceName, uint32_t clock, uint32_t timeout);
  
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
  I2C_DEVICE_GAMEPAD = 3,
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
  
private:
  int deviceCount;
  
  // Bus state
  I2CBusMetrics busMetrics;
  SemaphoreHandle_t busMutex;
  SemaphoreHandle_t managerMutex;
  uint32_t currentClockHz;
  uint32_t defaultClockHz;
  
  // Clock stack for nested transactions
  static const int CLOCK_STACK_MAX = 8;
  uint32_t clockStack[CLOCK_STACK_MAX];
  int clockStackDepth;
  
  // I2C device lifecycle
  I2CDeviceStartRequest deviceQueue[8];
  int queueHead;
  int queueTail;
  SemaphoreHandle_t queueMutex;
  volatile bool pollingPaused;
  
  // Singleton instance
  static I2CDeviceManager* instance;
  
  // Private constructor (singleton)
  I2CDeviceManager();
  
  // Internal helpers
  bool clockStackPush(uint32_t hz);
  void clockStackPop();
  uint32_t clockStackTopOrDefault();
  void setWire1Clock(uint32_t hz);
  void updateMetrics(uint32_t waitUs, uint32_t txDurationUs, uint32_t clockHz);
  void updateHistogram(uint32_t txDurationUs);
  
public:
  // Singleton access
  static I2CDeviceManager* getInstance();
  static void initialize();
  
  // Device registration
  I2CDevice* registerDevice(uint8_t addr, const char* name, uint32_t clockHz = 100000, uint32_t timeoutMs = 200);
  I2CDevice* getDevice(uint8_t addr);
  I2CDevice* getDeviceByName(const char* name);
  int getDeviceCount() const { return deviceCount; }
  
  // Bus operations
  void initBuses();
  void performBusRecovery();
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
  
  // Metrics access
  const I2CBusMetrics& getMetrics() const { return busMetrics; }
  void resetMetrics();
  
  // Transaction execution (called by I2CDevice)
  template<typename Func>
  auto executeTransaction(I2CDevice* device, Func&& operation, 
                         I2CDevice::Mode mode) -> decltype(operation());
  
  // Mutex access for external use (legacy compatibility during migration)
  SemaphoreHandle_t getBusMutex() { return busMutex; }
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
                                          I2CDevice::Mode mode) -> decltype(operation()) {
  using ReturnType = decltype(operation());
  
  if (!device || !busMutex) {
    DEBUG_I2CF("[TX] ABORT: device=%p busMutex=%p", device, busMutex);
    return ReturnType();
  }
  
  // Check if device is degraded (allow recovery after timeout)
  if (device->isDegraded()) {
    DEBUG_I2CF("[TX] SKIP 0x%02X (%s): device degraded", device->address, device->name);
    return ReturnType();
  }
  
  // DEBUG_I2CF("[TX] START 0x%02X (%s) clock=%luHz timeout=%lums",
  //            device->address, device->name, 
  //            (unsigned long)device->clockHz, (unsigned long)device->adaptiveTimeoutMs);
  
  // Track transaction start
  uint32_t startUs = micros();
  busMetrics.totalTransactions++;
  
  // Acquire bus mutex with device's adaptive timeout
  BaseType_t acquired = xSemaphoreTakeRecursive(busMutex, 
                                                 pdMS_TO_TICKS(device->adaptiveTimeoutMs));
  uint32_t waitUs = micros() - startUs;
  
  if (acquired != pdTRUE) {
    busMetrics.mutexTimeouts++;
    DEBUG_I2CF("[TX] MUTEX_TIMEOUT 0x%02X (%s) waited=%luus",
               device->address, device->name, (unsigned long)waitUs);
    return ReturnType();
  }
  
  // Verbose mutex contention logging disabled - too noisy
  // if (waitUs > 1000) {
  //   DEBUG_I2CF("[TX] MUTEX_CONTENTION 0x%02X (%s) waited=%luus",
  //              device->address, device->name, (unsigned long)waitUs);
  // }
  (void)waitUs;  // Suppress unused warning
  
  // Push clock to stack
  if (!clockStackPush(device->clockHz)) {
    DEBUG_I2CF("[TX] CLOCK_STACK_OVERFLOW 0x%02X (%s)", device->address, device->name);
    xSemaphoreGiveRecursive(busMutex);
    return ReturnType();
  }
  
  // Set device clock
  uint32_t prevClock = currentClockHz;
  setWire1Clock(device->clockHz);
  // Verbose clock change logging disabled - issue resolved
  // if (prevClock != device->clockHz) {
  //   DEBUG_I2CF("[TX] CLOCK_CHANGE 0x%02X: %luHz -> %luHz",
  //              device->address, (unsigned long)prevClock, (unsigned long)device->clockHz);
  // }
  (void)prevClock;  // Suppress unused warning
  
  // Execute operation and track duration
  uint32_t txStartUs = micros();
  
  // Handle void vs non-void return types
  if constexpr (std::is_same<ReturnType, void>::value) {
    operation();
    uint32_t txDurationUs = micros() - txStartUs;
    
    // Restore clock
    clockStackPop();
    uint32_t restoreClock = clockStackTopOrDefault();
    // Verbose clock restore logging disabled - issue resolved
    // if (restoreClock != device->clockHz) {
    //   DEBUG_I2CF("[TX] CLOCK_RESTORE 0x%02X: %luHz -> %luHz",
    //              device->address, (unsigned long)device->clockHz, (unsigned long)restoreClock);
    // }
    setWire1Clock(restoreClock);
    
    // Release mutex
    xSemaphoreGiveRecursive(busMutex);
    
    // Update metrics
    updateMetrics(waitUs, txDurationUs, device->clockHz);
    
    // DEBUG_I2CF("[TX] DONE 0x%02X (%s) duration=%luus result=void",
    //            device->address, device->name, (unsigned long)txDurationUs);
    
    // Health tracking (mode-dependent)
    if (mode != I2CDevice::Mode::NACK_TOLERANT) {
      device->recordSuccess();
    }
    
    return;
  } else {
    ReturnType result = operation();
    uint32_t txDurationUs = micros() - txStartUs;
    
    // Restore clock
    clockStackPop();
    uint32_t restoreClock = clockStackTopOrDefault();
    // Verbose clock restore logging disabled - issue resolved
    // if (restoreClock != device->clockHz) {
    //   DEBUG_I2CF("[TX] CLOCK_RESTORE 0x%02X: %luHz -> %luHz",
    //              device->address, (unsigned long)device->clockHz, (unsigned long)restoreClock);
    // }
    setWire1Clock(restoreClock);
    
    // Release mutex
    xSemaphoreGiveRecursive(busMutex);
    
    // Update metrics
    updateMetrics(waitUs, txDurationUs, device->clockHz);
    
    // Health tracking (mode-dependent)
    // Verbose transaction complete logging disabled - issue resolved
    if (mode != I2CDevice::Mode::NACK_TOLERANT) {
      if constexpr (std::is_same<ReturnType, bool>::value) {
        // DEBUG_I2CF("[TX] DONE 0x%02X (%s) duration=%luus result=%s",
        //            device->address, device->name, (unsigned long)txDurationUs,
        //            result ? "OK" : "FAIL");
        if (result) {
          device->recordSuccess();
        } else {
          device->recordError(I2CErrorType::NACK, 0x02);
        }
      } else {
        // DEBUG_I2CF("[TX] DONE 0x%02X (%s) duration=%luus",
        //            device->address, device->name, (unsigned long)txDurationUs);
        device->recordSuccess();
      }
    } else {
      // DEBUG_I2CF("[TX] DONE 0x%02X (%s) duration=%luus (NACK_TOLERANT)",
      //            device->address, device->name, (unsigned long)txDurationUs);
    }
    (void)txDurationUs;  // Suppress unused warning
    
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
