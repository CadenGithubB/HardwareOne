/**
 * I2C Device Manager Implementation
 * Unified I2C subsystem controller
 */

#include <Wire.h>
#include <driver/i2c.h>

#include "System_Debug.h"
#include "System_I2C_Manager.h"
#include "System_Logging.h"
#include "System_Settings.h"

// I2C bus configuration (defaults, overridden by settings at runtime)
#define I2C_WIRE1_DEFAULT_FREQ 100000

// Singleton instance
I2CDeviceManager* I2CDeviceManager::instance = nullptr;

// ============================================================================
// Singleton Management
// ============================================================================

I2CDeviceManager::I2CDeviceManager() 
  : deviceCount(0), busMutex(nullptr), managerMutex(nullptr),
    currentClockHz(0), defaultClockHz(100000), clockStackDepth(0),
    queueHead(0), queueTail(0), queueMutex(nullptr), pollingPaused(false) {
  memset(&busMetrics, 0, sizeof(busMetrics));
  memset(clockStack, 0, sizeof(clockStack));
  memset(deviceQueue, 0, sizeof(deviceQueue));
}

void I2CDeviceManager::initialize() {
  if (instance) return;
  
  instance = new I2CDeviceManager();
  if (!instance) {
    Serial.println("[I2C_MGR] FATAL: Failed to allocate manager");
    while(1) delay(1000);
  }
  
  // Create mutexes
  instance->busMutex = xSemaphoreCreateRecursiveMutex();
  instance->managerMutex = xSemaphoreCreateMutex();
  instance->queueMutex = xSemaphoreCreateMutex();
  
  if (!instance->busMutex || !instance->managerMutex || !instance->queueMutex) {
    Serial.println("[I2C_MGR] FATAL: Failed to create mutexes");
    while(1) delay(1000);
  }
  
  INFO_I2CF("Manager initialized successfully");
}

I2CDeviceManager* I2CDeviceManager::getInstance() {
  if (!instance) {
    initialize();
  }
  return instance;
}

// ============================================================================
// Device Registration
// ============================================================================

I2CDevice* I2CDeviceManager::registerDevice(uint8_t addr, const char* name, 
                                             uint32_t clockHz, uint32_t timeoutMs) {
  if (!managerMutex) return nullptr;
  
  if (xSemaphoreTake(managerMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    // Check if already registered
    for (int i = 0; i < deviceCount; i++) {
      if (devices[i].address == addr) {
        // Update name if upgrading from "Auto" to a real name
        if (strcmp(devices[i].name, "Auto") == 0 && strcmp(name, "Auto") != 0) {
          devices[i].name = name;
          devices[i].clockHz = clockHz;
          devices[i].adaptiveTimeoutMs = timeoutMs;
          INFO_I2CF("Updated device 0x%02X: Auto -> %s clock=%luHz timeout=%lums",
                    addr, name, (unsigned long)clockHz, (unsigned long)timeoutMs);
        }
        xSemaphoreGive(managerMutex);
        return &devices[i];
      }
    }
    
    if (deviceCount >= MAX_DEVICES) {
      ERROR_I2CF("Cannot register 0x%02X - max devices reached", addr);
      xSemaphoreGive(managerMutex);
      return nullptr;
    }
    
    I2CDevice* dev = &devices[deviceCount++];
    dev->init(addr, name, clockHz, timeoutMs);
    
    INFO_I2CF("Registered device 0x%02X (%s) clock=%luHz timeout=%lums",
              addr, name, (unsigned long)clockHz, (unsigned long)timeoutMs);
    
    xSemaphoreGive(managerMutex);
    return dev;
  }
  
  return nullptr;
}

I2CDevice* I2CDeviceManager::getDevice(uint8_t addr) {
  for (int i = 0; i < deviceCount; i++) {
    if (devices[i].address == addr) {
      return &devices[i];
    }
  }
  return nullptr;
}

I2CDevice* I2CDeviceManager::getDeviceByName(const char* name) {
  if (!name) return nullptr;
  
  for (int i = 0; i < deviceCount; i++) {
    if (devices[i].name && strcmp(devices[i].name, name) == 0) {
      return &devices[i];
    }
  }
  return nullptr;
}

// ============================================================================
// Bus Operations
// ============================================================================

void I2CDeviceManager::initBuses() {
  // Use configurable pins from settings (defaults: SDA=22, SCL=19 for original, SDA=22, SCL=20 for Feather V2)
  Wire1.begin(gSettings.i2cSdaPin, gSettings.i2cSclPin);
  Wire1.setClock(I2C_WIRE1_DEFAULT_FREQ);
  currentClockHz = I2C_WIRE1_DEFAULT_FREQ;
  // Glitch filter: ignore pulses < 7 APB cycles (~88ns at 80MHz).
  // Prevents spurious bus errors from EMI/noise that trigger i2c_hw_disable
  // -> I2C_ENTER_CRITICAL -> periph_spinlock deadlock -> interrupt WDT crash.
  i2c_filter_enable(I2C_NUM_1, 7);
  
  delay(100);
  
  INFO_I2CF("Buses initialized: Wire1 (SDA=%d, SCL=%d, %lu Hz)",
            gSettings.i2cSdaPin, gSettings.i2cSclPin, (unsigned long)defaultClockHz);
}

void I2CDeviceManager::performBusRecovery() {
  WARN_I2CF("Performing bus recovery");
  
  // Pause all polling and acquire bus mutex
  bool prevPaused = pollingPaused;
  pausePolling();  // This syncs gSensorPollingPaused
  
  bool locked = (busMutex && xSemaphoreTakeRecursive(busMutex, pdMS_TO_TICKS(2000)) == pdTRUE);
  if (!locked) {
    pollingPaused = prevPaused;
    ERROR_I2CF("Bus recovery failed - couldn't acquire mutex");
    return;
  }
  
  // 1. End Wire1 session
  Wire1.end();
  delay(10);
  
  // 2. Manual clock toggle to release stuck devices
  pinMode(gSettings.i2cSclPin, OUTPUT);
  pinMode(gSettings.i2cSdaPin, INPUT_PULLUP);
  
  for (int i = 0; i < 9; i++) {
    digitalWrite(gSettings.i2cSclPin, LOW);
    delayMicroseconds(5);
    digitalWrite(gSettings.i2cSclPin, HIGH);
    delayMicroseconds(5);
    
    if (digitalRead(gSettings.i2cSdaPin)) {
      INFO_I2CF("SDA released after %d clock pulses", i + 1);
      break;
    }
  }
  
  // 3. Generate STOP condition
  pinMode(gSettings.i2cSdaPin, OUTPUT);
  digitalWrite(gSettings.i2cSdaPin, LOW);
  delayMicroseconds(5);
  digitalWrite(gSettings.i2cSclPin, HIGH);
  delayMicroseconds(5);
  digitalWrite(gSettings.i2cSdaPin, HIGH);
  delayMicroseconds(5);
  
  // 4. Reinitialize Wire1 with configured pins
  Wire1.begin(gSettings.i2cSdaPin, gSettings.i2cSclPin);
  Wire1.setClock(defaultClockHz);
  currentClockHz = defaultClockHz;
  i2c_filter_enable(I2C_NUM_1, 7);
  delay(50);
  
  // 5. Reset all device health
  for (int i = 0; i < deviceCount; i++) {
    devices[i].health.consecutiveErrors = 0;
    devices[i].health.degraded = false;
  }
  
  if (busMutex) xSemaphoreGiveRecursive(busMutex);
  
  // Restore previous pause state
  if (!prevPaused) {
    resumePolling();  // Only resume if it wasn't paused before
  }
  
  INFO_I2CF("Bus recovery complete");
}

void I2CDeviceManager::checkBusRecoveryNeeded() {
  // Count degraded devices (called when a device becomes degraded)
  int degradedCount = 0;
  for (int i = 0; i < deviceCount; i++) {
    if (devices[i].isDegraded()) {
      degradedCount++;
    }
  }
  
  // Calculate degradation percentage
  if (deviceCount == 0) return;
  float degradationPercent = (degradedCount * 100.0f) / deviceCount;
  
  // Trigger bus recovery if >66% degraded (2/3 threshold)
  if (degradationPercent > 66.0f) {
    ERROR_I2CF("CRITICAL: %d/%d devices degraded (%.1f%%) - triggering bus recovery",
               degradedCount, deviceCount, degradationPercent);
    performBusRecovery();
  } else {
    INFO_I2CF("Bus health: %d/%d devices degraded (%.1f%%) - recovery threshold not reached",
              degradedCount, deviceCount, degradationPercent);
  }
}

// ============================================================================
// Clock Management
// ============================================================================

bool I2CDeviceManager::clockStackPush(uint32_t hz) {
  if (clockStackDepth >= CLOCK_STACK_MAX) {
    broadcastOutput("[I2C_MGR] CRITICAL: clock stack overflow - operation aborted");
    return false;
  }
  clockStack[clockStackDepth++] = hz;
  return true;
}

void I2CDeviceManager::clockStackPop() {
  if (clockStackDepth > 0) {
    clockStackDepth--;
  }
}

uint32_t I2CDeviceManager::clockStackTopOrDefault() {
  return (clockStackDepth > 0) ? clockStack[clockStackDepth - 1] : defaultClockHz;
}

void I2CDeviceManager::setWire1Clock(uint32_t hz) {
  if (currentClockHz != hz) {
    Wire1.setClock(hz);
    currentClockHz = hz;
    delayMicroseconds(50);
  }
}

// ============================================================================
// Metrics Tracking
// ============================================================================

void I2CDeviceManager::updateMetrics(uint32_t waitUs, uint32_t txDurationUs, uint32_t clockHz) {
  // Mutex wait metrics
  if (waitUs > 0) busMetrics.mutexContentions++;
  if (waitUs > busMetrics.maxWaitTimeUs) busMetrics.maxWaitTimeUs = waitUs;
  busMetrics.avgWaitTimeUs = (busMetrics.avgWaitTimeUs * 7 + waitUs) / 8;
  
  // Transaction duration metrics
  if (txDurationUs > busMetrics.maxTransactionDurationUs) {
    busMetrics.maxTransactionDurationUs = txDurationUs;
  }
  busMetrics.avgTransactionDurationUs = 
    (busMetrics.avgTransactionDurationUs * 7 + txDurationUs) / 8;
  
  // Estimate bytes transferred
  uint32_t estimatedBytes = (txDurationUs * clockHz) / (8 * 1000000);
  if (estimatedBytes > 0) {
    busMetrics.totalBytesTransferred += estimatedBytes;
  }
  
  updateHistogram(txDurationUs);
}

void I2CDeviceManager::updateHistogram(uint32_t txDurationUs) {
  if (txDurationUs < 100) {
    busMetrics.txDuration_0_100us++;
  } else if (txDurationUs < 500) {
    busMetrics.txDuration_100_500us++;
  } else if (txDurationUs < 2000) {
    busMetrics.txDuration_500_2000us++;
  } else {
    busMetrics.txDuration_2000plus_us++;
  }
}


// ============================================================================
// I2C Device Lifecycle Management
// ============================================================================

bool I2CDeviceManager::enqueueDeviceStart(I2CDeviceType sensor) {
  if (!queueMutex) return false;
  
  if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
    int nextTail = (queueTail + 1) % 8;
    if (nextTail == queueHead) {
      xSemaphoreGive(queueMutex);
      return false;  // Queue full
    }
    
    deviceQueue[queueTail].device = sensor;
    deviceQueue[queueTail].queuedAt = millis();
    queueTail = nextTail;
    
    xSemaphoreGive(queueMutex);
    return true;
  }
  
  return false;
}

bool I2CDeviceManager::dequeueDeviceStart(I2CDeviceStartRequest* req) {
  if (!queueMutex || !req) return false;
  
  if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
    if (queueHead == queueTail) {
      xSemaphoreGive(queueMutex);
      return false;  // Queue empty
    }
    
    *req = deviceQueue[queueHead];
    queueHead = (queueHead + 1) % 8;
    
    xSemaphoreGive(queueMutex);
    return true;
  }
  
  return false;
}

bool I2CDeviceManager::isInQueue(I2CDeviceType sensor) {
  if (!queueMutex) return false;
  
  bool found = false;
  if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
    for (int i = queueHead; i != queueTail; i = (i + 1) % 8) {
      if (deviceQueue[i].device == sensor) {
        found = true;
        break;
      }
    }
    xSemaphoreGive(queueMutex);
  }
  
  return found;
}

int I2CDeviceManager::getQueuePosition(I2CDeviceType sensor) {
  if (!queueMutex) return -1;
  
  int pos = -1;
  if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
    int idx = 1;
    for (int i = queueHead; i != queueTail; i = (i + 1) % 8) {
      if (deviceQueue[i].device == sensor) {
        pos = idx;
        break;
      }
      idx++;
    }
    xSemaphoreGive(queueMutex);
  }
  
  return pos;
}

int I2CDeviceManager::getQueueDepth() {
  return (queueTail - queueHead + 8) % 8;
}

void I2CDeviceManager::pausePolling() {
  pollingPaused = true;
  extern volatile bool gSensorPollingPaused;
  gSensorPollingPaused = true;
  INFO_I2CF("Sensor polling paused");
}

void I2CDeviceManager::resumePolling() {
  pollingPaused = false;
  extern volatile bool gSensorPollingPaused;
  gSensorPollingPaused = false;
  INFO_I2CF("Sensor polling resumed");
}

// ============================================================================
// Device Discovery
// ============================================================================

void I2CDeviceManager::discoverDevices() {
  INFO_I2CF("Starting device discovery");
  
  // Discovery implementation will scan registered device addresses
  // For now, devices are pre-registered from database in initI2CManager()
  // Future: Add runtime scanning capability
  
  INFO_I2CF("Discovery complete - %d devices registered", deviceCount);
}

// ============================================================================
// Metrics Reset
// ============================================================================


// ============================================================================
// I2CDevice Implementation (merged from System_I2C_Device.cpp)
// ============================================================================

I2CErrorType classifyI2CError(uint8_t espError) {
  switch (espError) {
    case 0x00:
      return I2CErrorType::NONE;
    case 0x02:  // Arduino Wire endTransmission NACK
      return I2CErrorType::NACK;
    case 0x03:  // Arduino Wire endTransmission timeout
    case 0x107: // ESP_ERR_TIMEOUT
      return I2CErrorType::TIMEOUT;
    case 0x01:  // ESP_FAIL (generic)
    case 0x103: // ESP_ERR_INVALID_STATE (arbitration lost / bus stuck)
      return I2CErrorType::BUS_ERROR;
    case 0x04:  // Buffer overflow
      return I2CErrorType::BUFFER_OVERFLOW;
    default:
      return I2CErrorType::BUS_ERROR;
  }
}

I2CDevice::I2CDevice() 
  : address(0), name(nullptr), clockHz(100000), 
    baseTimeoutMs(200), adaptiveTimeoutMs(200) {
  memset(&health, 0, sizeof(health));
}

void I2CDevice::init(uint8_t addr, const char* deviceName, uint32_t clock, uint32_t timeout) {
  address = addr;
  name = deviceName;
  clockHz = clock > 0 ? clock : 100000;
  baseTimeoutMs = timeout > 0 ? timeout : 200;
  adaptiveTimeoutMs = baseTimeoutMs;
  
  // Initialize health
  health.consecutiveErrors = 0;
  health.totalErrors = 0;
  health.degraded = false;
  health.lastErrorTime = 0;
  health.lastSuccessTime = millis();
  health.registrationTime = millis();
  health.nackCount = 0;
  health.timeoutCount = 0;
  health.busErrorCount = 0;
  health.lastErrorType = I2CErrorType::NONE;
}

void I2CDevice::recordSuccess() {
  health.consecutiveErrors = 0;
  health.lastSuccessTime = millis();
  
  if (health.degraded) {
    health.degraded = false;
    INFO_I2CF("Device 0x%02X (%s) recovered", address, name);
    logI2CRecovery(address, name, health.totalErrors);
  }
}

void I2CDevice::recordError(I2CErrorType errorType, uint8_t espError) {
  health.consecutiveErrors++;
  health.totalErrors++;
  health.lastErrorTime = millis();
  health.lastErrorType = errorType;
  
  // Type-specific error tracking and recovery
  switch (errorType) {
    case I2CErrorType::NACK:
      health.nackCount++;
      WARN_I2CF("Device 0x%02X (%s) NACK (count=%d, consecutive=%d)",
                address, name, health.nackCount, health.consecutiveErrors);
      
      if (health.consecutiveErrors >= 3) {
        health.degraded = true;
        ERROR_I2CF("Device 0x%02X (%s) marked DEGRADED after %d NACKs",
                   address, name, health.nackCount);
        logI2CError(address, name, health.consecutiveErrors, health.totalErrors, true);
        
        // Check if bus recovery is needed (decentralized check)
        I2CDeviceManager::getInstance()->checkBusRecoveryNeeded();
      }
      break;
      
    case I2CErrorType::TIMEOUT:
      health.timeoutCount++;
      WARN_I2CF("Device 0x%02X (%s) TIMEOUT (count=%d, consecutive=%d)",
                address, name, health.timeoutCount, health.consecutiveErrors);
      
      // Adaptive timeout increase
      if (adaptiveTimeoutMs < 5000) {
        uint32_t oldTimeout = adaptiveTimeoutMs;
        adaptiveTimeoutMs = min(adaptiveTimeoutMs * 2, (uint32_t)5000);
        INFO_I2CF("Device 0x%02X (%s) timeout increased: %lu -> %lu ms",
                  address, name, (unsigned long)oldTimeout, 
                  (unsigned long)adaptiveTimeoutMs);
      }
      
      if (health.consecutiveErrors >= 3) {
        health.degraded = true;
        ERROR_I2CF("Device 0x%02X (%s) marked DEGRADED after %d timeouts",
                   address, name, health.timeoutCount);
        logI2CError(address, name, health.consecutiveErrors, health.totalErrors, true);
        
        // Check if bus recovery is needed (decentralized check)
        I2CDeviceManager::getInstance()->checkBusRecoveryNeeded();
      }
      break;
      
    case I2CErrorType::BUS_ERROR:
      health.busErrorCount++;
      ERROR_I2CF("Device 0x%02X (%s) BUS_ERROR (count=%d, espErr=0x%02X)",
                 address, name, health.busErrorCount, espError);
      
      logI2CError(address, name, health.consecutiveErrors, health.totalErrors, false);
      
      // Trigger immediate bus recovery via manager
      I2CDeviceManager::getInstance()->performBusRecovery();
      break;
      
    case I2CErrorType::BUFFER_OVERFLOW:
      ERROR_I2CF("Device 0x%02X (%s) BUFFER_OVERFLOW (espErr=0x%02X)",
                 address, name, espError);
      logI2CError(address, name, 0, health.totalErrors, false);
      break;
      
    case I2CErrorType::NONE:
      break;
  }
}

bool I2CDevice::isDegraded() const {
  if (!health.degraded) return false;
  
  // Auto-recovery after timeout
  const uint32_t RECOVERY_TIMEOUT_MS = 30000;
  if (millis() - health.lastErrorTime > RECOVERY_TIMEOUT_MS) {
    return false;  // Allow retry
  }
  
  return true;
}

void I2CDevice::attemptRecovery() {
  if (!health.degraded) return;
  
  INFO_I2CF("Device 0x%02X (%s) attempting recovery", address, name);
  health.degraded = false;
  health.consecutiveErrors = 0;
}

void I2CDevice::resetGracePeriod() {
  health.registrationTime = millis();
  health.consecutiveErrors = 0;
  health.degraded = false;
  INFO_I2CF("Device 0x%02X (%s) grace period reset", address, name);
}
