/**
 * Sensor System - I2C Sensor Tasks and Commands
 * i2c_system.cpp
 * I2C sensor task implementations and management
 * Extracted from HardwareOne.ino
 */

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Wire.h>

#include "i2csensor-rda5807.h"
#include "System_BuildConfig.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_FirstTimeSetup.h"
#include "System_I2C.h"
#include "System_Logging.h"
#include "System_Mutex.h"
#include "System_MemUtil.h"
#include "System_Notifications.h"
#include "System_SensorRegistry.h"
#include "System_SensorStubs.h"

#if ENABLE_ESPNOW
#include "System_ESPNow.h"
#include "System_ESPNow_Sensors.h"
#endif
#include "System_Settings.h"
#include "System_TaskUtils.h"
#include "System_Utils.h"

#if ENABLE_APDS_SENSOR
#include "Adafruit_APDS9960.h"
#include "i2csensor-apds9960.h"
#endif
#if ENABLE_GAMEPAD_SENSOR
#include "Adafruit_seesaw.h"
#include "i2csensor-seesaw.h"
#endif
#if ENABLE_GPS_SENSOR
#include <Adafruit_GPS.h>
#include "i2csensor-pa1010d.h"
#endif
#if ENABLE_IMU_SENSOR
#include <Adafruit_BNO055.h>
#include <Adafruit_Sensor.h>
#include "i2csensor-bno055.h"
#endif
#if ENABLE_THERMAL_SENSOR
#include <Adafruit_MLX90640.h>
#include "i2csensor-mlx90640.h"
#endif
#if ENABLE_TOF_SENSOR
#include "i2csensor-vl53l4cx.h"
#include "vl53l4cx_class.h"
#endif
#if ENABLE_RTC_SENSOR
#include "i2csensor-ds3231.h"
#endif
#if ENABLE_PRESENCE_SENSOR
#include "i2csensor-sths34pf80.h"
#endif

// ============================================================================
// Unified I2C Manager Initialization
// ============================================================================

// Helper function to check if a sensor is compiled in
// Uses module name matching against compile-time flags
static bool isSensorCompiled(const I2CSensorEntry& sensor) {
  if (sensor.moduleName == nullptr) {
    // Infrastructure devices (SSD1306, PCA9685) - check by address
    if (sensor.address == 0x3C || sensor.address == 0x3D) {
      // SSD1306 OLED
      #if !ENABLE_OLED_DISPLAY
        return false;
      #endif
    }
    if (sensor.address == 0x40) {
      // PCA9685 Servo
      #if !ENABLE_SERVO
        return false;
      #endif
    }
    return true;
  }
  
  #if !ENABLE_THERMAL_SENSOR
    if (strcmp(sensor.moduleName, "thermal") == 0) return false;
  #endif
  #if !ENABLE_TOF_SENSOR
    if (strcmp(sensor.moduleName, "tof") == 0) return false;
  #endif
  #if !ENABLE_IMU_SENSOR
    if (strcmp(sensor.moduleName, "imu") == 0) return false;
  #endif
  #if !ENABLE_GAMEPAD_SENSOR
    if (strcmp(sensor.moduleName, "gamepad") == 0) return false;
  #endif
  #if !ENABLE_APDS_SENSOR
    if (strcmp(sensor.moduleName, "apds") == 0) return false;
  #endif
  #if !ENABLE_GPS_SENSOR
    if (strcmp(sensor.moduleName, "gps") == 0) return false;
  #endif
  #if !ENABLE_FM_RADIO
    if (strcmp(sensor.moduleName, "fmradio") == 0) return false;
  #endif
  #if !ENABLE_RTC_SENSOR
    if (strcmp(sensor.moduleName, "rtc") == 0) return false;
  #endif
  #if !ENABLE_PRESENCE_SENSOR
    if (strcmp(sensor.moduleName, "presence") == 0) return false;
  #endif
  #if !ENABLE_SERVO
    if (strcmp(sensor.moduleName, "servo") == 0) return false;
  #endif
  
  return true;
}

// Initialize the unified I2C manager singleton
void initI2CManager() {
  I2CDeviceManager::initialize();
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  
  // Print device registry capacity
  INFO_I2CF("[I2C_REGISTRY] Device manager initialized with capacity for %d devices", I2CDeviceManager::MAX_DEVICES);
  
  // Bridge legacy i2cMutex to manager's busMutex for backward compatibility
  extern SemaphoreHandle_t i2cMutex;
  if (mgr && mgr->getBusMutex()) {
    i2cMutex = mgr->getBusMutex();
    INFO_I2CF("Bridged legacy i2cMutex to manager busMutex");
  }
  
  // Pre-register only compiled-in devices from database with their timing parameters
  int compiledCount = 0;
  for (size_t i = 0; i < i2cSensorsCount; i++) {
    const I2CSensorEntry& sensor = i2cSensors[i];
    if (isSensorCompiled(sensor)) {
      uint32_t clock = sensor.i2cClockHz > 0 ? sensor.i2cClockHz : 100000;
      uint32_t timeout = sensor.i2cTimeoutMs > 0 ? sensor.i2cTimeoutMs : 200;
      I2CDevice* dev = mgr->registerDevice(sensor.address, sensor.name, clock, timeout);
      if (dev) {
        compiledCount++;
        INFO_I2CF("Pre-registered compiled device: 0x%02X (%s)", sensor.address, sensor.name);
      } else {
        ERROR_I2CF("Failed to pre-register compiled device: 0x%02X (%s)", sensor.address, sensor.name);
      }
    }
  }
  
  INFO_I2CF("Pre-registered %d compiled devices from database", compiledCount);
  
  // Print registry summary
  INFO_I2CF("[I2C_REGISTRY] Registration summary: %d/%d slots used (%d available)", 
                compiledCount, I2CDeviceManager::MAX_DEVICES, 
                I2CDeviceManager::MAX_DEVICES - compiledCount);
}

// Global I2C bus enabled flag (mirrors gSettings.i2cBusEnabled)
bool gI2CBusEnabled = true;

// gSensorPollingPaused is defined in HardwareOne.cpp
extern volatile bool gSensorPollingPaused;

// Queue processor task handle
TaskHandle_t queueProcessorTask = nullptr;

// External dependencies from .ino
extern Settings gSettings;
extern bool gCLIValidateOnly;
extern TaskHandle_t imuTaskHandle;
extern unsigned long imuLastStopTime;
// Clock management is now unified through I2CDeviceManager
// Legacy i2cSetWire1Clock() removed - all sensors use i2cDeviceTransaction wrapper

// Sensor status system dependencies
extern volatile unsigned long gSensorStatusSeq;
extern const char* gLastStatusCause;
extern void sensorStatusBump();
extern bool apdsColorEnabled;
extern bool apdsProximityEnabled;
extern bool apdsGestureEnabled;
#if ENABLE_SERVO
extern bool pwmDriverConnected;
#endif

// BROADCAST_PRINTF now defined in debug_system.h with performance optimizations

// Sensor connection status (defined in sensor files)
extern bool gamepadConnected;
extern bool imuConnected;
extern bool apdsConnected;
extern bool tofConnected;
extern bool thermalConnected;

// ============================================================================
// I2C Clock Management (Wire1)
// ============================================================================

// Device Registry Global Variables (definitions)
// Array of devices detected during I2C scan
ConnectedDevice connectedDevices[MAX_CONNECTED_DEVICES];
int connectedDeviceCount = 0;
int discoveryCount = 0;

// I2C sensor database (moved from .ino to fix linker issues)
// I2CSensorEntry struct is defined in i2c_system.h

// I2C Sensor Database - Sensors actually used/detected in this system
// Entry format: { address, name, description, manufacturer, multiAddress, altAddress,
//                libraryHeapBytes, libraryName, headerGuard, moduleName, i2cClockHz, i2cTimeoutMs }
const I2CSensorEntry i2cSensors[] = {
  // Sensors with CLI Modules
  // { addr, name, desc, mfr, multiAddr, altAddr, heapBytes, library, headerGuard, module, clockHz, timeoutMs }
  { 0x28, "BNO055", "9-DOF IMU", "Adafruit", true, 0x29, 1500, "Adafruit_BNO055", "_ADAFRUIT_BNO055_H_", "imu", 100000, 300 },
  { 0x39, "APDS9960", "RGB, Gesture & Proximity", "Adafruit", false, 0x00, 500, "Adafruit_APDS9960", "_ADAFRUIT_APDS9960_H_", "apds", 100000, 200 },
  { 0x29, "VL53L4CX", "ToF Distance (up to 6m)", "Adafruit", false, 0x00, 1000, "VL53L4CX", "_VL53L4CX_CLASS_H_", "tof", 400000, 250 },
  { 0x50, "Seesaw", "Mini I2C Gamepad", "Adafruit", false, 0x00, 800, "Adafruit_seesaw", "_ADAFRUIT_SEESAW_H_", "gamepad", 400000, 200 },
  { 0x33, "MLX90640", "32x24 Thermal Camera", "Adafruit", false, 0x00, 2000, "Adafruit_MLX90640", "_ADAFRUIT_MLX90640_H_", "thermal", 100000, 500 },
  { 0x10, "PA1010D", "Mini GPS Module", "Adafruit", false, 0x00, 500, "Adafruit_GPS", "_ADAFRUIT_GPS_H", "gps", 100000, 200 },
  { 0x11, "RDA5807", "FM Radio Receiver", "ScoutMakes", false, 0x00, 500, "RDA5807", NULL, "fmradio", 100000, 200 },
  { 0x68, "DS3231", "Precision RTC", "Adafruit", false, 0x00, 100, NULL, NULL, "rtc", 100000, 100 },
  { 0x5A, "STHS34PF80", "IR Presence/Motion", "ST", false, 0x00, 200, NULL, NULL, "presence", 100000, 200 },
  
  // Detected Infrastructure (no CLI modules)
  { 0x3D, "SSD1306", "OLED 128x64 Display", "Adafruit", true, 0x3C, 0, NULL, NULL, NULL, 400000, 50 },
  { 0x40, "PCA9685", "16-Channel 12-bit PWM/Servo Driver", "Adafruit", true, 0x70, 0, "Adafruit_PWMServoDriver", "_ADAFRUIT_PWMSERVODRIVER_H_", NULL, 100000, 200 },
};

// Export array size for use in .ino file
const size_t i2cSensorsCount = sizeof(i2cSensors) / sizeof(i2cSensors[0]);

// Helper: Check if a sensor is connected by name (used by help system)
// Uses sensor database to map module name to device name dynamically
bool isSensorConnected(const char* moduleName) {
  if (!moduleName) return false;
  
  // Find sensor in database by module name
  const char* deviceName = nullptr;
  for (size_t i = 0; i < i2cSensorsCount; i++) {
    if (i2cSensors[i].moduleName && strcmp(i2cSensors[i].moduleName, moduleName) == 0) {
      deviceName = i2cSensors[i].name;
      break;
    }
  }
  
  if (!deviceName) return false;  // Module name not in sensor database
  
  // Check if device is in the connected device registry
  for (int i = 0; i < connectedDeviceCount; i++) {
    if (strstr(connectedDevices[i].name, deviceName) != nullptr) {
      return true;  // Found in registry
    }
  }
  
  return false;  // Not found in registry
}

// Initialize queue mutex (called from setup())
// Now managed by I2CDeviceManager - this is a no-op
void initSensorQueue() {
  // Queue mutex is created by I2CDeviceManager::initialize()
  INFO_I2CF("initSensorQueue() - queue managed by I2CDeviceManager");
}

// =========================================================================
// Queued Sensor Start Commands (moved from .ino)
// =========================================================================

extern bool thermalEnabled;
extern bool tofEnabled;
extern bool imuEnabled;

static const char* cmd_sensorstart_queued(I2CDeviceType sensor, const char* displayName, const bool& enabledFlag, const char* eventTag) {
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  if (enabledFlag) {
    snprintf(getDebugBuffer(), 1024, "%s sensor already running", displayName);
    return getDebugBuffer();
  }
  if (isInQueue(sensor)) {
    int pos = getQueuePosition(sensor);
    snprintf(getDebugBuffer(), 1024, "%s sensor already queued (position %d)", displayName, pos);
    return getDebugBuffer();
  }

  if (enqueueDeviceStart(sensor)) {
    sensorStatusBumpWith(eventTag);
    int pos = getQueuePosition(sensor);
    snprintf(getDebugBuffer(), 1024, "%s sensor queued for start (position %d, queue depth: %d)",
             displayName, pos, getQueueDepth());
    return getDebugBuffer();
  } else {
    snprintf(getDebugBuffer(), 1024, "Failed to queue %s sensor (queue full)", displayName);
    return getDebugBuffer();
  }
}

const char* cmd_thermalstart_queued(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return cmd_sensorstart_queued(I2C_DEVICE_THERMAL, "Thermal", thermalEnabled, "openthermal@enqueue");
}

const char* cmd_tofstart_queued(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return cmd_sensorstart_queued(I2C_DEVICE_TOF, "ToF", tofEnabled, "opentof@enqueue");
}

const char* cmd_imustart_queued(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return cmd_sensorstart_queued(I2C_DEVICE_IMU, "IMU", imuEnabled, "openimu@enqueue");
}

const char* cmd_apdsstart_queued(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return cmd_sensorstart_queued(I2C_DEVICE_APDS, "APDS", apdsColorEnabled || apdsProximityEnabled || apdsGestureEnabled, "openapds@enqueue");
}

extern bool gamepadEnabled;
const char* cmd_gamepadstart_queued(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return cmd_sensorstart_queued(I2C_DEVICE_GAMEPAD, "Gamepad", gamepadEnabled, "opengamepad@enqueue");
}

// ========== End Sensor Startup Queue System ==========

// ========== I2C Bus Initialization ==========

// Track if we've already logged the "bus disabled" message (to avoid spam)
static bool gI2CBusDisabledLogged = false;

void initI2CBuses() {
  // During first-time setup, force I2C enabled so OLED wizard can run
  // User can disable I2C in wizard, which takes effect after reboot
  bool forceForSetup = isFirstTimeSetup();
  
  if (forceForSetup) {
    gI2CBusEnabled = true;
    INFO_I2CF("[I2C] Force-enabling for first-time setup wizard");
  } else {
    // Copy setting to global flag
    gI2CBusEnabled = gSettings.i2cBusEnabled;
  }
  
  // Early exit if I2C bus is disabled via settings (and not first-time setup)
  if (!gI2CBusEnabled) {
    if (!gI2CBusDisabledLogged) {
      INFO_I2CF("[I2C] Bus disabled via settings - skipping initialization");
      INFO_I2CF("[I2C] OLED display and I2C sensors will not be available");
      gI2CBusDisabledLogged = true;
    }
    return;
  }

  // Initialize unified I2C manager
  initI2CManager();
  
  // Delegate bus initialization to manager
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (mgr) {
    mgr->initBuses();
  }
}

// ========== End I2C Bus Initialization ==========

void i2cResetGracePeriod(uint8_t address) {
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (!mgr) return;
  I2CDevice* dev = mgr->getDevice(address);
  if (dev) dev->resetGracePeriod();
}

const char* cmd_i2chealth(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (!mgr) return "Error: I2C manager not initialized";
  
  char* p = getDebugBuffer();
  int remaining = 1024;
  
  int deviceCount = mgr->getDeviceCount();
  int n = snprintf(p, remaining, "I2C Device Health (%d devices):\n", deviceCount);
  p += n; remaining -= n;
  
  if (deviceCount == 0) {
    snprintf(p, remaining, "  No devices registered\n");
    return getDebugBuffer();
  }
  
  for (int i = 0; i < deviceCount && remaining > 100; i++) {
    I2CDevice* dev = &mgr->devices[i];
    if (!dev->isInitialized()) continue;
    
    const I2CDevice::Health& h = dev->getHealth();
    
    // Device header line
    n = snprintf(p, remaining, 
      "  0x%02X %-10s: err=%d/%d %s\n",
      dev->address, dev->name ? dev->name : "?", 
      h.consecutiveErrors, h.totalErrors,
      dev->isDegraded() ? "[DEGRADED]" : "OK");
    p += n; remaining -= n;
    
    // Error classification breakdown
    if (h.totalErrors > 0 && remaining > 100) {
      n = snprintf(p, remaining,
        "       NACK=%d TIMEOUT=%d BUS_ERR=%d | timeout=%lums\n",
        h.nackCount, h.timeoutCount, h.busErrorCount,
        (unsigned long)dev->getAdaptiveTimeout());
      p += n; remaining -= n;
    }
  }
  
  return getDebugBuffer();
}

const char* cmd_i2cmetrics(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (!mgr) return "Error: I2C manager not initialized";
  
  const I2CBusMetrics& metrics = mgr->getMetrics();
  
  char* p = getDebugBuffer();
  int remaining = 1024;
  
  // Calculate uptime since last reset
  unsigned long uptimeMs = millis() - metrics.lastResetMs;
  unsigned long uptimeSec = uptimeMs / 1000;
  
  // Calculate transactions per second
  float tps = (uptimeSec > 0) ? (float)metrics.totalTransactions / uptimeSec : 0.0f;
  
  // Calculate contention rate
  float contentionRate = (metrics.totalTransactions > 0) 
    ? (float)metrics.mutexContentions * 100.0f / metrics.totalTransactions 
    : 0.0f;
  
  // Calculate timeout rate
  float timeoutRate = (metrics.totalTransactions > 0)
    ? (float)metrics.mutexTimeouts * 100.0f / metrics.totalTransactions
    : 0.0f;
  
  // Calculate bandwidth
  float bytesPerSec = (uptimeSec > 0) ? (float)metrics.totalBytesTransferred / uptimeSec : 0.0f;
  
  int n = snprintf(p, remaining, 
    "I2C Bus Metrics (uptime: %lu sec):\n"
    "  Total Transactions:  %lu (%.1f/sec)\n"
    "  Mutex Timeouts:      %lu (%.2f%%)\n"
    "  Bus Contentions:     %lu (%.2f%%)\n"
    "  Avg Wait Time:       %lu us\n"
    "  Peak Wait Time:      %lu us\n"
    "\n"
    "Bandwidth Metrics:\n"
    "  Total Bytes:         %lu (%.1f bytes/sec)\n"
    "  Avg TX Duration:     %lu us\n"
    "  Peak TX Duration:    %lu us\n"
    "\n"
    "Transaction Duration Distribution:\n"
    "  0-100us (fast):      %lu (%.1f%%)\n"
    "  100-500us (normal):  %lu (%.1f%%)\n"
    "  500-2000us (slow):   %lu (%.1f%%)\n"
    "  2000+us (very slow): %lu (%.1f%%)\n",
    (unsigned long)uptimeSec,
    (unsigned long)metrics.totalTransactions, tps,
    (unsigned long)metrics.mutexTimeouts, timeoutRate,
    (unsigned long)metrics.mutexContentions, contentionRate,
    (unsigned long)metrics.avgWaitTimeUs,
    (unsigned long)metrics.maxWaitTimeUs,
    (unsigned long)metrics.totalBytesTransferred, bytesPerSec,
    (unsigned long)metrics.avgTransactionDurationUs,
    (unsigned long)metrics.maxTransactionDurationUs,
    (unsigned long)metrics.txDuration_0_100us,
    metrics.totalTransactions > 0 ? (float)metrics.txDuration_0_100us * 100.0f / metrics.totalTransactions : 0.0f,
    (unsigned long)metrics.txDuration_100_500us,
    metrics.totalTransactions > 0 ? (float)metrics.txDuration_100_500us * 100.0f / metrics.totalTransactions : 0.0f,
    (unsigned long)metrics.txDuration_500_2000us,
    metrics.totalTransactions > 0 ? (float)metrics.txDuration_500_2000us * 100.0f / metrics.totalTransactions : 0.0f,
    (unsigned long)metrics.txDuration_2000plus_us,
    metrics.totalTransactions > 0 ? (float)metrics.txDuration_2000plus_us * 100.0f / metrics.totalTransactions : 0.0f);
  p += n; remaining -= n;
  
  // Add health check recommendations
  if (metrics.mutexTimeouts > 0) {
    n = snprintf(p, remaining, "\n⚠ WARNING: %lu mutex timeouts detected - bus overloaded\n",
                 (unsigned long)metrics.mutexTimeouts);
    p += n; remaining -= n;
  }
  
  if (contentionRate > 50.0f) {
    n = snprintf(p, remaining, "⚠ WARNING: High contention (%.1f%%) - consider reducing polling rates\n",
                 contentionRate);
    p += n; remaining -= n;
  }
  
  if (metrics.avgWaitTimeUs > 5000) {
    n = snprintf(p, remaining, "⚠ WARNING: High avg wait time (%lu us) - bus bottleneck detected\n",
                 (unsigned long)metrics.avgWaitTimeUs);
    p += n; remaining -= n;
  }
  
  return getDebugBuffer();
}

// ========== End I2C Device Health Tracking ==========

// ========== I2C Helper Functions ==========

// Helper function to identify sensor by I2C address
String identifySensor(uint8_t address) {
  for (size_t i = 0; i < 64; i++) {  // Reasonable max for sensor database
    const I2CSensorEntry& sensor = i2cSensors[i];
    if (sensor.name == nullptr) break;  // End of array
    if (sensor.address == address || (sensor.multiAddress && sensor.altAddress == address)) {
      String result = sensor.name;
      result += " (";
      result += sensor.description;
      result += ")";
      return result;
    }
  }
  return "Unknown Device";
}

// ========== End I2C Helper Functions ==========

// ========== I2C Infrastructure Commands ==========

const char* cmd_i2csdapin(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  if (valStr.length() == 0) return "Usage: i2cSdaPin <0..39> (reboot required)";
  int v = valStr.toInt();
  if (v < 0) v = 0;
  if (v > 39) v = 39;
  setSetting(gSettings.i2cSdaPin, v);
  snprintf(getDebugBuffer(), 1024, "i2cSdaPin set to %d (reboot required)", v);
  return getDebugBuffer();
}

const char* cmd_i2csclpin(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  if (valStr.length() == 0) return "Usage: i2cSclPin <0..39> (reboot required)";
  int v = valStr.toInt();
  if (v < 0) v = 0;
  if (v > 39) v = 39;
  setSetting(gSettings.i2cSclPin, v);
  snprintf(getDebugBuffer(), 1024, "i2cSclPin set to %d (reboot required)", v);
  return getDebugBuffer();
}

const char* cmd_i2cclockthermalhz(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  if (valStr.length() == 0) return "Usage: i2cClockThermalHz <100000..1000000>";
  int v = valStr.toInt();
  if (v < 100000) v = 100000;
  if (v > 1000000) v = 1000000;
  setSetting(gSettings.i2cClockThermalHz, v);
  snprintf(getDebugBuffer(), 1024, "i2cClockThermalHz set to %d", v);
  return getDebugBuffer();
}

const char* cmd_i2cclocktofhz(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  if (valStr.length() == 0) return "Usage: i2cClockToFHz <50000..400000>";
  int v = valStr.toInt();
  if (v < 50000) v = 50000;
  if (v > 400000) v = 400000;
  setSetting(gSettings.i2cClockToFHz, v);
  snprintf(getDebugBuffer(), 1024, "i2cClockToFHz set to %d", v);
  return getDebugBuffer();
}

const char* cmd_i2cscan(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  char* p = getDebugBuffer();
  size_t remaining = 1024;

  int n = snprintf(p, remaining, "I2C Bus Scan with Device Identification:\n");
  p += n;
  remaining -= n;

  n = snprintf(p, remaining, "========================================\n");
  p += n;
  remaining -= n;

  // Scan Wire1 (sensor bus) with configurable pins
  n = snprintf(p, remaining, "Wire1 (SDA=%d, SCL=%d):\n", gSettings.i2cSdaPin, gSettings.i2cSclPin);
  p += n;
  remaining -= n;

  // Wire1 already initialized in setup() via initI2CBuses()
  int count1 = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    if (i2cPingAddress(addr, 100000, 50)) {
      String identification = identifySensor(addr);
      n = snprintf(p, remaining, "  0x%02X (%d) - %s\n", addr, addr, identification.c_str());
      p += n;
      remaining -= n;
      count1++;
      if (remaining < 100) break;  // Safety check
    }
  }
  if (count1 == 0) {
    n = snprintf(p, remaining, "  No devices found\n");
    p += n;
    remaining -= n;
  }

  n = snprintf(p, remaining, "\nTotal devices found: %d\n", count1);
  p += n;
  remaining -= n;

  n = snprintf(p, remaining, "Use 'sensors' to see full sensor database\n");
  p += n;
  remaining -= n;

  n = snprintf(p, remaining, "Use 'sensorinfo <name>' for detailed sensor information");
  p += n;
  remaining -= n;

  return getDebugBuffer();
}

const char* cmd_i2cstats(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  broadcastOutput("I2C Bus Statistics:");
  broadcastOutput("==================");

  // Wire1 bus info (configurable)
  broadcastOutput("");

  // Wire1 bus info (sensor bus)
  broadcastOutput("Wire1 (Sensor I2C):");
  BROADCAST_PRINTF("  SDA Pin: %d", gSettings.i2cSdaPin);
  BROADCAST_PRINTF("  SCL Pin: %d", gSettings.i2cSclPin);
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (mgr) {
    BROADCAST_PRINTF("  Clock: Managed by I2CDeviceManager (per-device)");
  }
  broadcastOutput("");

  // Sensor connection status
  broadcastOutput("Connected Sensors:");

  if (gamepadConnected) {
    broadcastOutput("  Gamepad (seesaw)");
  }
  if (imuConnected) {
    broadcastOutput("  IMU (BNO055)");
  }
  if (apdsConnected) {
    broadcastOutput("  APDS9960");
  }
  if (tofConnected) {
    broadcastOutput("  ToF (VL53L4CX)");
  }
  if (thermalConnected) {
    broadcastOutput("  Thermal (MLX90640)");
  }

  if (!gamepadConnected && !imuConnected && !apdsConnected && !tofConnected && !thermalConnected) {
    broadcastOutput("  No sensors connected");
  }

  return "[I2C] Health status displayed";
}

// ========== End I2C Infrastructure Commands ==========

#if ENABLE_TOF_SENSOR
extern bool tofEnabled;
extern bool tofConnected;
extern VL53L4CX* gVL53L4CX;
// gTofWatermarkNow, gTofWatermarkMin, tofLastStopTime now in i2c_system.h
extern TaskHandle_t tofTaskHandle;
extern bool readToFObjects();
#endif
// i2cOledTransactionVoid/i2cOledTransaction and i2cDeviceTransaction are template functions in System_I2C.h
extern bool thermalEnabled;

// SensorCache struct is now defined in i2c_system.h

// IMU sensor globals
#if ENABLE_IMU_SENSOR
extern bool imuEnabled;
extern bool imuConnected;
extern Adafruit_BNO055* gBNO055;
// gIMUWatermarkNow, gIMUWatermarkMin, imuInitRequested, imuInitResult, imuInitDone, initIMUSensor now in i2c_system.h
extern void readIMUSensor();
#endif

// Thermal sensor globals
#if ENABLE_THERMAL_SENSOR
extern bool thermalConnected;
extern TaskHandle_t thermalTaskHandle;
// thermalSensor, thermalLastStopTime, gThermalWatermarkNow, gThermalWatermarkMin, thermalInitRequested, thermalInitResult, thermalInitDone now in i2c_system.h
extern bool initThermalSensor();
extern bool readThermalPixels();
#endif
// thermalArmAtMs, thermalPendingFirstFrame now in Sensor_Thermal_MLX90640.h
// lockThermalCache, unlockThermalCache now in i2c_system.h

// Thermal cache comes from Sensor_Thermal_MLX90640.h

// Gamepad sensor globals
#if ENABLE_GAMEPAD_SENSOR
extern bool gamepadEnabled;
extern bool gamepadConnected;
extern Adafruit_seesaw gGamepadSeesaw;
extern TaskHandle_t gamepadTaskHandle;
// gGamepadWatermarkNow, gGamepadWatermarkMin now in i2c_system.h
#endif

// APDS9960 sensor globals
#if ENABLE_APDS_SENSOR
extern bool apdsColorEnabled;
extern bool apdsProximityEnabled;
extern bool apdsGestureEnabled;
extern bool apdsConnected;
extern Adafruit_APDS9960* gAPDS9960;
extern TaskHandle_t apdsTaskHandle;
#endif

// GPS sensor globals
#if ENABLE_GPS_SENSOR
extern bool gpsEnabled;
extern bool gpsConnected;
extern Adafruit_GPS* gPA1010D;
extern TaskHandle_t gpsTaskHandle;
#endif

// RTC sensor globals
#if ENABLE_RTC_SENSOR
#include "i2csensor-ds3231.h"
#endif

// ============================================================================
// Sensor Cache Lock/Unlock Helpers (moved from .ino)
// ============================================================================
// Legacy lockSensorCache/unlockSensorCache removed - use modular sensor cache locks

bool lockThermalCache(TickType_t timeout) {
  return gThermalCache.mutex && (xSemaphoreTake(gThermalCache.mutex, timeout) == pdTRUE);
}

void unlockThermalCache() {
  if (gThermalCache.mutex) {
    xSemaphoreGive(gThermalCache.mutex);
  }
}

// ============================================================================
// Sensor Task Implementations
// ============================================================================

// All sensor tasks moved to their respective modules for full modularization:
// tofTask() -> Sensor_ToF_VL53L4CX.cpp
// imuTask() -> Sensor_IMU_BNO055.cpp
// thermalTask() -> Sensor_Thermal_MLX90640.cpp
// gamepadTask() -> Sensor_Gamepad_Seesaw.cpp
// apdsTask() -> Sensor_APDS_APDS9960.cpp
// gpsTask() -> Sensor_GPS_PA1010D.cpp

// ============================================================================
// I2C Device Registry Helper Functions
// ============================================================================

// External dependencies for device registry
// connectedDevices, connectedDeviceCount, discoveryCount defined above
extern const I2CSensorEntry i2cSensors[];
extern const size_t i2cSensorsCount;
extern bool readText(const char* path, String& out);

void ensureDeviceRegistryFile();  // Forward declaration
static void scanBusForDevicesSmart(uint8_t busNumber, const uint8_t* addresses, int addressCount);  // Smart scan

static void createEmptyDeviceRegistry() {
  FsLockGuard guard("i2c.devices.create");
  File file = LittleFS.open("/system/devices.json", "w");
  if (file) {
    file.println("{");
    file.println("  \"lastDiscovery\": 0,");
    file.println("  \"discoveryCount\": 0,");
    file.println("  \"devices\": []");
    file.println("}");
    file.close();
  }
}

void ensureDeviceRegistryFile() {
  FsLockGuard guard("i2c.devices.ensure");
  if (!LittleFS.exists("/system/devices.json")) {
    createEmptyDeviceRegistry();
  }
}

static int findSensorIndexByAddress(uint8_t address) {
  // Pass 1: prefer exact primary address matches
  for (size_t i = 0; i < i2cSensorsCount; i++) {
    if (i2cSensors[i].address == address) {
      return i;
    }
  }
  // Pass 2: then allow alternate address matches if declared
  for (size_t i = 0; i < i2cSensorsCount; i++) {
    if (i2cSensors[i].multiAddress && i2cSensors[i].altAddress == address) {
      return i;
    }
  }
  return -1;  // Not found
}

static void addDiscoveredDevice(uint8_t address, uint8_t bus) {
  // MAX_CONNECTED_DEVICES is now a #define in i2c_system.h
  if (connectedDeviceCount >= MAX_CONNECTED_DEVICES) return;

  int sensorIndex = findSensorIndexByAddress(address);
  unsigned long now = millis();

  ConnectedDevice& device = connectedDevices[connectedDeviceCount++];
  device.address = address;
  device.bus = bus;
  device.isConnected = true;
  device.lastSeen = now;
  device.firstDiscovered = now;

  if (sensorIndex >= 0) {
    device.name = i2cSensors[sensorIndex].name;
    device.description = i2cSensors[sensorIndex].description;
    device.manufacturer = i2cSensors[sensorIndex].manufacturer;
    INFO_I2CF("Found device at 0x%02X on bus %d - %s (%s)", address, bus, device.name, device.description);
    
    // Don't register here - devices register themselves when initialized by their sensor modules
  } else {
    device.name = "Unknown";
    device.description = "Unidentified Device";
    device.manufacturer = "Unknown";
    INFO_I2CF("Found device at 0x%02X on bus %d - Unknown device", address, bus);
    
    // Don't register unknown devices - only actual initialized sensors should be in the manager
  }
}

static void scanBusForDevices(uint8_t busNumber) {
  TwoWire* wire = (busNumber == 0) ? &Wire : &Wire1;

  // Prevent concurrent I2C usage (e.g. gamepad/OLED tasks) while reinitializing/scanning
  extern SemaphoreHandle_t i2cMutex;
  extern volatile bool gSensorPollingPaused;
  bool prevPaused = gSensorPollingPaused;
  gSensorPollingPaused = true;
  bool locked = (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(2000)) == pdTRUE);
  if (!locked) {
    gSensorPollingPaused = prevPaused;
    return;
  }

  // Re-initialize Arduino I2C buses before scanning (safeguards against driver teardown)
  // Only Wire1 is used - Wire bus is not initialized
  if (busNumber == 1) {
    // Sensor Wire1 bus on configurable STEMMA QT pins
    Wire1.begin(gSettings.i2cSdaPin, gSettings.i2cSclPin);
    Wire1.setClock(I2C_WIRE1_DEFAULT_FREQ);
  }

  // Small delay to let bus stabilize
  delay(10);

  for (uint8_t addr = 1; addr < 127; addr++) {
    wire->beginTransmission(addr);
    if (wire->endTransmission() == 0) {
      addDiscoveredDevice(addr, busNumber);
    }
  }

  if (i2cMutex) xSemaphoreGive(i2cMutex);
  gSensorPollingPaused = prevPaused;
}

// Smart scan function - only checks specific addresses (used by discoverI2CDevices)
static void scanBusForDevicesSmart(uint8_t busNumber, const uint8_t* addresses, int addressCount) {
  TwoWire* wire = (busNumber == 0) ? &Wire : &Wire1;

  // Prevent concurrent I2C usage (e.g. gamepad/OLED tasks) while reinitializing/scanning
  extern SemaphoreHandle_t i2cMutex;
  extern volatile bool gSensorPollingPaused;
  bool prevPaused = gSensorPollingPaused;
  gSensorPollingPaused = true;
  bool locked = (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(2000)) == pdTRUE);
  if (!locked) {
    gSensorPollingPaused = prevPaused;
    return;
  }

  // Re-initialize Arduino I2C buses before scanning (safeguards against driver teardown)
  // Only Wire1 is used - Wire bus is not initialized
  if (busNumber == 1) {
    // Sensor Wire1 bus on configurable STEMMA QT pins
    Wire1.begin(gSettings.i2cSdaPin, gSettings.i2cSclPin);
    Wire1.setClock(I2C_WIRE1_DEFAULT_FREQ);
  }

  // Small delay to let bus stabilize
  delay(10);

  // Scan only the specified addresses
  for (int i = 0; i < addressCount; i++) {
    uint8_t addr = addresses[i];
    if (addr == 0) continue;  // Skip invalid addresses
    
    wire->beginTransmission(addr);
    if (wire->endTransmission() == 0) {
      addDiscoveredDevice(addr, busNumber);
    }
  }

  if (i2cMutex) xSemaphoreGive(i2cMutex);
  gSensorPollingPaused = prevPaused;
}

static void saveDeviceRegistryToJSON() {
  ensureDeviceRegistryFile();

  FsLockGuard guard("i2c.devices.save");

  File file = LittleFS.open("/system/devices.json", "w");
  if (!file) return;

  file.println("{");
  file.printf("  \"lastDiscovery\": %lu,\n", (unsigned long)millis());
  file.printf("  \"discoveryCount\": %d,\n", discoveryCount);
  file.println("  \"devices\": [");

  for (int i = 0; i < connectedDeviceCount; i++) {
    ConnectedDevice& device = connectedDevices[i];

    String hexAddr = String(device.address, HEX);
    if (device.address < 16) hexAddr = "0" + hexAddr;
    hexAddr.toUpperCase();

    file.print("    {");
    file.printf("\"address\": %d, ", device.address);
    file.printf("\"addressHex\": \"0x%s\", ", hexAddr.c_str());
    file.printf("\"name\": \"%s\", ", device.name);
    file.printf("\"description\": \"%s\", ", device.description);
    file.printf("\"manufacturer\": \"%s\", ", device.manufacturer);
    file.printf("\"bus\": %d, ", device.bus);
    file.printf("\"isConnected\": %s, ", device.isConnected ? "true" : "false");
    file.printf("\"lastSeen\": %lu, ", (unsigned long)device.lastSeen);
    file.printf("\"firstDiscovered\": %lu", (unsigned long)device.firstDiscovered);
    file.print("}");

    if (i < connectedDeviceCount - 1) file.print(",");
    file.println();
  }

  file.println("  ]");
  file.println("}");
  file.close();
}

void discoverI2CDevices() {
  // Early exit if I2C bus is disabled
  if (!gI2CBusEnabled) {
    INFO_I2CF("Device discovery skipped - bus disabled");
    return;
  }
  
  INFO_I2CF("Starting device discovery (smart scan - compiled sensors only)");
  ensureDeviceRegistryFile();

  // Clear existing registry
  connectedDeviceCount = 0;
  discoveryCount++;

  // Build smart scan list from compiled sensors only
  uint8_t scanAddresses[32];  // Max addresses to scan
  int scanCount = 0;
  
  for (size_t i = 0; i < i2cSensorsCount && scanCount < 32; i++) {
    // Check if sensor is compiled in (via header guard)
    bool compiled = true;
    if (i2cSensors[i].headerGuard != nullptr) {
      // Sensor has a header guard - check if it's defined
      // For now, assume all sensors with guards are compiled (compile-time check)
      // Runtime check would require preprocessor macros passed as runtime flags
      #ifndef ENABLE_THERMAL_SENSOR
        if (strcmp(i2cSensors[i].headerGuard, "_ADAFRUIT_MLX90640_H_") == 0) compiled = false;
      #endif
      #ifndef ENABLE_TOF_SENSOR
        if (strcmp(i2cSensors[i].headerGuard, "_VL53L4CX_CLASS_H_") == 0) compiled = false;
      #endif
      #ifndef ENABLE_IMU_SENSOR
        if (strcmp(i2cSensors[i].headerGuard, "_ADAFRUIT_BNO055_H_") == 0) compiled = false;
      #endif
      #ifndef ENABLE_GAMEPAD_SENSOR
        if (strcmp(i2cSensors[i].headerGuard, "_ADAFRUIT_SEESAW_H_") == 0) compiled = false;
      #endif
      #ifndef ENABLE_APDS_SENSOR
        if (strcmp(i2cSensors[i].headerGuard, "_ADAFRUIT_APDS9960_H_") == 0) compiled = false;
      #endif
      #ifndef ENABLE_GPS_SENSOR
        if (strcmp(i2cSensors[i].headerGuard, "_ADAFRUIT_GPS_H") == 0) compiled = false;
      #endif
    }
    
    if (compiled) {
      scanAddresses[scanCount++] = i2cSensors[i].address;
      if (i2cSensors[i].multiAddress && scanCount < 32) {
        scanAddresses[scanCount++] = i2cSensors[i].altAddress;
      }
    }
  }
  
  INFO_I2CF("Smart scan: %d addresses to check (vs 254 in full scan)", scanCount);

  // Scan Wire1 (sensor bus) - use smart scan list
  INFO_I2CF("Scanning Wire1 (SDA=%d, SCL=%d) - smart scan", gSettings.i2cSdaPin, gSettings.i2cSclPin);
  scanBusForDevicesSmart(1, scanAddresses, scanCount);  // Wire1 - smart scan

  INFO_I2CF("Found %d total devices", connectedDeviceCount);

  // Save results to JSON file
  INFO_I2CF("Saving device registry to /system/devices.json");
  saveDeviceRegistryToJSON();
  INFO_I2CF("Device registry saved successfully");
}

static void streamDeviceRegistryOutput() {
  broadcastOutput("Connected I2C Devices:");
  broadcastOutput("=====================");

  if (connectedDeviceCount == 0) {
    broadcastOutput("No devices discovered. Run 'discover' to scan for devices.");
    return;
  }

  broadcastOutput("Bus  Addr Name         Description                    Status    Last Seen");
  broadcastOutput("---- ---- ------------ ------------------------------ --------- ---------");

  for (int i = 0; i < connectedDeviceCount; i++) {
    ConnectedDevice& device = connectedDevices[i];

    const char* busStr = (device.bus == 0) ? "W0" : "W1";
    char hexAddr[5];
    snprintf(hexAddr, sizeof(hexAddr), "%02X", device.address);

    const char* status = device.isConnected ? "Connected" : "Disconn";
    unsigned long timeSince = (millis() - device.lastSeen) / 1000;

    BROADCAST_PRINTF("%-4s 0x%s %-12.12s %-30.30s %-9s %lus ago",
                     busStr, hexAddr, device.name, device.description, status, timeSince);
  }

  BROADCAST_PRINTF("\nTotal: %d devices (Discovery #%d)", connectedDeviceCount, discoveryCount);
}

// ============================================================================
// I2C Device Registry Command Handlers
// ============================================================================

const char* cmd_devices(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  ensureDeviceRegistryFile();
  streamDeviceRegistryOutput();
  return "[I2C] Device registry displayed";
}

const char* cmd_discover(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  ensureDeviceRegistryFile();
  discoverI2CDevices();

  BROADCAST_PRINTF("Device discovery completed. Found %d devices.", connectedDeviceCount);
  broadcastOutput("Registry saved to /system/devices.json\n");

  // Initialize FM radio if detected to prevent I2C bus lockups
  bool fmRadioDetected = false;
  for (int i = 0; i < connectedDeviceCount; i++) {
    if (connectedDevices[i].address == 0x11 && strcmp(connectedDevices[i].name, "RDA5807") == 0) {
      fmRadioDetected = true;
      break;
    }
  }
  
  if (fmRadioDetected) {
    DEBUG_SYSTEMF("FM radio detected, initializing to prevent I2C bus interference");
    // Initialize radio and keep it in stable low-power state
    extern bool initFMRadio();
    if (initFMRadio()) {
      INFO_SENSORSF("FM radio initialized successfully - kept in low-power state");
    } else {
      WARN_SENSORSF("FM radio initialization failed, may cause I2C interference");
    }
  }

  streamDeviceRegistryOutput();

  return "Discovery complete";
}

const char* cmd_devicefile(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!LittleFS.exists("/system/devices.json")) {
    return "Device registry file not found. Run 'discover' to create it.";
  }

  String content;
  if (!readText("/system/devices.json", content)) {
    return "Error: Could not read /system/devices.json";
  }

  broadcastOutput("Device Registry JSON (/system/devices.json):");
  broadcastOutput(content.c_str());
  return "[I2C] Registry JSON displayed";
}

const char* cmd_sensors(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  String args = argsIn;
  args.trim();

  broadcastOutput("I2C Sensor Database:");
  broadcastOutput("===================");

  // Check for filter arguments
  String filter = "";
  if (args.length() > 0) {
    filter = args;
    filter.toLowerCase();
    BROADCAST_PRINTF("Filter: '%s'", args.c_str());
    broadcastOutput("");
  }

  broadcastOutput("Addr Name         Description                    Manufacturer");
  broadcastOutput("---- ------------ ------------------------------ ------------");

  int count = 0;
  for (size_t i = 0; i < i2cSensorsCount; i++) {
    const I2CSensorEntry& sensor = i2cSensors[i];

    // Apply filter if specified
    if (filter.length() > 0) {
      String sensorName = String(sensor.name);
      String sensorDesc = String(sensor.description);
      String sensorMfg = String(sensor.manufacturer);
      sensorName.toLowerCase();
      sensorDesc.toLowerCase();
      sensorMfg.toLowerCase();

      if (sensorName.indexOf(filter) < 0 && sensorDesc.indexOf(filter) < 0 && sensorMfg.indexOf(filter) < 0) {
        continue;
      }
    }

    // Format address with leading zero if needed
    char hexAddr[5];
    snprintf(hexAddr, sizeof(hexAddr), "%02X", sensor.address);

    // Print sensor line with fixed-width formatting
    if (sensor.multiAddress) {
      char altHex[5];
      snprintf(altHex, sizeof(altHex), "%02X", sensor.altAddress);
      BROADCAST_PRINTF("0x%s %-12.12s %-30.30s %s (alt: 0x%s)",
                       hexAddr, sensor.name, sensor.description, sensor.manufacturer, altHex);
    } else {
      BROADCAST_PRINTF("0x%s %-12.12s %-30.30s %s",
                       hexAddr, sensor.name, sensor.description, sensor.manufacturer);
    }
    count++;
  }

  // Footer
  broadcastOutput("");
  BROADCAST_PRINTF("Total sensors in database: %zu", i2cSensorsCount);
  if (filter.length() > 0) {
    BROADCAST_PRINTF(" (showing %d matches)", count);
  }

  broadcastOutput("");
  broadcastOutput("Usage: sensors [filter] - filter by name, description, or manufacturer");
  broadcastOutput("Example: sensors temperature, sensors adafruit, sensors imu");

  return "[I2C] Sensor list displayed";
}

const char* cmd_sensorinfo(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  String args = argsIn;
  args.trim();

  if (args.length() == 0) {
    broadcastOutput("Usage: sensorinfo <sensor_name>");
    broadcastOutput("Example: sensorinfo BNO055");
    return "ERROR";
  }

  // Find sensor by name (case insensitive)
  const I2CSensorEntry* foundSensor = nullptr;
  String searchName = args;
  searchName.toLowerCase();

  for (size_t i = 0; i < i2cSensorsCount; i++) {
    String sensorName = String(i2cSensors[i].name);
    sensorName.toLowerCase();
    if (sensorName == searchName) {
      foundSensor = &i2cSensors[i];
      break;
    }
  }

  if (!foundSensor) {
    BROADCAST_PRINTF("Sensor '%s' not found in database.", args.c_str());
    broadcastOutput("");
    broadcastOutput("Available sensors:");

    for (size_t i = 0; i < i2cSensorsCount; i++) {
      BROADCAST_PRINTF("  %s", i2cSensors[i].name);
      if (i > 10) {
        BROADCAST_PRINTF("  ... and %zu more", i2cSensorsCount - i - 1);
        break;
      }
    }

    broadcastOutput("");
    broadcastOutput("Use 'sensors' to see the full list");
    return "ERROR";
  }

  broadcastOutput("Sensor Information:");
  broadcastOutput("==================");
  BROADCAST_PRINTF("Name: %s", foundSensor->name);
  BROADCAST_PRINTF("Description: %s", foundSensor->description);
  BROADCAST_PRINTF("Manufacturer: %s", foundSensor->manufacturer);

  char hexAddr[5];
  snprintf(hexAddr, sizeof(hexAddr), "%02X", foundSensor->address);
  BROADCAST_PRINTF("I2C Address: 0x%s (%d)", hexAddr, foundSensor->address);

  if (foundSensor->multiAddress) {
    char altHex[5];
    snprintf(altHex, sizeof(altHex), "%02X", foundSensor->altAddress);
    BROADCAST_PRINTF("Alternative Address: 0x%s (%d)", altHex, foundSensor->altAddress);
  }

  // Check if this sensor is currently connected
  bool connectedWire0 = false, connectedWire1 = false;

  Wire.beginTransmission(foundSensor->address);
  if (Wire.endTransmission() == 0) connectedWire0 = true;

  if (i2cPingAddress(foundSensor->address, 100000, 50)) connectedWire1 = true;

  if (foundSensor->multiAddress) {
    Wire.beginTransmission(foundSensor->altAddress);
    if (Wire.endTransmission() == 0) connectedWire0 = true;

    if (i2cPingAddress(foundSensor->altAddress, 100000, 50)) connectedWire1 = true;
  }

  broadcastOutput("");
  broadcastOutput("Connection Status:");

  if (connectedWire1) {
    char buf[64];
    snprintf(buf, sizeof(buf), "  ✓ Connected on Wire1 (SDA=%d, SCL=%d)", gSettings.i2cSdaPin, gSettings.i2cSclPin);
    broadcastOutput(buf);
  }

  if (!connectedWire0 && !connectedWire1) {
    broadcastOutput("  ✗ Not currently connected");
  }

  return "[I2C] Sensor info displayed";
}

// ============================================================================
// Sensor Configuration Commands
// ============================================================================

// Estimated heap cost per sensor (in KB) - measured/approximated values
// These are task stack + buffers + driver overhead
struct SensorHeapCost {
  const char* name;
  const char* shortName;
  bool* autoStartFlag;
  uint16_t heapCostKB;  // Estimated heap usage in KB
};

static const SensorHeapCost sensorHeapCosts[] = {
  { "Thermal Camera", "thermal", &gSettings.thermalAutoStart, 32 },  // MLX90640: large frame buffer
  { "ToF Distance",   "tof",     &gSettings.tofAutoStart,      8 },  // VL53L4CX: moderate
  { "IMU",            "imu",     &gSettings.imuAutoStart,     12 },  // BNO055: calibration + buffers
  { "GPS",            "gps",     &gSettings.gpsAutoStart,      4 },  // PA1010D: NMEA parsing
  { "FM Radio",       "fmradio", &gSettings.fmRadioAutoStart,  2 },  // RDA5807: minimal
  { "APDS Gesture",   "apds",    &gSettings.apdsAutoStart,     4 },  // APDS9960: gesture buffers
  { "Gamepad",        "gamepad", &gSettings.gamepadAutoStart,  2 },  // Seesaw: minimal
  { "RTC Clock",      "rtc",     &gSettings.rtcAutoStart,      2 },  // DS3231: minimal
  { "Presence",       "presence",&gSettings.presenceAutoStart, 2 },  // STHS34PF80: minimal
};
static const size_t sensorHeapCostCount = sizeof(sensorHeapCosts) / sizeof(sensorHeapCosts[0]);

// Calculate total estimated heap for enabled sensors
static uint32_t getEnabledSensorHeapEstimate() {
  uint32_t total = 0;
  for (size_t i = 0; i < sensorHeapCostCount; i++) {
    if (*sensorHeapCosts[i].autoStartFlag) {
      total += sensorHeapCosts[i].heapCostKB;
    }
  }
  return total;
}

static const char* cmd_sensorautostart(const String& argsIn) {
  extern bool gCLIValidateOnly;
  if (gCLIValidateOnly) return "VALID";
  
  String args = argsIn;
  args.trim();
  
  // No args - show current settings with heap estimates
  if (args.length() == 0) {
    PSRAM_STATIC_BUF(buf, 1024);
    uint32_t freeHeapKB = ESP.getFreeHeap() / 1024;
    uint32_t enabledCost = getEnabledSensorHeapEstimate();
    
    int pos = snprintf(buf, buf_SIZE,
      "[Sensor Auto-Start] (heap estimates)\n"
      "%-12s %-4s %s\n"
      "─────────────────────────────────\n",
      "Sensor", "Cost", "Status");
    
    for (size_t i = 0; i < sensorHeapCostCount; i++) {
      bool enabled = *sensorHeapCosts[i].autoStartFlag;
      pos += snprintf(buf + pos, buf_SIZE - pos,
        "%-12s ~%2dKB  %s\n",
        sensorHeapCosts[i].shortName,
        sensorHeapCosts[i].heapCostKB,
        enabled ? "[ON]" : "off");
    }
    
    pos += snprintf(buf + pos, buf_SIZE - pos,
      "─────────────────────────────────\n"
      "Enabled total: ~%luKB | Free heap: %luKB\n"
      "Usage: sensorautostart <sensor> <on|off>",
      (unsigned long)enabledCost, (unsigned long)freeHeapKB);
    
    return buf;
  }
  
  int secondSpace = args.indexOf(' ');
  if (secondSpace < 0) {
    return "Usage: sensorautostart <sensor> <on|off>";
  }
  
  String sensor = args.substring(0, secondSpace);
  String value = args.substring(secondSpace + 1);
  sensor.toLowerCase();
  value.toLowerCase();
  
  bool enable = (value == "on" || value == "true" || value == "1");
  bool disable = (value == "off" || value == "false" || value == "0");
  
  if (!enable && !disable) {
    return "Value must be on/off, true/false, or 1/0";
  }
  
  // Find sensor in cost table
  const SensorHeapCost* found = nullptr;
  for (size_t i = 0; i < sensorHeapCostCount; i++) {
    if (sensor == sensorHeapCosts[i].shortName || 
        (sensor == "fm" && strcmp(sensorHeapCosts[i].shortName, "fmradio") == 0)) {
      found = &sensorHeapCosts[i];
      break;
    }
  }
  
  if (sensor == "all") {
    // Set all sensors and show total heap impact
    uint32_t totalCost = 0;
    for (size_t i = 0; i < sensorHeapCostCount; i++) {
      *sensorHeapCosts[i].autoStartFlag = enable;
      if (enable) totalCost += sensorHeapCosts[i].heapCostKB;
    }
    writeSettingsJson();
    
    static char result[128];
    uint32_t freeHeapKB = ESP.getFreeHeap() / 1024;
    if (enable) {
      snprintf(result, sizeof(result), 
        "[AutoStart] All sensors enabled (~%luKB total, %luKB free)",
        (unsigned long)totalCost, (unsigned long)freeHeapKB);
    } else {
      snprintf(result, sizeof(result), "[AutoStart] All sensors disabled");
    }
    return result;
  }
  
  if (!found) {
    return "Unknown sensor. Options: thermal, tof, imu, gps, fmradio, apds, gamepad, all";
  }
  
  bool wasEnabled = *found->autoStartFlag;
  *found->autoStartFlag = enable;
  writeSettingsJson();
  
  static char result[128];
  uint32_t freeHeapKB = ESP.getFreeHeap() / 1024;
  
  if (enable && !wasEnabled) {
    snprintf(result, sizeof(result), 
      "[AutoStart] %s enabled (~%dKB, %luKB free after boot)",
      found->name, found->heapCostKB, (unsigned long)(freeHeapKB - found->heapCostKB));
  } else if (!enable && wasEnabled) {
    snprintf(result, sizeof(result), 
      "[AutoStart] %s disabled (+%dKB freed after reboot)",
      found->name, found->heapCostKB);
  } else {
    snprintf(result, sizeof(result), 
      "[AutoStart] %s already %s",
      found->name, enable ? "enabled" : "disabled");
  }
  return result;
}

// ============================================================================
// Sensor Command Registry
// ============================================================================

// Forward declarations for queued sensor start commands (from main .ino)
extern const char* cmd_thermalstart_queued(const String& cmd);
extern const char* cmd_tofstart_queued(const String& cmd);
extern const char* cmd_imustart_queued(const String& cmd);
extern const char* cmd_apdsstart_queued(const String& cmd);

// ============================================================================
// I2C Command Registry
// ============================================================================

const CommandEntry i2cCommands[] = {
  // Bus Configuration
  { "i2csdapin", "Set I2C SDA pin: <0..39>", true, cmd_i2csdapin, "Usage: i2cSdaPin <0..39>" },
  { "i2csclpin", "Set I2C SCL pin: <0..39>", true, cmd_i2csclpin, "Usage: i2cSclPin <0..39>" },
  { "i2cclockthermalhz", "I2C clock thermal: <100000..1000000>", true, cmd_i2cclockthermalhz, "Usage: i2cClockThermalHz <100000..1000000>" },
  { "i2cclocktofhz", "I2C clock ToF: <50000..400000>", true, cmd_i2cclocktofhz, "Usage: i2cClockToFHz <50000..400000>" },
  
  // Diagnostics
  { "i2cmetrics", "Show I2C bus performance metrics.", false, cmd_i2cmetrics },
  { "i2cscan", "Scan I2C bus for devices.", false, cmd_i2cscan },
  { "i2cstats", "I2C bus statistics and errors.", false, cmd_i2cstats },
  { "i2chealth", "Show per-device I2C health status.", false, cmd_i2chealth },
  
  // Device Registry
  { "sensors", "List I2C sensors [filter]", false, cmd_sensors, "Usage: sensors [filter] - filter by name, description, or manufacturer\nExample: sensors temperature, sensors adafruit, sensors imu" },
  { "sensorinfo", "Sensor details: <name>", false, cmd_sensorinfo, "Usage: sensorinfo <sensor_name>\nExample: sensorinfo BNO055" },
  { "devices", "Show discovered I2C device registry.", false, cmd_devices },
  { "discover", "Re-scan and register I2C devices.", false, cmd_discover },
  { "devicefile", "Show device registry JSON file.", false, cmd_devicefile },
  
  // Sensor Auto-Start
  { "sensorautostart", "Sensor auto-start: [sensor] [on|off]", true, cmd_sensorautostart, "Usage: sensorautostart [sensor] [on|off]\n       sensorautostart all [on|off]\nSensors: thermal, tof, imu, gps, fmradio, apds, gamepad" }
};

const size_t i2cCommandsCount = sizeof(i2cCommands) / sizeof(i2cCommands[0]);

// ============================================================================
// Command Registration
// ============================================================================
static CommandModuleRegistrar _i2c_registrar(i2cCommands, i2cCommandsCount, "i2c");

// ============================================================================
// Sensor Status System (moved from HardwareOne.ino)
// ============================================================================

const char* deviceTypeDisplayName(I2CDeviceType sensor) {
  switch (sensor) {
    case I2C_DEVICE_THERMAL:  return "Thermal";
    case I2C_DEVICE_TOF:      return "ToF";
    case I2C_DEVICE_IMU:      return "IMU";
    case I2C_DEVICE_GAMEPAD:  return "Gamepad";
    case I2C_DEVICE_GPS:      return "GPS";
    case I2C_DEVICE_FMRADIO:  return "FM Radio";
    case I2C_DEVICE_APDS:     return "APDS";
    case I2C_DEVICE_RTC:      return "RTC";
    case I2C_DEVICE_PRESENCE: return "Presence";
    default:              return "Unknown";
  }
}

void handleDeviceStopped(I2CDeviceType sensor) {
  const char* name = deviceTypeDisplayName(sensor);

  // Set enabled flag to false and record stop time (common boilerplate)
  switch (sensor) {
    case I2C_DEVICE_THERMAL:
#if ENABLE_THERMAL_SENSOR
      thermalEnabled = false;
      thermalLastStopTime = millis();
#endif
      break;
    case I2C_DEVICE_TOF:
#if ENABLE_TOF_SENSOR
      tofEnabled = false;
      tofLastStopTime = millis();
#endif
      break;
    case I2C_DEVICE_IMU:
#if ENABLE_IMU_SENSOR
      imuEnabled = false;
      imuLastStopTime = millis();
#endif
      break;
    case I2C_DEVICE_GAMEPAD:
#if ENABLE_GAMEPAD_SENSOR
      gamepadEnabled = false;
      gamepadLastStopTime = millis();
#endif
      break;
    case I2C_DEVICE_GPS:
#if ENABLE_GPS_SENSOR
      gpsEnabled = false;
      gpsLastStopTime = millis();
#endif
      break;
    case I2C_DEVICE_FMRADIO:
#if ENABLE_FM_RADIO
      fmRadioEnabled = false;
      fmRadioLastStopTime = millis();
#endif
      break;
    case I2C_DEVICE_APDS:
#if ENABLE_APDS_SENSOR
      apdsColorEnabled = false;
      apdsProximityEnabled = false;
      apdsGestureEnabled = false;
      apdsLastStopTime = millis();
#endif
      break;
    case I2C_DEVICE_RTC:
#if ENABLE_RTC_SENSOR
      rtcEnabled = false;
      rtcLastStopTime = millis();
#endif
      break;
    case I2C_DEVICE_PRESENCE:
#if ENABLE_PRESENCE_SENSOR
      presenceEnabled = false;
      presenceLastStopTime = millis();
#endif
      break;
    default: break;
  }

#if ENABLE_ESPNOW
  // Broadcast status to mesh peers for sensors that have remote types
  switch (sensor) {
    case I2C_DEVICE_THERMAL:  broadcastSensorStatus(REMOTE_SENSOR_THERMAL, false);  break;
    case I2C_DEVICE_TOF:      broadcastSensorStatus(REMOTE_SENSOR_TOF, false);      break;
    case I2C_DEVICE_IMU:      broadcastSensorStatus(REMOTE_SENSOR_IMU, false);      break;
    case I2C_DEVICE_GAMEPAD:  broadcastSensorStatus(REMOTE_SENSOR_GAMEPAD, false);  break;
    case I2C_DEVICE_GPS:      broadcastSensorStatus(REMOTE_SENSOR_GPS, false);      break;
    case I2C_DEVICE_FMRADIO:  broadcastSensorStatus(REMOTE_SENSOR_FMRADIO, false);  break;
    default: break;  // APDS, RTC, Presence have no remote sensor type
  }
#endif

  notifySensorStopped(name);

  // Bump sensor status so SSE + bonded peer get notified immediately
  char cause[48];
  snprintf(cause, sizeof(cause), "close_%s@handleDeviceStopped", name);
  sensorStatusBumpWith(cause);
}

// Helper: set cause then bump (to preserve existing call-sites)
void sensorStatusBumpWith(const char* cause) {
  INFO_SENSORSF("Status bump: %s", cause ? cause : "(null)");
  gLastStatusCause = cause ? cause : "";
  sensorStatusBump();
}

const char* buildSensorStatusJson() {
  // PSRAM buffer allocated once, reused forever (zero stack impact)
  static char* buf = nullptr;
  static const size_t kBufSize = 1024;

  if (!buf) {
    buf = (char*)ps_alloc(kBufSize, AllocPref::PreferPSRAM, "sensor.status.json");
    if (!buf) {
      // Fallback to empty JSON on allocation failure
      static const char* kEmptyJson = "{}";
      return kEmptyJson;
    }
  }

  // Build JSON using ArduinoJson (stack-allocated, no heap churn)
  PSRAM_JSON_DOC(doc);
  
  // Basic sensor enable flags
  doc["seq"] = gSensorStatusSeq;
  doc["thermalEnabled"] = thermalEnabled;
  doc["tofEnabled"] = tofEnabled;
  doc["imuEnabled"] = imuEnabled;
  doc["apdsColorEnabled"] = apdsColorEnabled;
  doc["apdsProximityEnabled"] = apdsProximityEnabled;
  doc["apdsGestureEnabled"] = apdsGestureEnabled;
  doc["gamepadEnabled"] = gamepadEnabled;
#if ENABLE_SERVO
  doc["pwmDriverConnected"] = pwmDriverConnected;
#else
  doc["pwmDriverConnected"] = false;
#endif
  doc["gpsEnabled"] = gpsEnabled;
  doc["fmRadioEnabled"] = fmRadioEnabled;
#if ENABLE_RTC_SENSOR
  doc["rtcEnabled"] = rtcEnabled;
#else
  doc["rtcEnabled"] = false;
#endif
  
#if ENABLE_PRESENCE_SENSOR
  extern bool presenceEnabled;
  doc["presenceEnabled"] = presenceEnabled;
#else
  doc["presenceEnabled"] = false;
#endif

  // Compile-time capabilities (module compiled into firmware)
#if ENABLE_THERMAL_SENSOR
  doc["thermalCompiled"] = true;
#else
  doc["thermalCompiled"] = false;
#endif

#if ENABLE_TOF_SENSOR
  doc["tofCompiled"] = true;
#else
  doc["tofCompiled"] = false;
#endif

#if ENABLE_IMU_SENSOR
  doc["imuCompiled"] = true;
#else
  doc["imuCompiled"] = false;
#endif

#if ENABLE_GAMEPAD_SENSOR
  doc["gamepadCompiled"] = true;
#else
  doc["gamepadCompiled"] = false;
#endif

#if ENABLE_APDS_SENSOR
  doc["apdsCompiled"] = true;
#else
  doc["apdsCompiled"] = false;
#endif

#if ENABLE_GPS_SENSOR
  doc["gpsCompiled"] = true;
#else
  doc["gpsCompiled"] = false;
#endif

#if ENABLE_RTC_SENSOR
  doc["rtcCompiled"] = true;
#else
  doc["rtcCompiled"] = false;
#endif

#if ENABLE_PRESENCE_SENSOR
  doc["presenceCompiled"] = true;
#else
  doc["presenceCompiled"] = false;
#endif

  // Not modularized yet
  doc["fmradioCompiled"] = true;
  doc["servoCompiled"] = true;

#if ENABLE_CAMERA_SENSOR
  extern bool cameraEnabled;
  extern bool cameraStreaming;
  doc["cameraEnabled"] = cameraEnabled;
  doc["cameraStreaming"] = cameraStreaming;
  doc["cameraCompiled"] = true;
#else
  doc["cameraEnabled"] = false;
  doc["cameraStreaming"] = false;
  doc["cameraCompiled"] = false;
#endif

#if ENABLE_MICROPHONE_SENSOR
  extern bool micEnabled;
  extern bool micRecording;
  doc["micEnabled"] = micEnabled;
  doc["micRecording"] = micRecording;
  doc["micCompiled"] = true;
#else
  doc["micEnabled"] = false;
  doc["micRecording"] = false;
  doc["micCompiled"] = false;
#endif

#if ENABLE_EDGE_IMPULSE
  extern bool isEdgeImpulseModelLoaded();
  doc["eiEnabled"] = gSettings.edgeImpulseEnabled;
  doc["eiModelLoaded"] = isEdgeImpulseModelLoaded();
  doc["eiCompiled"] = true;
#else
  doc["eiEnabled"] = false;
  doc["eiModelLoaded"] = false;
  doc["eiCompiled"] = false;
#endif

  // Non-I2C sensors from registry (standardized format)
  JsonObject sensors = doc["sensors"].to<JsonObject>();
  for (size_t i = 0; i < nonI2CSensorsCount; i++) {
    const NonI2CSensorEntry& s = nonI2CSensors[i];
    JsonObject sensorObj = sensors[s.id].to<JsonObject>();
    sensorObj["connected"] = s.getConnected ? s.getConnected() : true;
    sensorObj["enabled"] = s.getEnabled ? s.getEnabled() : false;
    sensorObj["task"] = s.getTask ? s.getTask() : SENSOR_TASK_NONE;
    if (s.mlSettingsModule) {
      sensorObj["mlModule"] = s.mlSettingsModule;
    }
  }
  
  // Queue status
  doc["queueDepth"] = getQueueDepth();
  doc["thermalQueued"] = isInQueue(I2C_DEVICE_THERMAL);
  doc["tofQueued"] = isInQueue(I2C_DEVICE_TOF);
  doc["imuQueued"] = isInQueue(I2C_DEVICE_IMU);
  doc["apdsQueued"] = isInQueue(I2C_DEVICE_APDS);
  doc["gpsQueued"] = isInQueue(I2C_DEVICE_GPS);
  doc["gamepadQueued"] = isInQueue(I2C_DEVICE_GAMEPAD);
  doc["rtcQueued"] = isInQueue(I2C_DEVICE_RTC);
  doc["presenceQueued"] = isInQueue(I2C_DEVICE_PRESENCE);
  
  // Queue positions (only if present)
  int thermalPos = getQueuePosition(I2C_DEVICE_THERMAL);
  int tofPos = getQueuePosition(I2C_DEVICE_TOF);
  int imuPos = getQueuePosition(I2C_DEVICE_IMU);
  int apdsPos = getQueuePosition(I2C_DEVICE_APDS);
  int gpsPos = getQueuePosition(I2C_DEVICE_GPS);
  int gamepadPos = getQueuePosition(I2C_DEVICE_GAMEPAD);
  int rtcPos = getQueuePosition(I2C_DEVICE_RTC);
  int presencePos = getQueuePosition(I2C_DEVICE_PRESENCE);
  
  if (thermalPos > 0) {
    doc["thermalQueuePos"] = thermalPos;
  }
  if (tofPos > 0) {
    doc["tofQueuePos"] = tofPos;
  }
  if (imuPos > 0) {
    doc["imuQueuePos"] = imuPos;
  }
  if (apdsPos > 0) {
    doc["apdsQueuePos"] = apdsPos;
  }
  if (gpsPos > 0) {
    doc["gpsQueuePos"] = gpsPos;
  }
  if (gamepadPos > 0) {
    doc["gamepadQueuePos"] = gamepadPos;
  }
  if (rtcPos > 0) {
    doc["rtcQueuePos"] = rtcPos;
  }
  if (presencePos > 0) {
    doc["presenceQueuePos"] = presencePos;
  }
  
  // Serialize to buffer
  size_t required = measureJson(doc) + 1;
  if (required > kBufSize) {
    snprintf(buf, kBufSize, "{\"seq\":%lu,\"error\":\"status_too_large\",\"required\":%u}",
             (unsigned long)gSensorStatusSeq, (unsigned)required);
  } else {
    serializeJson(doc, buf, kBufSize);
  }
  
  return buf;
}

// ============================================================================
// Sensor Queue Processor Task (moved from HardwareOne.ino)
// ============================================================================

void sensorQueueProcessorTask(void* param) {
  DEBUG_CLIF("[QUEUE] Queue processor task started");
  static unsigned long lastSensorStartTime = 0;
  static I2CDeviceType lastI2CDeviceType = (I2CDeviceType)-1;
  
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (!mgr) {
    DEBUG_CLIF("[QUEUE] FATAL: I2C manager not initialized");
    vTaskDelete(nullptr);
    return;
  }

  while (true) {
    I2CDeviceStartRequest req;
    if (mgr->dequeueDeviceStart(&req)) {
      // Pause polling once for the entire batch of queued sensors.
      // This prevents already-running tasks (e.g. gamepad started during setup wizard)
      // from hammering the I2C bus with mutex timeouts while other sensors initialize.
      bool batchPaused = false;
      if (!gSensorPollingPaused) {
        mgr->pausePolling();
        INFO_I2CF("Paused polling for sensor initialization batch");
        batchPaused = true;
      }

      // Process all queued sensors in one batch while polling stays paused
      do {
      DEBUG_CLIF("[QUEUE] Processing queued sensor: type=%d, queuedAt=%lu",
                 req.device, req.queuedAt);

      // Stack instrumentation (do not assume any fixed stack size)
      if (isDebugFlagSet(DEBUG_MEMORY)) {
        UBaseType_t hwmWords = uxTaskGetStackHighWaterMark(NULL);
        DEBUG_MEMORYF("[STACK][QUEUE] before start type=%d hwm=%u words (%u bytes)",
                      (int)req.device, (unsigned)hwmWords, (unsigned)(hwmWords * 4));
      }

      // Calculate required delay based on LAST sensor type (to let it finish init)
      unsigned long requiredDelay = 0;
      if (lastI2CDeviceType != (I2CDeviceType)-1) {
        switch (lastI2CDeviceType) {
          case I2C_DEVICE_THERMAL:
            requiredDelay = 1500;  // Thermal needs longest init time
            break;
          case I2C_DEVICE_TOF:
            requiredDelay = 800;  // ToF needs medium init time
            break;
          case I2C_DEVICE_IMU:
            requiredDelay = 1000;  // IMU initialization can be slow
            break;
          case I2C_DEVICE_GAMEPAD:
            requiredDelay = 600;  // Gamepad init is relatively quick
            break;
          case I2C_DEVICE_APDS:
            requiredDelay = 600;  // APDS init is relatively quick
            break;
          case I2C_DEVICE_GPS:
            requiredDelay = 500;  // GPS init is quick (I2C setup only)
            break;
          case I2C_DEVICE_FMRADIO:
            requiredDelay = 600;  // FM radio init is relatively quick
            break;
          case I2C_DEVICE_RTC:
            requiredDelay = 300;  // RTC init is very quick
            break;
          case I2C_DEVICE_PRESENCE:
            requiredDelay = 400;  // Presence sensor init is relatively quick
            break;
        }
      }

      // Wait if a sensor was recently started (to let it complete init)
      if (lastSensorStartTime > 0 && requiredDelay > 0) {
        unsigned long elapsed = millis() - lastSensorStartTime;
        if (elapsed < requiredDelay) {
          unsigned long waitTime = requiredDelay - elapsed;
          DEBUG_CLIF("[QUEUE] Waiting for sensor (type=%d) to initialize", (int)lastI2CDeviceType);
          vTaskDelay(pdMS_TO_TICKS(waitTime));
        } else {
          DEBUG_CLIF("[QUEUE] Last sensor (type=%d) initialized - proceeding with sensor (type=%d)",
                     (int)lastI2CDeviceType, (int)req.device);
        }
      }

      // Safety check: skip if sensor is already running
      bool alreadyRunning = false;
      switch (req.device) {
        case I2C_DEVICE_THERMAL:
          alreadyRunning = thermalEnabled;
          break;
        case I2C_DEVICE_TOF:
          alreadyRunning = tofEnabled;
          break;
        case I2C_DEVICE_IMU:
          alreadyRunning = imuEnabled;
          break;
        case I2C_DEVICE_GAMEPAD:
          alreadyRunning = gamepadEnabled;
          break;
        case I2C_DEVICE_APDS:
          alreadyRunning = apdsColorEnabled || apdsProximityEnabled || apdsGestureEnabled;
          break;
        case I2C_DEVICE_GPS:
          alreadyRunning = gpsEnabled;
          break;
        case I2C_DEVICE_FMRADIO:
          alreadyRunning = fmRadioEnabled;
          break;
        case I2C_DEVICE_RTC:
#if ENABLE_RTC_SENSOR
          alreadyRunning = rtcEnabled;
#endif
          break;
        case I2C_DEVICE_PRESENCE:
#if ENABLE_PRESENCE_SENSOR
          { extern bool presenceEnabled;
          alreadyRunning = presenceEnabled; }
#endif
          break;
      }

      if (alreadyRunning) {
        DEBUG_CLIF("[QUEUE] Skipping sensor (already running): type=%d", req.device);
        sensorStatusBumpWith("queue@already_running");
      } else {

      // Use stack-efficient approach: discard result String, combine Serial calls
      switch (req.device) {
        case I2C_DEVICE_THERMAL:
          startThermalSensorInternal();
          INFO_SENSORSF("Thermal: %s", thermalEnabled ? "SUCCESS" : "FAILED");
          notifySensorStarted("Thermal", thermalEnabled);
          break;
        case I2C_DEVICE_TOF:
          startToFSensorInternal();
          INFO_SENSORSF("ToF: %s", tofEnabled ? "SUCCESS" : "FAILED");
          notifySensorStarted("ToF", tofEnabled);
          break;
        case I2C_DEVICE_IMU:
          startIMUSensorInternal();
          INFO_SENSORSF("IMU: %s", imuEnabled ? "SUCCESS" : "FAILED");
          notifySensorStarted("IMU", imuEnabled);
          break;
        case I2C_DEVICE_GAMEPAD:
          startGamepadInternal();
          INFO_SENSORSF("Gamepad: %s", gamepadEnabled ? "SUCCESS" : "FAILED");
          notifySensorStarted("Gamepad", gamepadEnabled);
          break;
        case I2C_DEVICE_APDS:
#if ENABLE_APDS_SENSOR
          startAPDSSensorInternal();
          { bool apdsOk = apdsColorEnabled || apdsProximityEnabled || apdsGestureEnabled;
          INFO_SENSORSF("APDS: %s (color=%d prox=%d gest=%d)",
                        apdsOk ? "SUCCESS" : "FAILED",
                        apdsColorEnabled ? 1 : 0, apdsProximityEnabled ? 1 : 0, apdsGestureEnabled ? 1 : 0);
          notifySensorStarted("APDS", apdsOk); }
#else
          INFO_SENSORSF("APDS: skipped (not compiled)");
#endif
          break;
        case I2C_DEVICE_GPS:
          startGPSInternal();
          INFO_SENSORSF("GPS: %s", gpsEnabled ? "SUCCESS" : "FAILED");
          notifySensorStarted("GPS", gpsEnabled);
          break;
        case I2C_DEVICE_FMRADIO:
#if ENABLE_FM_RADIO
          startFMRadioInternal();
          INFO_SENSORSF("FM Radio: %s", fmRadioEnabled ? "SUCCESS" : "FAILED");
          notifySensorStarted("FM Radio", fmRadioEnabled);
#else
          INFO_SENSORSF("FM Radio: skipped (not compiled)");
#endif
          break;
        case I2C_DEVICE_RTC:
#if ENABLE_RTC_SENSOR
          startRTCSensorInternal();
          INFO_SENSORSF("RTC: %s", rtcEnabled ? "SUCCESS" : "FAILED");
          notifySensorStarted("RTC", rtcEnabled);
#else
          INFO_SENSORSF("RTC: skipped (not compiled)");
#endif
          break;
        case I2C_DEVICE_PRESENCE:
#if ENABLE_PRESENCE_SENSOR
          extern bool startPresenceSensorInternal();
          extern bool presenceEnabled;
          startPresenceSensorInternal();
          INFO_SENSORSF("Presence: %s", presenceEnabled ? "SUCCESS" : "FAILED");
          notifySensorStarted("Presence", presenceEnabled);
#else
          INFO_SENSORSF("Presence: skipped (not compiled)");
#endif
          break;
      }

      if (isDebugFlagSet(DEBUG_MEMORY)) {
        UBaseType_t hwmWords = uxTaskGetStackHighWaterMark(NULL);
        INFO_MEMORYF("[STACK][QUEUE] after  start type=%d hwm=%u words (%u bytes)",
                      (int)req.device, (unsigned)hwmWords, (unsigned)(hwmWords * 4));
      }

      lastSensorStartTime = millis();
      lastI2CDeviceType = req.device;  // Track this sensor for next iteration's delay

      // Force stack and heap check after sensor start (high resource usage point)
      if (isDebugFlagSet(DEBUG_MEMORY)) {
        UBaseType_t stackHighWater = uxTaskGetStackHighWaterMark(NULL);
        const uint32_t sensorQueueStackWords = 3072;
        uint32_t stackPeak = (sensorQueueStackWords * 4) - (stackHighWater * 4);
        int peakPct = (stackPeak * 100) / (sensorQueueStackWords * 4);
        size_t heapFree = ESP.getFreeHeap();
        size_t heapMin = ESP.getMinFreeHeap();
        DEBUG_MEMORYF("[STACK] sensor_queue: peak=%lu bytes (%d%%), free_min=%lu bytes | heap=%lu min=%lu",
                      (unsigned long)stackPeak, peakPct,
                      (unsigned long)(stackHighWater * 4),
                      (unsigned long)heapFree, (unsigned long)heapMin);
      }

      // Note: Each sensor's start function already calls sensorStatusBumpWith(),
      // so we don't need to bump here (would cause redundant SSE events)

      } // end else (not alreadyRunning)

      } while (mgr->dequeueDeviceStart(&req)); // drain entire batch

      // Resume sensor polling after ALL queued sensors are initialized
      if (batchPaused) {
        mgr->resumePolling();
        INFO_I2CF("Resumed sensor polling after initialization batch");
      }
    } else {
      // Queue empty, sleep for a bit
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

// ============================================================================
// I2C Settings Module (for modular settings registry)
// ============================================================================

// I2C settings are always available but only apply when enabled
// This allows runtime toggling without recompiling (reboot required)
static const SettingEntry i2cSettingEntries[] = {
  { "i2cBusEnabled", SETTING_BOOL, &gSettings.i2cBusEnabled, 1, 0, nullptr, 0, 1, "I2C Bus Enabled (reboot required)", nullptr },
  { "i2cSensorsEnabled", SETTING_BOOL, &gSettings.i2cSensorsEnabled, 1, 0, nullptr, 0, 1, "I2C Sensors Enabled", nullptr },
  { "i2cSdaPin", SETTING_INT, &gSettings.i2cSdaPin, I2C_SDA_PIN_DEFAULT,
    0, nullptr, 0, 48, "I2C SDA Pin (reboot required)", nullptr },
  { "i2cSclPin", SETTING_INT, &gSettings.i2cSclPin, I2C_SCL_PIN_DEFAULT,
    0, nullptr, 0, 48, "I2C SCL Pin (reboot required)", nullptr },
  { "i2cClockThermalHz", SETTING_INT, &gSettings.i2cClockThermalHz, 800000, 0, nullptr, 100000, 1000000, "Thermal I2C Clock (Hz)", nullptr },
  { "i2cClockToFHz", SETTING_INT, &gSettings.i2cClockToFHz, 200000, 0, nullptr, 50000, 400000, "ToF I2C Clock (Hz)", nullptr }
};

extern const SettingsModule i2cSettingsModule = {
  "i2c", "i2c", i2cSettingEntries,
  sizeof(i2cSettingEntries) / sizeof(i2cSettingEntries[0])
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp

// ============================================================================
// Process Sensor Auto-Start on Boot
// Note: autoStart settings are now in each sensor's own module:
// - thermal (i2csensor-mlx90640.cpp)
// - tof (i2csensor-vl53l4cx.cpp)
// - imu (i2csensor-bno055.cpp)
// - gps (i2csensor-pa1010d.cpp)
// - fmradio (i2csensor-rda5807.cpp)
// - apds (i2csensor-apds9960.cpp)
// - gamepad (i2csensor-seesaw.cpp)
// ============================================================================

void processAutoStartSensors() {
  // Debug: Print I2C flags to diagnose auto-start issues
  DEBUG_I2CF("[AutoStart] I2C check: i2cBus=%d i2cSensors=%d",
                gSettings.i2cBusEnabled ? 1 : 0,
                gSettings.i2cSensorsEnabled ? 1 : 0);
  
  if (!gSettings.i2cBusEnabled || !gSettings.i2cSensorsEnabled) {
    INFO_I2CF("[AutoStart] I2C disabled, skipping sensor auto-start");
    return;
  }

 #if ENABLE_I2C_SYSTEM
  if (!queueProcessorTask) {
    const uint32_t queueStackWords = SENSOR_QUEUE_STACK_WORDS;
    if (xTaskCreateLogged(sensorQueueProcessorTask, "sensor_queue_task", queueStackWords, nullptr, 1, &queueProcessorTask, "sensor.queue") != pdPASS) {
      ERROR_I2CF("[I2C_SENSORS] Failed to create sensor queue processor task (late init)");
      queueProcessorTask = nullptr;
      return;
    }
    INFO_I2CF("[I2C_SENSORS] Queue processor task created (late init)");
  }
 #endif
  
  INFO_I2CF("[AutoStart] Processing sensor auto-start settings...");
  
  // Debug: Print all auto-start flag values to diagnose first-time setup issues
  DEBUG_I2CF("[AutoStart] Flags: thermal=%d tof=%d imu=%d gps=%d fmradio=%d apds=%d gamepad=%d rtc=%d presence=%d",
                gSettings.thermalAutoStart ? 1 : 0,
                gSettings.tofAutoStart ? 1 : 0,
                gSettings.imuAutoStart ? 1 : 0,
                gSettings.gpsAutoStart ? 1 : 0,
                gSettings.fmRadioAutoStart ? 1 : 0,
                gSettings.apdsAutoStart ? 1 : 0,
                gSettings.gamepadAutoStart ? 1 : 0,
                gSettings.rtcAutoStart ? 1 : 0,
                gSettings.presenceAutoStart ? 1 : 0);
  
  #if ENABLE_THERMAL_SENSOR
  if (gSettings.thermalAutoStart) {
    if (isSensorConnected("thermal")) {
      INFO_I2CF("[AutoStart] Queuing thermal sensor");
      enqueueDeviceStart(I2C_DEVICE_THERMAL);
    } else {
      INFO_I2CF("[AutoStart] Skipping thermal sensor (not detected on I2C bus)");
    }
  }
  #endif
  
  #if ENABLE_TOF_SENSOR
  if (gSettings.tofAutoStart) {
    if (isSensorConnected("tof")) {
      INFO_I2CF("[AutoStart] Queuing ToF sensor");
      enqueueDeviceStart(I2C_DEVICE_TOF);
    } else {
      INFO_I2CF("[AutoStart] Skipping ToF sensor (not detected on I2C bus)");
    }
  }
  #endif
  
  #if ENABLE_IMU_SENSOR
  if (gSettings.imuAutoStart) {
    if (isSensorConnected("imu")) {
      INFO_I2CF("[AutoStart] Queuing IMU sensor");
      enqueueDeviceStart(I2C_DEVICE_IMU);
    } else {
      INFO_I2CF("[AutoStart] Skipping IMU sensor (not detected on I2C bus)");
    }
  }
  #endif
  
  #if ENABLE_GPS_SENSOR
  if (gSettings.gpsAutoStart) {
    if (isSensorConnected("gps")) {
      INFO_I2CF("[AutoStart] Queuing GPS sensor");
      enqueueDeviceStart(I2C_DEVICE_GPS);
    } else {
      INFO_I2CF("[AutoStart] Skipping GPS sensor (not detected on I2C bus)");
    }
  }
  #endif
  
  #if ENABLE_FMRADIO_SENSOR
  if (gSettings.fmRadioAutoStart) {
    if (isSensorConnected("fmradio")) {
      INFO_I2CF("[AutoStart] Queuing FM Radio sensor");
      enqueueDeviceStart(I2C_DEVICE_FMRADIO);
    } else {
      INFO_I2CF("[AutoStart] Skipping FM Radio sensor (not detected on I2C bus)");
    }
  }
  #endif
  
  #if ENABLE_APDS_SENSOR
  if (gSettings.apdsAutoStart) {
    if (isSensorConnected("apds")) {
      INFO_I2CF("[AutoStart] Queuing APDS sensor");
      enqueueDeviceStart(I2C_DEVICE_APDS);
    } else {
      INFO_I2CF("[AutoStart] Skipping APDS sensor (not detected on I2C bus)");
    }
  }
  #endif
  
  #if ENABLE_GAMEPAD_SENSOR
  if (gSettings.gamepadAutoStart) {
    if (isSensorConnected("gamepad")) {
      INFO_I2CF("[AutoStart] Queuing Gamepad sensor");
      enqueueDeviceStart(I2C_DEVICE_GAMEPAD);
    } else {
      INFO_I2CF("[AutoStart] Skipping Gamepad sensor (not detected on I2C bus)");
    }
  }
  #endif
  
  #if ENABLE_RTC_SENSOR
  if (gSettings.rtcAutoStart) {
    if (isSensorConnected("rtc")) {
      INFO_I2CF("[AutoStart] Queuing RTC sensor");
      enqueueDeviceStart(I2C_DEVICE_RTC);
    } else {
      INFO_I2CF("[AutoStart] Skipping RTC sensor (not detected on I2C bus)");
    }
  }
  #endif
  
  #if ENABLE_PRESENCE_SENSOR
  if (gSettings.presenceAutoStart) {
    if (isSensorConnected("presence")) {
      INFO_I2CF("[AutoStart] Queuing Presence sensor");
      enqueueDeviceStart(I2C_DEVICE_PRESENCE);
    } else {
      INFO_I2CF("[AutoStart] Skipping Presence sensor (not detected on I2C bus)");
    }
  }
  #endif
  
  INFO_I2CF("[AutoStart] Sensor auto-start processing complete");
}
