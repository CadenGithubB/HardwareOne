/**
 * System_I2C.h - Unified I2C System Interface
 * Clean interface that delegates to I2CDevice/Manager architecture
 */

#ifndef I2C_SYSTEM_H
#define I2C_SYSTEM_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <Wire.h>

#include "System_BuildConfig.h"
#include "System_I2C_Manager.h"
#include "System_Mutex.h"
#include "System_Utils.h"

// ============================================================================
// Legacy Wrapper Functions - Delegate to Manager
// ============================================================================

// Transaction wrappers
template<typename Func>
auto i2cDeviceTransaction(uint8_t address, uint32_t clockHz, uint32_t timeoutMs, Func&& operation) 
    -> decltype(operation()) {
  extern bool gI2CBusEnabled;
  if (!gI2CBusEnabled) return decltype(operation())();
  
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (!mgr) return decltype(operation())();
  
  I2CDevice* dev = mgr->getDevice(address);
  if (!dev) dev = mgr->registerDevice(address, "Auto", clockHz, timeoutMs);
  if (!dev) return decltype(operation())();
  
  return dev->transaction(std::forward<Func>(operation), I2CDevice::Mode::STANDARD);
}

template<typename Func>
void i2cDeviceTransactionVoid(uint8_t address, uint32_t clockHz, uint32_t timeoutMs, Func&& operation) {
  extern bool gI2CBusEnabled;
  if (!gI2CBusEnabled) return;
  
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (!mgr) return;
  
  I2CDevice* dev = mgr->getDevice(address);
  if (!dev) dev = mgr->registerDevice(address, "Auto", clockHz, timeoutMs);
  if (!dev) return;
  
  dev->transaction(std::forward<Func>(operation), I2CDevice::Mode::STANDARD);
}

template<typename Func>
auto i2cTaskWithStandardTimeout(uint8_t address, uint32_t clockHz, Func&& operation) 
    -> decltype(operation()) {
  return i2cDeviceTransaction(address, clockHz, 1000, std::forward<Func>(operation));
}

template<typename Func>
auto i2cTaskWithTimeout(uint8_t address, uint32_t clockHz, uint32_t maxMs, Func&& operation) 
    -> decltype(operation()) {
  return i2cDeviceTransaction(address, clockHz, maxMs, std::forward<Func>(operation));
}

template<typename Func>
void i2cTransactionVoid(uint32_t clockHz, uint32_t timeoutMs, Func&& operation) {
  i2cDeviceTransactionVoid(0x3D, clockHz, timeoutMs, std::forward<Func>(operation));
}

template<typename Func>
auto i2cTransaction(uint32_t clockHz, uint32_t timeoutMs, Func&& operation) 
    -> decltype(operation()) {
  return i2cDeviceTransaction(0x3D, clockHz, timeoutMs, std::forward<Func>(operation));
}

template<typename Func>
void i2cTransactionNACKTolerant(uint8_t address, uint32_t clockHz, uint32_t timeoutMs, Func&& operation) {
  extern bool gI2CBusEnabled;
  if (!gI2CBusEnabled) return;
  
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (!mgr) return;
  
  I2CDevice* dev = mgr->getDevice(address);
  if (!dev) dev = mgr->registerDevice(address, "Auto", clockHz, timeoutMs);
  if (!dev) return;
  
  dev->transaction(std::forward<Func>(operation), I2CDevice::Mode::NACK_TOLERANT);
}

// Ping/probe helpers - DO NOT auto-register devices during probe!
// These are used by i2cscan to check if devices exist, not to set them up
inline uint8_t i2cProbeAddress(uint8_t address, uint32_t clockHz, uint32_t timeoutMs) {
  extern TwoWire Wire1;
  extern SemaphoreHandle_t i2cMutex;
  extern bool gI2CBusEnabled;
  
  if (!gI2CBusEnabled) return 4;
  
  uint8_t err = 4;
  if (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE) {
    Wire1.setClock(clockHz);
    Wire1.beginTransmission(address);
    err = Wire1.endTransmission();
    xSemaphoreGive(i2cMutex);
  }
  return err;
}

inline bool i2cPingAddress(uint8_t address, uint32_t clockHz, uint32_t timeoutMs) {
  return (i2cProbeAddress(address, clockHz, timeoutMs) == 0);
}

// Queue functions
inline bool enqueueSensorStart(SensorType sensor) {
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  return mgr ? mgr->enqueueSensorStart(sensor) : false;
}

inline bool dequeueSensorStart(SensorStartRequest* req) {
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  return mgr ? mgr->dequeueSensorStart(req) : false;
}

inline bool isInQueue(SensorType sensor) {
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  return mgr ? mgr->isInQueue(sensor) : false;
}

inline int getQueuePosition(SensorType sensor) {
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  return mgr ? mgr->getQueuePosition(sensor) : -1;
}

inline int getQueueDepth() {
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  return mgr ? mgr->getQueueDepth() : 0;
}

// Health functions
inline bool i2cDeviceIsDegraded(uint8_t address) {
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (!mgr) return false;
  I2CDevice* dev = mgr->getDevice(address);
  return dev ? dev->isDegraded() : false;
}

inline void i2cDeviceSuccess(uint8_t address) {
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (!mgr) return;
  I2CDevice* dev = mgr->getDevice(address);
  if (dev) dev->recordSuccess();
}

inline void i2cDeviceError(uint8_t address) {
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (!mgr) return;
  I2CDevice* dev = mgr->getDevice(address);
  if (dev) dev->recordError(I2CErrorType::NACK, 0x02);
}

// Check if sensor should auto-disable based on consecutive I2C failures
// Uses existing I2CDevice health tracking - no local counters needed in sensor tasks
inline bool i2cShouldAutoDisable(uint8_t address, uint8_t maxConsecutiveErrors = 5) {
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (!mgr) return false;
  I2CDevice* dev = mgr->getDevice(address);
  if (!dev) return false;
  return dev->getHealth().consecutiveErrors >= maxConsecutiveErrors;
}

// Get current consecutive error count for a device (for logging)
inline uint8_t i2cGetConsecutiveErrors(uint8_t address) {
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (!mgr) return 0;
  I2CDevice* dev = mgr->getDevice(address);
  return dev ? dev->getHealth().consecutiveErrors : 0;
}

inline bool i2cBusRecovery() {
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (!mgr) return false;
  mgr->performBusRecovery();
  return true;
}

inline void i2cBusHealthCheck() {
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (mgr) mgr->healthCheck();
}

#if ENABLE_THERMAL_SENSOR
#include <Adafruit_MLX90640.h>
#endif

// ============================================================================
// I2C Device Addresses
// ============================================================================
#define I2C_ADDR_GPS        0x10
#define I2C_ADDR_FM_RADIO   0x11
#define I2C_ADDR_IMU        0x28
#define I2C_ADDR_TOF        0x29
#define I2C_ADDR_THERMAL    0x33
#define I2C_ADDR_APDS       0x39
#define I2C_ADDR_OLED       0x3D
#define I2C_ADDR_GAMEPAD    0x50

// ============================================================================
// Global Flags and Configuration
// ============================================================================
extern bool gI2CBusEnabled;
extern volatile bool gSensorPollingPaused;

// ============================================================================
// I2C Sensor Database
// ============================================================================
struct I2CSensorEntry {
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
};

extern const I2CSensorEntry i2cSensors[];
extern const size_t i2cSensorsCount;

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

// ============================================================================
// Initialization Functions
// ============================================================================
void initI2CBuses();
void initI2CManager();
void initSensorQueue();

// ============================================================================
// Discovery and Helper Functions
// ============================================================================
void discoverI2CDevices();
bool isSensorConnected(const char* name);
String identifySensor(uint8_t address);

// ============================================================================
// Sensor Task Functions
// ============================================================================
void tofTask(void* parameter);
void imuTask(void* parameter);
void thermalTask(void* parameter);
void gamepadTask(void* parameter);
void apdsTask(void* parameter);
void gpsTask(void* parameter);
void sensorQueueProcessorTask(void* param);

// Queue processor task handle (defined in System_I2C.cpp)
extern TaskHandle_t queueProcessorTask;

// Sensor Auto-Start (call during boot after I2C init)
void processAutoStartSensors();

// ============================================================================
// Sensor Command Handlers
// ============================================================================
const char* cmd_thermalstart(const String& cmd);
const char* cmd_thermalstop(const String& cmd);
const char* cmd_tofstart(const String& cmd);
const char* cmd_tofstop(const String& cmd);
const char* cmd_tof(const String& cmd);
const char* cmd_imustart(const String& cmd);
const char* cmd_imustop(const String& cmd);
const char* cmd_imu(const String& cmd);
const char* cmd_gamepadstart(const String& cmd);
const char* cmd_gamepadstop(const String& cmd);
const char* cmd_gamepad(const String& cmd);
const char* cmd_apdscolor(const String& cmd);
const char* cmd_apdsproximity(const String& cmd);
const char* cmd_apdsgesture(const String& cmd);
const char* cmd_apdscolorstart(const String& cmd);
const char* cmd_apdscolorstop(const String& cmd);
const char* cmd_apdsproximitystart(const String& cmd);
const char* cmd_apdsproximitystop(const String& cmd);
const char* cmd_apdsgesturestart(const String& cmd);
const char* cmd_apdsgesturestop(const String& cmd);
const char* cmd_gpsstart(const String& cmd);
const char* cmd_gpsstop(const String& cmd);
const char* cmd_gps(const String& cmd);
const char* cmd_imuactions(const String& cmd);
const char* cmd_sensorlog(const String& cmd);
const char* cmd_i2cclockthermalhz(const String& cmd);
const char* cmd_i2cclocktofhz(const String& cmd);
const char* cmd_i2cscan(const String& cmd);
const char* cmd_i2cstats(const String& cmd);
const char* cmd_i2cmetrics(const String& cmd);
const char* cmd_i2chealth(const String& cmd);
const char* cmd_sensors(const String& cmd);
const char* cmd_sensorinfo(const String& cmd);
const char* cmd_devices(const String& cmd);
const char* cmd_devicefile(const String& cmd);

// ============================================================================
// I2C Command Registry
// ============================================================================
extern const CommandEntry i2cCommands[];
extern const size_t i2cCommandsCount;

// ============================================================================
// Sensor State Variables
// ============================================================================
extern volatile UBaseType_t gTofWatermarkNow;
extern volatile UBaseType_t gTofWatermarkMin;
extern volatile UBaseType_t gIMUWatermarkNow;
extern volatile UBaseType_t gIMUWatermarkMin;
extern volatile UBaseType_t gThermalWatermarkNow;
extern volatile UBaseType_t gThermalWatermarkMin;
extern volatile UBaseType_t gGamepadWatermarkNow;
extern volatile UBaseType_t gGamepadWatermarkMin;
extern uint32_t gWire1DefaultHz;
extern unsigned long tofLastStopTime;
extern unsigned long thermalLastStopTime;
extern volatile bool imuInitRequested;
extern volatile bool imuInitResult;
extern volatile bool imuInitDone;
extern volatile bool thermalInitRequested;
extern volatile bool thermalInitResult;
extern volatile bool thermalInitDone;

// ============================================================================
// Helper Functions
// ============================================================================
void drainDebugRing();
bool lockThermalCache(TickType_t timeout = portMAX_DELAY);
void unlockThermalCache();
void i2cSetDefaultWire1Clock();
void sensorStatusBumpWith(const char* reason);
const char* buildSensorStatusJson();
bool initIMUSensor();
bool initFMRadio();
void startFMRadioInternal();
void stopFMRadioInternal();

// ============================================================================
// Thermal Sensor Externs
// ============================================================================
#if ENABLE_THERMAL_SENSOR
extern Adafruit_MLX90640* gMLX90640;
extern volatile uint32_t thermalArmAtMs;
extern volatile bool thermalPendingFirstFrame;
#endif

// ============================================================================
// I2C Bus Configuration
// ============================================================================
// Note: I2C pins are now configurable via settings (gSettings.i2cSdaPin, gSettings.i2cSclPin)
// Defaults are board-specific - see System_BuildConfig.h for BOARD_NAME and pin mappings
#define I2C_WIRE1_DEFAULT_FREQ 100000

// ============================================================================
// Legacy Compatibility Externs (for old code still using these)
// ============================================================================
extern uint32_t gWire1CurrentHz;
extern uint32_t* gI2CClockStack;
extern int gI2CClockStackDepth;
extern const int kI2CClockStackMax;

// Legacy health tracking (kept for compatibility, delegates to manager)
#define MAX_TRACKED_I2C_DEVICES 8
#define I2C_DEVICE_ERROR_THRESHOLD 3
#define I2C_DEVICE_RECOVERY_TIMEOUT_MS 30000
#define I2C_DEVICE_INIT_GRACE_PERIOD_MS 15000

struct I2CDeviceHealth {
  uint8_t address;
  uint8_t consecutiveErrors;
  uint16_t totalErrors;
  bool degraded;
  uint32_t lastErrorTime;
  uint32_t lastSuccessTime;
  uint32_t registrationTime;
  const char* deviceName;
  uint8_t nackCount;
  uint8_t timeoutCount;
  uint8_t busErrorCount;
  I2CErrorType lastErrorType;
  uint32_t adaptiveTimeoutMs;
};

extern I2CDeviceHealth gI2CDeviceHealth[MAX_TRACKED_I2C_DEVICES];
extern int gI2CDeviceCount;

// Legacy health API (delegates to manager)
void i2cRegisterDevice(uint8_t address, const char* name);
void i2cResetGracePeriod(uint8_t address);
bool i2cAttemptDeviceRecovery(uint8_t address);

#endif // I2C_SYSTEM_H
