/**
 * Sensor System - I2C Sensor Tasks and Commands
 * i2c_system.cpp
 * I2C sensor task implementations and management
 * Extracted from HardwareOne.ino
 */

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Wire.h>
#include <esp_log.h>   // esp_log_level_set — quiet probe NACKs during detect scan
#include <soc/soc_caps.h>  // SOC_GPIO_PIN_COUNT — board's GPIO count for pin-range limits

#include "i2csensor_rda5807.h"
#include "System_BuildConfig.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_FirstTimeSetup.h"
#include "System_FeatureRegistry.h"  // getFeatureById/isFeatureEnabled — hardware-detect diff
#include "System_PollPause.h"        // PollPauseGuard — quiesce a bus during the detect scan
#include "System_AuthIdentity.h"     // currentExecIsAdmin — gate 'detect apply'
#include "System_I2C.h"
#include "System_RamFlush.h"
#include "System_Logging.h"
#include "System_Mutex.h"
#include "System_MemUtil.h"
#include "System_Notifications.h"
#include "System_SensorRegistry.h"
#include "System_SensorStubs.h"
#include "System_Events.h"          // systemEventPost — sensor lifecycle events

#if ENABLE_ESPNOW
#include "System_ESPNow.h"
#include "System_ESPNow_Sensors.h"
#endif
#include "System_Settings.h"
#include "System_TaskUtils.h"
#include "System_Utils.h"

#if ENABLE_APDS_SENSOR
#include "Adafruit_APDS9960.h"
#include "i2csensor_apds9960.h"
#endif
#if ENABLE_GAMEPAD_SENSOR
#include "Adafruit_seesaw.h"
#include "i2csensor_seesaw.h"
#endif
#if ENABLE_GPS_SENSOR
#include <Adafruit_GPS.h>
#include "i2csensor_pa1010d.h"
#endif
#if ENABLE_IMU_SENSOR
#include <Adafruit_BNO055.h>
#include <Adafruit_Sensor.h>
#include "i2csensor_bno055.h"
#endif
#if ENABLE_THERMAL_SENSOR
#include <Adafruit_MLX90640.h>
#include "i2csensor_mlx90640.h"
#endif
#if ENABLE_TOF_SENSOR
#include "i2csensor_vl53l4cx.h"
#include "vl53l4cx_class.h"
#endif
#if ENABLE_RTC_SENSOR
#include "i2csensor_ds3231.h"
#endif
#if ENABLE_PRESENCE_SENSOR
#include "i2csensor_sths34pf80.h"
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
  #if !ENABLE_OLED_INPUT
    if (strcmp(sensor.moduleName, "input") == 0) return false;
  #endif
  // Legacy "gamepad" and "anoencoder" module names (pre-unification) — kept
  // here so any external code path that still passes the old strings to
  // isSensorConnected() / lookup helpers reports a sensible result.
  #if !ENABLE_GAMEPAD_SENSOR
    if (strcmp(sensor.moduleName, "gamepad") == 0) return false;
  #endif
  #if !ENABLE_ANO_ENCODER
    if (strcmp(sensor.moduleName, "anoencoder") == 0) return false;
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
  INFO_I2C_DISCOVERYF("[I2C_REGISTRY] Device manager initialized with capacity for %d devices", I2CDeviceManager::MAX_DEVICES);
  
  
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
        INFO_I2C_DISCOVERYF("Pre-registered compiled device: 0x%02X (%s)", sensor.address, sensor.name);
      } else {
        ERROR_I2CF("Failed to pre-register compiled device: 0x%02X (%s)", sensor.address, sensor.name);
      }
    }
  }
  
  INFO_I2C_DISCOVERYF("Pre-registered %d compiled devices from database", compiledCount);
  
  // Print registry summary
  INFO_I2C_DISCOVERYF("[I2C_REGISTRY] Registration summary: %d/%d slots used (%d available)", 
                compiledCount, I2CDeviceManager::MAX_DEVICES, 
                I2CDeviceManager::MAX_DEVICES - compiledCount);
}

// Global I2C bus enabled flag (mirrors gSettings.i2cEnabled)
// Defaults to false; set correctly inside initI2CBuses() before any transactions run
bool gI2CBusRunning = false;

// The sensor-polling pause primitive (gSensorPollingPaused mirror + the
// reference-counted pollPause/pollResume / sensorPolling* ops) moved to its own
// module, System_PollPause.{h,cpp}. The API arrives here via System_I2C.h's
// include of System_PollPause.h, so the readers and callers below are unchanged.

// Queue processor task handle
TaskHandle_t queueProcessorTask = nullptr;

// gImuTaskHandle and gImuLastStopTime are declared in i2csensor_bno055.h (included above)
// Clock management is now unified through I2CDeviceManager
// Legacy i2cSetWire1Clock() removed - all sensors use i2cDeviceTransaction wrapper

// Sensor status system dependencies
extern volatile unsigned long gSensorStatusSeq;
extern const char* gLastStatusCause;
extern void sensorStatusBump();
// gApds{Color,Proximity,Gesture}Enabled provided by i2csensor_apds9960.h (included above)
#if ENABLE_SERVO
extern bool gPwmDriverConnected;
#endif

// BROADCAST_PRINTF now defined in debug_system.h with performance optimizations

// Sensor connection status — provided by the per-sensor i2csensor_*.h headers (all included above)

// ============================================================================
// I2C Clock Management (Wire1)
// ============================================================================

// Device Registry Global Variables (definitions)
// Array of devices detected during I2C scan
EXT_RAM_BSS_ATTR ConnectedDevice connectedDevices[MAX_CONNECTED_DEVICES];
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
  // Both seesaw I2C addresses (0x49 + 0x50) belong to the unified "input"
  // module — either the seesaw Mini Gamepad or the ANO Rotary Encoder can
  // appear at either address depending on hardware jumpers. The compiled-in
  // driver disambiguates at runtime; the descriptions show the relevant
  // device for the active build.
#if ENABLE_ANO_ENCODER
  { 0x49, "Seesaw", "ANO Rotary Encoder (default)", "Adafruit", false, 0x00, 800, "Adafruit_seesaw", "_ADAFRUIT_SEESAW_H_", "input", 400000, 200 },
  { 0x50, "Seesaw", "ANO Rotary Encoder (alt addr)", "Adafruit", false, 0x00, 800, "Adafruit_seesaw", "_ADAFRUIT_SEESAW_H_", "input", 400000, 200 },
#else
  { 0x50, "Seesaw", "Mini I2C Gamepad",              "Adafruit", false, 0x00, 800, "Adafruit_seesaw", "_ADAFRUIT_SEESAW_H_", "input", 400000, 200 },
  { 0x49, "Seesaw", "Mini I2C Gamepad (alt addr)",   "Adafruit", false, 0x00, 800, "Adafruit_seesaw", "_ADAFRUIT_SEESAW_H_", "input", 400000, 200 },
#endif
  { 0x33, "MLX90640", "32x24 Thermal Camera", "Adafruit", false, 0x00, 2000, "Adafruit_MLX90640", "_ADAFRUIT_MLX90640_H_", "thermal", 100000, 500 },
  { 0x10, "PA1010D", "Mini GPS Module", "Adafruit", false, 0x00, 500, "Adafruit_GPS", "_ADAFRUIT_GPS_H", "gps", 100000, 200 },
  { 0x11, "RDA5807", "FM Radio Receiver", "ScoutMakes", false, 0x00, 500, "RDA5807", NULL, "fmradio", 100000, 200 },
  { 0x68, "DS3231", "Precision RTC", "Adafruit", false, 0x00, 100, NULL, NULL, "rtc", 100000, 100 },
  { 0x5A, "STHS34PF80", "IR Presence/Motion", "ST", false, 0x00, 200, NULL, NULL, "presence", 100000, 200 },
  
  // Detected Infrastructure (no CLI modules)
  { 0x3D, "SSD1306", "OLED 128x64 Display", "Adafruit", true, 0x3C, 0, NULL, NULL, NULL, 400000, 50 },
#if BATTERY_BACKEND_FUEL_GAUGE
  // On-board LiPo fuel gauge (battery backend, not a toggleable sensor) — listed
  // so the I2C discovery scan probes 0x36 and names it instead of "Unknown".
  { 0x36, "MAX17048", "LiPo Fuel Gauge", "Maxim", false, 0x00, 0, NULL, NULL, NULL, 400000, 100 },
#endif
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
  INFO_I2C_DISCOVERYF("initSensorQueue() - queue managed by I2CDeviceManager");
}

// =========================================================================
// Queued Sensor Start Commands (moved from .ino)
// =========================================================================

// gThermalRunning / gTofRunning / gImuRunning provided by their i2csensor_*.h headers

// Map I2CDeviceType → I2C address for pre-start hardware presence checks.
// Returns 0 if the device type has no single fixed address (or should skip the check).
static uint8_t i2cAddressForDeviceType(I2CDeviceType sensor) {
  switch (sensor) {
    case I2C_DEVICE_THERMAL:  return I2C_ADDR_THERMAL;
    case I2C_DEVICE_TOF:      return I2C_ADDR_TOF;
    case I2C_DEVICE_IMU:      return I2C_ADDR_IMU;
    case I2C_DEVICE_INPUT:
#if ENABLE_ANO_ENCODER
      return (gSettings.anoEncoderI2cAddr > 0 && gSettings.anoEncoderI2cAddr < 0x80)
               ? (uint8_t)gSettings.anoEncoderI2cAddr : I2C_ADDR_ANO_ENCODER;
#else
      return I2C_ADDR_GAMEPAD;
#endif
    case I2C_DEVICE_GPS:      return I2C_ADDR_GPS;
    case I2C_DEVICE_FMRADIO:  return I2C_ADDR_FM_RADIO;
    case I2C_DEVICE_APDS:     return I2C_ADDR_APDS;
    case I2C_DEVICE_RTC:      return I2C_ADDR_DS3231;
    case I2C_DEVICE_PRESENCE: return I2C_ADDR_PRESENCE;
    default:                  return 0;
  }
}

// Externally linkable so HAL_Input.cpp's unified cmd_openinput can dispatch
// through the same queue as the other sensor start commands.
const char* cmd_sensorstart_queued(I2CDeviceType sensor, const char* displayName, const bool& enabledFlag, const char* eventTag) {
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  // Reject immediately if the I2C bus is disabled at runtime (distinct from hardware not connected)
  if (!gI2CBusRunning) {
    snprintf(getDebugBuffer(), 1024, "[%s] I2C bus is disabled — enable it in settings and reboot", displayName);
    return getDebugBuffer();
  }

  if (enabledFlag) {
    snprintf(getDebugBuffer(), 1024, "%s sensor already running", displayName);
    return getDebugBuffer();
  }
  if (isInQueue(sensor)) {
    int pos = getQueuePosition(sensor);
    snprintf(getDebugBuffer(), 1024, "%s sensor already queued (position %d)", displayName, pos);
    return getDebugBuffer();
  }

  // Verify hardware is physically present before attempting start
  uint8_t addr = i2cAddressForDeviceType(sensor);
  if (addr != 0 && !i2cPingAddress(addr, 100000, 50)) {
    snprintf(getDebugBuffer(), 1024, "[%s] Not detected on I2C bus", displayName);
    return getDebugBuffer();
  }

  if (enqueueDeviceStart(sensor)) {
    sensorStatusBumpWith(eventTag);
    int pos = getQueuePosition(sensor);
    snprintf(getDebugBuffer(), 1024, "%s sensor queued for start (position %d, queue depth: %d)",
             displayName, pos, getQueueDepth());
    return getDebugBuffer();
  } else {
    snprintf(getDebugBuffer(), 1024, "Error: Failed to queue %s sensor (queue full)", displayName);
    return getDebugBuffer();
  }
}

const char* cmd_thermalstart_queued(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return cmd_sensorstart_queued(I2C_DEVICE_THERMAL, "Thermal", gThermalRunning, "openthermal@enqueue");
}

const char* cmd_tofstart_queued(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return cmd_sensorstart_queued(I2C_DEVICE_TOF, "ToF", gTofRunning, "opentof@enqueue");
}

const char* cmd_imustart_queued(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return cmd_sensorstart_queued(I2C_DEVICE_IMU, "IMU", gImuRunning, "openimu@enqueue");
}

const char* cmd_apdsstart_queued(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return cmd_sensorstart_queued(I2C_DEVICE_APDS, "APDS", gApdsColorRunning || gApdsProximityRunning || gApdsGestureRunning, "openapds@enqueue");
}

// cmd_openinput (in HAL_Input.cpp) handles the input device's queued start —
// the gamepad-specific wrapper that used to live here has been deleted now
// that there's only one unified entrypoint for either driver.

// ========== End Sensor Startup Queue System ==========

// ========== I2C Bus Initialization ==========

// Track if we've already logged the "bus disabled" message (to avoid spam)
static bool gI2CBusDisabledLogged = false;

void initI2CBuses() {
  // During first-time setup, force I2C enabled so OLED wizard can run
  // User can disable I2C in wizard, which takes effect after reboot
  bool forceForSetup = isFirstTimeSetup();
  
  if (forceForSetup) {
    gI2CBusRunning = true;
    INFO_I2C_BUSF("[I2C] Force-enabling for first-time setup wizard");
  } else {
    // Copy setting to global flag
    gI2CBusRunning = gSettings.i2cEnabled;
  }
  
  // Early exit if I2C bus is disabled via settings (and not first-time setup)
  if (!gI2CBusRunning) {
    if (!gI2CBusDisabledLogged) {
      INFO_I2C_BUSF("[I2C] Bus disabled via settings - skipping initialization");
      INFO_I2C_BUSF("[I2C] OLED display and I2C sensors will not be available");
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

  // Bus warm-up: perform a dummy probe on each initialized bus to exercise
  // the ESP32 I2C hardware. Without OLED or RTC active, zero I2C activity
  // occurs between init and discovery, which can leave the bus in an
  // unreliable state for the first real transaction. Probe address 0x00
  // (general call) — no device will ACK, but the bus is exercised.
  for (uint8_t b = 0; b < I2CDeviceManager::NUM_BUSES; b++) {
    if (mgr && mgr->isBusInitialized(b)) {
      TwoWire* wire = mgr->getWire(b);
      if (wire) {
        wire->beginTransmission(0x00);
        wire->endTransmission();
      }
    }
  }
  delayMicroseconds(100);
}

// ========== End I2C Bus Initialization ==========

void i2cResetGracePeriod(uint8_t address) {
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (!mgr) return;
  I2CDevice* dev = mgr->getDevice(address);
  if (dev) dev->resetGracePeriod();
}

const char* cmd_i2chealth(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (!mgr) return "Error: I2C manager not initialized";

  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    doc["deviceCount"] = mgr->getDeviceCount();
    JsonArray arr = doc["devices"].to<JsonArray>();
    for (int i = 0; i < mgr->getDeviceCount(); i++) {
      I2CDevice* dev = &mgr->devices[i];
      if (!dev->isInitialized()) continue;
      const I2CDevice::Health& h = dev->getHealth();
      JsonObject o = arr.add<JsonObject>();
      o["address"]            = dev->address;
      o["name"]               = dev->name ? dev->name : "?";
      o["consecutiveErrors"]  = h.consecutiveErrors;
      o["totalErrors"]        = h.totalErrors;
      o["degraded"]           = dev->isDegraded();
      o["nack"]               = h.nackCount;
      o["timeout"]            = h.timeoutCount;
      o["busError"]           = h.busErrorCount;
      o["adaptiveTimeoutMs"]  = (unsigned long)dev->getAdaptiveTimeout();
    }
    serializeJson(doc, getDebugBuffer(), 1024);
    return getDebugBuffer();
  }

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

const char* cmd_i2cmetrics(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (!mgr) return "Error: I2C manager not initialized";

  const I2CBusMetrics& metrics = mgr->getMetrics();

  if (argWantsJson(argsInput)) {
    unsigned long upSec = (millis() - metrics.lastResetMs) / 1000;
    snprintf(getDebugBuffer(), 1024,
      "{\"schema\":1,\"uptimeSec\":%lu,\"totalTransactions\":%lu,\"mutexTimeouts\":%lu,"
      "\"busContentions\":%lu,\"totalBytes\":%lu}",
      upSec, (unsigned long)metrics.totalTransactions, (unsigned long)metrics.mutexTimeouts,
      (unsigned long)metrics.mutexContentions, (unsigned long)metrics.totalBytesTransferred);
    return getDebugBuffer();
  }

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

// Highest usable GPIO number on the chip this firmware is compiled for, taken
// from the SoC capabilities (classic ESP32 = 39, ESP32-S3 = 48). The I2C pin
// commands derive their upper bound from this so the limit tracks the running
// board instead of a hardcoded cap that was only correct for one chip.
#define HW_GPIO_MAX (SOC_GPIO_PIN_COUNT - 1)
#if SOC_GPIO_PIN_COUNT >= 49
  #define HW_GPIO_MAX_STR "48"
#elif SOC_GPIO_PIN_COUNT >= 40
  #define HW_GPIO_MAX_STR "39"
#else
  #define HW_GPIO_MAX_STR "max GPIO"
#endif

// ========== I2C Infrastructure Commands ==========

const char* cmd_i2cbusenabled(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = argsInput;
  valStr.trim();
  if (valStr.length() == 0) return "Error: invalid arguments — Usage: i2cBusEnabled <0|1> (reboot required)";
  bool v = (valStr.toInt() != 0);
  setSetting(gSettings.i2cEnabled, v);
  snprintf(getDebugBuffer(), 1024, "i2cBusEnabled set to %d (reboot required)", (int)v);
  return getDebugBuffer();
}

const char* cmd_i2csdapin(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = argsInput;
  valStr.trim();
  if (valStr.length() == 0) return "Error: invalid arguments — Usage: i2cSdaPin <0.." HW_GPIO_MAX_STR "> (max GPIO for this board; reboot required)";
  int v = valStr.toInt();
  if (v < 0) v = 0;
  if (v > HW_GPIO_MAX) v = HW_GPIO_MAX;
  setSetting(gSettings.i2cSdaPin, v);
  snprintf(getDebugBuffer(), 1024, "i2cSdaPin set to %d (reboot required)", v);
  return getDebugBuffer();
}

const char* cmd_i2csclpin(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = argsInput;
  valStr.trim();
  if (valStr.length() == 0) return "Error: invalid arguments — Usage: i2cSclPin <0.." HW_GPIO_MAX_STR "> (max GPIO for this board; reboot required)";
  int v = valStr.toInt();
  if (v < 0) v = 0;
  if (v > HW_GPIO_MAX) v = HW_GPIO_MAX;
  setSetting(gSettings.i2cSclPin, v);
  snprintf(getDebugBuffer(), 1024, "i2cSclPin set to %d (reboot required)", v);
  return getDebugBuffer();
}

// ---- Bus 1 (I2C2) variants ----
// Same shape as the bus 0 commands above, plus:
//   * Lower bound is -1 (vs 0 on bus 0) — -1 means "unavailable on this board"
//     (sentinel used on XIAO / QT Py / Feather V2 where there is no second I2C
//     port). Settings page surfaces this as the "(-1=unavailable)" hint.
//   * Upper bound is HW_GPIO_MAX (the SoC's highest GPIO: 39 on classic ESP32,
//     48 on ESP32-S3), shared with the bus-0 commands so both track the board.
const char* cmd_i2c2busenabled(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = argsInput;
  valStr.trim();
  if (valStr.length() == 0) return "Error: invalid arguments — Usage: i2c2BusEnabled <0|1> (reboot required)";
  bool v = (valStr.toInt() != 0);
  setSetting(gSettings.i2c2Enabled, v);
  snprintf(getDebugBuffer(), 1024, "i2c2BusEnabled set to %d (reboot required)", (int)v);
  return getDebugBuffer();
}

const char* cmd_i2c2sdapin(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = argsInput;
  valStr.trim();
  if (valStr.length() == 0) return "Error: invalid arguments — Usage: i2c2SdaPin <-1.." HW_GPIO_MAX_STR "> (-1=unavailable; max GPIO for this board; reboot required)";
  int v = valStr.toInt();
  if (v < -1) v = -1;
  if (v > HW_GPIO_MAX) v = HW_GPIO_MAX;
  setSetting(gSettings.i2c2SdaPin, v);
  snprintf(getDebugBuffer(), 1024, "i2c2SdaPin set to %d (reboot required)", v);
  return getDebugBuffer();
}

const char* cmd_i2c2sclpin(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = argsInput;
  valStr.trim();
  if (valStr.length() == 0) return "Error: invalid arguments — Usage: i2c2SclPin <-1.." HW_GPIO_MAX_STR "> (-1=unavailable; max GPIO for this board; reboot required)";
  int v = valStr.toInt();
  if (v < -1) v = -1;
  if (v > HW_GPIO_MAX) v = HW_GPIO_MAX;
  setSetting(gSettings.i2c2SclPin, v);
  snprintf(getDebugBuffer(), 1024, "i2c2SclPin set to %d (reboot required)", v);
  return getDebugBuffer();
}

// ---- Per-device bus assignment commands ----
// Each lets the user route a specific sensor to bus 0 (Wire1 / I2C1) or
// bus 1 (Wire / I2C2). All clamp to 0..NUM_BUSES-1 and require a reboot
// for the sensor's task to pick up the new value (drivers cache the bus
// at init time + the library object is constructed with the chosen Wire).
// One helper handles validate + parse + clamp + setSetting + report so the
// per-command stub is one line. The helper does NOT include the
// RETURN_VALID_IF_VALIDATE_CSTR macro because that returns from its caller
// — must stay at each cmd_* entry point.
static const char* setDeviceBusAndReport(int& target, const String& argsInput, const char* settingName) {
  String valStr = argsInput;
  valStr.trim();
  if (valStr.length() == 0) {
    if (ensureDebugBuffer()) {
      snprintf(getDebugBuffer(), 1024, "Usage: %s <0..%d> (reboot required)",
               settingName, (int)(I2CDeviceManager::NUM_BUSES - 1));
      return getDebugBuffer();
    }
    return "Error: invalid arguments — Usage: <bus> (reboot required)";
  }
  int v = valStr.toInt();
  if (v < 0) v = 0;
  if (v >= (int)I2CDeviceManager::NUM_BUSES) v = I2CDeviceManager::NUM_BUSES - 1;
  setSetting(target, v);
  if (ensureDebugBuffer()) {
    snprintf(getDebugBuffer(), 1024, "%s set to %d (reboot required)", settingName, v);
    return getDebugBuffer();
  }
  return "set (reboot required)";
}

const char* cmd_oledbus(const String& a)     { RETURN_VALID_IF_VALIDATE_CSTR(); return setDeviceBusAndReport(gSettings.oledBus,     a, "oledBus"); }
const char* cmd_inputbus(const String& a)    { RETURN_VALID_IF_VALIDATE_CSTR(); return setDeviceBusAndReport(gSettings.inputBus,  a, "inputBus"); }
const char* cmd_gpsbus(const String& a)      { RETURN_VALID_IF_VALIDATE_CSTR(); return setDeviceBusAndReport(gSettings.gpsBus,      a, "gpsBus"); }
const char* cmd_rtcbus(const String& a)      { RETURN_VALID_IF_VALIDATE_CSTR(); return setDeviceBusAndReport(gSettings.rtcBus,      a, "rtcBus"); }
const char* cmd_fmradiobus(const String& a)  { RETURN_VALID_IF_VALIDATE_CSTR(); return setDeviceBusAndReport(gSettings.fmRadioBus,  a, "fmRadioBus"); }
const char* cmd_presencebus(const String& a) { RETURN_VALID_IF_VALIDATE_CSTR(); return setDeviceBusAndReport(gSettings.presenceBus, a, "presenceBus"); }
const char* cmd_imubus(const String& a)      { RETURN_VALID_IF_VALIDATE_CSTR(); return setDeviceBusAndReport(gSettings.imuBus,      a, "imuBus"); }
const char* cmd_thermalbus(const String& a)  { RETURN_VALID_IF_VALIDATE_CSTR(); return setDeviceBusAndReport(gSettings.thermalBus,  a, "thermalBus"); }
const char* cmd_tofbus(const String& a)      { RETURN_VALID_IF_VALIDATE_CSTR(); return setDeviceBusAndReport(gSettings.tofBus,      a, "tofBus"); }
const char* cmd_apdsbus(const String& a)     { RETURN_VALID_IF_VALIDATE_CSTR(); return setDeviceBusAndReport(gSettings.apdsBus,     a, "apdsBus"); }
const char* cmd_servobus(const String& a)    { RETURN_VALID_IF_VALIDATE_CSTR(); return setDeviceBusAndReport(gSettings.servoBus,    a, "servoBus"); }
const char* cmd_fuelgaugebus(const String& a){ RETURN_VALID_IF_VALIDATE_CSTR(); return setDeviceBusAndReport(gSettings.fuelGaugeBus, a, "fuelGaugeBus"); }

// Sensor-specific I2C clock commands moved to their respective sensor modules:
// - thermalI2cClockHz -> i2csensor_mlx90640.cpp (thermal module)
// - tofI2cClockHz -> i2csensor_vl53l4cx.cpp (ToF module)

const char* cmd_i2cscan(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // Stream line-by-line via broadcastOutput / BROADCAST_PRINTF instead of
  // accumulating into the 1024-byte debug buffer. With dual-bus scans the
  // accumulated output (header + per-bus header + up to ~10 lines per device
  // x 2 buses + summary) easily exceeds 1024 B and truncates mid-line. This
  // matches the pattern cmd_i2cstats / streamDeviceRegistryOutput use.
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();

  broadcastOutput("I2C Bus Scan with Device Identification:\n========================================");

  int totalCount = 0;

  // Bus 0 (I2C1, Wire1) — always scanned when initialized.
  if (mgr && mgr->isBusInitialized(0)) {
    BROADCAST_PRINTF("I2C1 / Wire1 (bus 0, SDA=%d SCL=%d):",
                     gSettings.i2cSdaPin, gSettings.i2cSclPin);
    int count = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
      if (i2cPingAddress(addr, 100000, 50, 0) && i2cConfirmPresent(addr, 100000, 50, 0)) {
        String identification = identifySensor(addr);
        BROADCAST_PRINTF("  0x%02X (%d) - %s", addr, addr, identification.c_str());
        count++;
      }
    }
    if (count == 0) {
      broadcastOutput("  No devices found");
    }
    totalCount += count;
  }

  // Bus 1 (I2C2, Wire) — only when enabled + initialized. Boards without a
  // second port (XIAO, QT Py, Feather V2) skip this block entirely.
  if (mgr && mgr->isBusInitialized(1)) {
    broadcastOutput("");
    BROADCAST_PRINTF("I2C2 / Wire (bus 1, SDA=%d SCL=%d):",
                     gSettings.i2c2SdaPin, gSettings.i2c2SclPin);
    int count = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
      if (i2cPingAddress(addr, 100000, 50, 1) && i2cConfirmPresent(addr, 100000, 50, 1)) {
        // identifySensor() looks up by address only — same name on either bus.
        String identification = identifySensor(addr);
        BROADCAST_PRINTF("  0x%02X (%d) - %s", addr, addr, identification.c_str());
        count++;
      }
    }
    if (count == 0) {
      broadcastOutput("  No devices found");
    }
    totalCount += count;
  } else if (gSettings.i2c2Enabled) {
    // User asked for bus 1 but it didn't initialize (probably bad pins).
    broadcastOutput("");
    broadcastOutput("I2C2 / Wire (bus 1): NOT INITIALIZED — check i2c2SdaPin/i2c2SclPin settings");
  }

  BROADCAST_PRINTF(
    "\nTotal devices found: %d\n"
    "Use 'sensors' to see full sensor database\n"
    "Use 'sensorinfo <name>' for detailed sensor information",
    totalCount);

  // Return value is the short result string the CLI prints after the
  // streamed output. Doesn't need to repeat what we already broadcast.
  emitListingTrailer("detected I2C hardware",
                     "battery: run 'batterystatus'; a sensor: 'open<sensor>' then '<sensor>read'");
  if (ensureDebugBuffer()) {
    snprintf(getDebugBuffer(), 1024, "[i2cscan] %d device(s) found", totalCount);
    return getDebugBuffer();
  }
  return "[i2cscan] done";
}

const char* cmd_i2cstats(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  broadcastOutput("I2C Bus Statistics:\n==================");

  // Wire1 bus info (configurable)
  broadcastOutput("");

  // Wire1 bus info (sensor bus)
  BROADCAST_PRINTF("Wire1 (Sensor I2C):\n  SDA Pin: %d\n  SCL Pin: %d",
                   gSettings.i2cSdaPin, gSettings.i2cSclPin);
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (mgr) {
    BROADCAST_PRINTF("  Clock: Managed by I2CDeviceManager (per-device)");
  }
  broadcastOutput("");

  // Sensor connection status
  broadcastOutput("Connected Sensors:");

  if (gInputConnected) {
    broadcastOutput("  Gamepad (seesaw)");
  }
  if (gImuConnected) {
    broadcastOutput("  IMU (BNO055)");
  }
  if (gApdsConnected) {
    broadcastOutput("  APDS9960");
  }
  if (gTofConnected) {
    broadcastOutput("  ToF (VL53L4CX)");
  }
  if (gThermalConnected) {
    broadcastOutput("  Thermal (MLX90640)");
  }

  if (!gInputConnected && !gImuConnected && !gApdsConnected && !gTofConnected && !gThermalConnected) {
    broadcastOutput("  No sensors connected");
  }

  return "[I2C] Health status displayed";
}

// ========== End I2C Infrastructure Commands ==========

#if ENABLE_TOF_SENSOR
// gTofRunning / gTofConnected / gTofTaskHandle / gVL53L4CX provided by i2csensor_vl53l4cx.h
extern bool tofPoll();
#endif
// i2cOledTransactionVoid/i2cOledTransaction and i2cDeviceTransaction are template functions in System_I2C.h
// gThermalRunning provided by i2csensor_mlx90640.h

// SensorCache struct is now defined in i2c_system.h

// Per-sensor state, handles, and driver-object pointers are declared in the
// respective i2csensor_*.h headers (all already included at top of file).
// Only sensor-side function declarations that aren't in their headers remain.
#if ENABLE_IMU_SENSOR
extern void imuPoll();        // i2csensor_bno055.cpp
#endif
#if ENABLE_THERMAL_SENSOR
extern bool thermalInit();    // i2csensor_mlx90640.cpp
extern bool thermalPoll();
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
// tofTask() -> i2csensor_vl53l4cx.cpp
// imuTask() -> i2csensor_bno055.cpp
// thermalTask() -> i2csensor_mlx90640.cpp
// inputTask() -> i2csensor_seesaw.cpp (gamepad) OR i2csensor_ano_encoder.cpp (ANO)
// apdsTask() -> i2csensor_apds9960.cpp
// gpsTask() -> i2csensor_pa1010d.cpp

// ============================================================================
// I2C Device Registry Helper Functions
// ============================================================================

// External dependencies for device registry
// connectedDevices, connectedDeviceCount, discoveryCount defined above
extern const I2CSensorEntry i2cSensors[];
extern const size_t i2cSensorsCount;
extern bool readText(const char* path, String& out);

static void scanBusForDevicesSmart(uint8_t busNumber, const uint8_t* addresses, int addressCount);  // Smart scan

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
    INFO_I2C_DISCOVERYF("Found device at 0x%02X on bus %d - %s (%s)", address, bus, device.name, device.description);
    
    // Don't register here - devices register themselves when initialized by their sensor modules
  } else {
    device.name = "Unknown";
    device.description = "Unidentified Device";
    device.manufacturer = "Unknown";
    INFO_I2C_DISCOVERYF("Found device at 0x%02X on bus %d - Unknown device", address, bus);
    
    // Don't register unknown devices - only actual initialized sensors should be in the manager
  }
}

// ============================================================================
// Hardware detection (read-only scan vs. configured features)
// ============================================================================
// Probes the known device addresses on every bus (quiescing each bus's sensor
// polling during its scan) and classifies each against the feature registry.
// Reused by the `detect` command and (later) the setup wizard's auto-config step.

void detectHardware(DetectionResult& out) {
  out = DetectionResult();

  extern bool gI2CBusRunning;
  if (!gI2CBusRunning) { out.busDisabled = true; return; }

  const size_t n = i2cSensorsCount;
  static const size_t MAXSENS = 32;             // table is ~14; guard caps the rest
  bool    found[MAXSENS]    = { false };
  uint8_t foundBus[MAXSENS];
  for (size_t i = 0; i < MAXSENS; i++) foundBus[i] = 0xFF;

  // 1) Probe each known address on each bus, pausing that bus's polling so we
  //    don't collide with an in-flight gamepad/OLED/sensor transaction.
  //
  // Probe at a conservative fixed 100 kHz — presence is presence regardless of a
  // device's operating speed, and some chips (e.g. the MAX17048 fuel gauge, whose
  // driver itself runs at 100 kHz) are marginal at 400 kHz on the STEMMA QT chain
  // and NACK a fast probe → false negative. Slower is always safe for an ACK check.
  // Also silence the i2c_master NACK error log for the scan: probing absent
  // addresses NACKs by design, which would otherwise spam red errors.
  static const uint32_t kProbeClockHz = 100000;
  const esp_log_level_t prevI2cLog = esp_log_level_get("i2c.master");
  esp_log_level_set("i2c.master", ESP_LOG_NONE);

  for (uint8_t bus = 0; bus < POLL_NUM_BUSES; bus++) {
    PollPauseGuard guard(bus);
    for (size_t i = 0; i < n && i < MAXSENS; i++) {
      if (found[i]) continue;                   // already located on the other bus
      const I2CSensorEntry& s = i2cSensors[i];
      // Probe for an ACK, then confirm by read-back to reject phantom ACKs
      // (see i2cConfirmPresent — write-only OLED is exempt). Prevents a ghost
      // address (e.g. 0x68) from being reported as a real device.
      bool hit = (i2cProbeAddress(s.address, kProbeClockHz, s.i2cTimeoutMs, bus) == 0) &&
                 i2cConfirmPresent(s.address, kProbeClockHz, s.i2cTimeoutMs, bus);
      if (!hit && s.multiAddress) {
        hit = (i2cProbeAddress(s.altAddress, kProbeClockHz, s.i2cTimeoutMs, bus) == 0) &&
              i2cConfirmPresent(s.altAddress, kProbeClockHz, s.i2cTimeoutMs, bus);
      }
      if (hit) { found[i] = true; foundBus[i] = bus; }
    }
  }

  esp_log_level_set("i2c.master", prevI2cLog);

  auto addEntry = [&](uint8_t addr, uint8_t bus, const char* name,
                      const char* mod, uint16_t kb, DetectStatus st) {
    if (out.count >= DetectionResult::MAX) return;
    DetectedEntry& e = out.entries[out.count++];
    e.address = addr; e.bus = bus; e.name = name;
    e.moduleName = mod; e.heapCostKB = kb; e.status = st;
    switch (st) {
      case DetectStatus::PRESENT_ENABLED:  out.nPresentEnabled++;  break;
      case DetectStatus::PRESENT_DISABLED: out.nPresentDisabled++; break;
      case DetectStatus::MISSING:          out.nMissing++;         break;
      case DetectStatus::PRESENT_INFRA:    out.nInfra++;           break;
    }
  };

  // 2) Report toggleable sensors by FEATURE (dedups shared modules like the
  //    seesaw's 0x49/0x50; present(module) = any DB entry with it was found).
  const char* emitted[DetectionResult::MAX]; uint8_t emittedCount = 0;
  auto alreadyEmitted = [&](const char* m) -> bool {
    for (uint8_t k = 0; k < emittedCount; k++)
      if (strcmp(emitted[k], m) == 0) return true;
    return false;
  };

  for (size_t i = 0; i < n && i < MAXSENS; i++) {
    const I2CSensorEntry& s = i2cSensors[i];
    if (!s.moduleName) continue;                 // infrastructure handled below
    if (alreadyEmitted(s.moduleName)) continue;

    const FeatureEntry* f = getFeatureById(s.moduleName);
    if (f && !isFeatureCompiled(f)) continue;    // not in this build

    bool present = false; uint8_t pbus = 0xFF;
    for (size_t j = 0; j < n && j < MAXSENS; j++) {
      if (i2cSensors[j].moduleName &&
          strcmp(i2cSensors[j].moduleName, s.moduleName) == 0 && found[j]) {
        present = true; pbus = foundBus[j]; break;
      }
    }
    bool     enabled = f ? isFeatureEnabled(f) : false;
    uint16_t kb      = f ? f->heapCostKB : 0;

    if (present && enabled)        addEntry(s.address, pbus,  s.name, s.moduleName, kb, DetectStatus::PRESENT_ENABLED);
    else if (present && !enabled)  addEntry(s.address, pbus,  s.name, s.moduleName, kb, DetectStatus::PRESENT_DISABLED);
    else if (!present && enabled)  addEntry(s.address, 0xFF,  s.name, s.moduleName, kb, DetectStatus::MISSING);
    // else not present + not enabled → nothing to report.

    if (emittedCount < DetectionResult::MAX) emitted[emittedCount++] = s.moduleName;
  }

  // 3) Infrastructure devices (no toggleable feature): OLED, fuel gauge, PWM.
  for (size_t i = 0; i < n && i < MAXSENS; i++) {
    if (i2cSensors[i].moduleName || !found[i]) continue;
    addEntry(i2cSensors[i].address, foundBus[i], i2cSensors[i].name, nullptr, 0, DetectStatus::PRESENT_INFRA);
  }
}

// "Heavy" = spawns a heavy subsystem and/or carries the BLE-coexistence history;
// these are OFFERED (left off) rather than auto-enabled. Everything else with a
// toggleable feature auto-enables when found present-but-disabled.
static bool detectIsHeavyModule(const char* id) {
  if (!id) return false;
  return strcmp(id, "camera") == 0 || strcmp(id, "microphone") == 0 ||
         strcmp(id, "espsr")  == 0 || strcmp(id, "input") == 0;
}

ApplyResult applyDetectedHardware(const DetectionResult& r) {
  ApplyResult out{0, 0, false};
  for (uint8_t i = 0; i < r.count; i++) {
    const DetectedEntry& e = r.entries[i];
    if (e.status != DetectStatus::PRESENT_DISABLED) continue;            // only act on newly-present
    if (detectIsHeavyModule(e.moduleName)) { out.offered++; continue; } // offer, don't auto-enable
    const FeatureEntry* f = getFeatureById(e.moduleName);
    if (!f || !canToggleFeature(f) || !f->enabledSetting) continue;
    *f->enabledSetting = true;
    out.enabled++;
    if (f->flags & FEATURE_FLAG_REQUIRES_REBOOT) out.rebootNeeded = true;
  }
  if (out.enabled) writeSettingsJson();
  return out;
}

const char* cmd_detect(const String& argsInput) {
  CommandArgs a(argsInput);
  String sub = a.arg(0); sub.toLowerCase();
  const bool apply = (sub == "apply");

  if (apply && !currentExecIsAdmin())
    return "Error: [detect] 'apply' changes config — admin login required.";

  DetectionResult r;
  detectHardware(r);

  if (r.busDisabled)
    return "Error: [detect] I2C bus disabled — enable the 'i2c' feature to scan for hardware.";

  if (apply) {
    ApplyResult ar = applyDetectedHardware(r);
    BROADCAST_PRINTF("[detect apply] enabled %u newly-present device(s); left %u heavy device(s) off (enable manually).%s",
                     (unsigned)ar.enabled, (unsigned)ar.offered,
                     ar.rebootNeeded ? " Reboot for reboot-gated features." : "");
    return "[detect apply] done";
  }

  BROADCAST_PRINTF("[detect] Hardware vs. configuration (I2C1=bus0, I2C2=bus1):");

  if (r.nPresentEnabled) {
    BROADCAST_PRINTF("  Present & enabled:");
    for (uint8_t i = 0; i < r.count; i++) {
      const DetectedEntry& e = r.entries[i];
      if (e.status == DetectStatus::PRESENT_ENABLED)
        BROADCAST_PRINTF("    %-10s (%s)  I2C%d  0x%02X", e.name, e.moduleName, e.bus + 1, e.address);
    }
  }
  if (r.nPresentDisabled) {
    BROADCAST_PRINTF("  Present but DISABLED (attachable):");
    for (uint8_t i = 0; i < r.count; i++) {
      const DetectedEntry& e = r.entries[i];
      if (e.status == DetectStatus::PRESENT_DISABLED)
        BROADCAST_PRINTF("    %-10s (%s)  I2C%d  0x%02X  ~%uKB", e.name, e.moduleName, e.bus + 1, e.address, (unsigned)e.heapCostKB);
    }
  }
  if (r.nMissing) {
    BROADCAST_PRINTF("  Enabled but NOT found (check wiring):");
    for (uint8_t i = 0; i < r.count; i++) {
      const DetectedEntry& e = r.entries[i];
      if (e.status == DetectStatus::MISSING)
        BROADCAST_PRINTF("    %-10s (%s)  0x%02X", e.name, e.moduleName, e.address);
    }
  }
  if (r.nInfra) {
    BROADCAST_PRINTF("  Infrastructure:");
    for (uint8_t i = 0; i < r.count; i++) {
      const DetectedEntry& e = r.entries[i];
      if (e.status == DetectStatus::PRESENT_INFRA)
        BROADCAST_PRINTF("    %-10s  I2C%d  0x%02X", e.name, e.bus + 1, e.address);
    }
  }

  BROADCAST_PRINTF("  Summary: %u enabled, %u attachable, %u missing, %u infra. "
                   "(run 'i2cscan' for a raw bus dump incl. unknown devices)",
                   r.nPresentEnabled, r.nPresentDisabled, r.nMissing, r.nInfra);
  return "[detect] done";
}

// Bus convention (post-dual-bus refactor):
//   bus 0 → Wire1 → primary "I2C1" (horizontal STEMMA QT on FeatherS3[D])
//   bus 1 → Wire  → secondary "I2C2" (vertical STEMMA QT on FeatherS3[D])
// This flips the inverted mapping that the original single-bus stub had
// (`(busNumber == 0) ? &Wire : &Wire1`) and matches what users expect:
// bus 0 is the primary, bus 1 is the secondary. ConnectedDevice.bus is
// rendered as "I2C1" / "I2C2" in the registry output below.
static inline TwoWire* wireForBus(uint8_t busNumber) {
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  return mgr ? mgr->getWire(busNumber) : nullptr;
}

static void scanBusForDevices(uint8_t busNumber) {
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (!mgr || !mgr->isBusInitialized(busNumber)) return;
  TwoWire* wire = mgr->getWire(busNumber);
  if (!wire) return;

  // Prevent concurrent I2C usage (e.g. gamepad/OLED tasks) while reinitializing/
  // scanning (RAII — resumes on every return path; System_PollPause.h).
  SemaphoreHandle_t busMutex = mgr->getBusMutex(busNumber);
  PollPauseGuard pollGuard(busNumber);   // scope to the bus being scanned

  // Phase 1: Bus re-init under mutex (brief hold). Re-initializes from
  // the bus's settings pins (bus 0 = i2cSdaPin/SclPin; bus 1 = i2c2*).
  {
    bool locked = (busMutex && xSemaphoreTake(busMutex, pdMS_TO_TICKS(2000)) == pdTRUE);
    if (!locked) {
      return;
    }

    const int sda = (busNumber == 0) ? gSettings.i2cSdaPin  : gSettings.i2c2SdaPin;
    const int scl = (busNumber == 0) ? gSettings.i2cSclPin  : gSettings.i2c2SclPin;
    if (sda >= 0 && scl >= 0) {
      wire->begin(sda, scl);
      wire->setClock(I2C_WIRE1_DEFAULT_FREQ);
    }

    xSemaphoreGive(busMutex);
  }

  // Small delay to let bus stabilize (outside mutex)
  delay(10);

  // Phase 2: Per-probe mutex acquire/release (full scan of 126 addresses)
  for (uint8_t addr = 1; addr < 127; addr++) {
    if (busMutex && xSemaphoreTake(busMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
      wire->beginTransmission(addr);
      uint8_t err = wire->endTransmission();
      xSemaphoreGive(busMutex);

      // Confirm by read-back before registering. A bare address-ACK can be a
      // phantom (bus capacitance / a neighbour chip / a clock-stretch glitch),
      // which would otherwise be mislabeled from the address table (e.g. a
      // ghost 0x68 shown as "DS3231 RTC"). OLED is exempt (write-only).
      if (err == 0 && i2cConfirmPresent(addr, 100000, 200, busNumber)) {
        addDiscoveredDevice(addr, busNumber);
      }
    }
  }
  // pollGuard resumes sensor polling on return.
}

// Smart scan function - only checks specific addresses (used by discoverI2CDevices)
static void scanBusForDevicesSmart(uint8_t busNumber, const uint8_t* addresses, int addressCount) {
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (!mgr || !mgr->isBusInitialized(busNumber)) return;
  TwoWire* wire = mgr->getWire(busNumber);
  if (!wire) return;

  // Prevent concurrent I2C usage (e.g. gamepad/OLED tasks) while reinitializing/
  // scanning (RAII — resumes on every return path; System_PollPause.h).
  SemaphoreHandle_t busMutex = mgr->getBusMutex(busNumber);
  PollPauseGuard pollGuard(busNumber);   // scope to the bus being scanned

  // Phase 1: Bus re-init under mutex (brief hold)
  {
    bool locked = (busMutex && xSemaphoreTake(busMutex, pdMS_TO_TICKS(2000)) == pdTRUE);
    if (!locked) {
      return;
    }

    const int sda = (busNumber == 0) ? gSettings.i2cSdaPin  : gSettings.i2c2SdaPin;
    const int scl = (busNumber == 0) ? gSettings.i2cSclPin  : gSettings.i2c2SclPin;
    if (sda >= 0 && scl >= 0) {
      wire->begin(sda, scl);
      wire->setClock(I2C_WIRE1_DEFAULT_FREQ);
    }

    xSemaphoreGive(busMutex);
  }

  // Small delay to let bus stabilize (outside mutex)
  delay(10);

  // Phase 2: Probe each address with per-probe mutex acquire/release.
  // This avoids holding the bus mutex for the entire scan (~300ms+),
  // which would block any concurrent I2C transaction (e.g. manual openpresence).
  for (int i = 0; i < addressCount; i++) {
    uint8_t addr = addresses[i];
    if (addr == 0) continue;

    if (busMutex && xSemaphoreTake(busMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
      wire->beginTransmission(addr);
      uint8_t err = wire->endTransmission();
      xSemaphoreGive(busMutex);

      // Confirm by read-back before registering. A bare address-ACK can be a
      // phantom (bus capacitance / a neighbour chip / a clock-stretch glitch),
      // which would otherwise be mislabeled from the address table (e.g. a
      // ghost 0x68 shown as "DS3231 RTC"). OLED is exempt (write-only).
      if (err == 0 && i2cConfirmPresent(addr, 100000, 200, busNumber)) {
        addDiscoveredDevice(addr, busNumber);
      }
    }
  }
  // pollGuard resumes sensor polling on return.
}


void discoverI2CDevices() {
  // Early exit if I2C bus is disabled
  if (!gI2CBusRunning) {
    INFO_I2C_DISCOVERYF("Device discovery skipped - bus disabled");
    return;
  }
  
  INFO_I2C_DISCOVERYF("Starting device discovery (smart scan - compiled sensors only)");

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
      #if !ENABLE_THERMAL_SENSOR
        if (strcmp(i2cSensors[i].headerGuard, "_ADAFRUIT_MLX90640_H_") == 0) compiled = false;
      #endif
      #if !ENABLE_TOF_SENSOR
        if (strcmp(i2cSensors[i].headerGuard, "_VL53L4CX_CLASS_H_") == 0) compiled = false;
      #endif
      #if !ENABLE_IMU_SENSOR
        if (strcmp(i2cSensors[i].headerGuard, "_ADAFRUIT_BNO055_H_") == 0) compiled = false;
      #endif
      #if !ENABLE_GAMEPAD_SENSOR
        if (strcmp(i2cSensors[i].headerGuard, "_ADAFRUIT_SEESAW_H_") == 0) compiled = false;
      #endif
      #if !ENABLE_APDS_SENSOR
        if (strcmp(i2cSensors[i].headerGuard, "_ADAFRUIT_APDS9960_H_") == 0) compiled = false;
      #endif
      #if !ENABLE_GPS_SENSOR
        if (strcmp(i2cSensors[i].headerGuard, "_ADAFRUIT_GPS_H") == 0) compiled = false;
      #endif
      #if !ENABLE_RTC_SENSOR
        if (i2cSensors[i].moduleName && strcmp(i2cSensors[i].moduleName, "rtc") == 0) compiled = false;
      #endif
      #if !ENABLE_FM_RADIO
        if (i2cSensors[i].moduleName && strcmp(i2cSensors[i].moduleName, "fmradio") == 0) compiled = false;
      #endif
      #if !ENABLE_PRESENCE_SENSOR
        if (i2cSensors[i].moduleName && strcmp(i2cSensors[i].moduleName, "presence") == 0) compiled = false;
      #endif
    }
    
    if (compiled) {
      scanAddresses[scanCount++] = i2cSensors[i].address;
      if (i2cSensors[i].multiAddress && scanCount < 32) {
        scanAddresses[scanCount++] = i2cSensors[i].altAddress;
      }
    }
  }
  
  INFO_I2C_DISCOVERYF("[Discovery] Smart scan: %d addresses on I2C1 (bus 0, SDA=%d SCL=%d)",
            scanCount, gSettings.i2cSdaPin, gSettings.i2cSclPin);

  // Bus 0 (I2C1, Wire1) — always scanned when the primary bus is up. Note
  // the convention flip from the pre-dual-bus stub: bus 0 is now the
  // primary, not the empty secondary slot. See wireForBus()'s header.
  scanBusForDevicesSmart(0, scanAddresses, scanCount);

  // Bus 1 (I2C2, Wire) — scan only when the user has enabled the secondary
  // bus AND its pins are configured. Boards without a second port (XIAO,
  // QT Py, Feather V2) keep i2c2Enabled=false by default and skip this.
  I2CDeviceManager* mgrForScan = I2CDeviceManager::getInstance();
  if (gSettings.i2c2Enabled && mgrForScan && mgrForScan->isBusInitialized(1)) {
    INFO_I2C_DISCOVERYF("[Discovery] Smart scan: %d addresses on I2C2 (bus 1, SDA=%d SCL=%d)",
              scanCount, gSettings.i2c2SdaPin, gSettings.i2c2SclPin);
    scanBusForDevicesSmart(1, scanAddresses, scanCount);
  }

  {
    char devLine[240];
    int pos = snprintf(devLine, sizeof(devLine), "[Discovery] Found %d device(s)", connectedDeviceCount);
    if (connectedDeviceCount > 0 && pos > 0 && pos < (int)sizeof(devLine) - 4) {
      pos += snprintf(devLine + pos, sizeof(devLine) - (size_t)pos, ": ");
      for (int i = 0; i < connectedDeviceCount && pos < (int)sizeof(devLine) - 32; i++) {
        if (i > 0) {
          pos += snprintf(devLine + pos, sizeof(devLine) - (size_t)pos, ", ");
        }
        pos += snprintf(devLine + pos, sizeof(devLine) - (size_t)pos, "0x%02X (%s)",
                       connectedDevices[i].address, connectedDevices[i].name);
      }
    }
    INFO_I2C_DISCOVERYF("%s", devLine);
  }

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

    // Bus labels in dual-bus convention: I2C1 = primary STEMMA QT (Wire1),
    // I2C2 = secondary STEMMA QT (Wire). Matches the on-board silkscreen
    // on the FeatherS3[D] and what users see in the web UI.
    const char* busStr = (device.bus == 0) ? "I2C1" : "I2C2";
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

// Honest device-info counts for the status/device-info UI. These deliberately
// do NOT use the I2CDeviceManager registry counts (getDeviceCount /
// getActiveDeviceCount), which drift from physical reality via phantom bus-0
// pre-registrations and "ever-talked, never-reset" accounting — that mismatch
// is exactly what made the dashboard show "4 active" with only 3 detected.
//
//   i2cCompiledSensorCount() — how many sensor TYPES this build supports
//     (a capability count; the honest "N compiled").
//   i2cConnectedDeviceCount() — how many devices are physically on the bus
//     right now. SAME source + predicate (connectedDevices[].isConnected) as
//     buildI2cDeviceListJson() below, so this count and the emitted deviceList
//     can never disagree.
int i2cCompiledSensorCount() {
  int n = 0;
  for (size_t i = 0; i < i2cSensorsCount; i++) {
    if (isSensorCompiled(i2cSensors[i])) n++;
  }
  return n;
}

int i2cConnectedDeviceCount() {
  int n = 0;
  for (int i = 0; i < connectedDeviceCount; i++) {
    if (connectedDevices[i].isConnected) n++;
  }
  return n;
}

// Shared I2C device-list builder — the single source for every device-list
// serialization in the firmware. Two forms:
//   • lean (verbose=false, default): {name, addr, bus}, CONNECTED devices only.
//     Consumed by `devices json` (CLI/BLE) and buildSystemInfoJson()'s
//     connectivity.i2c.deviceList (status json / /api/system / SSE).
//   • verbose (verbose=true): the full registry record per device, ALL entries
//     (with isConnected as a field). Byte-compatible with the previous
//     hand-rolled serialization — same keys + value types — so /api/devices'
//     web page and the `devicefile` command keep working unchanged.
void buildI2cDeviceListJson(JsonArray& arr, bool verbose = false) {
  for (int i = 0; i < connectedDeviceCount; i++) {
    const ConnectedDevice& d = connectedDevices[i];
    if (!verbose && !d.isConnected) continue;   // lean: connected devices only
    JsonObject o = arr.add<JsonObject>();
    if (verbose) {
      char hexAddr[8];
      snprintf(hexAddr, sizeof(hexAddr), "0x%02X", d.address);
      o["address"]         = d.address;
      o["addressHex"]      = hexAddr;   // char[] -> ArduinoJson copies it
      o["name"]            = (d.name && d.name[0]) ? d.name : "device";
      o["description"]     = d.description  ? d.description  : "";
      o["manufacturer"]    = d.manufacturer ? d.manufacturer : "";
      o["bus"]             = d.bus;
      o["isConnected"]     = d.isConnected;
      o["lastSeen"]        = (unsigned long)d.lastSeen;
      o["firstDiscovered"] = (unsigned long)d.firstDiscovered;
    } else {
      o["name"] = (d.name && d.name[0]) ? d.name : "device";
      o["addr"] = d.address;
      o["bus"]  = d.bus;
    }
  }
}

// Rich device-registry document {lastDiscovery, discoveryCount, devices:[verbose]}
// — the single source for /api/devices (web) and the `devicefile` command,
// replacing two byte-identical hand-rolled String serializations.
void buildDeviceRegistryJson(JsonDocument& doc) {
  doc["lastDiscovery"]  = (unsigned long)millis();
  doc["discoveryCount"] = discoveryCount;
  JsonArray arr = doc["devices"].to<JsonArray>();
  buildI2cDeviceListJson(arr, /*verbose=*/true);
}

const char* cmd_devices(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // Structured path (output contract): machine-readable device list, returned
  // as one verbatim JSON blob via the return value. No broadcastOutput; no
  // Arduino String (DRAM) — serialize into a persistent PSRAM buffer.
  // Schema: {"schema":1,"hint":"...","devices":[{"name","addr","bus"}, ...],"count":N}
  if (argWantsJson(originalCmd)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    doc["hint"] = "battery: run 'batterystatus'; a sensor: 'open<sensor>' then '<sensor>read'";
    JsonArray arr = doc["devices"].to<JsonArray>();
    buildI2cDeviceListJson(arr);
    doc["count"] = arr.size();
    // One constant for the alloc AND the serialize — they used to be two
    // hand-typed 2048s that had to agree.
    static const size_t kDevJsonSize = 2048;
    static char* devJsonBuf = nullptr;
    if (!devJsonBuf) devJsonBuf = (char*)ps_alloc(kDevJsonSize, AllocPref::PreferPSRAM, "devices.json");
    if (!devJsonBuf) return "{\"error\":\"oom\"}";
    size_t len = serializeJson(doc, devJsonBuf, kDevJsonSize);
    if (len == 0 || len >= kDevJsonSize - 1) return "Error: device list outgrew the response buffer";
    return devJsonBuf;
  }

  streamDeviceRegistryOutput();
  emitListingTrailer("detected I2C hardware",
                     "battery: run 'batterystatus'; a sensor: 'open<sensor>' then '<sensor>read'");
  return "[I2C] Device registry displayed";
}

const char* cmd_discover(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  discoverI2CDevices();

  BROADCAST_PRINTF("Device discovery completed. Found %d devices.", connectedDeviceCount);

#if ENABLE_FM_RADIO
  // Initialize FM radio if detected to prevent I2C bus lockups. Gated on
  // ENABLE_FM_RADIO because (1) the device-scan loop is wasted work when the
  // feature is compiled out, and (2) without this guard the WARN_I2CF on
  // failure misleads — fmRadioInit() resolves to the stub (always false)
  // when ENABLE_FM_RADIO=0, so a plugged-in RDA5807 produces "FM radio
  // initialization failed, may cause I2C interference" when the real reason
  // is "this firmware build doesn't include FM radio support."
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
    extern bool fmRadioInit();
    if (fmRadioInit()) {
      INFO_I2C_BUSF("FM radio initialized successfully - kept in low-power state");
    } else {
      WARN_I2CF("FM radio initialization failed, may cause I2C interference");
    }
  }
#endif  // ENABLE_FM_RADIO

  streamDeviceRegistryOutput();

  emitListingTrailer("detected I2C hardware",
                     "battery: run 'batterystatus'; a sensor: 'open<sensor>' then '<sensor>read'");
  return "Discovery complete";
}

const char* cmd_devicefile(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // Serialize from in-memory connectedDevices[] instead of reading disk
  broadcastOutput("Device Registry (from memory):");

  if (connectedDeviceCount == 0) {
    broadcastOutput("  No devices discovered. Run 'discover' first.");
    return "[I2C] Registry empty";
  }

  // Single source of truth (also feeds /api/devices) — no hand-rolled String.
  PSRAM_JSON_DOC(doc);
  buildDeviceRegistryJson(doc);
  static const size_t kRegBufSize = 4096;
  static char* regBuf = nullptr;
  if (!regBuf) regBuf = (char*)ps_alloc(kRegBufSize, AllocPref::PreferPSRAM, "devicefile.json");
  if (!regBuf) return "Error: [I2C] Out of memory";
  size_t len = serializeJson(doc, regBuf, kRegBufSize);
  if (len == 0 || len >= kRegBufSize - 1) return "Error: device registry outgrew the response buffer";
  // NOTE: this still BROADCASTS the document and returns a human string, so the
  // JSON is clipped to 255 B ([CUT]) on every sink and no transport gets it as
  // an addressed reply. Same JSON-contract violation `events kinds json` had —
  // the fit check above is correct but does not fix that; it needs the same
  // return-the-document treatment.
  broadcastOutput(regBuf);
  return "[I2C] Registry JSON displayed";
}

// ---- Live sensor view (Phase 2: state + live `data` readings) -------------
// Contract: docs/BLE_SENSORS_INTEGRATION.md. One entry per COMPILED sensor:
// id/name/kind + live enabled/connected, plus `data` (the sensor's native
// readings, embedded verbatim) for active, non-stream sensors. Readings reuse
// the same per-sensor builders ESP-NOW/MQTT use — no new data plumbing.
typedef int (*SensorDataFn)(char*, size_t);
// Forward-declare the per-sensor builders (also declared in their headers;
// matching redeclarations are harmless). Guarded so only compiled ones link.
#if ENABLE_PRESENCE_SENSOR
extern int presenceBuildDataJSON(char* buf, size_t bufSize);
#endif
#if ENABLE_TOF_SENSOR
extern int tofBuildDataJSON(char* buf, size_t bufSize);
#endif
#if ENABLE_IMU_SENSOR
extern int imuBuildDataJSON(char* buf, size_t bufSize);
#endif
#if ENABLE_GPS_SENSOR
extern int gpsBuildDataJSON(char* buf, size_t bufSize);
#endif
#if ENABLE_FM_RADIO
extern int fmRadioBuildDataJSON(char* buf, size_t bufSize);
#endif
#if ENABLE_RTC_SENSOR
extern int rtcBuildDataJSON(char* buf, size_t bufSize);
#endif
#if ENABLE_GAMEPAD_SENSOR
extern int gamepadBuildDataJSON(char* buf, size_t bufSize);
#endif
#if ENABLE_APDS_SENSOR
extern int apdsBuildDataJSON(char* buf, size_t bufSize);
#endif
#if ENABLE_THERMAL_SENSOR
// Thermal registers its COMPACT summary builder (not the 768-px frame) so a
// stream sensor can still surface a glanceable reading in `sensors json`.
extern int thermalBuildSummaryJSON(char* buf, size_t bufSize);
#endif

// Shared sensor-reading envelope. See System_I2C.h for the contract. Writes the
// common opening (no closing brace) so every per-sensor builder emits ONE shape;
// the builder appends its own value keys and '}'. Returns bytes written, or 0 on
// overflow (so the caller never appends past the buffer).
int sensorEnvelopeBegin(char* buf, size_t bufSize, bool valid, bool connected,
                        unsigned long lastUpdateMs) {
  if (!buf || bufSize == 0) return 0;
  int n;
  if (lastUpdateMs != 0) {
    unsigned long now = millis();
    unsigned long age = (now >= lastUpdateMs) ? (now - lastUpdateMs) : 0UL;
    n = snprintf(buf, bufSize,
                 "{\"valid\":%s,\"connected\":%s,\"ts\":%lu,\"age\":%lu",
                 valid ? "true" : "false", connected ? "true" : "false",
                 lastUpdateMs, age);
  } else {
    n = snprintf(buf, bufSize,
                 "{\"valid\":%s,\"connected\":%s,\"ts\":0",
                 valid ? "true" : "false", connected ? "true" : "false");
  }
  // snprintf returns the would-be length; treat truncation or error as overflow
  // so the caller never writes past the end of the buffer.
  if (n < 0 || (size_t)n >= bufSize) { buf[0] = '\0'; return 0; }
  return n;
}

static void addSensorEntry(JsonArray& arr, const char* id, const char* name,
                           const char* kind, bool enabled, bool connected, SensorDataFn dataFn) {
  JsonObject o = arr.add<JsonObject>();
  o["id"]        = id;
  o["name"]      = name;
  o["kind"]      = kind;
  o["enabled"]   = enabled;     // LIVE running state (toggled by open<id>/close<id>), NOT autostart
  o["connected"] = connected;   // physically present on the bus right now

  // Live readings: native per-sensor JSON, embedded for any active sensor that
  // provides a data builder. Parsed into the doc (copied) so the scratch buffer
  // is safe to reuse across sensors. Stream sensors (thermal) register a COMPACT
  // SUMMARY builder here — never their full frame — so the 1 KB scratch holds.
  if (dataFn && enabled && connected) {
    static char* dbuf = nullptr;
    if (!dbuf) dbuf = (char*)ps_alloc(1024, AllocPref::PreferPSRAM, "sensor.data.scratch");
    if (dbuf) {
      int n = dataFn(dbuf, 1024);
      if (n > 0) {
        PSRAM_JSON_DOC(tmp);
        if (deserializeJson(tmp, dbuf) == DeserializationError::Ok) {
          o["data"] = tmp;  // deep-copied into the parent doc
        }
      }
    }
  }
}

// {"schema":1,"seq":N,"brief":BOOL,"sensors":[...]} — only sensors compiled into this build appear.
// `enabled` is the LIVE running flag (g<X>Enabled) — so the app's power toggle,
// which sends open<id>/close<id>, reflects reality. (Auto-start-on-boot is a
// SEPARATE persisted knob, surfaced in `controls json` as <id>AutoStart, NOT
// this toggle.) seq bumps on enable/connect changes.
// includeData=false → "brief" enumeration: per-sensor state only (id/name/kind/
// enabled/connected), no embedded readings. Small + bounded, so it never hits the
// 2 KB command-result ceiling regardless of sensor count — the app uses it to
// discover which sensors are live, then fetches each one's <x>read json.
static void buildSensorsJson(JsonDocument& doc, bool includeData = true) {
  doc["schema"]   = 1;
  doc["seq"] = (unsigned long)gSensorStatusSeq;
  doc["brief"] = !includeData;
  JsonArray arr = doc["sensors"].to<JsonArray>();
#if ENABLE_PRESENCE_SENSOR
  addSensorEntry(arr, "presence", "STHS34PF80 presence",    "scalar", gPresenceRunning, isSensorConnected("presence"), includeData ? presenceBuildDataJSON : nullptr);
#endif
#if ENABLE_TOF_SENSOR
  addSensorEntry(arr, "tof",      "VL53L4CX distance",      "vector", gTofRunning, isSensorConnected("tof"), includeData ? tofBuildDataJSON : nullptr);
#endif
#if ENABLE_IMU_SENSOR
  addSensorEntry(arr, "imu",      "BNO055 orientation",     "vector", gImuRunning, isSensorConnected("imu"), includeData ? imuBuildDataJSON : nullptr);
#endif
#if ENABLE_GPS_SENSOR
  addSensorEntry(arr, "gps",      "PA1010D GPS",            "vector", gGpsRunning, isSensorConnected("gps"), includeData ? gpsBuildDataJSON : nullptr);
#endif
#if ENABLE_FM_RADIO
  addSensorEntry(arr, "fmradio",  "RDA5807 FM radio",       "scalar", gFmRadioRunning, isSensorConnected("fmradio"), includeData ? fmRadioBuildDataJSON : nullptr);
#endif
#if ENABLE_RTC_SENSOR
  addSensorEntry(arr, "rtc",      "DS3231 RTC",             "scalar", gRtcRunning, isSensorConnected("rtc"), includeData ? rtcBuildDataJSON : nullptr);
#endif
#if ENABLE_APDS_SENSOR
  addSensorEntry(arr, "apds",     "APDS9960 gesture/color", "scalar",
                 (gApdsColorRunning || gApdsProximityRunning || gApdsGestureRunning),
                 isSensorConnected("apds"), includeData ? apdsBuildDataJSON : nullptr);
#endif
#if ENABLE_GAMEPAD_SENSOR
  // id is "input" — the canonical, unified module name (Seesaw gamepad OR ANO
  // encoder). Matches controls json / open<id>/close<id> / sensorautostart /
  // the I2C DB moduleName, so the app correlates on one name with no overrides.
  addSensorEntry(arr, "input",    "Seesaw gamepad",         "scalar", gInputRunning, gInputConnected, includeData ? gamepadBuildDataJSON : nullptr);
#endif
#if ENABLE_THERMAL_SENSOR
  addSensorEntry(arr, "thermal",  "MLX90640 thermal",       "stream", gThermalRunning, isSensorConnected("thermal"), includeData ? thermalBuildSummaryJSON : nullptr);
#endif
}

const char* cmd_sensors(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // Structured path: LIVE sensor state + embedded per-sensor readings.
  // `sensors json`       → full view (state + each active sensor's `data`).
  // `sensors json brief` → enumeration only (state, no `data`) — small + bounded,
  //   for discovery before fetching each sensor via its own <x>read json. Use the
  //   brief form over BLE; the full form can exceed the 2 KB command-result cap
  //   when several sensors are active. One verbatim PSRAM blob, no broadcastOutput.
  if (argWantsJson(argsInput)) {
    String la = argsInput; la.toLowerCase();
    const bool brief = (la.indexOf("brief") >= 0);
    PSRAM_JSON_DOC(doc);
    buildSensorsJson(doc, !brief);
    doc["hint"] = "battery: run 'batterystatus'; a sensor: 'open<sensor>' then '<sensor>read'";
    static const size_t kSensorsJsonSize = 4096;
    static char* sensorsJsonBuf = nullptr;
    if (!sensorsJsonBuf) sensorsJsonBuf = (char*)ps_alloc(kSensorsJsonSize, AllocPref::PreferPSRAM, "sensors.json");
    if (!sensorsJsonBuf) return "{\"error\":\"oom\"}";
    size_t len = serializeJson(doc, sensorsJsonBuf, kSensorsJsonSize);
    if (len == 0 || len >= kSensorsJsonSize - 1) return "Error: sensor list outgrew the response buffer";
    return sensorsJsonBuf;
  }

  String args = argsInput;
  args.trim();

  broadcastOutput("I2C Sensor Database:\n===================");

  // Check for filter arguments
  String filter = "";
  if (args.length() > 0) {
    filter = args;
    filter.toLowerCase();
    BROADCAST_PRINTF("Filter: '%s'", args.c_str());
    broadcastOutput("");
  }

  broadcastOutput(
    "Addr Name         Description                    Manufacturer\n"
    "---- ------------ ------------------------------ ------------");

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

  emitListingTrailer("detected sensors",
                     "battery: run 'batterystatus'; a sensor: 'open<sensor>' then '<sensor>read'",
                     "the filter matches sensor name/description, not an I2C address (e.g. 'sensors 0x36' matches nothing)");
  return "[I2C] Sensor list displayed";
}

const char* cmd_sensorinfo(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  String args = argsInput;
  args.trim();

  if (args.length() == 0) {
    broadcastOutput("Usage: sensorinfo <sensor_name>\nExample: sensorinfo BNO055");
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

  broadcastOutput("Sensor Information:\n==================");
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

  emitListingTrailer("detected sensors",
                     "battery: run 'batterystatus'; a sensor: 'open<sensor>' then '<sensor>read'",
                     "the filter matches sensor name/description, not an I2C address (e.g. 'sensors 0x36' matches nothing)");
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

// Columns: name, shortName, autoStartFlag, heapCostKB
static const SensorHeapCost sensorHeapCosts[] = {
  { "Thermal Camera", "thermal", &gSettings.thermalAutoStart, 32 },  // MLX90640: large frame buffer
  { "ToF Distance",   "tof",     &gSettings.tofAutoStart,      8 },  // VL53L4CX: moderate
  { "IMU",            "imu",     &gSettings.imuAutoStart,     12 },  // BNO055: calibration + buffers
  { "GPS",            "gps",     &gSettings.gpsAutoStart,      4 },  // PA1010D: NMEA parsing
  { "FM Radio",       "fmradio", &gSettings.fmRadioAutoStart,  2 },  // RDA5807: minimal
  { "APDS Gesture",   "apds",    &gSettings.apdsAutoStart,     4 },  // APDS9960: gesture buffers
#if ENABLE_ANO_ENCODER
  { "ANO Encoder",    "input",   &gSettings.inputAutoStart,    2 },  // Adafruit ANO seesaw: minimal
#else
  { "Gamepad",        "input",   &gSettings.inputAutoStart,    2 },  // Seesaw mini gamepad: minimal
#endif
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

static const char* cmd_sensorautostart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  CommandArgs a(argsInput);

  // No args - show current settings with heap estimates
  if (a.count() == 0) {
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
  
  if (!a.hasMinArgs(2)) {
    return "Error: invalid arguments — Usage: sensorautostart <sensor> <on|off>";
  }

  String sensor = a.arg(0);
  String value = a.arg(1);
  sensor.toLowerCase();
  value.toLowerCase();
  
  bool enable = (value == "on" || value == "true" || value == "1");
  bool disable = (value == "off" || value == "false" || value == "0");
  
  if (!enable && !disable) {
    return "Error: Value must be on/off, true/false, or 1/0";
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
    
    EXT_RAM_BSS_ATTR static char result[128];
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
    return "Error: Unknown sensor. Options: thermal, tof, imu, gps, fmradio, apds, input, all";
  }
  
  bool wasEnabled = *found->autoStartFlag;
  *found->autoStartFlag = enable;
  writeSettingsJson();
  
  EXT_RAM_BSS_ATTR static char result[128];
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
// I2C Bus Recovery / Pause / Resume Commands
// ============================================================================

const char* cmd_i2creset(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (!mgr) return "Error: I2C manager not initialized";
  
  broadcastOutput("[I2C] Starting bus reset...");
  broadcastOutput("[I2C] Step 1/3: Pausing sensor polling...");
  mgr->pausePolling();
  vTaskDelay(pdMS_TO_TICKS(200));
  
  broadcastOutput("[I2C] Step 2/3: Performing bus recovery (clock toggle + reinit)...");
  mgr->performBusRecovery();
  
  broadcastOutput("[I2C] Step 3/3: Resuming sensor polling...");
  mgr->resumePolling();
  
  broadcastOutput("[I2C] Bus reset complete. Run 'i2chealth' to check device status.");
  return "[I2C] Bus reset complete";
}

const char* cmd_i2cpause(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (!mgr) return "Error: I2C manager not initialized";
  
  if (mgr->isPollingPaused()) {
    return "[I2C] Polling already paused";
  }
  
  mgr->pausePolling();
  return "[I2C] Sensor polling paused. Use 'i2cresume' to resume.";
}

const char* cmd_i2cresume(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (!mgr) return "Error: I2C manager not initialized";
  
  if (!mgr->isPollingPaused()) {
    return "[I2C] Polling already running";
  }
  
  mgr->resumePolling();
  return "[I2C] Sensor polling resumed";
}

const char* cmd_i2crecover(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String args = argsInput;
  args.trim();
  
  if (args.isEmpty()) {
    return "Error: invalid arguments — Usage: i2crecover <address>\nExample: i2crecover 0x5A";
  }
  
  // Parse address (hex or decimal)
  uint8_t address = 0;
  if (args.startsWith("0x") || args.startsWith("0X")) {
    address = (uint8_t)strtol(args.c_str(), nullptr, 16);
  } else {
    address = (uint8_t)args.toInt();
  }
  
  if (address == 0 || address > 127) {
    return "Error: Invalid I2C address (must be 1-127 or 0x01-0x7F)";
  }
  
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (!mgr) return "Error: I2C manager not initialized";
  
  I2CDevice* dev = mgr->getDevice(address);
  if (!dev || !dev->isInitialized()) {
    EXT_RAM_BSS_ATTR static char result[64];
    snprintf(result, sizeof(result), "[I2C] Device 0x%02X not registered", address);
    return result;
  }
  
  dev->attemptRecovery();
  
  EXT_RAM_BSS_ATTR static char result[128];
  snprintf(result, sizeof(result), 
    "[I2C] Device 0x%02X (%s) degraded state cleared. Retry sensor start command.",
    address, dev->name);
  return result;
}

// ============================================================================
// Sensor Command Registry
// ============================================================================

// Forward declarations for queued sensor start commands (from main .ino)
extern const char* cmd_thermalstart_queued(const String& argsInput);
extern const char* cmd_tofstart_queued(const String& argsInput);
extern const char* cmd_imustart_queued(const String& argsInput);
extern const char* cmd_apdsstart_queued(const String& argsInput);

// ============================================================================
// I2C Command Registry
// ============================================================================

// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
const CommandEntry i2cCommands[] = {
  // Bus Configuration
  { "i2cbusenabled", "Enable/disable I2C1 bus: <0|1> (reboot required)", true, cmd_i2cbusenabled, "Usage: i2cBusEnabled <0|1>" },
  { "i2csdapin", "Set I2C1 SDA pin: <0.." HW_GPIO_MAX_STR "> (max GPIO for this board)", true, cmd_i2csdapin, "Usage: i2cSdaPin <0.." HW_GPIO_MAX_STR "> (max GPIO for this board)" },
  { "i2csclpin", "Set I2C1 SCL pin: <0.." HW_GPIO_MAX_STR "> (max GPIO for this board)", true, cmd_i2csclpin, "Usage: i2cSclPin <0.." HW_GPIO_MAX_STR "> (max GPIO for this board)" },
  { "i2c2busenabled", "Enable/disable I2C2 bus: <0|1> (reboot required)", true, cmd_i2c2busenabled, "Usage: i2c2BusEnabled <0|1>" },
  { "i2c2sdapin", "Set I2C2 SDA pin: <-1.." HW_GPIO_MAX_STR "> (-1=unavailable)", true, cmd_i2c2sdapin, "Usage: i2c2SdaPin <-1.." HW_GPIO_MAX_STR "> (-1=unavailable)" },
  { "i2c2sclpin", "Set I2C2 SCL pin: <-1.." HW_GPIO_MAX_STR "> (-1=unavailable)", true, cmd_i2c2sclpin, "Usage: i2c2SclPin <-1.." HW_GPIO_MAX_STR "> (-1=unavailable)" },
  // Per-device bus assignment — route a sensor to bus 0 (I2C1) or bus 1 (I2C2).
  { "oledbus",     "Route OLED to bus: <0|1> (reboot required)",         true, cmd_oledbus,     "Usage: oledBus <0|1>" },
  { "inputbus",    "Route input device to bus: <0|1> (reboot required)",  true, cmd_inputbus,    "Usage: inputBus <0|1>" },
  { "gpsbus",      "Route PA1010D GPS to bus: <0|1> (reboot required)",  true, cmd_gpsbus,      "Usage: gpsBus <0|1>" },
  { "rtcbus",      "Route DS3231 RTC to bus: <0|1> (reboot required)",   true, cmd_rtcbus,      "Usage: rtcBus <0|1>" },
  { "fmradiobus",  "Route RDA5807 FM radio to bus: <0|1> (reboot required)", true, cmd_fmradiobus, "Usage: fmRadioBus <0|1>" },
  { "presencebus", "Route STHS34PF80 presence to bus: <0|1> (reboot required)", true, cmd_presencebus, "Usage: presenceBus <0|1>" },
  { "imubus",      "Route BNO055 IMU to bus: <0|1> (reboot required)",   true, cmd_imubus,      "Usage: imuBus <0|1>" },
  { "thermalbus",  "Route MLX90640 thermal to bus: <0|1> (reboot required)", true, cmd_thermalbus, "Usage: thermalBus <0|1>" },
  { "tofbus",      "Route VL53L4CX ToF to bus: <0|1> (reboot required)", true, cmd_tofbus,      "Usage: tofBus <0|1>" },
  { "apdsbus",     "Route APDS9960 gesture to bus: <0|1> (reboot required)", true, cmd_apdsbus, "Usage: apdsBus <0|1>" },
  { "servobus",    "Route PCA9685 servo to bus: <0|1> (reboot required)", true, cmd_servobus,  "Usage: servoBus <0|1>" },
  { "fuelgaugebus","Route MAX17048 fuel gauge to bus: <0|1> (reboot required)", true, cmd_fuelgaugebus, "Usage: fuelGaugeBus <0|1>" },
  // Note: Sensor-specific I2C clock commands (thermalI2cClockHz, tofI2cClockHz) are in their respective sensor modules
  
  // Bus Management
  { "i2creset", "Reset I2C bus: pause polling, recover bus, resume.", true, cmd_i2creset },
  { "i2cpause", "Pause all I2C sensor polling.", true, cmd_i2cpause },
  { "i2cresume", "Resume I2C sensor polling.", true, cmd_i2cresume },
  { "i2crecover", "Clear degraded state for device: <address>", true, cmd_i2crecover, "Usage: i2crecover <address>  (hex 0x01-0x7F or decimal 1-127)" },
  
  // Diagnostics
  { "i2cmetrics", "Show I2C bus performance metrics. (add 'json' for JSON output)", false, cmd_i2cmetrics },
  { "i2cscan", "Scan I2C bus for devices.", false, cmd_i2cscan },
  { "detect", "Detect hardware: scan I2C buses, diff vs. configured features.", false, cmd_detect, "Usage: detect [apply]\n  detect       - read-only report (present/enabled/missing)\n  detect apply - auto-enable cheap detected devices (admin; reboot for some)" },
  { "i2cstats", "I2C bus statistics and errors.", false, cmd_i2cstats },
  { "i2chealth", "Show per-device I2C health status. (add 'json' for JSON output)", false, cmd_i2chealth },
  
  // Device Registry
  { "sensors", "List I2C sensors [filter]", false, cmd_sensors, "Usage: sensors [filter] - filter by name, description, or manufacturer\n       sensors json [brief] - live state (+readings; 'brief' = state only, no data)\nExample: sensors temperature, sensors json brief" },
  { "sensorinfo", "Sensor details: <name>", false, cmd_sensorinfo, "Usage: sensorinfo <sensor_name>\nExample: sensorinfo BNO055" },
  { "devices", "Show discovered I2C device registry. (add 'json' for JSON output)", false, cmd_devices },
  { "discover", "Re-scan and register I2C devices.", false, cmd_discover },
  { "devicefile", "Show device registry JSON file.", false, cmd_devicefile },
  
  // Sensor Auto-Start
  { "sensorautostart", "Sensor auto-start: [sensor] [on|off]", true, cmd_sensorautostart, "Usage: sensorautostart [sensor] [on|off]\n       sensorautostart all [on|off]\nSensors: thermal, tof, imu, gps, fmradio, apds, input" }
};

const size_t i2cCommandsCount = sizeof(i2cCommands) / sizeof(i2cCommands[0]);

// ============================================================================
// Command Registration
// ============================================================================
// Registration handled by gCommandModules[] in System_Utils.cpp

// ============================================================================
// Sensor Status System (moved from HardwareOne.ino)
// ============================================================================

const char* deviceTypeDisplayName(I2CDeviceType sensor) {
  switch (sensor) {
    case I2C_DEVICE_THERMAL:  return "Thermal";
    case I2C_DEVICE_TOF:      return "ToF";
    case I2C_DEVICE_IMU:      return "IMU";
#if ENABLE_ANO_ENCODER
    case I2C_DEVICE_INPUT:  return "ANO Encoder";
#else
    case I2C_DEVICE_INPUT:  return "Gamepad";
#endif
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
      gThermalRunning = false;
      gThermalLastStopTime = millis();
#endif
      break;
    case I2C_DEVICE_TOF:
#if ENABLE_TOF_SENSOR
      gTofRunning = false;
      gTofLastStopTime = millis();
#endif
      break;
    case I2C_DEVICE_IMU:
#if ENABLE_IMU_SENSOR
      gImuRunning = false;
      gImuLastStopTime = millis();
#endif
      break;
    case I2C_DEVICE_INPUT:
#if ENABLE_GAMEPAD_SENSOR
      gInputRunning = false;
      gGamepadLastStopTime = millis();
#endif
      break;
    case I2C_DEVICE_GPS:
#if ENABLE_GPS_SENSOR
      gGpsRunning = false;
      gGpsLastStopTime = millis();
#endif
      break;
    case I2C_DEVICE_FMRADIO:
#if ENABLE_FM_RADIO
      gFmRadioRunning = false;
      gFmRadioLastStopTime = millis();
#endif
      break;
    case I2C_DEVICE_APDS:
#if ENABLE_APDS_SENSOR
      gApdsColorRunning = false;
      gApdsProximityRunning = false;
      gApdsGestureRunning = false;
      gApdsLastStopTime = millis();
#endif
      break;
    case I2C_DEVICE_RTC:
#if ENABLE_RTC_SENSOR
      gRtcRunning = false;
      gRtcLastStopTime = millis();
#endif
      break;
    case I2C_DEVICE_PRESENCE:
#if ENABLE_PRESENCE_SENSOR
      gPresenceRunning = false;
      gPresenceLastStopTime = millis();
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
    case I2C_DEVICE_INPUT:  broadcastSensorStatus(REMOTE_SENSOR_INPUT, false);  break;
    case I2C_DEVICE_GPS:      broadcastSensorStatus(REMOTE_SENSOR_GPS, false);      break;
    case I2C_DEVICE_FMRADIO:  broadcastSensorStatus(REMOTE_SENSOR_FMRADIO, false);  break;
    default: break;  // APDS, RTC, Presence have no remote sensor type
  }
#endif

  systemEventPost(SYSEVT_SENSOR_STOPPED, name);

  // Bump sensor status so SSE + bonded peer get notified immediately
  char cause[48];
  snprintf(cause, sizeof(cause), "close_%s@handleDeviceStopped", name);
  sensorStatusBumpWith(cause);
}

// Helper: set cause then bump (to preserve existing call-sites)
void sensorStatusBumpWith(const char* cause) {
  INFO_I2C_BUSF("Status bump: %s", cause ? cause : "(null)");
  gLastStatusCause = cause ? cause : "";
  sensorStatusBump();
}

const char* buildSensorStatusJson() {
  // PSRAM buffer allocated once, reused forever (zero stack impact)
  static char* buf = nullptr;
  static const size_t kBufSize = 2048;

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
  doc["thermalRunning"] = gThermalRunning;
  doc["tofRunning"] = gTofRunning;
  doc["imuRunning"] = gImuRunning;
  doc["apdsColorRunning"] = gApdsColorRunning;
  doc["apdsProximityRunning"] = gApdsProximityRunning;
  doc["apdsGestureRunning"] = gApdsGestureRunning;
  doc["inputRunning"] = gInputRunning;
#if ENABLE_SERVO
  doc["pwmDriverConnected"] = gPwmDriverConnected;
#else
  doc["pwmDriverConnected"] = false;
#endif
  doc["gpsRunning"] = gGpsRunning;
  doc["fmRadioRunning"] = gFmRadioRunning;
#if ENABLE_RTC_SENSOR
  doc["rtcRunning"] = gRtcRunning;
#else
  doc["rtcRunning"] = false;
#endif
  
#if ENABLE_PRESENCE_SENSOR
  extern bool gPresenceRunning;
  doc["presenceRunning"] = gPresenceRunning;
#else
  doc["presenceRunning"] = false;
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

#if ENABLE_OLED_INPUT
  doc["inputCompiled"] = true;
#else
  doc["inputCompiled"] = false;
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

#if ENABLE_FM_RADIO
  doc["fmradioCompiled"] = true;
#else
  doc["fmradioCompiled"] = false;
#endif
#if ENABLE_SERVO
  doc["servoCompiled"] = true;
#else
  doc["servoCompiled"] = false;
#endif

#if ENABLE_CAMERA_SENSOR
  extern bool gCameraRunning;
  extern bool cameraStreaming;
  extern bool videoRecording;
  doc["cameraRunning"] = gCameraRunning;
  doc["cameraStreaming"] = cameraStreaming;
  doc["cameraRecording"] = videoRecording;
  doc["cameraCompiled"] = true;
#else
  doc["cameraRunning"] = false;
  doc["cameraStreaming"] = false;
  doc["cameraRecording"] = false;
  doc["cameraCompiled"] = false;
#endif

  // Storage availability — exposed so UI can gate features like video
  // recording that require SD card. Two distinct states:
  //   sdAvailable = driver says card is mounted (SD.begin succeeded)
  //   sdWritable  = round-trip write probe has actually succeeded
  // The UI should gate "requires SD write" features on sdWritable, while
  // diagnostics / file listing can use sdAvailable.
  {
    extern bool sdCardIsMountedForStatus();
    extern bool sdCardIsWritableForStatus();
    doc["sdAvailable"] = sdCardIsMountedForStatus();
    doc["sdWritable"]  = sdCardIsWritableForStatus();
  }

#if ENABLE_MICROPHONE
  extern bool gMicRunning;
  extern bool micRecording;
  doc["micRunning"] = gMicRunning;
  doc["micRecording"] = micRecording;
  doc["micCompiled"] = true;
#else
  doc["micRunning"] = false;
  doc["micRecording"] = false;
  doc["micCompiled"] = false;
#endif

#if ENABLE_EDGE_IMPULSE
  extern bool isEdgeImpulseModelLoaded();
  doc["eiEnabled"] = gSettings.eiEnabled;
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
  
  // I2C bus runtime state — lets the web UI know whether the bus is active,
  // distinct from compile-time flags and individual sensor enable flags
  doc["i2cBusRunning"] = gI2CBusRunning;
#if ENABLE_I2C_SYSTEM
  doc["i2cSystemCompiled"] = true;
#else
  doc["i2cSystemCompiled"] = false;
#endif

  // Queue status
  doc["queueDepth"] = getQueueDepth();
  doc["thermalQueued"] = isInQueue(I2C_DEVICE_THERMAL);
  doc["tofQueued"] = isInQueue(I2C_DEVICE_TOF);
  doc["imuQueued"] = isInQueue(I2C_DEVICE_IMU);
  doc["apdsQueued"] = isInQueue(I2C_DEVICE_APDS);
  doc["gpsQueued"] = isInQueue(I2C_DEVICE_GPS);
  doc["inputQueued"] = isInQueue(I2C_DEVICE_INPUT);
  doc["rtcQueued"] = isInQueue(I2C_DEVICE_RTC);
  doc["presenceQueued"] = isInQueue(I2C_DEVICE_PRESENCE);
  
  // Queue positions (only if present)
  int thermalPos = getQueuePosition(I2C_DEVICE_THERMAL);
  int tofPos = getQueuePosition(I2C_DEVICE_TOF);
  int imuPos = getQueuePosition(I2C_DEVICE_IMU);
  int apdsPos = getQueuePosition(I2C_DEVICE_APDS);
  int gpsPos = getQueuePosition(I2C_DEVICE_GPS);
  int inputPos = getQueuePosition(I2C_DEVICE_INPUT);
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
  if (inputPos > 0) {
    doc["inputQueuePos"] = inputPos;
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

// Post the sensor lifecycle event + the durable [EVENT][SENSOR] line the old
// notifySensorStarted wrapper emitted — at boot this is the manifest of what
// came up vs what was configured-but-failed (Phase-1 cutover).
static void announceSensorStart(const char* name, bool ok) {
  systemEventPost(ok ? SYSEVT_SENSOR_STARTED : SYSEVT_SENSOR_START_FAILED, name);
  logSystemEvent("SENSOR", "%s %s", name, ok ? "online" : "start FAILED");
}

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
      // Blanket-pause for the whole init batch (queued sensors span buses).
      // Ref-counted, so it composes safely with any blanket/per-bus pause that
      // is already active — no need to check first (the old `if
      // (!gSensorPollingPaused)` guard would now misfire when only a per-bus
      // pause is active). Uses the global primitive rather than
      // mgr->pausePolling() so the i2cpause/i2cresume CLI latch is left alone.
      pollPause();
      INFO_I2C_AUTOSTARTF("Paused polling for sensor initialization batch");

      // Process all queued sensors in one batch while polling stays paused
      do {
      DEBUG_CLIF("[QUEUE] Processing queued sensor: type=%d, queuedAt=%lu",
                 req.device, req.queuedAt);

      // Stack instrumentation (do not assume any fixed stack size)
      if (isDebugFlagSet(DEBUG_MEMORY)) {
        // HWM is in BYTES on this port (StackType_t is uint8_t) — the old
        // "words (x4 bytes)" pair was 4x wrong. See System_TaskUtils.h.
        UBaseType_t hwmBytes = uxTaskGetStackHighWaterMark(NULL);
        DEBUG_MEMORY_STACKF("[STACK][QUEUE] before start type=%d hwm=%u bytes",
                      (int)req.device, (unsigned)hwmBytes);
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
          case I2C_DEVICE_INPUT:
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
          alreadyRunning = gThermalRunning;
          break;
        case I2C_DEVICE_TOF:
          alreadyRunning = gTofRunning;
          break;
        case I2C_DEVICE_IMU:
          alreadyRunning = gImuRunning;
          break;
        case I2C_DEVICE_INPUT:
          alreadyRunning = gInputRunning;
          break;
        case I2C_DEVICE_APDS:
          alreadyRunning = gApdsColorRunning || gApdsProximityRunning || gApdsGestureRunning;
          break;
        case I2C_DEVICE_GPS:
          alreadyRunning = gGpsRunning;
          break;
        case I2C_DEVICE_FMRADIO:
          alreadyRunning = gFmRadioRunning;
          break;
        case I2C_DEVICE_RTC:
#if ENABLE_RTC_SENSOR
          alreadyRunning = gRtcRunning;
#endif
          break;
        case I2C_DEVICE_PRESENCE:
#if ENABLE_PRESENCE_SENSOR
          { extern bool gPresenceRunning;
          alreadyRunning = gPresenceRunning; }
#endif
          break;
      }

      if (alreadyRunning) {
        DEBUG_CLIF("[QUEUE] Skipping sensor (already running): type=%d", req.device);
        sensorStatusBumpWith("queue@already_running");
      } else {

      // Clear any stale health errors before init — boot-time probes/scans may have
      // recorded NACKs for devices that are now ready. Without this, a device could
      // enter degraded state from just one failed init combined with stale boot errors.
      {
        uint8_t devAddr = i2cAddressForDeviceType(req.device);
        if (devAddr != 0) {
          i2cResetGracePeriod(devAddr);
        }
      }

      // Use stack-efficient approach: discard result String, combine Serial calls
      switch (req.device) {
        case I2C_DEVICE_THERMAL:
          thermalStartInternal();
          INFO_I2C_AUTOSTARTF("Thermal: %s", gThermalRunning ? "SUCCESS" : "FAILED");
          announceSensorStart("Thermal", gThermalRunning);
          break;
        case I2C_DEVICE_TOF:
          tofStartInternal();
          INFO_I2C_AUTOSTARTF("ToF: %s", gTofRunning ? "SUCCESS" : "FAILED");
          announceSensorStart("ToF", gTofRunning);
          break;
        case I2C_DEVICE_IMU:
          imuStartInternal();
          INFO_I2C_AUTOSTARTF("IMU: %s", gImuRunning ? "SUCCESS" : "FAILED");
          announceSensorStart("IMU", gImuRunning);
          break;
        case I2C_DEVICE_INPUT:
          // I2C_DEVICE_INPUT is the shared input-device slot — under ANO
          // build, inputStartInternal() comes from the ANO driver. The label
          // reflects whichever input device is compiled in.
          inputStartInternal();
#if ENABLE_ANO_ENCODER
          INFO_I2C_AUTOSTARTF("ANO Encoder: %s", gInputRunning ? "SUCCESS" : "FAILED");
          announceSensorStart("ANO Encoder", gInputRunning);
#else
          INFO_I2C_AUTOSTARTF("Gamepad: %s", gInputRunning ? "SUCCESS" : "FAILED");
          announceSensorStart("Gamepad", gInputRunning);
#endif
          break;
        case I2C_DEVICE_APDS:
#if ENABLE_APDS_SENSOR
          apdsStartInternal();
          { bool apdsOk = gApdsColorRunning || gApdsProximityRunning || gApdsGestureRunning;
          INFO_I2C_AUTOSTARTF("APDS: %s (color=%d prox=%d gest=%d)",
                        apdsOk ? "SUCCESS" : "FAILED",
                        gApdsColorRunning ? 1 : 0, gApdsProximityRunning ? 1 : 0, gApdsGestureRunning ? 1 : 0);
          announceSensorStart("APDS", apdsOk); }
#else
          INFO_I2C_AUTOSTARTF("APDS: skipped (not compiled)");
#endif
          break;
        case I2C_DEVICE_GPS:
          gpsStartInternal();
          INFO_I2C_AUTOSTARTF("GPS: %s", gGpsRunning ? "SUCCESS" : "FAILED");
          announceSensorStart("GPS", gGpsRunning);
          break;
        case I2C_DEVICE_FMRADIO:
#if ENABLE_FM_RADIO
          fmRadioStartInternal();
          INFO_I2C_AUTOSTARTF("FM Radio: %s", gFmRadioRunning ? "SUCCESS" : "FAILED");
          announceSensorStart("FM Radio", gFmRadioRunning);
#else
          INFO_I2C_AUTOSTARTF("FM Radio: skipped (not compiled)");
#endif
          break;
        case I2C_DEVICE_RTC:
#if ENABLE_RTC_SENSOR
          rtcStartInternal();
          INFO_I2C_AUTOSTARTF("RTC: %s", gRtcRunning ? "SUCCESS" : "FAILED");
          announceSensorStart("RTC", gRtcRunning);
#else
          INFO_I2C_AUTOSTARTF("RTC: skipped (not compiled)");
#endif
          break;
        case I2C_DEVICE_PRESENCE:
#if ENABLE_PRESENCE_SENSOR
          extern bool presenceStartInternal();
          extern bool gPresenceRunning;
          presenceStartInternal();
          INFO_I2C_AUTOSTARTF("Presence: %s", gPresenceRunning ? "SUCCESS" : "FAILED");
          announceSensorStart("Presence", gPresenceRunning);
#else
          INFO_I2C_AUTOSTARTF("Presence: skipped (not compiled)");
#endif
          break;
      }

      if (isDebugFlagSet(DEBUG_MEMORY)) {
        UBaseType_t hwmBytes = uxTaskGetStackHighWaterMark(NULL);
        INFO_MEMORYF("[STACK][QUEUE] after  start type=%d hwm=%u bytes",
                      (int)req.device, (unsigned)hwmBytes);
      }

      lastSensorStartTime = millis();
      lastI2CDeviceType = req.device;  // Track this sensor for next iteration's delay

      // Force stack and heap check after sensor start (high resource usage point)
      if (isDebugFlagSet(DEBUG_MEMORY)) {
        // BYTES throughout (see System_TaskUtils.h). Two bugs fixed here: the
        // x4 scaling, and a hardcoded 3072 that had drifted from the real
        // SENSOR_QUEUE_STACK_WORDS (4096) — so peak% was computed against the
        // wrong total. Use the constant so it can't drift again.
        UBaseType_t stackHighWater = uxTaskGetStackHighWaterMark(NULL);
        constexpr uint32_t sensorQueueStackBytes = SENSOR_QUEUE_STACK_WORDS;
        uint32_t stackPeak = sensorQueueStackBytes - (uint32_t)stackHighWater;
        int peakPct = (stackPeak * 100) / sensorQueueStackBytes;
        size_t heapFree = ESP.getFreeHeap();
        size_t heapMin = ESP.getMinFreeHeap();
        DEBUG_MEMORY_STACKF("[STACK] sensor_queue: peak=%lu bytes (%d%%), free_min=%lu bytes | heap=%lu min=%lu",
                      (unsigned long)stackPeak, peakPct,
                      (unsigned long)stackHighWater,
                      (unsigned long)heapFree, (unsigned long)heapMin);
      }

      // Note: Each sensor's start function already calls sensorStatusBumpWith(),
      // so we don't need to bump here (would cause redundant SSE events)

      } // end else (not alreadyRunning)

      } while (mgr->dequeueDeviceStart(&req)); // drain entire batch

      // Resume sensor polling after ALL queued sensors are initialized.
      pollResume();
      INFO_I2C_AUTOSTARTF("Resumed sensor polling after initialization batch");
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
// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
//
// Bus 0 = "I2C1" = primary STEMMA QT (Wire1 internally). Always available.
// Bus 1 = "I2C2" = secondary STEMMA QT (Wire internally). Only meaningful on
//                  boards with a second I2C port wired (FeatherS3[D]'s
//                  vertical STEMMA QT). On other boards I2C2_*_PIN_DEFAULT
//                  is -1 so the pin range below clamps to the default and
//                  the bus stays disabled regardless of what the user toggles.
// The min/max of -1..HW_GPIO_MAX on the bus-1 SDA/SCL pin entries (vs 0..HW_GPIO_MAX
// on bus 0) allows the "unavailable" sentinel -1 to round-trip through settings save.
// HW_GPIO_MAX (defined above) is the SoC's highest GPIO, so the bound tracks the board.
static const SettingEntry i2cSettingEntries[] = {
  { "i2cBusEnabled", SETTING_BOOL, &gSettings.i2cEnabled, 1, 0, nullptr, 0, 1, "I2C1 Bus Enabled (reboot required)", nullptr, false, nullptr, "i2cbusenabled" },
  { "i2cSdaPin", SETTING_INT, &gSettings.i2cSdaPin, I2C_SDA_PIN_DEFAULT,
    0, nullptr, 0, HW_GPIO_MAX, "I2C1 SDA Pin (reboot required)", nullptr, false, nullptr, "i2csdapin" },
  { "i2cSclPin", SETTING_INT, &gSettings.i2cSclPin, I2C_SCL_PIN_DEFAULT,
    0, nullptr, 0, HW_GPIO_MAX, "I2C1 SCL Pin (reboot required)", nullptr, false, nullptr, "i2csclpin" },
  { "i2c2BusEnabled", SETTING_BOOL, &gSettings.i2c2Enabled, I2C2_BUS_ENABLED_DEFAULT, 0, nullptr, 0, 1, "I2C2 Bus Enabled (reboot required)", nullptr, false, nullptr, "i2c2busenabled" },
  { "i2c2SdaPin", SETTING_INT, &gSettings.i2c2SdaPin, I2C2_SDA_PIN_DEFAULT,
    0, nullptr, -1, HW_GPIO_MAX, "I2C2 SDA Pin (reboot required, -1=unavailable)", nullptr, false, nullptr, "i2c2sdapin" },
  { "i2c2SclPin", SETTING_INT, &gSettings.i2c2SclPin, I2C2_SCL_PIN_DEFAULT,
    0, nullptr, -1, HW_GPIO_MAX, "I2C2 SCL Pin (reboot required, -1=unavailable)", nullptr, false, nullptr, "i2c2sclpin" },
  // Per-device bus assignment (0=I2C1/Wire1, 1=I2C2/Wire). Reboot required.
  // `options = "0|I2C1,1|I2C2"` makes the Settings page render each as a
  // labeled <select> dropdown instead of a 0..1 number input.
  { "oledBus",     SETTING_INT, &gSettings.oledBus,     OLED_BUS_DEFAULT, 0, nullptr, 0, 1, "OLED bus (reboot required)",            "0|I2C1,1|I2C2", false, nullptr, "oledbus" },
  { "inputBus",    SETTING_INT, &gSettings.inputBus,    0, 0, nullptr, 0, 1, "Input device bus (reboot required)",     "0|I2C1,1|I2C2", false, nullptr, "inputbus" },
  { "gpsBus",      SETTING_INT, &gSettings.gpsBus,      0, 0, nullptr, 0, 1, "GPS bus (reboot required)",             "0|I2C1,1|I2C2", false, nullptr, "gpsbus" },
  { "rtcBus",      SETTING_INT, &gSettings.rtcBus,      0, 0, nullptr, 0, 1, "RTC bus (reboot required)",             "0|I2C1,1|I2C2", false, nullptr, "rtcbus" },
  { "fmRadioBus",  SETTING_INT, &gSettings.fmRadioBus,  0, 0, nullptr, 0, 1, "FM radio bus (reboot required)",        "0|I2C1,1|I2C2", false, nullptr, "fmradiobus" },
  { "presenceBus", SETTING_INT, &gSettings.presenceBus, 0, 0, nullptr, 0, 1, "Presence bus (reboot required)",        "0|I2C1,1|I2C2", false, nullptr, "presencebus" },
  { "imuBus",      SETTING_INT, &gSettings.imuBus,      0, 0, nullptr, 0, 1, "IMU bus (reboot required)",             "0|I2C1,1|I2C2", false, nullptr, "imubus" },
  { "thermalBus",  SETTING_INT, &gSettings.thermalBus,  0, 0, nullptr, 0, 1, "Thermal bus (reboot required)",         "0|I2C1,1|I2C2", false, nullptr, "thermalbus" },
  { "tofBus",      SETTING_INT, &gSettings.tofBus,      0, 0, nullptr, 0, 1, "ToF bus (reboot required)",             "0|I2C1,1|I2C2", false, nullptr, "tofbus" },
  { "apdsBus",     SETTING_INT, &gSettings.apdsBus,     0, 0, nullptr, 0, 1, "APDS bus (reboot required)",            "0|I2C1,1|I2C2", false, nullptr, "apdsbus" },
  { "servoBus",    SETTING_INT, &gSettings.servoBus,    0, 0, nullptr, 0, 1, "Servo bus (reboot required)",           "0|I2C1,1|I2C2", false, nullptr, "servobus" },
  { "fuelGaugeBus",SETTING_INT, &gSettings.fuelGaugeBus,0, 0, nullptr, 0, 1, "Fuel gauge bus (reboot required)",      "0|I2C1,1|I2C2", false, nullptr, "fuelgaugebus" }
};

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule i2cSettingsModule = {
  "i2c", "hardware.i2c", i2cSettingEntries,
  sizeof(i2cSettingEntries) / sizeof(i2cSettingEntries[0]),
  nullptr,
  "I2C bus configuration"
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp

// ============================================================================
// Process Sensor Auto-Start on Boot
// Note: autoStart settings are now in each sensor's own module:
// - thermal (i2csensor_mlx90640.cpp)
// - tof (i2csensor_vl53l4cx.cpp)
// - imu (i2csensor_bno055.cpp)
// - gps (i2csensor_pa1010d.cpp)
// - fmradio (i2csensor_rda5807.cpp)
// - apds (i2csensor_apds9960.cpp)
// - gamepad (i2csensor_seesaw.cpp)
// ============================================================================

// Check if a sensor is available for auto-start:
// 1. Fast path: check connectedDevices[] registry (populated by discoverI2CDevices)
// 2. Fallback: live I2C ping in case the scan missed the device (transient bus issue)
static bool isSensorAvailableForAutoStart(const char* moduleName, I2CDeviceType deviceType) {
  if (isSensorConnected(moduleName)) return true;
  // Registry miss — try a direct I2C ping as fallback
  uint8_t addr = i2cAddressForDeviceType(deviceType);
  if (addr != 0 && i2cPingAddress(addr, 100000, 50)) {
    INFO_I2C_AUTOSTARTF("[AutoStart] %s not in registry but responds to I2C ping", moduleName);
    return true;
  }
  // Reached only when the sensor's autostart flag is set (every caller gates on
  // it), so a false return here means "enabled in settings but not detected" —
  // durable so a silently-absent sensor is attributable after reboot.
  logSystemEvent("SENSOR", "%s autostart skipped: enabled in settings but not detected on I2C bus", moduleName);
  systemEventPost(SYSEVT_SENSOR_START_FAILED, moduleName, "not detected on bus");

  // Tell ramflush this sensor is off because it wasn't there, not because the user
  // turned it off. Without this, an unplugged sensor reads live=false against
  // intent=true and the next capture records "user turned it off" — which would
  // suppress the configured autostart on every later boot, with replugging the
  // sensor doing nothing to undo it.
  ramFlushMarkAutostartFailed(ramFlushIdForModule(moduleName));
  return false;
}

void processAutoStartSensors() {
  // Debug: Print I2C flags to diagnose auto-start issues
  DEBUG_I2C_AUTOSTARTF("[AutoStart] I2C check: i2cBus=%d", gSettings.i2cEnabled ? 1 : 0);
  
  if (!gSettings.i2cEnabled) {
    INFO_I2C_AUTOSTARTF("[AutoStart] I2C bus disabled, skipping sensor auto-start");
    return;
  }

 #if ENABLE_I2C_SYSTEM
  if (!queueProcessorTask) {
    const uint32_t queueStackWords = SENSOR_QUEUE_STACK_WORDS;
    // Pin to Core 1 (I2C_SENSOR_CORE) — same rationale as the primary create site
    // in HardwareOne.cpp: this task runs the I2C device-init transactions and must
    // not float onto Wi-Fi-saturated Core 0 (starve → bus-storm → panic(4)).
    if (xTaskCreateLogged(sensorQueueProcessorTask, "sensor_queue_task", queueStackWords, nullptr, TASK_PRIORITY_LOW, &queueProcessorTask, "sensor.queue", I2C_SENSOR_CORE) != pdPASS) {
      ERROR_I2CF("[I2C_SENSORS] Failed to create sensor queue processor task (late init)");
      queueProcessorTask = nullptr;
      return;
    }
    INFO_I2C_DISCOVERYF("[I2C_SENSORS] Queue processor task created (late init)");
  }
 #endif
  
  INFO_I2C_AUTOSTARTF("[AutoStart] Flags: thermal=%d tof=%d imu=%d gps=%d fmradio=%d apds=%d gamepad=%d rtc=%d presence=%d",
            gSettings.thermalAutoStart ? 1 : 0,
            gSettings.tofAutoStart ? 1 : 0,
            gSettings.imuAutoStart ? 1 : 0,
            gSettings.gpsAutoStart ? 1 : 0,
            gSettings.fmRadioAutoStart ? 1 : 0,
            gSettings.apdsAutoStart ? 1 : 0,
            gSettings.inputAutoStart ? 1 : 0,
            gSettings.rtcAutoStart ? 1 : 0,
            gSettings.presenceAutoStart ? 1 : 0);
  INFO_I2C_AUTOSTARTF("[AutoStart] Processing sensor auto-start settings...");
  int autoStartQueued = 0;
  
  #if ENABLE_THERMAL_SENSOR
  if (gSettings.thermalEnabled && ramFlushResolve(RF_THERMAL, gSettings.thermalAutoStart)) {
    if (isSensorAvailableForAutoStart("thermal", I2C_DEVICE_THERMAL)) {
      INFO_I2C_AUTOSTARTF("[AutoStart] Queuing thermal sensor");
      enqueueDeviceStart(I2C_DEVICE_THERMAL); autoStartQueued++;
    } else {
      INFO_I2C_AUTOSTARTF("[AutoStart] Skipping thermal sensor (not detected on I2C bus)");
    }
  }
  #endif
  
  #if ENABLE_TOF_SENSOR
  if (gSettings.tofEnabled && ramFlushResolve(RF_TOF, gSettings.tofAutoStart)) {
    if (isSensorAvailableForAutoStart("tof", I2C_DEVICE_TOF)) {
      INFO_I2C_AUTOSTARTF("[AutoStart] Queuing ToF sensor");
      enqueueDeviceStart(I2C_DEVICE_TOF); autoStartQueued++;
    } else {
      INFO_I2C_AUTOSTARTF("[AutoStart] Skipping ToF sensor (not detected on I2C bus)");
    }
  }
  #endif
  
  #if ENABLE_IMU_SENSOR
  if (gSettings.imuEnabled && ramFlushResolve(RF_IMU, gSettings.imuAutoStart)) {
    if (isSensorAvailableForAutoStart("imu", I2C_DEVICE_IMU)) {
      INFO_I2C_AUTOSTARTF("[AutoStart] Queuing IMU sensor");
      enqueueDeviceStart(I2C_DEVICE_IMU); autoStartQueued++;
    } else {
      INFO_I2C_AUTOSTARTF("[AutoStart] Skipping IMU sensor (not detected on I2C bus)");
    }
  }
  #endif
  
  #if ENABLE_GPS_SENSOR
  if (gSettings.gpsEnabled && ramFlushResolve(RF_GPS, gSettings.gpsAutoStart)) {
    if (isSensorAvailableForAutoStart("gps", I2C_DEVICE_GPS)) {
      INFO_I2C_AUTOSTARTF("[AutoStart] Queuing GPS sensor");
      enqueueDeviceStart(I2C_DEVICE_GPS); autoStartQueued++;
    } else {
      INFO_I2C_AUTOSTARTF("[AutoStart] Skipping GPS sensor (not detected on I2C bus)");
    }
  }
  #endif
  
  #if ENABLE_FM_RADIO
  if (gSettings.fmRadioEnabled && ramFlushResolve(RF_FMRADIO, gSettings.fmRadioAutoStart)) {
    if (isSensorAvailableForAutoStart("fmradio", I2C_DEVICE_FMRADIO)) {
      INFO_I2C_AUTOSTARTF("[AutoStart] Queuing FM Radio sensor");
      enqueueDeviceStart(I2C_DEVICE_FMRADIO); autoStartQueued++;
    } else {
      INFO_I2C_AUTOSTARTF("[AutoStart] Skipping FM Radio sensor (not detected on I2C bus)");
    }
  }
  #endif
  
  #if ENABLE_APDS_SENSOR
  if (gSettings.apdsEnabled && ramFlushResolve(RF_APDS, gSettings.apdsAutoStart)) {
    if (isSensorAvailableForAutoStart("apds", I2C_DEVICE_APDS)) {
      INFO_I2C_AUTOSTARTF("[AutoStart] Queuing APDS sensor");
      enqueueDeviceStart(I2C_DEVICE_APDS); autoStartQueued++;
    } else {
      INFO_I2C_AUTOSTARTF("[AutoStart] Skipping APDS sensor (not detected on I2C bus)");
    }
  }
  #endif
  
  #if ENABLE_GAMEPAD_SENSOR
  if (gSettings.inputEnabled && ramFlushResolve(RF_INPUT, gSettings.inputAutoStart)) {
    if (isSensorAvailableForAutoStart("gamepad", I2C_DEVICE_INPUT)) {
      INFO_I2C_AUTOSTARTF("[AutoStart] Queuing Gamepad sensor");
      enqueueDeviceStart(I2C_DEVICE_INPUT); autoStartQueued++;
    } else {
      INFO_I2C_AUTOSTARTF("[AutoStart] Skipping Gamepad sensor (not detected on I2C bus)");
    }
  }
  #endif
  
  #if ENABLE_RTC_SENSOR
  if (gSettings.rtcEnabled && ramFlushResolve(RF_RTC, gSettings.rtcAutoStart)) {
    if (isSensorAvailableForAutoStart("rtc", I2C_DEVICE_RTC)) {
      INFO_I2C_AUTOSTARTF("[AutoStart] Queuing RTC sensor");
      enqueueDeviceStart(I2C_DEVICE_RTC); autoStartQueued++;
    } else {
      INFO_I2C_AUTOSTARTF("[AutoStart] Skipping RTC sensor (not detected on I2C bus)");
    }
  }
  #endif
  
  #if ENABLE_PRESENCE_SENSOR
  if (gSettings.presenceEnabled && ramFlushResolve(RF_PRESENCE, gSettings.presenceAutoStart)) {
    if (isSensorAvailableForAutoStart("presence", I2C_DEVICE_PRESENCE)) {
      INFO_I2C_AUTOSTARTF("[AutoStart] Queuing Presence sensor");
      enqueueDeviceStart(I2C_DEVICE_PRESENCE); autoStartQueued++;
    } else {
      INFO_I2C_AUTOSTARTF("[AutoStart] Skipping Presence sensor (not detected on I2C bus)");
    }
  }
  #endif
  
  INFO_I2C_AUTOSTARTF("[AutoStart] Queued %d sensor(s) for startup", autoStartQueued);
  INFO_I2C_AUTOSTARTF("[AutoStart] Sensor auto-start processing complete");
}

// Reset Wire1 to the bus's default clock speed. Sensors that bump the bus
// to a higher frequency for their own transactions (e.g. MLX90640's 1 MHz
// burst reads, VL53L4CX's fast frames) call this in their init/cleanup
// paths to leave the bus in a known state for other devices that share it.
void i2cSetDefaultWire1Clock() {
  Wire1.setClock(I2C_WIRE1_DEFAULT_FREQ);
}
