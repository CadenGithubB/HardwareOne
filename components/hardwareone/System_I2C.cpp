/**
 * Sensor System - I2C Sensor Tasks and Commands
 * i2c_system.cpp
 * I2C sensor task implementations and management
 * Extracted from HardwareOne.ino
 */

#include <ArduinoJson.h>
#include <atomic>
#include <Wire.h>
#include <esp_log.h>   // esp_log_level_set — quiet probe NACKs during detect scan
#include <soc/soc_caps.h>  // SOC_GPIO_PIN_COUNT — board's GPIO count for pin-range limits

#include "i2csensor_rda5807.h"
#include "System_BuildConfig.h"
#include "Input_GPIOEncoder.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_FirstTimeSetup.h"
#include "System_FeatureRegistry.h"  // getFeatureById/isFeatureEnabled — hardware-detect diff
#include "System_PollPause.h"        // PollPauseGuard — quiesce a bus during the detect scan
#include "System_AuthIdentity.h"     // currentExecIsAdmin — gate 'detect apply'
#include "System_I2C.h"
#include "System_RamFlush.h"
#include "System_Logging.h"
#include "System_SelfDevice.h"        // SelfDevice::uptimeSeconds — i2cmetrics rates
#include "System_Mutex.h"
#include "System_MemUtil.h"
#include "System_Notifications.h"
#include "System_SensorRegistry.h"
#include "System_SensorRestartCore.h"  // backoff policy for auto-disabled sensors
#include "System_Events.h"          // systemEventPost — sensor lifecycle events

#if ENABLE_ESPNOW
#include "System_ESPNow.h"
#include "System_ESPNow_Sensors.h"
#endif
#include "System_Settings.h"
#include "i2csensor_ht16k33.h"
#include "System_TaskUtils.h"
#include "System_Utils.h"
#include "System_CommandRegistry.h"
#include "System_Camera_Video.h"
#include "System_Camera_DVP.h"
#if ENABLE_EDGE_IMPULSE
#include "System_EdgeImpulse.h"
#endif
#include "System_Microphone.h"
#if ENABLE_SERVO
#include "i2csensor_pca9685.h"
#endif

#include "i2csensor_apds9960.h"
#if ENABLE_ANO_ENCODER
#include "i2csensor_ano_encoder.h"
#endif
#include "i2csensor_seesaw.h"
#include "i2csensor_pa1010d.h"
#include "i2csensor_bno055.h"
#include "i2csensor_mlx90640.h"
#include "i2csensor_vl53l4cx.h"
#include "i2csensor_ds3231.h"
#include "i2csensor_sths34pf80.h"
#include "i2csensor_encoder.h"
#include "System_SeesawIdentity.h"

// ============================================================================
// Unified I2C Manager Initialization
// ============================================================================

// A row's driver is in this build. Each i2cSensors[] row states it.
static bool isSensorCompiled(const I2cSensorEntry& sensor) {
  return sensor.compiled;
}

// The registry is keyed by (bus, address), so boot-time identity records must
// follow the same per-device routing settings used by the drivers.  Registering
// every compiled device on bus 0 creates a "ghost" primary-bus record whenever
// the real device is routed to I2C2; address-only health lookups can then report
// and recover the wrong record. Each i2cSensors[] row names its bus setting.
// Instance n's bus (System_I2CInstanceCore.h); a row without a bus setting
// registers on bus 0.
static uint8_t configuredBusForSensor(const I2cSensorEntry& sensor, uint8_t n = 1) {
  const int rowBus = i2cRowBusAt(sensor, n);
  const int configuredBus = rowBus < 0 ? 0 : rowBus;

  if (configuredBus < 0 || configuredBus >= I2cDeviceManager::NUM_BUSES) {
    WARN_I2CF("Invalid configured bus %d for 0x%02X (%s); using bus 0",
              configuredBus, sensor.address, sensor.name ? sensor.name : "?");
    return 0;
  }
  return (uint8_t)configuredBus;
}

// Resolve configurable identity consistently for scans and health
// registration: the row with instance n's address (0 when instance n is not
// configured).
static I2cSensorEntry configuredSensor(const I2cSensorEntry& source, uint8_t n = 1) {
  I2cSensorEntry sensor = source;
  if (sensor.addressAt || n != 1) sensor.address = i2cRowAddressAt(source, n);
  return sensor;
}

// Initialize the unified I2C manager singleton
void initI2cManager() {
  I2cDeviceManager::initialize();
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  
  // Print device registry capacity
  INFO_I2C_DISCOVERYF("[I2C_REGISTRY] Device manager initialized with capacity for %d devices", I2cDeviceManager::MAX_DEVICES);
  
  
  // Pre-register only compiled-in device identities on their configured buses.
  // Clock and lock-wait policy belongs to each transaction, not this registry.
  // Instance 1 of every compiled row registers as before; an instance n >= 2
  // only when it is configured (an address set), named for its health events
  // ("Encoder 2"). Two rows of one device (the encoder boards) share one record.
  int compiledCount = 0;
  for (size_t i = 0; i < i2cSensorsCount; i++) {
    if (!isSensorCompiled(i2cSensors[i])) continue;
    const uint8_t instances = i2cRowInstances(i2cSensors[i]);
    for (uint8_t n = 1; n <= instances; n++) {
      const I2cSensorEntry sensor = configuredSensor(i2cSensors[i], n);
      if (n >= 2 && sensor.address == 0) continue;  // instance n is not configured
      // The SSD1306 may live at 0x3D or 0x3C. Register it only after probing,
      // otherwise a permanent 0x3D ghost pollutes health when the panel is 0x3C.
      if (sensor.address == 0x3D && sensor.altAddress == 0x3C) continue;
      const uint8_t bus = configuredBusForSensor(sensor, n);
      I2cDevice* dev = mgr->registerDevice(sensor.address, sensor.name, bus);
      if (dev) {
        if (n >= 2) {
          dev->typeId = static_cast<uint8_t>(sensor.deviceType);
          dev->instance = n;
        }
        compiledCount++;
        INFO_I2C_DISCOVERYF("Pre-registered compiled device: 0x%02X (%s, instance %u) on bus %u",
                            sensor.address, sensor.name, (unsigned)n, bus);
      } else {
        ERROR_I2CF("Failed to pre-register compiled device: 0x%02X (%s, instance %u) on bus %u",
                   sensor.address, sensor.name, (unsigned)n, bus);
      }
    }
  }
  
  INFO_I2C_DISCOVERYF("Pre-registered %d compiled devices from database", compiledCount);
  
  // Print registry summary
  INFO_I2C_DISCOVERYF("[I2C_REGISTRY] Registration summary: %d/%d slots used (%d available)", 
                compiledCount, I2cDeviceManager::MAX_DEVICES, 
                I2cDeviceManager::MAX_DEVICES - compiledCount);
}

// Global I2C bus enabled flag (mirrors gSettings.i2cEnabled)
// Defaults to false; set correctly inside initI2cBuses() before any transactions run
bool gI2cBusRunning = false;

// The sensor-polling pause primitive (gSensorPollingPaused mirror + the
// reference-counted pollPause/pollResume / sensorPolling* ops) moved to its own
// module, System_PollPause.{h,cpp}. The API arrives here via System_I2C.h's
// include of System_PollPause.h, so the readers and callers below are unchanged.

// Queue processor task handle
TaskHandle_t queueProcessorTask = nullptr;

// gImuTaskHandle and gImuLastStopTime are declared in i2csensor_bno055.h (included above)
// Clock management is now unified through I2cDeviceManager
// Legacy i2cSetWire1Clock() removed - all sensors use i2cDeviceTransaction wrapper

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
// I2cSensorEntry struct is defined in i2c_system.h

// I2C Sensor Database - Sensors actually used/detected in this system
// Entry format: { address, name, description, manufacturer, multiAddress, altAddress,
//                libraryHeapBytes, libraryName, headerGuard, moduleName, i2cClockHz, i2cTimeoutMs,
//                compiled, busAt, addressAt, deviceType[, seesawProductId[, instances]] }
// The second line of a row is what the row is: whether its driver is in this
// build, the setting that routes each instance to a bus, each instance's
// address when that is a setting, its id in the start/stop queue, for a
// seesaw board the product id that names it and, for a multi-instance device,
// how many instances it can be configured with.
#if ENABLE_LED_MATRIX
#define MATRIX_BUS     [](uint8_t) -> int { return matrixDeviceBus(); }
#define MATRIX_ADDRESS [](uint8_t) -> uint8_t { return matrixDeviceAddress(); }
#else
#define MATRIX_BUS     nullptr
#define MATRIX_ADDRESS nullptr
#endif
// The encoder rows name every instance (System_I2CInstanceCore.h): instance
// n's bus and address come from its settings, address 0 when it has none.
#if ENABLE_ENCODER
#define ENCODER_BUS       [](uint8_t n) -> int { return encoderInstanceBus(n); }
#define ENCODER_ADDRESS   [](uint8_t n) -> uint8_t { return encoderInstanceAddress(n); }
#define ENCODER_INSTANCES kI2cTypes[I2C_DEVICE_ENCODER].maxInstances
#else
#define ENCODER_BUS       nullptr
#define ENCODER_ADDRESS   nullptr
#define ENCODER_INSTANCES 1
#endif
const I2cSensorEntry i2cSensors[] = {
  // Sensors with CLI Modules
  // { addr, name, desc, mfr, multiAddr, altAddr, heapBytes, library, headerGuard, module, clockHz, timeoutMs }
  { 0x28, "BNO055", "9-DOF IMU", "Adafruit", true, 0x29, 1500, "Adafruit_BNO055", "_ADAFRUIT_BNO055_H_", "imu", 100000, 300,
    ENABLE_IMU_SENSOR, [](uint8_t) -> int { return gSettings.imuBus; }, nullptr, I2C_DEVICE_IMU },
  { 0x39, "APDS9960", "RGB, Gesture & Proximity", "Adafruit", false, 0x00, 500, "Adafruit_APDS9960", "_ADAFRUIT_APDS9960_H_", "apds", 100000, 200,
    ENABLE_APDS_SENSOR, [](uint8_t) -> int { return gSettings.apdsBus; }, nullptr, I2C_DEVICE_APDS },
  { 0x29, "VL53L4CX", "ToF Distance (up to 6m)", "Adafruit", false, 0x00, 1000, "VL53L4CX", "_VL53L4CX_CLASS_H_", "tof", 400000, 250,
    ENABLE_TOF_SENSOR, [](uint8_t) -> int { return gSettings.tofBus; }, nullptr, I2C_DEVICE_TOF },
  // Both seesaw I2C addresses (0x49 + 0x50) belong to the unified "input"
  // module — either the seesaw Mini Gamepad or the ANO Rotary Encoder can
  // appear at either address depending on hardware jumpers. The compiled-in
  // driver disambiguates at runtime; the descriptions show the relevant
  // device for the active build.
#if ENABLE_GT911_TOUCH
  { 0x5d, "GT911", "Single touchscreen", "Goodix", false, 0x00, 100, NULL, NULL, "input", 400000, 100,
    (ENABLE_GAMEPAD_SENSOR || ENABLE_ANO_ENCODER || ENABLE_GT911_TOUCH), [](uint8_t) -> int { return gSettings.inputBus; }, nullptr, I2C_DEVICE_INPUT },
  { 0x14, "GT911", "Single touchscreen (alt)", "Goodix", false, 0x00, 100, NULL, NULL, "input", 400000, 100,
    (ENABLE_GAMEPAD_SENSOR || ENABLE_ANO_ENCODER || ENABLE_GT911_TOUCH), [](uint8_t) -> int { return gSettings.inputBus; }, nullptr, I2C_DEVICE_INPUT },
#elif ENABLE_ANO_ENCODER
  { 0x49, "Seesaw", "ANO Rotary Encoder (default)", "Adafruit", false, 0x00, 800, "Adafruit_seesaw", "_ADAFRUIT_SEESAW_H_", "input", 400000, 200,
    (ENABLE_GAMEPAD_SENSOR || ENABLE_ANO_ENCODER || ENABLE_GT911_TOUCH), [](uint8_t) -> int { return gSettings.inputBus; }, nullptr, I2C_DEVICE_INPUT, kSeesawProductAnoEncoder },
  { 0x50, "Seesaw", "ANO Rotary Encoder (alt addr)", "Adafruit", false, 0x00, 800, "Adafruit_seesaw", "_ADAFRUIT_SEESAW_H_", "input", 400000, 200,
    (ENABLE_GAMEPAD_SENSOR || ENABLE_ANO_ENCODER || ENABLE_GT911_TOUCH), [](uint8_t) -> int { return gSettings.inputBus; }, nullptr, I2C_DEVICE_INPUT, kSeesawProductAnoEncoder },
#else
  { 0x50, "Seesaw", "Mini I2C Gamepad",              "Adafruit", false, 0x00, 800, "Adafruit_seesaw", "_ADAFRUIT_SEESAW_H_", "input", 400000, 200,
    (ENABLE_GAMEPAD_SENSOR || ENABLE_ANO_ENCODER || ENABLE_GT911_TOUCH), [](uint8_t) -> int { return gSettings.inputBus; }, nullptr, I2C_DEVICE_INPUT, kSeesawProductGamepad },
  { 0x49, "Seesaw", "Mini I2C Gamepad (alt addr)",   "Adafruit", false, 0x00, 800, "Adafruit_seesaw", "_ADAFRUIT_SEESAW_H_", "input", 400000, 200,
    (ENABLE_GAMEPAD_SENSOR || ENABLE_ANO_ENCODER || ENABLE_GT911_TOUCH), [](uint8_t) -> int { return gSettings.inputBus; }, nullptr, I2C_DEVICE_INPUT, kSeesawProductGamepad },
#endif
  { 0x33, "MLX90640", "32x24 Thermal Camera", "Adafruit", false, 0x00, 2000, "Adafruit_MLX90640", "_ADAFRUIT_MLX90640_H_", "thermal", 100000, 500,
    ENABLE_THERMAL_SENSOR, [](uint8_t) -> int { return gSettings.thermalBus; }, nullptr, I2C_DEVICE_THERMAL },
  { 0x10, "PA1010D", "Mini GPS Module", "Adafruit", false, 0x00, 500, "Adafruit_GPS", "_ADAFRUIT_GPS_H", "gps", 100000, 200,
    ENABLE_GPS_SENSOR, [](uint8_t) -> int { return gSettings.gpsBus; }, nullptr, I2C_DEVICE_GPS },
  { 0x11, "RDA5807", "FM Radio Receiver", "ScoutMakes", false, 0x00, 500, "RDA5807", NULL, "fmradio", 100000, 200,
    ENABLE_FM_RADIO, [](uint8_t) -> int { return gSettings.fmRadioBus; }, nullptr, I2C_DEVICE_FMRADIO },
  { 0x68, "DS3231", "Precision RTC", "Adafruit", false, 0x00, 100, NULL, NULL, "rtc", 100000, 100,
    ENABLE_RTC_SENSOR, [](uint8_t) -> int { return gSettings.rtcBus; }, nullptr, I2C_DEVICE_RTC },
  { 0x5A, "STHS34PF80", "IR Presence/Motion", "ST", false, 0x00, 200, NULL, NULL, "presence", 100000, 200,
    ENABLE_PRESENCE_SENSOR, [](uint8_t) -> int { return gSettings.presenceBus; }, nullptr, I2C_DEVICE_PRESENCE },
  
  // Detected Infrastructure (no CLI modules)
  // ES8311 speaker codec of the Waveshare P4 board: the board fixes its bus.
#if defined(WS_SPEAKER_I2C_BUS) && ENABLE_SPEAKER
  { 0x18, "ES8311", "Speaker Audio Codec", "Everest Semiconductor", false, 0x00, 0, NULL, NULL, NULL, 100000, 500,
    true, [](uint8_t) -> int { return WS_SPEAKER_I2C_BUS; }, nullptr, I2C_DEVICE_COUNT },
#else
  { 0x18, "ES8311", "Speaker Audio Codec", "Everest Semiconductor", false, 0x00, 0, NULL, NULL, NULL, 100000, 500,
    false, nullptr, nullptr, I2C_DEVICE_COUNT },
#endif
  { 0x3D, "SSD1306", "OLED 128x64 Display", "Adafruit", true, 0x3C, 0, NULL, NULL, NULL, 400000, 50,
    ENABLE_OLED_DISPLAY, [](uint8_t) -> int { return gSettings.oledBus; }, nullptr, I2C_DEVICE_COUNT },
#if BATTERY_BACKEND_FUEL_GAUGE
  // On-board LiPo fuel gauge (battery backend, not a toggleable sensor) — listed
  // so the I2C discovery scan probes 0x36 and names it instead of "Unknown".
  { 0x36, "MAX17048", "LiPo Fuel Gauge", "Maxim", false, 0x00, 0, NULL, NULL, NULL, 400000, 100,
    true, [](uint8_t) -> int { return gSettings.fuelGaugeBus; }, nullptr, I2C_DEVICE_COUNT },
#endif
  // 0x70 is PCA9685's broadcast all-call address, not a second device identity.
  { 0x40, "PCA9685", "16-Channel 12-bit PWM/Servo Driver", "Adafruit", false, 0x00, 0, "Adafruit_PWMServoDriver", "_ADAFRUIT_PWMSERVODRIVER_H_", NULL, 100000, 200,
    ENABLE_SERVO, [](uint8_t) -> int { return gSettings.servoBus; }, nullptr, I2C_DEVICE_COUNT },
  { 0x70, "HT16K33", "Monochrome 16x8 LED Matrix", "Adafruit", false, 0x00, 0, "Adafruit_LEDBackpack", "Adafruit_LEDBackpack_h", "matrix", 100000, 200,
    ENABLE_LED_MATRIX, MATRIX_BUS, MATRIX_ADDRESS, I2C_DEVICE_COUNT },
  // The encoder device: either Adafruit seesaw encoder board, used for its
  // RGB pixels. Both rows claim the one configured address and the product id
  // the board reports names it (0x49 is also the ANO default and the gamepad
  // alternate; 0x36 is also the MAX17048 fuel gauge). Last in the table, so an
  // address-only label prefers the rows above. Without the device, each row
  // keeps its board's default address for the scan's label.
  { 0x49, "Encoder", "Adafruit 5752 Quad Rotary Encoder, 4 RGB pixels", "Adafruit", false, 0x00, 300, "Adafruit_seesaw", "_ADAFRUIT_SEESAW_H_", "encoder", 400000, 200,
    ENABLE_ENCODER, ENCODER_BUS, ENCODER_ADDRESS, I2C_DEVICE_ENCODER, kSeesawProductQuadEncoder, ENCODER_INSTANCES },
  { 0x36, "Encoder", "Adafruit 4991 Rotary Encoder, 1 RGB pixel", "Adafruit", false, 0x00, 300, "Adafruit_seesaw", "_ADAFRUIT_SEESAW_H_", "encoder", 400000, 200,
    ENABLE_ENCODER, ENCODER_BUS, ENCODER_ADDRESS, I2C_DEVICE_ENCODER, kSeesawProductRotaryEncoder, ENCODER_INSTANCES },
};

#undef MATRIX_BUS
#undef MATRIX_ADDRESS
#undef ENCODER_BUS
#undef ENCODER_ADDRESS
#undef ENCODER_INSTANCES

// Export array size for use in .ino file
const size_t i2cSensorsCount = sizeof(i2cSensors) / sizeof(i2cSensors[0]);

// Pre-registration gives every row and every configured instance n >= 2 a
// health record; drivers add a few more (the OLED at its probed address, the
// camera's SCCB), which the remaining room covers.
static_assert(sizeof(i2cSensors) / sizeof(i2cSensors[0]) + kI2cExtraSlots + 2 <=
                  (size_t)I2cDeviceManager::MAX_DEVICES,
              "the health registry must hold every table row, every extra instance and the "
              "driver-registered records");

const char* i2cModuleNameForDeviceType(I2cDeviceType type) {
  if (type >= I2C_DEVICE_COUNT) return nullptr;
  for (size_t i = 0; i < i2cSensorsCount; i++) {
    if (i2cSensors[i].deviceType == type) return i2cSensors[i].moduleName;
  }
  return nullptr;
}

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
// Now managed by I2cDeviceManager - this is a no-op
void initSensorQueue() {
  // Queue mutex is created by I2cDeviceManager::initialize()
  INFO_I2C_DISCOVERYF("initSensorQueue() - queue managed by I2CDeviceManager");
}

// =========================================================================
// Queued Sensor Start Commands (moved from .ino)
// =========================================================================

// gThermalRunning / gTofRunning / gImuRunning provided by their i2csensor_*.h headers

// Instances (System_I2CInstanceCore.h): the per-type switches below take the
// instance n. Only the encoder's driver keeps one state per instance
// (i2csensor_encoder.h); every other branch serves instance 1, the only one
// its type has. The manager refuses an instance a type does not have before
// any of them runs.

// Whether the type's driver serves instance n: instance 1 of every type, and
// every instance of a type whose driver keeps per-instance state.
static bool driverServesInstance(I2cDeviceType sensor, uint8_t n) {
  return n == 1 || (sensor == I2C_DEVICE_ENCODER && i2cSlot(sensor, n) != kI2cNoSlot);
}

// Map I2cDeviceType → I2C address for pre-start hardware presence checks.
// Returns 0 if the device type has no single fixed address (or should skip the check).
static uint8_t i2cAddressForDeviceType(I2cDeviceType sensor, uint8_t n = 1) {
  if (!driverServesInstance(sensor, n)) return 0;
  switch (sensor) {
    case I2C_DEVICE_THERMAL:  return I2C_ADDR_THERMAL;
    case I2C_DEVICE_TOF:      return I2C_ADDR_TOF;
    case I2C_DEVICE_IMU:      return I2C_ADDR_IMU;
    case I2C_DEVICE_INPUT:
#if ENABLE_GT911_TOUCH
      return 0; // Backend probes 0x5d and 0x14 through the shared manager.
#elif ENABLE_ANO_ENCODER
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
    case I2C_DEVICE_ENCODER:  return encoderInstanceAddress(n);  // 0: instance n has none set
    default:                  return 0;
  }
}

// Keep queue/autostart probes on the same physical bus as the corresponding
// driver.  The old default-argument calls always probed bus 0, so a correctly
// configured bus-1 sensor was rejected before its start request reached the
// driver.
static uint8_t i2cBusForDeviceType(I2cDeviceType sensor, uint8_t n = 1) {
  if (!driverServesInstance(sensor, n)) return 0;
  switch (sensor) {
    case I2C_DEVICE_THERMAL:  return (uint8_t)gSettings.thermalBus;
    case I2C_DEVICE_TOF:      return (uint8_t)gSettings.tofBus;
    case I2C_DEVICE_IMU:      return (uint8_t)gSettings.imuBus;
    case I2C_DEVICE_INPUT:    return (uint8_t)gSettings.inputBus;
    case I2C_DEVICE_GPS:      return (uint8_t)gSettings.gpsBus;
    case I2C_DEVICE_FMRADIO:  return (uint8_t)gSettings.fmRadioBus;
    case I2C_DEVICE_APDS:     return (uint8_t)gSettings.apdsBus;
    case I2C_DEVICE_RTC:      return (uint8_t)gSettings.rtcBus;
    case I2C_DEVICE_PRESENCE: return (uint8_t)gSettings.presenceBus;
    case I2C_DEVICE_ENCODER:  return encoderInstanceBus(n);
    default:                  return 0;
  }
}

// ---------------------------------------------------------------------------
// Address collisions (System_I2CInstanceCore.h, design section 1.3)
// ---------------------------------------------------------------------------

// Every address in use on a bus, one at a time, until `visit` returns true:
// - each compiled device without a queue type (fuel gauge, OLED, PWM driver,
//   LED matrix, speaker codec) at its address and alternate, on its bus;
// - instance 1 of each compiled queue type at the address its driver uses
//   (i2cAddressForDeviceType: the gamepad's 0x50, the ANO encoder's setting,
//   the encoder's setting); a driver without one address (the GT911 probes
//   two) at each of its rows' addresses;
// - each configured instance n >= 2 at its row's busAt(n)/addressAt(n).
// Nothing is allocated; the uses are read from the table and the settings.
template <typename Visit>
static void forEachI2cAddressUse(Visit&& visit) {
  for (size_t i = 0; i < i2cSensorsCount; i++) {
    const I2cSensorEntry& row = i2cSensors[i];
    if (!row.compiled || row.deviceType != I2C_DEVICE_COUNT) continue;
    I2cAddressUse use{configuredBusForSensor(row), i2cRowAddressAt(row, 1), I2C_DEVICE_COUNT, 0,
                      row.name, row.description};
    if (use.address != 0 && visit(use)) return;
    if (row.multiAddress) {
      use.address = row.altAddress;
      if (visit(use)) return;
    }
  }
  for (int t = 0; t < I2C_DEVICE_COUNT; t++) {
    const I2cDeviceType type = static_cast<I2cDeviceType>(t);
    const I2cSensorEntry* first = nullptr;
    for (size_t i = 0; i < i2cSensorsCount && !first; i++) {
      if (i2cSensors[i].compiled && i2cSensors[i].deviceType == type) first = &i2cSensors[i];
    }
    if (!first) continue;  // not in this build
    const uint8_t instances = i2cRowInstances(*first);
    for (uint8_t k = 1; k <= instances; k++) {
      if (k >= 2) {
        // Instance n >= 2 at its own settings, from the type's rows.
        const I2cAddressUse use{configuredBusForSensor(*first, k), i2cRowAddressAt(*first, k), type,
                                k, first->name, first->description};
        if (use.address != 0 && visit(use)) return;
        continue;
      }
      const uint8_t address = i2cAddressForDeviceType(type, k);
      if (address != 0) {
        // Named by the row at that address when there is one (the gamepad's
        // 0x50 row), else by the type's first row.
        const I2cSensorEntry* named = first;
        for (size_t i = 0; i < i2cSensorsCount; i++) {
          if (i2cSensors[i].compiled && i2cSensors[i].deviceType == type &&
              i2cRowAddressAt(i2cSensors[i], 1) == address) {
            named = &i2cSensors[i];
            break;
          }
        }
        const I2cAddressUse use{i2cBusForDeviceType(type, k), address, type, k, named->name,
                                named->description};
        if (visit(use)) return;
      } else {
        for (size_t i = 0; i < i2cSensorsCount; i++) {
          const I2cSensorEntry& row = i2cSensors[i];
          if (!row.compiled || row.deviceType != type) continue;
          const I2cAddressUse use{configuredBusForSensor(row), i2cRowAddressAt(row, 1), type, 1,
                                  row.name, row.description};
          if (use.address != 0 && visit(use)) return;
        }
      }
    }
  }
}

bool i2cAddressTakenBy(uint8_t bus, uint8_t address, I2cDeviceType exceptType, uint8_t exceptN,
                       char* why, size_t whyLen) {
  bool taken = false;
  forEachI2cAddressUse([&](const I2cAddressUse& use) {
    if (!i2cAddressUseClashes(use, bus, address, exceptType, exceptN)) return false;
    taken = true;
    if (why && whyLen) i2cAddressClashText(use, why, whyLen);
    return true;
  });
  return taken;
}

// Lifecycle state stays with the sensor owners. These reads coordinate queue
// admission; the owner repeats the cleanup guard immediately before startup.
static bool sensorIsRunning(I2cDeviceType sensor, uint8_t n = 1) {
  if (!driverServesInstance(sensor, n)) return false;
  switch (sensor) {
    case I2C_DEVICE_THERMAL: return gThermalRunning;
    case I2C_DEVICE_TOF: return gTofRunning;
    case I2C_DEVICE_IMU: return gImuRunning;
    case I2C_DEVICE_INPUT: return gInputRunning;
    case I2C_DEVICE_GPS: return gGpsRunning;
    case I2C_DEVICE_FMRADIO: return gFmRadioRunning;
    case I2C_DEVICE_APDS:
#if ENABLE_APDS_SENSOR
      return gApdsRunning;
#else
      return false;
#endif
    case I2C_DEVICE_RTC: return gRtcRunning;
    case I2C_DEVICE_PRESENCE: return gPresenceRunning;
    case I2C_DEVICE_ENCODER: return encoderRunning(n);
    default: return false;
  }
}

static bool sensorHasTask(I2cDeviceType sensor, uint8_t n = 1) {
  if (!driverServesInstance(sensor, n)) return false;
  switch (sensor) {
    case I2C_DEVICE_THERMAL: return gThermalTaskHandle != nullptr;
    case I2C_DEVICE_TOF: return gTofTaskHandle != nullptr;
    case I2C_DEVICE_IMU: return gImuTaskHandle != nullptr;
    case I2C_DEVICE_INPUT: return gInputTaskHandle != nullptr;
    case I2C_DEVICE_GPS: return gGpsTaskHandle != nullptr;
    case I2C_DEVICE_FMRADIO: return gFmRadioTaskHandle != nullptr;
    case I2C_DEVICE_APDS: return gApdsTaskHandle != nullptr;
    case I2C_DEVICE_RTC: return gRtcTaskHandle != nullptr;
    case I2C_DEVICE_PRESENCE: return gPresenceTaskHandle != nullptr;
    case I2C_DEVICE_ENCODER: return false;  // no task: the main loop ticks its frames
    default: return false;
  }
}

// Start-at-boot (<x>autostart) of each queue-managed device, in one table:
// the automatic restart obeys it as boot does. An open by hand is never
// refused for a setting. Read-only pointers: nothing writes a setting through
// this table.
struct SensorSwitchRow {
  I2cDeviceType sensor;
  const bool* autoStart;
};
static const SensorSwitchRow kSensorSwitches[] = {
  { I2C_DEVICE_THERMAL,  &gSettings.thermalAutoStart },
  { I2C_DEVICE_TOF,      &gSettings.tofAutoStart },
  { I2C_DEVICE_IMU,      &gSettings.imuAutoStart },
  { I2C_DEVICE_INPUT,    &gSettings.inputAutoStart },
  { I2C_DEVICE_GPS,      &gSettings.gpsAutoStart },
  { I2C_DEVICE_FMRADIO,  &gSettings.fmRadioAutoStart },
  { I2C_DEVICE_APDS,     &gSettings.apdsAutoStart },
  { I2C_DEVICE_RTC,      &gSettings.rtcAutoStart },
  { I2C_DEVICE_PRESENCE, &gSettings.presenceAutoStart },
  { I2C_DEVICE_ENCODER, encoderAutoStartSetting(1) },  // gEncoderInst (instance 1)
};
static_assert(sizeof(kSensorSwitches) / sizeof(kSensorSwitches[0]) == (size_t)I2C_DEVICE_COUNT,
              "every queue-managed device type needs its switch row");

static const SensorSwitchRow* sensorSwitchRow(I2cDeviceType sensor) {
  for (const SensorSwitchRow& row : kSensorSwitches) {
    if (row.sensor == sensor) return &row;
  }
  return nullptr;
}

// Start-at-boot of instance n. Instance 1 is the table above; an instance
// n >= 2 reads its own switch (the encoder's, i2csensor_encoder.h).
static bool sensorAutoStartFor(I2cDeviceType sensor, uint8_t n) {
  if (n != 1) return sensor == I2C_DEVICE_ENCODER && encoderAutoStartOf(n);
  const SensorSwitchRow* row = sensorSwitchRow(sensor);
  return row && *row->autoStart;
}

// Admitted starts the queue worker has not settled yet. A stop that cancels
// one (queued, or dropped while starting) still closes it with the stop event;
// a stop with nothing running, starting or queued publishes nothing.
// Indexed by instance slot: instance 1 (slot = type) stays in internal RAM as
// before, the extra instances' slots live in PSRAM.
static std::atomic<bool> sSensorStartOutstanding[I2C_DEVICE_COUNT];
EXT_RAM_BSS_ATTR static std::atomic<bool> sSensorStartOutstandingExtra[kI2cExtraSlotStorage];
static std::atomic<bool>& sensorStartOutstanding(uint8_t slot) {
  return slot < I2C_DEVICE_COUNT ? sSensorStartOutstanding[slot]
                                 : sSensorStartOutstandingExtra[slot - I2C_DEVICE_COUNT];
}

bool enqueueDeviceStart(I2cDeviceType device, uint8_t n) {
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  const uint8_t slot = i2cSlot(device, n);
  if (!mgr || slot == kI2cNoSlot) return false;
  // Marked before the request is visible to the worker, which clears it.
  std::atomic<bool>& outstanding = sensorStartOutstanding(slot);
  const bool wasOutstanding = outstanding.exchange(true);
  if (mgr->enqueueDeviceStart(device, n)) return true;
  if (!wasOutstanding) outstanding = false;
  return false;
}

// ---------------------------------------------------------------------------
// Automatic restart of a sensor that switched itself off (bus errors or
// invalid reads). Policy: System_SensorRestartCore.h. The queue worker owns
// the ladder position and makes every try; other tasks only set flags.
// All of it lives in PSRAM, one entry per instance slot (System_I2CInstanceCore.h;
// instance 1's slot is its type): 3 flag bytes + 8 bytes of ladder position
// per slot, and 4 bytes of tick time (internal on a board without PSRAM, where
// the attribute does nothing).
// ---------------------------------------------------------------------------
// Set by a sensor task at its auto-disable site; consumed by the worker.
EXT_RAM_BSS_ATTR static std::atomic<bool> sSensorRestartNoted[kI2cSlotCount];
// Set by a manual open or close; consumed by the worker, which ends the episode.
// Which of the two it was decides a report the worker has not looked at yet:
// a close wins over it (the sensor stays off), an open does not (the report is
// about the sensor that open started).
enum : uint8_t { kRestartHandNone = 0, kRestartHandOpened = 1, kRestartHandClosed = 2 };
EXT_RAM_BSS_ATTR static std::atomic<uint8_t> sSensorRestartByHand[kI2cSlotCount];
// Published by the worker: a try is waiting or queued (read by the close command).
EXT_RAM_BSS_ATTR static std::atomic<bool> sSensorRestartPending[kI2cSlotCount];
// Ladder position per instance slot. Queue worker only.
EXT_RAM_BSS_ATTR static sensor_restart::State sSensorRestart[kI2cSlotCount];
EXT_RAM_BSS_ATTR static uint32_t sSensorRestartLastTickMs;

void noteSensorAutoDisabled(I2cDeviceType sensor, uint8_t n) {
  const uint8_t slot = i2cSlot(sensor, n);
  if (slot == kI2cNoSlot) return;
  sSensorRestartNoted[slot].store(true);
}

// A close by hand ends the automatic restart. True when a try was waiting or
// queued, so the close command can say what it cancelled.
static bool sensorRestartClosedByHand(I2cDeviceType sensor, uint8_t n) {
  const uint8_t slot = i2cSlot(sensor, n);
  if (slot == kI2cNoSlot) return false;
  sSensorRestartByHand[slot].store(kRestartHandClosed);
  return sSensorRestartPending[slot].exchange(false);
}

// An accepted open by hand replaces a waiting try. A report from before the
// open is void: the user's start supersedes it. A report that arrives after
// this call is about the sensor that open started, and the worker keeps it
// (it ends the old episode first, then begins a new one), so a sensor that
// fails again right after a manual open is still started again.
static void sensorRestartOpenedByHand(I2cDeviceType sensor, uint8_t n) {
  const uint8_t slot = i2cSlot(sensor, n);
  if (slot == kI2cNoSlot) return;
  sSensorRestartNoted[slot].store(false);
  sSensorRestartByHand[slot].store(kRestartHandOpened);
  sSensorRestartPending[slot].store(false);
}

// Externally linkable so HAL_Input.cpp's unified cmd_openinput can dispatch
// through the same queue as the other sensor start commands.
bool requestSensorStart(I2cDeviceType sensor, const char* eventTag, SensorStartRefusal* why,
                        uint8_t n) {
  auto refuse = [&](SensorStartRefusal reason) {
    if (why) *why = reason;
    return false;
  };
  // Reject immediately if the I2C bus is disabled at runtime (distinct from hardware not connected)
  if (!gI2cBusRunning) return refuse(SensorStartRefusal::BusDisabled);

  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  if (!mgr) return refuse(SensorStartRefusal::QueueUnavailable);
  if (mgr->hasPendingDeviceStop(sensor, n) ||
      (!sensorIsRunning(sensor, n) && sensorHasTask(sensor, n))) {
    return refuse(SensorStartRefusal::StopPending);
  }
  if (sensorIsRunning(sensor, n)) return refuse(SensorStartRefusal::AlreadyRunning);
  if (isInQueue(sensor, n)) return refuse(SensorStartRefusal::AlreadyQueued);

  // Verify hardware is physically present before attempting start
  const uint8_t addr = i2cAddressForDeviceType(sensor, n);
  const uint8_t bus = i2cBusForDeviceType(sensor, n);
  const uint8_t probe = addr ? i2cProbeAddress(addr, 100000, 50, bus) : 0;
  if (probe == 2) return refuse(SensorStartRefusal::NoAck);
  if (probe != 0) return refuse(SensorStartRefusal::ProbeFailed);

  if (!enqueueDeviceStart(sensor, n)) return refuse(SensorStartRefusal::NotQueued);
  sensorStatusBumpWith(eventTag);
  if (why) *why = SensorStartRefusal::None;
  return true;
}

const char* sensorStartRefusalText(SensorStartRefusal why) {
  switch (why) {
    case SensorStartRefusal::None:             return "queued";
    case SensorStartRefusal::BusDisabled:      return "I2C bus is disabled";
    case SensorStartRefusal::QueueUnavailable: return "sensor request queue is unavailable";
    case SensorStartRefusal::StopPending:      return "stop/cleanup is still pending";
    case SensorStartRefusal::AlreadyRunning:   return "already running";
    case SensorStartRefusal::AlreadyQueued:    return "already queued";
    case SensorStartRefusal::NoAck:            return "no acknowledgement from device on I2C bus";
    case SensorStartRefusal::ProbeFailed:      return "I2C probe could not complete";
    case SensorStartRefusal::NotQueued:        return "could not queue (queue unavailable/full or stop pending)";
  }
  return "unknown";
}

// Command wrapper: the only place that renders the admission outcome, into
// cmd_exec's debug buffer. Message texts are unchanged.
const char* cmd_sensorstart_queued(I2cDeviceType sensor, const char* displayName, const char* eventTag,
                                   const char* hint, uint8_t n) {
#if ENABLE_GPIO_ENCODER
  if (sensor == I2C_DEVICE_INPUT) return "Error: GPIO input is not an I2C device; use openinput";
#endif
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  SensorStartRefusal why = SensorStartRefusal::None;
  // The instance a command names follows its command word ("closeencoder 2").
  char instanceSuffix[6] = "";
  if (n != 1) snprintf(instanceSuffix, sizeof(instanceSuffix), " %u", (unsigned)n);
  if (requestSensorStart(sensor, eventTag, &why, n)) {
    // An open by hand replaces any automatic restart that was waiting.
    sensorRestartOpenedByHand(sensor, n);
    snprintf(getDebugBuffer(), 1024, "%s sensor queued for start (position %d, queue depth: %d)",
             displayName, getQueuePosition(sensor, n), getQueueDepth());
    // A refusal is its own answer; the next step only follows a queued start.
    if (hint) cliHint(hint);
    return getDebugBuffer();
  }
  switch (why) {
    case SensorStartRefusal::BusDisabled:
      snprintf(getDebugBuffer(), 1024, "Error: [%s] I2C bus is disabled — enable it in settings and reboot", displayName);
      break;
    case SensorStartRefusal::QueueUnavailable:
      return "Error: Sensor request queue is unavailable";
    case SensorStartRefusal::StopPending:
      snprintf(getDebugBuffer(), 1024, "Error: [%s] Stop/cleanup is still pending; retry open shortly", displayName);
      break;
    case SensorStartRefusal::AlreadyRunning: {
      // The stop command that pairs with this start (kept in this function:
      // the host test extracts it on its own).
      const char* closeCmd = "the close command";
      switch (sensor) {
        case I2C_DEVICE_THERMAL:  closeCmd = "closethermal"; break;
        case I2C_DEVICE_TOF:      closeCmd = "closetof"; break;
        case I2C_DEVICE_IMU:      closeCmd = "closeimu"; break;
        case I2C_DEVICE_INPUT:    closeCmd = "closeinput"; break;
        case I2C_DEVICE_GPS:      closeCmd = "closegps"; break;
        case I2C_DEVICE_FMRADIO:  closeCmd = "closefmradio"; break;
        case I2C_DEVICE_APDS:     closeCmd = "closeapds"; break;
        case I2C_DEVICE_RTC:      closeCmd = "closertc"; break;
        case I2C_DEVICE_PRESENCE: closeCmd = "closepresence"; break;
        case I2C_DEVICE_ENCODER: closeCmd = "closeencoder"; break;
        default: break;
      }
      snprintf(getDebugBuffer(), 1024,
               "Error: %s sensor is already running. Stop it first with '%s%s' to restart it.",
               displayName, closeCmd, instanceSuffix);
      break;
    }
    case SensorStartRefusal::AlreadyQueued:
      snprintf(getDebugBuffer(), 1024,
               "Error: %s sensor is already queued for start (position %d). "
               "Check it in a moment with 'sensors json brief'.",
               displayName, getQueuePosition(sensor, n));
      break;
    case SensorStartRefusal::NoAck:
      snprintf(getDebugBuffer(), 1024, "Error: [%s] No acknowledgement from device on I2C bus", displayName);
      break;
    case SensorStartRefusal::ProbeFailed:
      snprintf(getDebugBuffer(), 1024, "Error: [%s] I2C probe could not complete (bus busy, unavailable or transfer error); retry", displayName);
      break;
    case SensorStartRefusal::NotQueued:
    case SensorStartRefusal::None:
      snprintf(getDebugBuffer(), 1024, "Error: Could not queue %s sensor (queue unavailable/full or stop pending)", displayName);
      break;
  }
  return getDebugBuffer();
}

// The start command that pairs with a sensor's stop, for the not-running reply.
static const char* sensorOpenCommand(I2cDeviceType sensor) {
  switch (sensor) {
    case I2C_DEVICE_THERMAL:  return "openthermal";
    case I2C_DEVICE_TOF:      return "opentof";
    case I2C_DEVICE_IMU:      return "openimu";
    case I2C_DEVICE_INPUT:    return "openinput";
    case I2C_DEVICE_GPS:      return "opengps";
    case I2C_DEVICE_FMRADIO:  return "openfmradio";
    case I2C_DEVICE_APDS:     return "openapds";
    case I2C_DEVICE_RTC:      return "openrtc";
    case I2C_DEVICE_PRESENCE: return "openpresence";
    case I2C_DEVICE_ENCODER: return "openencoder";
    default:                  return "the open command";
  }
}

// A stop has something to do: the sensor runs, still has a task, has a start
// admitted or queued, or was initialized without a task (FM tune/boot, RTC).
// The same set applySensorStop() counts as active.
static bool sensorStopHasWork(I2cDeviceType sensor, uint8_t n) {
  const uint8_t slot = i2cSlot(sensor, n);
  if (slot == kI2cNoSlot) return false;
  // The outstanding mark first: the worker clears it only after the start
  // settled, so a start that completes between the reads is still seen.
  if (sensorStartOutstanding(slot).load()) return true;
  if (sensorIsRunning(sensor, n) || sensorHasTask(sensor, n) || isInQueue(sensor, n)) return true;
#if ENABLE_FM_RADIO
  if (sensor == I2C_DEVICE_FMRADIO && gRadioInitialized) return true;
#endif
#if ENABLE_RTC_SENSOR
  if (sensor == I2C_DEVICE_RTC && gRtcConnected) return true;
#endif
  return false;
}

// Command wrapper for a queued stop: the one place that renders its outcome.
// A sensor that is not running (the bus disabled at boot, or nothing open,
// starting or queued) answers an Error naming the command that starts it.
// After that a refused request means the manager or its queue does not exist.
//
// A close by hand also ends an automatic restart. That happens before the
// idle test: a sensor that switched itself off and waits for its next try is
// not running, and without this it would come back minutes after the close.
// Here and not in requestSensorStop(), which the input drivers call for their
// own auto-disable.
const char* cmd_sensorstop_queued(I2cDeviceType sensor, const char* displayName, uint8_t n) {
  const bool busDisabled = !gI2cBusRunning;
  const bool restartCancelled = !busDisabled && sensorRestartClosedByHand(sensor, n);
  const bool idle = !busDisabled && !sensorStopHasWork(sensor, n);
  const bool queued = !busDisabled && !idle && requestSensorStop(sensor, n);
  // The instance a command names follows its command word ("openencoder 2").
  char instanceSuffix[6] = "";
  if (n != 1) snprintf(instanceSuffix, sizeof(instanceSuffix), " %u", (unsigned)n);
  if (!ensureDebugBuffer()) {
    if (busDisabled) {
      return "Error: The sensor is not running: the I2C bus is disabled. "
             "Enable it with 'i2cbusenabled 1', then reboot.";
    }
    if (idle && restartCancelled) return "Automatic restart cancelled; the sensor stays off until it is opened again";
    if (idle) return "Error: The sensor is not running. Start it with its open command.";
    return queued ? "Stop requested; cleanup will complete asynchronously"
                  : "Error: Sensor request queue is unavailable. Reboot the device and retry.";
  }
  if (busDisabled) {
    snprintf(getDebugBuffer(), 1024,
             "Error: %s sensor is not running: the I2C bus is disabled. "
             "Enable it with 'i2cbusenabled 1', then reboot.", displayName);
  } else if (idle && restartCancelled) {
    snprintf(getDebugBuffer(), 1024,
             "[%s] Automatic restart cancelled; the sensor stays off until '%s%s'",
             displayName, sensorOpenCommand(sensor), instanceSuffix);
  } else if (idle) {
    snprintf(getDebugBuffer(), 1024, "Error: %s sensor is not running. Start it with '%s%s'.",
             displayName, sensorOpenCommand(sensor), instanceSuffix);
  } else if (!queued) {
    snprintf(getDebugBuffer(), 1024,
             "Error: [%s] Sensor request queue is unavailable. Reboot the device and retry.", displayName);
  } else {
    snprintf(getDebugBuffer(), 1024, "[%s] Stop requested; cleanup will complete asynchronously", displayName);
  }
  return getDebugBuffer();
}

const char* cmd_thermalstart_queued(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return cmd_sensorstart_queued(I2C_DEVICE_THERMAL, "Thermal", "openthermal@enqueue");
}

const char* cmd_tofstart_queued(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return cmd_sensorstart_queued(I2C_DEVICE_TOF, "ToF", "opentof@enqueue");
}

const char* cmd_imustart_queued(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return cmd_sensorstart_queued(I2C_DEVICE_IMU, "IMU", "openimu@enqueue");
}

const char* cmd_apdsstart_queued(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return cmd_sensorstart_queued(I2C_DEVICE_APDS, "APDS", "openapds@enqueue");
}

// cmd_openinput (in HAL_Input.cpp) handles the input device's queued start —
// the gamepad-specific wrapper that used to live here has been deleted now
// that there's only one unified entrypoint for either driver.

// ========== End Sensor Startup Queue System ==========

// ========== I2C Bus Initialization ==========

// Track if we've already logged the "bus disabled" message (to avoid spam)
static bool gI2cBusDisabledLogged = false;

void initI2cBuses() {
  // During first-time setup, force I2C enabled so OLED wizard can run
  // User can disable I2C in wizard, which takes effect after reboot
  bool forceForSetup = isFirstTimeSetup();
  
  if (forceForSetup) {
    gI2cBusRunning = true;
    INFO_I2C_BUSF("[I2C] Force-enabling for first-time setup wizard");
  } else {
    // Copy setting to global flag
    gI2cBusRunning = gSettings.i2cEnabled;
  }
  
  // Early exit if I2C bus is disabled via settings (and not first-time setup)
  if (!gI2cBusRunning) {
    if (!gI2cBusDisabledLogged) {
      INFO_I2C_BUSF("[I2C] Bus disabled via settings - skipping initialization");
      INFO_I2C_BUSF("[I2C] OLED display and I2C sensors will not be available");
      gI2cBusDisabledLogged = true;
    }
    return;
  }

  // Initialize unified I2C manager
  initI2cManager();
  
  // Delegate bus initialization to manager
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  if (mgr) {
    mgr->initBuses();
  }

  // Bus warm-up: perform a dummy probe on each initialized bus to exercise
  // the ESP32 I2C hardware. Without OLED or RTC active, zero I2C activity
  // occurs between init and discovery, which can leave the bus in an
  // unreliable state for the first real transaction. Probe address 0x00
  // (general call) — no device will ACK, but the bus is exercised.
  for (uint8_t b = 0; b < I2cDeviceManager::NUM_BUSES; b++) {
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

void i2cResetGracePeriod(uint8_t address, uint8_t bus) {
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  if (!mgr) return;
  I2cDevice* dev = mgr->getDevice(address, bus);
  if (dev) dev->resetGracePeriod();
}

const char* cmd_i2chealth(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  if (!mgr) return "Error: I2C manager not initialized";

  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 3;   // 3: transaction timing moved out of the health registry
    doc["deviceCount"] = mgr->getDeviceCount();
    JsonArray arr = doc["devices"].to<JsonArray>();
    for (int i = 0; i < mgr->getDeviceCount(); i++) {
      I2cDevice* dev = &mgr->devices[i];
      if (!dev->isInitialized()) continue;
      const I2cDevice::Health& h = dev->getHealth();
      JsonObject o = arr.add<JsonObject>();
      o["bus"]                = dev->bus;
      o["address"]            = dev->address;
      o["name"]               = dev->name ? dev->name : "?";
      o["consecutiveErrors"]  = h.consecutiveErrors;
      o["totalErrors"]        = h.totalErrors;
      o["degraded"]           = dev->isDegraded();
      o["nack"]               = h.nackCount;
      o["timeout"]            = h.timeoutCount;
      o["busError"]           = h.busErrorCount;
    }
    // Size against the real buffer (4096), not the stale 1024 literal — 16
    // devices x ~150 B of JSON overruns 1 KB and serializeJson would emit a
    // truncated, unparseable document.
    serializeJson(doc, getDebugBuffer(), GLOBAL_DEBUG_BUFFER_SIZE);
    return getDebugBuffer();
  }

  char* p = getDebugBuffer();
  int remaining = GLOBAL_DEBUG_BUFFER_SIZE;

  int deviceCount = mgr->getDeviceCount();
  int n = snprintf(p, remaining, "I2C Device Health (%d devices):\n", deviceCount);
  p += n; remaining -= n;

  if (deviceCount == 0) {
    snprintf(p, remaining, "  No devices registered\n");
    return getDebugBuffer();
  }

  // Bus column: the registry is keyed on (address, bus), so the same address
  // can legitimately appear twice — once per bus — with independent health.
  // Without this column those rows are indistinguishable.
  int omitted = 0;
  for (int i = 0; i < deviceCount; i++) {
    I2cDevice* dev = &mgr->devices[i];
    if (!dev->isInitialized()) continue;   // empty slot, not an omission

    if (remaining <= 100) { omitted++; continue; }

    const I2cDevice::Health& h = dev->getHealth();

    // Device header line
    n = snprintf(p, remaining,
      "  bus%u 0x%02X %-10s: err=%d/%d %s\n",
      dev->bus, dev->address, dev->name ? dev->name : "?",
      h.consecutiveErrors, h.totalErrors,
      dev->isDegraded() ? "[DEGRADED]" : "OK");
    p += n; remaining -= n;

    // Error classification breakdown
    if (h.totalErrors > 0 && remaining > 100) {
      n = snprintf(p, remaining,
        "            NACK=%d TIMEOUT=%d BUS_ERR=%d\n",
        h.nackCount, h.timeoutCount, h.busErrorCount);
      p += n; remaining -= n;
    }
  }

  // Never truncate silently — say so if the buffer ran out mid-table.
  if (omitted > 0 && remaining > 40) {
    snprintf(p, remaining, "  ... %d more device(s) not shown (buffer full)\n", omitted);
  }

  return getDebugBuffer();
}

// Human-readable label for a bus index, matching what i2cscan / i2cstats print.
static const char* i2cBusLabel(uint8_t bus) {
  return (bus == 0) ? "I2C1/Wire1" : "I2C2/Wire";
}

// Render one bus's metrics block (stats + per-bus warnings) into `p`.
// Returns bytes written. Each bus is reported independently: they have
// separate mutexes, clock stacks and metric counters, so a bottleneck on one
// says nothing about the other.
static int renderBusMetrics(char* p, int remaining, uint8_t bus,
                            const I2cBusMetrics& m, unsigned long uptimeSec) {
  float tps = (uptimeSec > 0) ? (float)m.totalTransactions / uptimeSec : 0.0f;
  float contentionRate = (m.totalTransactions > 0)
    ? (float)m.mutexContentions * 100.0f / m.totalTransactions : 0.0f;
  float timeoutRate = (m.totalTransactions > 0)
    ? (float)m.mutexTimeouts * 100.0f / m.totalTransactions : 0.0f;
  float bytesPerSec = (uptimeSec > 0) ? (float)m.totalBytesTransferred / uptimeSec : 0.0f;
  float pctFast   = m.totalTransactions > 0 ? (float)m.txDuration_0_100us     * 100.0f / m.totalTransactions : 0.0f;
  float pctNormal = m.totalTransactions > 0 ? (float)m.txDuration_100_500us   * 100.0f / m.totalTransactions : 0.0f;
  float pctSlow   = m.totalTransactions > 0 ? (float)m.txDuration_500_2000us  * 100.0f / m.totalTransactions : 0.0f;
  float pctVSlow  = m.totalTransactions > 0 ? (float)m.txDuration_2000plus_us * 100.0f / m.totalTransactions : 0.0f;

  int written = 0;
  int n = snprintf(p, remaining,
    "\nBus %u (%s):\n"
    "  Total Transactions:  %lu (%.1f/sec)\n"
    "  Mutex Timeouts:      %lu (%.2f%%)\n"
    "  Bus Contentions:     %lu (%.2f%%)\n"
    "  Avg Wait Time:       %lu us\n"
    "  Peak Wait Time:      %lu us\n"
    "  Total Bytes:         %lu (%.1f bytes/sec)\n"
    "  Avg TX Duration:     %lu us\n"
    "  Peak TX Duration:    %lu us\n"
    "  Duration spread:     %lu fast (%.1f%%) / %lu normal (%.1f%%) / "
    "%lu slow (%.1f%%) / %lu very slow (%.1f%%)\n",
    bus, i2cBusLabel(bus),
    (unsigned long)m.totalTransactions, tps,
    (unsigned long)m.mutexTimeouts, timeoutRate,
    (unsigned long)m.mutexContentions, contentionRate,
    (unsigned long)m.avgWaitTimeUs,
    (unsigned long)m.maxWaitTimeUs,
    (unsigned long)m.totalBytesTransferred, bytesPerSec,
    (unsigned long)m.avgTransactionDurationUs,
    (unsigned long)m.maxTransactionDurationUs,
    (unsigned long)m.txDuration_0_100us,     pctFast,
    (unsigned long)m.txDuration_100_500us,   pctNormal,
    (unsigned long)m.txDuration_500_2000us,  pctSlow,
    (unsigned long)m.txDuration_2000plus_us, pctVSlow);
  if (n < 0 || n >= remaining) return (n < 0) ? 0 : remaining - 1;
  p += n; remaining -= n; written += n;

  // Health recommendations — scoped to THIS bus.
  if (m.mutexTimeouts > 0 && remaining > 80) {
    n = snprintf(p, remaining, "  WARN: %lu mutex timeouts on bus %u - bus overloaded\n",
                 (unsigned long)m.mutexTimeouts, bus);
    p += n; remaining -= n; written += n;
  }
  if (contentionRate > 50.0f && remaining > 80) {
    n = snprintf(p, remaining, "  WARN: high contention on bus %u (%.1f%%) - consider reducing polling rates\n",
                 bus, contentionRate);
    p += n; remaining -= n; written += n;
  }
  if (m.avgWaitTimeUs > 5000 && remaining > 80) {
    n = snprintf(p, remaining, "  WARN: high avg wait on bus %u (%lu us) - bus bottleneck\n",
                 bus, (unsigned long)m.avgWaitTimeUs);
    p += n; remaining -= n; written += n;
  }
  return written;
}

const char* cmd_i2cmetrics(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  if (!mgr) return "Error: I2C manager not initialized";

  // Counters are cumulative since boot — nothing clears them at runtime.
  unsigned long uptimeSec = SelfDevice::uptimeSeconds();

  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 2;   // 2: per-bus array (was a single flat bus-0 object)
    doc["uptimeSec"] = uptimeSec;
    JsonArray arr = doc["buses"].to<JsonArray>();
    for (uint8_t b = 0; b < I2cDeviceManager::NUM_BUSES; b++) {
      if (!mgr->isBusInitialized(b)) continue;
      const I2cBusMetrics& m = mgr->getMetrics(b);
      JsonObject o = arr.add<JsonObject>();
      o["bus"]               = b;
      o["label"]             = i2cBusLabel(b);
      o["totalTransactions"] = (unsigned long)m.totalTransactions;
      o["mutexTimeouts"]     = (unsigned long)m.mutexTimeouts;
      o["busContentions"]    = (unsigned long)m.mutexContentions;
      o["avgWaitUs"]         = (unsigned long)m.avgWaitTimeUs;
      o["maxWaitUs"]         = (unsigned long)m.maxWaitTimeUs;
      o["totalBytes"]        = (unsigned long)m.totalBytesTransferred;
      o["avgTxUs"]           = (unsigned long)m.avgTransactionDurationUs;
      o["maxTxUs"]           = (unsigned long)m.maxTransactionDurationUs;
      JsonObject hist = o["hist"].to<JsonObject>();
      hist["us0_100"]    = (unsigned long)m.txDuration_0_100us;
      hist["us100_500"]  = (unsigned long)m.txDuration_100_500us;
      hist["us500_2000"] = (unsigned long)m.txDuration_500_2000us;
      hist["us2000plus"] = (unsigned long)m.txDuration_2000plus_us;
    }
    serializeJson(doc, getDebugBuffer(), GLOBAL_DEBUG_BUFFER_SIZE);
    return getDebugBuffer();
  }

  char* p = getDebugBuffer();
  int remaining = GLOBAL_DEBUG_BUFFER_SIZE;

  int n = snprintf(p, remaining, "I2C Bus Metrics (uptime: %lu sec, counters since boot):\n",
                   uptimeSec);
  p += n; remaining -= n;

  int busesShown = 0;
  for (uint8_t b = 0; b < I2cDeviceManager::NUM_BUSES; b++) {
    if (!mgr->isBusInitialized(b)) continue;
    if (remaining < 400) break;   // leave room rather than emit a half block
    n = renderBusMetrics(p, remaining, b, mgr->getMetrics(b), uptimeSec);
    p += n; remaining -= n;
    busesShown++;
  }

  if (busesShown == 0) {
    snprintf(p, remaining, "  No initialized buses\n");
  }

  return getDebugBuffer();
}

// ========== End I2C Device Health Tracking ==========

// ========== I2C Helper Functions ==========

// Shared address-table lookup. Labels are hints; drivers validate identity.
// Instance 1's address first, then a configured instance n >= 2, then an
// alternate address (System_I2CInstanceCore.h).
static int findSensorIndexByAddress(uint8_t address) {
  return i2cRowIndexByAddress(i2cSensors, i2cSensorsCount, address);
}

// Which configured instance of row `index` is at (address, bus): 1..max, or 0
// (no row, a row not in this build, or an address that is no instance's).
static uint8_t rowInstanceAt(int index, uint8_t address, uint8_t bus) {
  if (index < 0 || (size_t)index >= i2cSensorsCount || !i2cSensors[index].compiled) return 0;
  return i2cRowInstanceAt(i2cSensors[index], address, bus);
}

bool i2cReadSeesawProductId(uint8_t bus, uint8_t address, uint16_t& productId) {
  I2cDeviceManager* mgr = gI2cBusRunning ? I2cDeviceManager::getInstance() : nullptr;
  if (!mgr) return false;
  const uint8_t command[2] = { kSeesawStatusBase, kSeesawStatusVersion };
  uint8_t reply[4] = {};
  if (mgr->identityRead(bus, address, command, sizeof(command), kSeesawReadSettleUs,
                        reply, sizeof(reply), I2cTransactionOptions{100000, 50}) != 0) {
    return false;
  }
  productId = seesawProductFromVersion(seesawVersionFromBytes(reply));
  return true;
}

bool seesawReadProductIdOnHeldBus(TwoWire* wire, uint8_t address, uint16_t& productId) {
  const uint8_t command[2] = { kSeesawStatusBase, kSeesawStatusVersion };
  uint8_t reply[4] = {};
  if (i2cWriteThenReadHeld(wire, address, command, sizeof(command), kSeesawReadSettleUs,
                           reply, sizeof(reply)) != 0) {
    return false;
  }
  productId = seesawProductFromVersion(seesawVersionFromBytes(reply));
  return true;
}

// Whether a compiled row can claim `address` on `bus`: one of its configured
// instances (or instance 1's alternate address) on that instance's bus (a row
// without a bus setting claims either bus).
static bool rowClaims(const I2cSensorEntry& row, uint8_t address, uint8_t bus) {
  return i2cRowClaims(row, address, bus);
}

// A scan reads the product id only where two compiled rows of different
// products can claim the address on that bus (System_SeesawIdentity.h). Both
// encoder rows claim every configured encoder address, so those addresses
// are always among them; a build without a second seesaw driver makes no
// identity read anywhere else.
static bool seesawIdentityNeededAt(uint8_t address, uint8_t bus) {
  uint16_t claiming[8] = {};
  size_t count = 0;
  for (size_t i = 0; i < i2cSensorsCount && count < 8; i++) {
    if (rowClaims(i2cSensors[i], address, bus)) claiming[count++] = i2cSensors[i].seesawProductId;
  }
  return seesawIdentityNeeded(claiming, count);
}

// The address-only label among the rows that are not seesaw boards: the
// device at an ambiguous address that reported no product a row names (the
// fuel gauge at an encoder address, for instance).
static int findPlainSensorIndexByAddress(uint8_t address) {
  for (size_t i = 0; i < i2cSensorsCount; i++) {
    if (i2cSensors[i].seesawProductId == 0 && configuredSensor(i2cSensors[i]).address == address) return i;
  }
  return -1;
}

// Whether some row of the table names the board that reported `product`.
static bool productNamedByARow(uint16_t product) {
  for (size_t i = 0; i < i2cSensorsCount; i++) {
    if (product != 0 && i2cSensors[i].seesawProductId == product) return true;
  }
  return false;
}

// What answered at `address` on `bus`: a row index, or -1.
static int identifyRowAt(uint8_t address, uint8_t bus) {
  if (!seesawIdentityNeededAt(address, bus)) return findSensorIndexByAddress(address);
  uint16_t product = 0;
  if (i2cReadSeesawProductId(bus, address, product)) {
    // A compiled row first: a build names its own board before the label of
    // another build's driver.
    uint16_t products[32] = {};
    const size_t count = i2cSensorsCount < 32 ? i2cSensorsCount : 32;
    for (int pass = 0; pass < 2; ++pass) {
      for (size_t i = 0; i < count; i++) {
        products[i] = (pass == 1 || i2cSensors[i].compiled) ? i2cSensors[i].seesawProductId : 0;
      }
      const int index = seesawRowForProduct(products, count, product);
      if (index >= 0) return index;
    }
  }
  return findPlainSensorIndexByAddress(address);
}

String identifySensor(uint8_t address, uint8_t bus) {
  const int index = identifyRowAt(address, bus);
  if (index < 0) return "Unknown Device";
  const I2cSensorEntry sensor = configuredSensor(i2cSensors[index]);
  String result = sensor.name;
  result += " (";
  result += sensor.description;
  result += ")";
  return result;
}

// ========== End I2C Helper Functions ==========

// Highest usable GPIO number on the chip this firmware is compiled for, taken
// from the SoC capabilities (classic ESP32 = 39, ESP32-S3 = 48). The I2C pin
// commands derive their upper bound from this so the limit tracks the running
// board instead of a hardcoded cap that was only correct for one chip.
#define HW_GPIO_MAX (SOC_GPIO_PIN_COUNT - 1)
// The same bound as text, for the help and usage rows of the command table
// (the pin commands print the number itself). A pin count this list does not
// know is named in words, never as another chip's number.
#if SOC_GPIO_PIN_COUNT == 55      // ESP32-P4
  #define HW_GPIO_MAX_STR "54"
#elif SOC_GPIO_PIN_COUNT == 49    // ESP32-S3
  #define HW_GPIO_MAX_STR "48"
#elif SOC_GPIO_PIN_COUNT == 40    // classic ESP32
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
  const int parsed = parseBoolArg(valStr);
  if (parsed < 0) return kCommandReplyNotOnOff;
  const bool v = (parsed == 1);
  if (v && CAMERA_SCCB_I2C_PORT == 1 && !CAMERA_SCCB_SHARED)
    return "Error: I2C1/Wire1 controller is reserved for the camera in this build; select camera SCCB I2C0 to use the primary sensor bus.";
  setSetting(gSettings.i2cEnabled, v);
  return v ? "i2cBusEnabled set to 1 (reboot required)" : "i2cBusEnabled set to 0 (reboot required)";
}

// One body for the four bus pin commands: usage, whole-number parse, range
// check, save and report. `low` is 0 for bus 0 and -1 for bus 1, where -1
// marks the bus as unavailable on the board. A pin outside low..HW_GPIO_MAX is
// refused. Does not contain RETURN_VALID_IF_VALIDATE_CSTR (it returns from its
// caller), so that stays at each cmd_* entry point.
static const char* i2cSetPinAndReport(int& target, const String& argsInput, const char* settingName, int low) {
  if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
  String valStr = argsInput;
  valStr.trim();
  if (valStr.length() == 0) {
    snprintf(getDebugBuffer(), 1024,
             "Error: invalid arguments — Usage: %s <%d..%d> (%smax GPIO for this board; reboot required)",
             settingName, low, (int)HW_GPIO_MAX, low < 0 ? "-1=unavailable; " : "");
    return getDebugBuffer();
  }
  int v = 0;
  if (!parseIntArg(valStr, v)) return kCommandReplyNotANumber;
  if (v < low || v > HW_GPIO_MAX) {
    snprintf(getDebugBuffer(), 1024, "Error: %s must be %d..%d%s. Give a GPIO number of this board.",
             settingName, low, (int)HW_GPIO_MAX, low < 0 ? " (-1 = unavailable)" : "");
    return getDebugBuffer();
  }
  setSetting(target, v);
  snprintf(getDebugBuffer(), 1024, "%s set to %d (reboot required)", settingName, v);
  return getDebugBuffer();
}

const char* cmd_i2csdapin(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return i2cSetPinAndReport(gSettings.i2cSdaPin, argsInput, "i2cSdaPin", 0);
}

const char* cmd_i2csclpin(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return i2cSetPinAndReport(gSettings.i2cSclPin, argsInput, "i2cSclPin", 0);
}

// ---- Bus 1 (I2C2) variants ----
// Same shape as the bus 0 commands above, plus:
//   * Lower bound is -1 (vs 0 on bus 0) — -1 means "unavailable on this board"
//     (sentinel used on XIAO / QT Py / Feather V2 where there is no second I2C
//     port). Settings page surfaces this as the "(-1=unavailable)" hint.
//   * Upper bound is HW_GPIO_MAX (the SoC's highest GPIO: 39 on classic ESP32,
//     48 on ESP32-S3, 54 on ESP32-P4), shared with the bus-0 commands so both
//     track the board.
const char* cmd_i2c2busenabled(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = argsInput;
  valStr.trim();
  if (valStr.length() == 0) return "Error: invalid arguments — Usage: i2c2BusEnabled <0|1> (reboot required)";
  const int parsed = parseBoolArg(valStr);
  if (parsed < 0) return kCommandReplyNotOnOff;
  const bool v = (parsed == 1);
  if (v && CAMERA_SCCB_I2C_PORT == 0 && !CAMERA_SCCB_SHARED)
    return "Error: I2C2/Wire controller is reserved for the camera in this build; use I2C1/Wire1.";
  setSetting(gSettings.i2c2Enabled, v);
  return v ? "i2c2BusEnabled set to 1 (reboot required)" : "i2c2BusEnabled set to 0 (reboot required)";
}

const char* cmd_i2c2sdapin(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return i2cSetPinAndReport(gSettings.i2c2SdaPin, argsInput, "i2c2SdaPin", -1);
}

const char* cmd_i2c2sclpin(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return i2cSetPinAndReport(gSettings.i2c2SclPin, argsInput, "i2c2SclPin", -1);
}

// ---- Per-device bus assignment commands ----
// Each lets the user route a specific sensor to bus 0 (Wire1 / I2C1) or
// bus 1 (Wire / I2C2). A bus outside 0..NUM_BUSES-1 is refused. The change
// needs a reboot for the sensor's task to pick up the new value (drivers cache
// the bus at init time + the library object is constructed with the chosen Wire).
// One helper handles parse + range check + setSetting + report so the
// per-command stub is one line. The helper does NOT include the
// RETURN_VALID_IF_VALIDATE_CSTR macro because that returns from its caller
// — must stay at each cmd_* entry point.
const char* i2cSetDeviceBusAndReport(int& target, const String& argsInput, const char* settingName) {
  String valStr = argsInput;
  valStr.trim();
  if (valStr.length() == 0) {
    if (ensureDebugBuffer()) {
      snprintf(getDebugBuffer(), 1024, "Usage: %s <0..%d> (reboot required)",
               settingName, (int)(I2cDeviceManager::NUM_BUSES - 1));
      return getDebugBuffer();
    }
    return "Error: invalid arguments — Usage: <bus> (reboot required)";
  }
  int v = 0;
  if (!parseIntArg(valStr, v)) return kCommandReplyNotANumber;
  if (v < 0 || v >= (int)I2cDeviceManager::NUM_BUSES) {
    static_assert(I2cDeviceManager::NUM_BUSES == 2, "update the bus range messages");
    if (!ensureDebugBuffer()) return "Error: Bus number out of range. Use 0 or 1.";
    snprintf(getDebugBuffer(), 1024, "Error: %s must be 0..%d. Use 0 for I2C1 or 1 for I2C2.",
             settingName, (int)(I2cDeviceManager::NUM_BUSES - 1));
    return getDebugBuffer();
  }
  setSetting(target, v);
  if (ensureDebugBuffer()) {
    snprintf(getDebugBuffer(), 1024, "%s set to %d (reboot required)", settingName, v);
    return getDebugBuffer();
  }
  return "set (reboot required)";
}

const char* cmd_oledbus(const String& a)     { RETURN_VALID_IF_VALIDATE_CSTR(); return i2cSetDeviceBusAndReport(gSettings.oledBus,     a, "oledBus"); }
const char* cmd_inputbus(const String& a)    { RETURN_VALID_IF_VALIDATE_CSTR(); return i2cSetDeviceBusAndReport(gSettings.inputBus,  a, "inputBus"); }
const char* cmd_fuelgaugebus(const String& a){ RETURN_VALID_IF_VALIDATE_CSTR(); return i2cSetDeviceBusAndReport(gSettings.fuelGaugeBus, a, "fuelGaugeBus"); }

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
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();

  broadcastOutput("I2C Bus Scan (address-table labels):\n========================================");

  int totalCount = 0;
  int busesScanned = 0;
  bool bus1NotInitialized = false;

  // Bus 0 (I2C1, Wire1) — always scanned when initialized.
  if (mgr && mgr->isBusInitialized(0)) {
    busesScanned++;
    BROADCAST_PRINTF("I2C1 / Wire1 (bus 0, SDA=%d SCL=%d):",
                     gSettings.i2cSdaPin, gSettings.i2cSclPin);
    PollPauseGuard pollGuard(0);
    int count = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
      if (i2cPingAddress(addr, 100000, 50, 0)) {
        String identification = identifySensor(addr, 0);
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
    busesScanned++;
    broadcastOutput("");
    BROADCAST_PRINTF("I2C2 / Wire (bus 1, SDA=%d SCL=%d):",
                     gSettings.i2c2SdaPin, gSettings.i2c2SclPin);
    PollPauseGuard pollGuard(1);
    int count = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
      if (i2cPingAddress(addr, 100000, 50, 1)) {
        String identification = identifySensor(addr, 1);
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
    bus1NotInitialized = true;
    broadcastOutput("");
    broadcastOutput("I2C2 / Wire (bus 1): NOT INITIALIZED — check i2c2SdaPin/i2c2SclPin settings");
  }

  // No bus was probed: that is not a scan that found nothing.
  if (busesScanned == 0)
    return "Error: No I2C bus is initialized. Enable the 'i2c' feature and check the pin settings, then reboot.";

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
    if (bus1NotInitialized) {
      snprintf(getDebugBuffer(), 1024,
               "[i2cscan] %d device(s) found on bus 0; bus 1 is enabled but not initialized", totalCount);
    } else {
      snprintf(getDebugBuffer(), 1024, "[i2cscan] %d device(s) found", totalCount);
    }
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
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
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

// i2cDeviceTransaction helpers are template functions in System_I2C.h
// gThermalRunning provided by i2csensor_mlx90640.h

// SensorCache struct is now defined in i2c_system.h

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




static void addDiscoveredDevice(uint8_t address, uint8_t bus) {
  // MAX_CONNECTED_DEVICES is now a #define in i2c_system.h
  if (connectedDeviceCount >= MAX_CONNECTED_DEVICES) return;

  const int sensorIndex = identifyRowAt(address, bus);
  unsigned long now = millis();

  ConnectedDevice& device = connectedDevices[connectedDeviceCount++];
  device.address = address;
  device.bus = bus;
  device.isConnected = true;
  device.lastSeen = now;
  device.firstDiscovered = now;
  // The configured instance at this (address, bus), 0 when it is none.
  device.instance = rowInstanceAt(sensorIndex, address, bus);

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

  if (!gI2cBusRunning) { out.busDisabled = true; return; }

  const size_t n = i2cSensorsCount;
  static const size_t MAXSENS = 32;             // table is ~14; guard caps the rest
  // Found per (row, instance): instance 1 of every row, and each configured
  // instance n >= 2 of a multi-instance row (index n - 1).
  static const uint8_t MAXINST = kI2cRowInstanceCap;
  bool    found[MAXSENS][MAXINST] = {};
  uint8_t foundBus[MAXSENS][MAXINST];
  for (size_t i = 0; i < MAXSENS; i++)
    for (uint8_t k = 0; k < MAXINST; k++) foundBus[i][k] = 0xFF;

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
      const I2cSensorEntry& row = i2cSensors[i];
      // A seesaw row another build compiles: its board is not looked for.
      if (row.seesawProductId != 0 && !row.compiled) continue;
      const uint8_t instances = i2cRowInstances(row);
      for (uint8_t k = 1; k <= instances; k++) {
        if (found[i][k - 1]) continue;          // already located on the other bus
        const I2cSensorEntry s = configuredSensor(row, k);
        if (s.address == 0) continue;           // instance k is not configured
        // Discovery checks address replies; initialization belongs to the driver.
        // Where boards of different products share the address, the row also
        // needs its product id (the only identity read; see identifySensor).
        auto answers = [&](uint8_t address) {
          if (!i2cPingAddress(address, kProbeClockHz, s.i2cTimeoutMs, bus)) return false;
          if (!seesawIdentityNeededAt(address, bus)) return true;
          uint16_t product = 0;
          const bool read = i2cReadSeesawProductId(bus, address, product);
          return s.seesawProductId != 0 ? (read && product == s.seesawProductId)
                                         : !(read && productNamedByARow(product));
        };
        bool hit = answers(s.address);
        if (!hit && k == 1 && s.multiAddress) hit = answers(s.altAddress);
        if (hit) { found[i][k - 1] = true; foundBus[i][k - 1] = bus; }
      }
    }
  }

  esp_log_level_set("i2c.master", prevI2cLog);

  auto addEntry = [&](uint8_t addr, uint8_t bus, const char* name, const char* mod,
                      uint16_t kb, DetectStatus st, uint8_t instance) {
    if (out.count >= DetectionResult::MAX) return;
    DetectedEntry& e = out.entries[out.count++];
    e.address = addr; e.instance = instance; e.bus = bus; e.name = name;
    e.moduleName = mod; e.heapCostKb = kb; e.status = st;
    switch (st) {
      case DetectStatus::PRESENT_ENABLED:  out.nPresentEnabled++;  break;
      case DetectStatus::PRESENT_DISABLED: out.nPresentDisabled++; break;
      case DetectStatus::MISSING:          out.nMissing++;         break;
      case DetectStatus::PRESENT_INFRA:    out.nInfra++;           break;
    }
  };

  // 2) Report toggleable sensors by FEATURE and instance (dedups shared
  //    modules like the seesaw's 0x49/0x50; present(module, k) = any DB entry
  //    with it was found as instance k). Instance 1 is reported as before; an
  //    instance n >= 2 only when it is configured, as its own entry.
  const char* emitted[DetectionResult::MAX]; uint8_t emittedCount = 0;
  auto alreadyEmitted = [&](const char* m) -> bool {
    for (uint8_t k = 0; k < emittedCount; k++)
      if (strcmp(emitted[k], m) == 0) return true;
    return false;
  };

  for (size_t i = 0; i < n && i < MAXSENS; i++) {
    const I2cSensorEntry s = configuredSensor(i2cSensors[i]);
    if (!s.moduleName) continue;                 // infrastructure handled below
    if (alreadyEmitted(s.moduleName)) continue;

    const FeatureEntry* f = getFeatureById(s.moduleName);
    if (f && !isFeatureCompiled(f)) continue;    // not in this build

    bool     enabled = f ? isFeatureEnabled(f) : false;
    uint16_t kb      = f ? f->heapCostKb : 0;

    for (uint8_t k = 1; k <= MAXINST; k++) {
      // The module's address for instance k: instance 1 is the first row's,
      // an instance n >= 2 is reported only where a row of the module has it
      // configured.
      uint8_t address = k == 1 ? s.address : 0;
      bool present = false; uint8_t pbus = 0xFF;
      for (size_t j = 0; j < n && j < MAXSENS; j++) {
        const I2cSensorEntry& row = i2cSensors[j];
        if (!row.moduleName || strcmp(row.moduleName, s.moduleName) != 0) continue;
        if (k > i2cRowInstances(row)) continue;
        if (address == 0) address = i2cRowAddressAt(row, k);
        if (found[j][k - 1]) { present = true; pbus = foundBus[j][k - 1]; break; }
      }
      if (address == 0) continue;                // instance k is not configured

      if (present && enabled)        addEntry(address, pbus,  s.name, s.moduleName, kb, DetectStatus::PRESENT_ENABLED, k);
      else if (present && !enabled)  addEntry(address, pbus,  s.name, s.moduleName, kb, DetectStatus::PRESENT_DISABLED, k);
      else if (!present && enabled)  addEntry(address, 0xFF,  s.name, s.moduleName, kb, DetectStatus::MISSING, k);
      // else not present + not enabled → nothing to report.
    }

    if (emittedCount < DetectionResult::MAX) emitted[emittedCount++] = s.moduleName;
  }

  // 3) Infrastructure devices (no toggleable feature): OLED, fuel gauge, PWM.
  for (size_t i = 0; i < n && i < MAXSENS; i++) {
    if (i2cSensors[i].moduleName || !found[i][0]) continue;
    addEntry(i2cSensors[i].address, foundBus[i][0], i2cSensors[i].name, nullptr, 0, DetectStatus::PRESENT_INFRA, 1);
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
  if (out.enabled) (void)requestSettingsPersist();
  return out;
}

const char* cmd_detect(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);
  String sub = a.arg(0); sub.toLowerCase();
  const bool apply = (sub == "apply");
  if (a.count() > 1 || (a.count() == 1 && !apply))
    return "Error: invalid arguments — Usage: detect [apply]";

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

  BROADCAST_PRINTF("[detect] Address matches vs. configuration (I2C1=bus0, I2C2=bus1):");

  // An instance n >= 2 of a multi-instance device is named by its number
  // ("Encoder 2"); instance 1 keeps the row's name.
  char label[24];
  auto labelOf = [&](const DetectedEntry& e) -> const char* {
    if (e.instance >= 2) snprintf(label, sizeof(label), "%s %u", e.name, (unsigned)e.instance);
    else snprintf(label, sizeof(label), "%s", e.name);
    return label;
  };

  if (r.nPresentEnabled) {
    BROADCAST_PRINTF("  Present & enabled:");
    for (uint8_t i = 0; i < r.count; i++) {
      const DetectedEntry& e = r.entries[i];
      if (e.status == DetectStatus::PRESENT_ENABLED)
        BROADCAST_PRINTF("    %-10s (%s)  I2C%d  0x%02X", labelOf(e), e.moduleName, e.bus + 1, e.address);
    }
  }
  if (r.nPresentDisabled) {
    BROADCAST_PRINTF("  Present but DISABLED (attachable):");
    for (uint8_t i = 0; i < r.count; i++) {
      const DetectedEntry& e = r.entries[i];
      if (e.status == DetectStatus::PRESENT_DISABLED)
        BROADCAST_PRINTF("    %-10s (%s)  I2C%d  0x%02X  ~%uKB", labelOf(e), e.moduleName, e.bus + 1, e.address, (unsigned)e.heapCostKb);
    }
  }
  if (r.nMissing) {
    BROADCAST_PRINTF("  Enabled but NOT found (check wiring):");
    for (uint8_t i = 0; i < r.count; i++) {
      const DetectedEntry& e = r.entries[i];
      if (e.status == DetectStatus::MISSING)
        BROADCAST_PRINTF("    %-10s (%s)  0x%02X", labelOf(e), e.moduleName, e.address);
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

// Boot discovery uses the same probe, locking and clock restoration as the
// command/UI scans. Bus 0 is primary Wire1; bus 1 is secondary Wire.
static void scanBusForDevicesSmart(uint8_t busNumber, const uint8_t* addresses, int addressCount) {
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  if (!mgr || !mgr->isBusInitialized(busNumber)) return;

  PollPauseGuard pollGuard(busNumber);
  for (int i = 0; i < addressCount; i++) {
    const uint8_t address = addresses[i];
    if (address != 0 && i2cPingAddress(address, 100000, 200, busNumber)) {
      addDiscoveredDevice(address, busNumber);
    }
  }
}

void discoverI2cDevices() {
  // Early exit if I2C bus is disabled
  if (!gI2cBusRunning) {
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
    // Use the same feature predicate as manager pre-registration and hardware
    // detection. The old duplicate header-guard filter accidentally admitted
    // disabled headerless entries (RTC, FM, presence, OLED, and PCA9685), so a
    // gauge-only build still probed unrelated addresses at every boot.
    if (!isSensorCompiled(i2cSensors[i])) continue;

    // Two rows can name one address (the seesaw boards): probe it once.
    auto addAddress = [&](uint8_t address) {
      for (int k = 0; k < scanCount; k++) if (scanAddresses[k] == address) return;
      if (scanCount < 32) scanAddresses[scanCount++] = address;
    };
    addAddress(configuredSensor(i2cSensors[i]).address);
    if (i2cSensors[i].multiAddress) addAddress(i2cSensors[i].altAddress);
    // Each configured instance n >= 2 at its own address.
    const uint8_t instances = i2cRowInstances(i2cSensors[i]);
    for (uint8_t n = 2; n <= instances; n++) {
      const uint8_t address = i2cRowAddressAt(i2cSensors[i], n);
      if (address != 0) addAddress(address);
    }
  }
  
  INFO_I2C_DISCOVERYF("[Discovery] Smart scan: %d addresses on I2C1 (bus 0, SDA=%d SCL=%d)",
            scanCount, gSettings.i2cSdaPin, gSettings.i2cSclPin);

  // Bus 0 (I2C1, Wire1) — always scanned when the primary bus is up. Note
  // the convention flip from the pre-dual-bus stub: bus 0 is now the
  // primary, not the empty secondary slot.
  scanBusForDevicesSmart(0, scanAddresses, scanCount);

  // Bus 1 (I2C2, Wire) — scan only when the user has enabled the secondary
  // bus AND its pins are configured. Boards without a second port (XIAO,
  // QT Py, Feather V2) keep i2c2Enabled=false by default and skip this.
  I2cDeviceManager* mgrForScan = I2cDeviceManager::getInstance();
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
// do NOT use the I2cDeviceManager registry counts (getDeviceCount /
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
void buildI2cDeviceListJson(JsonArray& arr, bool verbose) {
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
    if (!devJsonBuf) devJsonBuf = (char*)ps_alloc(kDevJsonSize, AllocPref::PreferPsram, "devices.json");
    if (!devJsonBuf) return kCommandReplyOutOfMemory;
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
  // discoverI2cDevices() skips a disabled bus without a word to the caller.
  if (!gI2cBusRunning)
    return "Error: I2C bus is disabled. Enable the 'i2c' feature and reboot, then run discover.";
  discoverI2cDevices();

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
  if (connectedDeviceCount == 0) {
    broadcastOutput("Device Registry (from memory):");
    broadcastOutput("  No devices discovered. Run 'discover' first.");
    return "[I2C] Registry empty";
  }

  // Single source of truth (also feeds /api/devices) — no hand-rolled String.
  PSRAM_JSON_DOC(doc);
  buildDeviceRegistryJson(doc);
  static const size_t kRegBufSize = 4096;
  static char* regBuf = nullptr;
  if (!regBuf) regBuf = (char*)ps_alloc(kRegBufSize, AllocPref::PreferPsram, "devicefile.json");
  if (!regBuf) return "Error: [I2C] Out of memory";
  size_t len = serializeJson(doc, regBuf, kRegBufSize);
  if (len == 0 || len >= kRegBufSize - 1) return "Error: device registry outgrew the response buffer";
  // The document is the reply (output contract channel 2): whole, and nothing
  // on the stream, where one line is cut at 255 B.
  return regBuf;
}

// ---- Live sensor view (Phase 2: state + live `data` readings) -------------
// Contract: docs/BLE_SENSORS_INTEGRATION.md. One entry per COMPILED sensor:
// id/name/kind + live enabled/connected, plus `data` (the sensor's native
// readings, embedded verbatim) for active, non-stream sensors. Readings reuse
// the same per-sensor builders ESP-NOW/MQTT use — no new data plumbing.
typedef int (*SensorDataFn)(char*, size_t);

// Shared sensor-reading envelope. See System_I2C.h for the contract. Writes the
// common opening (no closing brace) so every per-sensor builder emits ONE shape;
// the builder appends its own value keys and '}'. Returns bytes written, or 0 on
// overflow (so the caller never appends past the buffer).
int sensorEnvelopeBegin(char* buf, size_t bufSize, bool valid, bool connected,
                        unsigned long lastUpdateMs) {
  if (!buf || bufSize == 0) return 0;
  int n = snprintf(buf, bufSize,
                   "{\"valid\":%s,\"connected\":%s,\"ts\":%lu",
                   valid ? "true" : "false", connected ? "true" : "false",
                   lastUpdateMs);
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
  // Gate on `enabled` (LIVE running flag) only — deliberately NOT on `connected`.
  // `connected` here is the boot-scan registry (isSensorConnected), refreshed only
  // at boot and on manual `discover`. Gating on it meant a sensor hot-plugged and
  // opened after boot had its reading SILENTLY DROPPED from `sensors json` — the
  // entry reported connected:false with no `data` — until the next reboot/discover.
  // If the sensor is running, its builder is authoritative: it emits its own LIVE
  // `connected` and a valid:false envelope when the cache is stale, so a running-
  // but-absent sensor degrades honestly instead of vanishing.
  if (dataFn && enabled) {
    static char* dbuf = nullptr;
    if (!dbuf) dbuf = (char*)ps_alloc(1024, AllocPref::PreferPsram, "sensor.data.scratch");
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
  addSensorEntry(arr, "presence", "STHS34PF80 presence",    SENSOR_KIND_SCALAR, gPresenceRunning, isSensorConnected("presence"), includeData ? presenceBuildDataJson : nullptr);
#endif
#if ENABLE_TOF_SENSOR
  addSensorEntry(arr, "tof",      "VL53L4CX distance",      SENSOR_KIND_VECTOR, gTofRunning, isSensorConnected("tof"), includeData ? tofBuildDataJson : nullptr);
#endif
#if ENABLE_IMU_SENSOR
  addSensorEntry(arr, "imu",      "BNO055 orientation",     SENSOR_KIND_VECTOR, gImuRunning, isSensorConnected("imu"), includeData ? imuBuildDataJson : nullptr);
#endif
#if ENABLE_GPS_SENSOR
  addSensorEntry(arr, "gps",      "PA1010D GPS",            SENSOR_KIND_VECTOR, gGpsRunning, isSensorConnected("gps"), includeData ? gpsBuildDataJson : nullptr);
#endif
#if ENABLE_FM_RADIO
  addSensorEntry(arr, "fmradio",  "RDA5807 FM radio",       SENSOR_KIND_SCALAR, gFmRadioRunning, isSensorConnected("fmradio"), includeData ? fmRadioBuildDataJson : nullptr);
#endif
#if ENABLE_RTC_SENSOR
  addSensorEntry(arr, "rtc",      "DS3231 RTC",             SENSOR_KIND_SCALAR, gRtcRunning, isSensorConnected("rtc"), includeData ? rtcBuildDataJson : nullptr);
#endif
#if ENABLE_APDS_SENSOR
  addSensorEntry(arr, "apds",     "APDS9960 gesture/color", SENSOR_KIND_SCALAR,
                 (gApdsColorRunning || gApdsProximityRunning || gApdsGestureRunning),
                 isSensorConnected("apds"), includeData ? apdsBuildDataJson : nullptr);
#endif
#if ENABLE_GAMEPAD_SENSOR
  // id is "input" — the canonical, unified module name (Seesaw gamepad OR ANO
  // encoder). Matches controls json / open<id>/close<id> / sensorautostart /
  // the I2C DB moduleName, so the app correlates on one name with no overrides.
  addSensorEntry(arr, "input",    "Seesaw gamepad",         SENSOR_KIND_SCALAR, gInputRunning, gInputConnected, includeData ? gamepadBuildDataJson : nullptr);
#elif ENABLE_GT911_TOUCH
  addSensorEntry(arr, "input", "GT911 single touch", SENSOR_KIND_SCALAR, gInputRunning, gInputConnected, nullptr);
#elif ENABLE_GPIO_ENCODER
  addSensorEntry(arr, "input",    "GPIO rotary encoder",    SENSOR_KIND_SCALAR, gInputRunning, gInputConnected, includeData ? gpioEncoderBuildDataJson : nullptr);
#endif
#if ENABLE_THERMAL_SENSOR
  addSensorEntry(arr, "thermal",  "MLX90640 thermal",       SENSOR_KIND_STREAM, gThermalRunning, isSensorConnected("thermal"), includeData ? thermalBuildSummaryJson : nullptr);
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
    if (!sensorsJsonBuf) sensorsJsonBuf = (char*)ps_alloc(kSensorsJsonSize, AllocPref::PreferPsram, "sensors.json");
    if (!sensorsJsonBuf) return kCommandReplyOutOfMemory;
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
    const I2cSensorEntry sensor = configuredSensor(i2cSensors[i]);

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
    return "Usage: sensorinfo <sensor_name>\nExample: sensorinfo BNO055";
  }

  // Find sensor by name (case insensitive)
  const I2cSensorEntry* foundSensor = nullptr;
  I2cSensorEntry configuredInfo{};
  String searchName = args;
  searchName.toLowerCase();

  for (size_t i = 0; i < i2cSensorsCount; i++) {
    String sensorName = String(i2cSensors[i].name);
    sensorName.toLowerCase();
    if (sensorName == searchName) {
      configuredInfo = configuredSensor(i2cSensors[i]);
      foundSensor = &configuredInfo;
      break;
    }
  }

  if (!foundSensor) {
    if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
    snprintf(getDebugBuffer(), GLOBAL_DEBUG_BUFFER_SIZE,
             "Error: Sensor '%s' not found in database. Use 'sensors' to see the full list.",
             args.c_str());
    return getDebugBuffer();
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

  // Check both initialized manager buses.  The old code mixed a raw `Wire`
  // transfer with a default-bus helper, skipped the bus mutex for one probe,
  // and never printed the secondary-bus hit.
  bool connectedBus[I2cDeviceManager::NUM_BUSES] = {};
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  for (uint8_t bus = 0; bus < I2cDeviceManager::NUM_BUSES; ++bus) {
    if (!mgr || !mgr->isBusInitialized(bus)) continue;
    connectedBus[bus] = i2cPingAddress(foundSensor->address, 100000, 50, bus);
    if (!connectedBus[bus] && foundSensor->multiAddress) {
      connectedBus[bus] = i2cPingAddress(foundSensor->altAddress, 100000, 50, bus);
    }
  }

  broadcastOutput("");
  broadcastOutput("Connection Status:");

  if (connectedBus[0]) {
    char buf[64];
    snprintf(buf, sizeof(buf), "  ✓ Connected on I2C1/Wire1 (SDA=%d, SCL=%d)",
             gSettings.i2cSdaPin, gSettings.i2cSclPin);
    broadcastOutput(buf);
  }

  if (connectedBus[1]) {
    char buf[64];
    snprintf(buf, sizeof(buf), "  ✓ Connected on I2C2/Wire (SDA=%d, SCL=%d)",
             gSettings.i2c2SdaPin, gSettings.i2c2SclPin);
    broadcastOutput(buf);
  }

  if (!connectedBus[0] && !connectedBus[1]) {
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
  uint16_t heapCostKb;  // Estimated heap usage in KB
};

// Columns: name, shortName, autoStartFlag, heapCostKb
static const SensorHeapCost sensorHeapCosts[] = {
  { "Thermal Camera", "thermal", &gSettings.thermalAutoStart, 32 },  // MLX90640: large frame buffer
  { "ToF Distance",   "tof",     &gSettings.tofAutoStart,      8 },  // VL53L4CX: moderate
  { "IMU",            "imu",     &gSettings.imuAutoStart,     12 },  // BNO055: calibration + buffers
  { "GPS",            "gps",     &gSettings.gpsAutoStart,      4 },  // PA1010D: NMEA parsing
  { "FM Radio",       "fmradio", &gSettings.fmRadioAutoStart,  2 },  // RDA5807: minimal
  { "APDS Gesture",   "apds",    &gSettings.apdsAutoStart,     4 },  // APDS9960: gesture buffers
#if ENABLE_GPIO_ENCODER
  { "GPIO Encoder",   "input",   &gSettings.inputAutoStart,    4 },
#elif ENABLE_ANO_ENCODER
  { "ANO Encoder",    "input",   &gSettings.inputAutoStart,    2 },  // Adafruit ANO seesaw: minimal
#else
  { "Gamepad",        "input",   &gSettings.inputAutoStart,    2 },  // Seesaw mini gamepad: minimal
#endif
  { "RTC Clock",      "rtc",     &gSettings.rtcAutoStart,      2 },  // DS3231: minimal
  { "Presence",       "presence",&gSettings.presenceAutoStart, 2 },  // STHS34PF80: minimal
#if ENABLE_ENCODER
  { "Encoder",        "encoder", &gEncoderInst[0].autoStart,   1 },  // seesaw encoder pixels: no task
#endif
};
static const size_t sensorHeapCostCount = sizeof(sensorHeapCosts) / sizeof(sensorHeapCosts[0]);

// Calculate total estimated heap for enabled sensors
static uint32_t getEnabledSensorHeapEstimate() {
  uint32_t total = 0;
  for (size_t i = 0; i < sensorHeapCostCount; i++) {
    if (*sensorHeapCosts[i].autoStartFlag) {
      total += sensorHeapCosts[i].heapCostKb;
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
    uint32_t freeHeapKb = ESP.getFreeHeap() / 1024;
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
        sensorHeapCosts[i].heapCostKb,
        enabled ? "[ON]" : "off");
    }
    
    pos += snprintf(buf + pos, buf_SIZE - pos,
      "─────────────────────────────────\n"
      "Enabled total: ~%luKB | Free heap: %luKB\n"
      "Usage: sensorautostart <sensor> <on|off>",
      (unsigned long)enabledCost, (unsigned long)freeHeapKb);
    
    return buf;
  }
  
  if (!a.hasMinArgs(2)) {
    return "Error: invalid arguments — Usage: sensorautostart <sensor> [n] <on|off>";
  }

  String sensor = a.arg(0);
  String value = a.arg(1);
  sensor.toLowerCase();

  // G4 (System_I2CInstanceCore.h): sensorautostart <sensor> <n> <on|off>.
  // Instance 1 is accepted for every type and means what the two-word form
  // does; an instance n >= 2 exists only for a type that has several.
  if (a.count() > 3) {
    return "Error: invalid arguments — Usage: sensorautostart <sensor> [n] <on|off>";
  }
  if (a.count() == 3) {
    if (sensor == "all") return "Error: 'all' takes no instance. Usage: sensorautostart all <on|off>";
    I2cDeviceType type = I2C_DEVICE_COUNT;
    for (int i = 0; i < I2C_DEVICE_COUNT; ++i) {
      if (sensor == kI2cTypes[i].id || (sensor == "fm" && kI2cTypes[i].type == I2C_DEVICE_FMRADIO)) {
        type = kI2cTypes[i].type;
      }
    }
    if (type == I2C_DEVICE_COUNT) {
      return "Error: Unknown sensor. Use thermal, tof, imu, gps, fmradio, apds, input, rtc, presence, encoder or all.";
    }
    const String token = a.arg(1);
    I2cInstanceArg instance;
    if (!i2cInstanceNumber(token.c_str(), token.length(), instance.n)) {
      instance.error = I2cInstanceError::NotAnInstance;
    } else {
      i2cInstanceInRange(type, instance.n, instance);
    }
    if (instance.error != I2cInstanceError::None) {
      instance.token = token.c_str();
      instance.tokenLen = token.length();
      EXT_RAM_BSS_ATTR static char refused[160];
      return i2cInstanceErrorText(type, instance, "sensorautostart <sensor> [n] <on|off>", refused,
                                  sizeof(refused));
    }
    value = a.arg(2);
    value.toLowerCase();
    if (instance.n >= 2) {
      const bool on = argIsOn(value);
      if (!on && !argIsOff(value)) return kCommandReplyNotOnOff;
      const FeatureEntry* feature = getFeatureById(kI2cTypes[type].id);
      if (on && feature && !isFeatureCompiled(feature)) {
        return "Error: That sensor is not in this build. Run 'sensors json brief' to list the sensors this firmware has.";
      }
      // Only the encoder has instances n >= 2 (i2cInstanceInRange refused the rest).
      bool* setting = encoderAutoStartSetting(instance.n);
      if (!setting) return "Error: That sensor is not in this build.";
      bool& flag = *setting;
      const bool was = flag;
      setSetting(flag, on);
      EXT_RAM_BSS_ATTR static char instanceResult[96];
      snprintf(instanceResult, sizeof(instanceResult), "[AutoStart] %s %s%s",
               deviceTypeDisplayName(type, instance.n), was == on ? "already " : "",
               on ? "enabled" : "disabled");
      return instanceResult;
    }
  }
  value.toLowerCase();
  
  bool enable = argIsOn(value);
  bool disable = argIsOff(value);
  
  if (!enable && !disable) {
    return kCommandReplyNotOnOff;
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
      // Enabling leaves out a sensor whose driver is not compiled in, as the
      // single form refuses it. Disabling clears every row.
      const FeatureEntry* rowFeature = getFeatureById(sensorHeapCosts[i].shortName);
      if (enable && rowFeature && !isFeatureCompiled(rowFeature)) continue;
      *sensorHeapCosts[i].autoStartFlag = enable;
      if (enable) totalCost += sensorHeapCosts[i].heapCostKb;
    }
    (void)requestSettingsPersist();
    
    EXT_RAM_BSS_ATTR static char result[128];
    uint32_t freeHeapKb = ESP.getFreeHeap() / 1024;
    if (enable) {
      snprintf(result, sizeof(result), 
        "[AutoStart] All sensors enabled (~%luKB total, %luKB free)",
        (unsigned long)totalCost, (unsigned long)freeHeapKb);
    } else {
      snprintf(result, sizeof(result), "[AutoStart] All sensors disabled");
    }
    return result;
  }
  
  if (!found) {
    return "Error: Unknown sensor. Use thermal, tof, imu, gps, fmradio, apds, input, rtc, presence, encoder or all.";
  }

  // A sensor whose driver is not compiled in can never start, so enabling it
  // is refused. Disabling it is a plain success: it clears a flag left on by
  // the defaults or by a settings file from another build.
  const FeatureEntry* feature = getFeatureById(found->shortName);
  if (enable && feature && !isFeatureCompiled(feature)) {
    EXT_RAM_BSS_ATTR static char notBuilt[128];
    snprintf(notBuilt, sizeof(notBuilt),
             "Error: %s is not in this build. Run 'sensors json brief' to list the sensors this firmware has.",
             found->name);
    return notBuilt;
  }
  
  bool wasEnabled = *found->autoStartFlag;
  *found->autoStartFlag = enable;
  (void)requestSettingsPersist();
  
  EXT_RAM_BSS_ATTR static char result[128];
  uint32_t freeHeapKb = ESP.getFreeHeap() / 1024;
  
  if (enable && !wasEnabled) {
    snprintf(result, sizeof(result), 
      "[AutoStart] %s enabled (~%dKB, %luKB free after boot)",
      found->name, found->heapCostKb, (unsigned long)(freeHeapKb - found->heapCostKb));
  } else if (!enable && wasEnabled) {
    snprintf(result, sizeof(result), 
      "[AutoStart] %s disabled (+%dKB freed after reboot)",
      found->name, found->heapCostKb);
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
  static const char kInUse[] = "Error: I2C is in use by the camera. Stop camera before resetting the bus.";
  static const char kNotInitialized[] =
      "Error: I2C bus is not initialized. Enable the 'i2c' feature and reboot, then retry i2creset.";
  auto* manager = I2cDeviceManager::existingInstance();
  if (manager && manager->busHasClients(0)) return kInUse;
  
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  if (!mgr) return "Error: I2C manager not initialized";
  // Refuse a bus that was never brought up before any step is announced.
  if (!mgr->isBusInitialized(0)) return kNotInitialized;
  
  broadcastOutput("[I2C] Starting bus reset...");
  broadcastOutput("[I2C] Step 1/3: Pausing sensor polling...");
  mgr->pausePolling();
  vTaskDelay(pdMS_TO_TICKS(200));
  
  broadcastOutput("[I2C] Step 2/3: Performing bus recovery (clock toggle + reinit)...");
  const I2cBusRecovery recovery = mgr->performBusRecovery();
  
  broadcastOutput("[I2C] Step 3/3: Resuming sensor polling...");
  mgr->resumePolling();
  
  // A recovery that was skipped left the bus as it was: name the cause.
  switch (recovery) {
    case I2cBusRecovery::Done:
      break;
    case I2cBusRecovery::NotInitialized:
      return kNotInitialized;
    case I2cBusRecovery::PinsInvalid:
      return "Error: I2C pin settings are invalid. Set i2csdapin and i2csclpin, then reboot.";
    case I2cBusRecovery::Busy:
      return "Error: I2C bus is busy and could not be locked. Retry i2creset in a few seconds.";
    case I2cBusRecovery::InUse:
      return kInUse;
  }
  
  broadcastOutput("[I2C] Bus reset complete. Run 'i2chealth' to check device status.");
  return "[I2C] Bus reset complete";
}

const char* cmd_i2cpause(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  if (!mgr) return "Error: I2C manager not initialized";
  
  if (mgr->isPollingPaused()) {
    return "Error: I2C sensor polling is already paused. Resume it with 'i2cresume'.";
  }
  
  mgr->pausePolling();
  return "[I2C] Sensor polling paused. Use 'i2cresume' to resume.";
}

const char* cmd_i2cresume(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  if (!mgr) return "Error: I2C manager not initialized";
  
  if (!mgr->isPollingPaused()) {
    return "Error: I2C sensor polling is not paused. Pause it with 'i2cpause' first.";
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
  uint8_t address = 0;  // text that is not an address stays 0 and is refused below
  parseAddressArg(args, 1, 127, address);
  
  if (address == 0 || address > 127) {
    return "Error: Invalid I2C address (must be 1-127 or 0x01-0x7F)";
  }
  
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  if (!mgr) return "Error: I2C manager not initialized";
  
  // The command takes an address only, so resolve across buses — otherwise
  // `i2crecover` finds no registered device for anything on bus 1.
  I2cDevice* dev = mgr->getDeviceAnyBus(address);
  if (!dev || !dev->isInitialized()) {
    EXT_RAM_BSS_ATTR static char result[96];
    snprintf(result, sizeof(result), "Error: No registered device at 0x%02X. Run 'i2chealth' to list devices.", address);
    return result;
  }

  dev->attemptRecovery();

  EXT_RAM_BSS_ATTR static char result[128];
  snprintf(result, sizeof(result),
    "[I2C] Device 0x%02X (%s) on bus %u degraded state cleared. Retry sensor start command.",
    address, dev->name, dev->bus);
  return result;
}

// ============================================================================
// I2C Command Registry
// ============================================================================

// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
const CommandEntry i2cCommands[] = {
  // Bus Configuration
  { "i2cbusenabled", "Enable/disable I2C1 bus: <0|1> (reboot required)", true, cmd_i2cbusenabled, "Usage: i2cBusEnabled <0|1>" },
  { "i2csdapin", "Set I2C1 SDA pin: <0.." HW_GPIO_MAX_STR "> (max GPIO for this board)", true, cmd_i2csdapin, "Usage: i2cSdaPin <0.." HW_GPIO_MAX_STR "> (max GPIO for this board)" },
  { "i2csclpin", "Set I2C1 SCL pin: <0.." HW_GPIO_MAX_STR "> (max GPIO for this board)", true, cmd_i2csclpin, "Usage: i2cSclPin <0.." HW_GPIO_MAX_STR "> (max GPIO for this board)" },
  { "i2c2busenabled", "Enable/disable I2C2 bus: <0|1> (reboot required)", true, cmd_i2c2busenabled, "Usage: i2c2BusEnabled <0|1>" },
  { "i2c2sdapin", "Set I2C2 SDA pin: <-1.." HW_GPIO_MAX_STR "> (-1=unavailable)", true, cmd_i2c2sdapin, "Usage: i2c2SdaPin <-1.." HW_GPIO_MAX_STR "> (-1=unavailable)" },
  { "i2c2sclpin", "Set I2C2 SCL pin: <-1.." HW_GPIO_MAX_STR "> (-1=unavailable)", true, cmd_i2c2sclpin, "Usage: i2c2SclPin <-1.." HW_GPIO_MAX_STR "> (-1=unavailable)" },
  // Per-device bus assignment — route a device to bus 0 (I2C1) or bus 1 (I2C2).
  // Only devices whose driver is NOT its own build-gated module live here.
  // Each sensor's own <sensor>bus command lives in that sensor's .cpp next to
  // its other commands (i2csensor_pa1010d.cpp etc.), so it compiles out with
  // the driver instead of needing a guard here — same move the sensor-specific
  // I2C clock commands already made.
  { "oledbus",     "Route OLED to bus: <0|1> (reboot required)",         true, cmd_oledbus,     "Usage: oledBus <0|1>" },
  { "inputbus",    "Route input device to bus: <0|1> (reboot required)",  true, cmd_inputbus,    "Usage: inputBus <0|1>" },
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
  { "sensorautostart", "Sensor auto-start: [sensor] [n] [on|off]", true, cmd_sensorautostart, "Usage: sensorautostart [sensor] [n] [on|off]\n       sensorautostart all [on|off]\nSensors: thermal, tof, imu, gps, fmradio, apds, input, rtc, presence, encoder (n: encoder 1..4)" }
};

const size_t i2cCommandsCount = sizeof(i2cCommands) / sizeof(i2cCommands[0]);

// ============================================================================
// Command Registration
// ============================================================================
// Registration handled by gCommandModules[] in System_Utils.cpp

// ============================================================================
// Sensor Status System (moved from HardwareOne.ino)
// ============================================================================

const char* deviceTypeDisplayName(I2cDeviceType sensor, uint8_t n) {
  if (n != 1) {
    const uint8_t slot = i2cSlot(sensor, n);
    if (slot == kI2cNoSlot) return "Unknown";
    // One name per extra instance slot ("Encoder 2"), built once by a
    // thread-safe static initialization, so the text stays valid.
    EXT_RAM_BSS_ATTR static char names[kI2cExtraSlotStorage][24];
    static const bool built = [] {
      for (uint8_t extra = I2C_DEVICE_COUNT; extra < kI2cSlotCount; ++extra) {
        I2cDeviceType type = I2C_DEVICE_COUNT;
        uint8_t instance = 1;
        if (i2cSlotToRef(extra, type, instance)) {
          i2cInstanceName(type, instance, names[extra - I2C_DEVICE_COUNT], sizeof(names[0]));
        }
      }
      return true;
    }();
    (void)built;
    return names[slot - I2C_DEVICE_COUNT];
  }
  switch (sensor) {
    case I2C_DEVICE_THERMAL:  return "Thermal";
    case I2C_DEVICE_TOF:      return "ToF";
    case I2C_DEVICE_IMU:      return "IMU";
#if ENABLE_GT911_TOUCH
    case I2C_DEVICE_INPUT: return "GT911 Touch";
#elif ENABLE_ANO_ENCODER
    case I2C_DEVICE_INPUT:  return "ANO Encoder";
#else
    case I2C_DEVICE_INPUT:  return "Gamepad";
#endif
    case I2C_DEVICE_GPS:      return "GPS";
    case I2C_DEVICE_FMRADIO:  return "FM Radio";
    case I2C_DEVICE_APDS:     return "APDS";
    case I2C_DEVICE_RTC:      return "RTC";
    case I2C_DEVICE_PRESENCE: return "Presence";
    case I2C_DEVICE_ENCODER: return "Encoder";
    default:              return "Unknown";
  }
}

bool requestSensorStop(I2cDeviceType sensor, uint8_t n) {
  // Same admission as a start: with the bus disabled (fixed at boot) no queue
  // worker runs and nothing was started, so refuse instead of allocating the
  // manager for a request nothing would process.
  if (!gI2cBusRunning) return false;
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  return mgr && mgr->enqueueDeviceStop(sensor, n);
}

// Written only by the queue worker; tasks publish cleanup completion by
// clearing their handles. A stop event means that old task has finished.
// Indexed by instance slot: instance 1 (slot = type) in internal RAM as
// before, the extra instances' slots in PSRAM.
static bool sSensorStopAwaitingCleanup[I2C_DEVICE_COUNT] = {};
EXT_RAM_BSS_ATTR static bool sSensorStopAwaitingCleanupExtra[kI2cExtraSlotStorage];
static bool& sensorStopAwaitingCleanup(uint8_t slot) {
  return slot < I2C_DEVICE_COUNT ? sSensorStopAwaitingCleanup[slot]
                                 : sSensorStopAwaitingCleanupExtra[slot - I2C_DEVICE_COUNT];
}

// Only the queue worker changes lifecycle flags in response to commands. A
// close during initialization is applied after the owner returns, so teardown
// cannot free resources still being used by the starter.
static void applySensorStop(I2cDeviceType sensor, uint8_t n) {
  const uint8_t slot = i2cSlot(sensor, n);
  if (slot == kI2cNoSlot) return;
  // Only a sensor that was running, still has a task, was initialized without
  // one (FM tune/boot, RTC), or had an admitted start this stop cancels gets a
  // cleanup latch and a stop event. Stopping an idle sensor publishes nothing.
  const bool startOutstanding = sensorStartOutstanding(slot).exchange(false);
  const bool wasActive = startOutstanding || sensorIsRunning(sensor, n) || sensorHasTask(sensor, n)
#if ENABLE_FM_RADIO
      || (sensor == I2C_DEVICE_FMRADIO && gRadioInitialized)
#endif
#if ENABLE_RTC_SENSOR
      || (sensor == I2C_DEVICE_RTC && gRtcConnected)
#endif
      ;
  // Set enabled flag to false and record stop time (common boilerplate).
  // Only the encoder's driver has instances n >= 2 (driverServesInstance).
  switch (driverServesInstance(sensor, n) ? sensor : I2C_DEVICE_COUNT) {
    case I2C_DEVICE_COUNT:
      break;
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
#if ENABLE_GT911_TOUCH
      touchStop();
#elif ENABLE_GAMEPAD_SENSOR || ENABLE_ANO_ENCODER
      gInputRunning = false;
#if ENABLE_ANO_ENCODER
      gAnoEncoderEnabled = false;
#else
      gGamepadLastStopTime = millis();
#endif
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
      // Tune/boot can initialize the radio without creating a polling task.
      if (!gFmRadioTaskHandle) fmRadioDeinit();
#endif
      break;
    case I2C_DEVICE_APDS:
#if ENABLE_APDS_SENSOR
      gApdsRunning = false;
      gApdsColorRunning = false;
      gApdsProximityRunning = false;
      gApdsGestureRunning = false;
      gApdsLastStopTime = millis();
#endif
      break;
    case I2C_DEVICE_RTC:
#if ENABLE_RTC_SENSOR
      gRtcLastStopTime = millis();
      rtcStop();  // The owner also handles initialization without a task.
#endif
      break;
    case I2C_DEVICE_PRESENCE:
#if ENABLE_PRESENCE_SENSOR
      gPresenceRunning = false;
      gPresenceLastStopTime = millis();
#endif
      break;
    case I2C_DEVICE_ENCODER:
      encoderStop(n);  // no task: the owner darkens the pixels and stops now
      break;
    default: return;
  }

  if (!wasActive) return;
  sensorStopAwaitingCleanup(slot) = true;
  sensorStatusBumpWith("stop@requested");
}

static void finishSensorStops() {
  for (uint8_t slot = 0; slot < kI2cSlotCount; ++slot) {
    I2cDeviceType sensor = I2C_DEVICE_COUNT;
    uint8_t n = 1;
    if (!i2cSlotToRef(slot, sensor, n)) continue;
    bool& awaiting = sensorStopAwaitingCleanup(slot);
    if (!awaiting || sensorHasTask(sensor, n)) continue;
    awaiting = false;
    const char* name = deviceTypeDisplayName(sensor, n);
#if ENABLE_ESPNOW
    // Tell the receiver of this device's readings (SENSOR_STATUS) for sensors
    // that have remote types. The wire carries instance 1 only (1.0).
    switch (n == 1 ? sensor : I2C_DEVICE_COUNT) {
      case I2C_DEVICE_THERMAL:  sensorPresenceChanged(REMOTE_SENSOR_THERMAL, false);  break;
      case I2C_DEVICE_TOF:      sensorPresenceChanged(REMOTE_SENSOR_TOF, false);      break;
      case I2C_DEVICE_IMU:      sensorPresenceChanged(REMOTE_SENSOR_IMU, false);      break;
      case I2C_DEVICE_INPUT:  sensorPresenceChanged(REMOTE_SENSOR_INPUT, false);  break;
      case I2C_DEVICE_GPS:      sensorPresenceChanged(REMOTE_SENSOR_GPS, false);      break;
      case I2C_DEVICE_FMRADIO:  sensorPresenceChanged(REMOTE_SENSOR_FMRADIO, false);  break;
      case I2C_DEVICE_RTC:      sensorPresenceChanged(REMOTE_SENSOR_RTC, false);      break;
      case I2C_DEVICE_PRESENCE: sensorPresenceChanged(REMOTE_SENSOR_PRESENCE, false); break;
      default: break;  // APDS has no remote sensor type
    }
#endif

    systemEventPost(SYSEVT_SENSOR_STOPPED, name);

    sensorStatusBumpWith("stop@complete");
  }
}

// Helper: set cause then bump (to preserve existing call-sites)
void sensorStatusBumpWith(const char* cause) {
  INFO_I2C_BUSF("Status bump: %s", cause ? cause : "(null)");
  gLastStatusCause = cause ? cause : "";
  sensorStatusBump();
}

const char* buildSensorStatusJson() {
  // PSRAM buffer allocated once, reused forever (zero stack impact).
  //
  // 2048 is deliberate and is NOT an oversight beside the 4096 used for the
  // system-info JSON (WebServer_Events.cpp sysJsonBuf). The difference is that
  // THIS document has no runtime-variable content: it is ~46 fixed top-level
  // keys, at most 7 `<x>QueuePos` keys (the start queue is a mod-8 ring, so
  // depth tops out at 7), and the `nonI2c` map over nonI2cSensors[], whose
  // length is a compile-time array size. buildSystemInfoJson needs 4096 because
  // it embeds connectivity.i2c.deviceList — an actually unbounded JsonArray of
  // DISCOVERED devices (see System_Utils.cpp, "the only unbounded section").
  //
  // Measured serialized sizes: ~993 B on a gamepad+OLED build, ~1117 B during a
  // boot storm with 7 sensors queued, ~1273 B in the worst configuration that
  // can exist (full I2C set + camera + microphone registry entries) = 62% of
  // this buffer. Growth is per-sensor-added-to-the-source (~60-80 B each for
  // Running/Compiled/Queued/QueuePos), not per-device-plugged-in, so ~10 new
  // sensors would have to land before this needs revisiting — and the guard
  // below fails loudly with "status_too_large" rather than truncating.
  //
  // Callers must not assume a smaller ceiling: a consumer that copied this into
  // a char[1200] silently truncated every send above 1170 B and, because the
  // cut ate the SSE frame terminator, swallowed the NEXT event as well.
  static char* buf = nullptr;
  static const size_t kBufSize = 2048;

  if (!buf) {
    buf = (char*)ps_alloc(kBufSize, AllocPref::PreferPsram, "sensor.status.json");
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
  doc["matrixCompiled"] = static_cast<bool>(ENABLE_LED_MATRIX);

#if ENABLE_CAMERA_SENSOR
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
    doc["sdAvailable"] = sdCardIsMountedForStatus();
    doc["sdWritable"]  = sdCardIsWritableForStatus();
  }

#if ENABLE_MICROPHONE
  const MicRecordingState micRecState = getMicRecordingState();
  doc["micRunning"] = gMicRunning;
  doc["micRecording"] = micRecState != MicRecordingState::IDLE;
  doc["micRecordingState"] = micRecordingStateName(micRecState);
  doc["micCompiled"] = true;
#else
  doc["micRunning"] = false;
  doc["micRecording"] = false;
  doc["micRecordingState"] = "idle";
  doc["micCompiled"] = false;
#endif

#if ENABLE_EDGE_IMPULSE
  doc["eiModelLoaded"] = isEdgeImpulseModelLoaded();
  doc["eiCompiled"] = true;
#else
  doc["eiModelLoaded"] = false;
  doc["eiCompiled"] = false;
#endif

  // Non-I2C sensors from registry (standardized format)
  // Key is "nonI2c", NOT "sensors" — `sensors json` (buildSensorsJson) already
  // uses the key "sensors" for an ARRAY of {id,name,kind,enabled,connected,data},
  // while this is a MAP keyed by id holding only {connected,enabled,task,mlModule}
  // for the non-I2C registry (camera, microphone). Two endpoints publishing
  // incompatible types under one key means no client can handle "sensors"
  // generically. Renamed rather than reshaped because this map had zero readers —
  // every consumer of /api/sensors/status reads the flat scalars (imuRunning,
  // inputCompiled, ...), verified across Dashboard/Sensors/Maps/Games.
  JsonObject sensors = doc["nonI2c"].to<JsonObject>();
  for (size_t i = 0; i < nonI2cSensorsCount; i++) {
    const NonI2cSensorEntry& s = nonI2cSensors[i];
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
  doc["i2cBusRunning"] = gI2cBusRunning;
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

static bool startSensorInternal(I2cDeviceType sensor, uint8_t n) {
  if (!driverServesInstance(sensor, n)) return false;  // only the encoder has instances n >= 2
  switch (sensor) {
    case I2C_DEVICE_THERMAL: return thermalStartInternal();
    case I2C_DEVICE_TOF: return tofStartInternal();
    case I2C_DEVICE_IMU: return imuStartInternal();
    case I2C_DEVICE_GPS: return gpsStartInternal();
#if ENABLE_GAMEPAD_SENSOR || ENABLE_ANO_ENCODER || ENABLE_GT911_TOUCH
    case I2C_DEVICE_INPUT: return inputStartInternal();
#endif
#if ENABLE_APDS_SENSOR
    case I2C_DEVICE_APDS: return apdsStartInternal();
#endif
#if ENABLE_FM_RADIO
    case I2C_DEVICE_FMRADIO: return fmRadioStartInternal();
#endif
#if ENABLE_RTC_SENSOR
    case I2C_DEVICE_RTC: return rtcStartInternal();
#endif
#if ENABLE_PRESENCE_SENSOR
    case I2C_DEVICE_PRESENCE: return presenceStartInternal();
#endif
    case I2C_DEVICE_ENCODER: return encoderStartInternal(n);  // false without the device
    default: return false;
  }
}

static void broadcastStartedSensor(I2cDeviceType sensor, uint8_t n) {
#if ENABLE_ESPNOW
  // The ESP-NOW wire carries instance 1 only (1.0).
  switch (n == 1 ? sensor : I2C_DEVICE_COUNT) {
    case I2C_DEVICE_THERMAL: sensorPresenceChanged(REMOTE_SENSOR_THERMAL, true); break;
    case I2C_DEVICE_TOF: sensorPresenceChanged(REMOTE_SENSOR_TOF, true); break;
    case I2C_DEVICE_IMU: sensorPresenceChanged(REMOTE_SENSOR_IMU, true); break;
    case I2C_DEVICE_INPUT: sensorPresenceChanged(REMOTE_SENSOR_INPUT, true); break;
    case I2C_DEVICE_GPS: sensorPresenceChanged(REMOTE_SENSOR_GPS, true); break;
    case I2C_DEVICE_FMRADIO: sensorPresenceChanged(REMOTE_SENSOR_FMRADIO, true); break;
    // RTC and presence are remote types too: their status frame lists them,
    // and a bond master leases only what its partner's list names, so a start
    // after the bond session's first list must be told as well.
    case I2C_DEVICE_RTC: sensorPresenceChanged(REMOTE_SENSOR_RTC, true); break;
    case I2C_DEVICE_PRESENCE: sensorPresenceChanged(REMOTE_SENSOR_PRESENCE, true); break;
    default: break;  // APDS has no remote sensor type
  }
#else
  (void)sensor;
  (void)n;
#endif
}

// Post the sensor lifecycle event + the durable [EVENT][SENSOR] line the old
// notifySensorStarted wrapper emitted — at boot this is the manifest of what
// came up vs what was configured-but-failed (Phase-1 cutover).
static void announceSensorStart(const char* name, bool ok) {
  systemEventPost(ok ? SYSEVT_SENSOR_STARTED : SYSEVT_SENSOR_START_FAILED, name);
  logSystemEvent("SENSOR", "%s %s", name, ok ? "online" : "start FAILED");
}

// --- Automatic restart: the worker's side (see the state near requestSensorStart) ---

// The boot rule: start-at-boot on as the RAM-flush resume resolved it. Read
// each time it matters, so `<x>autostart 0`, the setup wizard and a settings
// import all end a restart without a hook in any of them (close<x> ends it
// through sensorRestartClosedByHand).
static bool sensorRestartGateOpen(I2cDeviceType sensor, uint8_t n) {
  const SensorSwitchRow* row = sensorSwitchRow(sensor);
  // An instance n >= 2 without its own start-at-boot switch stays closed.
  if (n != 1) return gI2cBusRunning && row && sensorAutoStartFor(sensor, n);
  return gI2cBusRunning && row &&
         ramFlushResolve(ramFlushIdForDevice(sensor), sensorAutoStartFor(sensor, n));
}

// The worker settled a start. For a start the automatic restart queued, a
// repeated failure is only logged: one start-failed event (a toast) per
// episode, however many tries follow. Returns true when the worker should
// announce the outcome as usual.
static bool sensorRestartStartSettled(I2cDeviceType sensor, uint8_t n, bool started) {
  const uint8_t slot = i2cSlot(sensor, n);
  if (slot == kI2cNoSlot) return true;
  sensor_restart::State& state = sSensorRestart[slot];
  const bool ours = state.phase == sensor_restart::Phase::Starting;
  const bool announce = sensor_restart::startSettled(state, started, millis());
  if (!ours) return announce;
  sSensorRestartPending[slot].store(sensor_restart::pending(state));
  if (started) {
    // Running again: it is no longer "off because it failed" for a RAM flush.
    ramFlushClearAutostartFailed(sensor, n);
  } else if (!announce) {
    logSystemEvent("SENSOR", "%s automatic restart failed again, next try in %lu s",
                   deviceTypeDisplayName(sensor, n),
                   (unsigned long)(sensor_restart::delayForRung(state.rung) / 1000u));
  }
  return announce;
}

// A close by hand that found nothing to stop, because the worker queued the
// automatic start just after the close looked, still wins: the worker drops
// that start instead of running it, and its next tick ends the episode. (A
// close that arrives later finds the start outstanding and queues a stop,
// which the worker honours as for any start.)
static bool sensorRestartStartWithdrawn(I2cDeviceType sensor, uint8_t n) {
  const uint8_t slot = i2cSlot(sensor, n);
  if (slot == kI2cNoSlot) return false;
  return sSensorRestart[slot].phase == sensor_restart::Phase::Starting &&
         sSensorRestartByHand[slot].load() == kRestartHandClosed;
}

// One try, made when its wait is over. Goes through requestSensorStart(): the
// device is probed first, so an absent sensor costs one address probe and one
// log line, with no driver init, no event and no error on its health record.
static void sensorRestartAttempt(I2cDeviceType sensor, uint8_t n, sensor_restart::State& state,
                                 uint32_t nowMs) {
  SensorStartRefusal why = SensorStartRefusal::None;
  if (requestSensorStart(sensor, "restart@enqueue", &why, n)) {
    sensor_restart::attemptQueued(state, nowMs);  // The outcome is reported when the start settles.
    return;
  }
  // A used try: one line saying why and when the next one is due.
  auto logUsedTry = [&]() {
    logSystemEvent("SENSOR", "%s automatic restart: %s, next try in %lu s",
                   deviceTypeDisplayName(sensor, n), sensorStartRefusalText(why),
                   (unsigned long)(sensor_restart::delayForRung(state.rung) / 1000u));
  };
  switch (why) {
    case SensorStartRefusal::AlreadyRunning:
      // Started by someone else (the menu's input auto-start): watch it too.
      sensor_restart::attemptFoundRunning(state, nowMs);
      ramFlushClearAutostartFailed(sensor, n);
      break;
    case SensorStartRefusal::ProbeFailed:
      // The bus was busy (another sensor's long read) or the transfer failed.
      // Ask again on the next ticks; only a run of them counts as a used try.
      if (sensor_restart::attemptBusBusy(state, nowMs)) logUsedTry();
      break;
    case SensorStartRefusal::NoAck:
    case SensorStartRefusal::BusDisabled:
      sensor_restart::attemptRefused(state, nowMs);
      logUsedTry();
      break;
    case SensorStartRefusal::StopPending:
    case SensorStartRefusal::AlreadyQueued:
    case SensorStartRefusal::QueueUnavailable:
    case SensorStartRefusal::NotQueued:
    case SensorStartRefusal::None:
      break;  // Not possible yet. The wait stays due; the next tick asks again.
  }
}

// Runs on the queue worker whenever its queue is empty (every 100 ms); the
// body runs once a second.
static void sensorRestartTick(I2cDeviceManager* mgr) {
  const uint32_t nowMs = millis();
  if (sensor_restart::elapsedMs(nowMs, sSensorRestartLastTickMs) < 1000u) return;
  sSensorRestartLastTickMs = nowMs;

  // One ladder per instance slot (instance 1's slot is its type).
  for (uint8_t slot = 0; slot < kI2cSlotCount; ++slot) {
    I2cDeviceType sensor = I2C_DEVICE_COUNT;
    uint8_t n = 1;
    if (!i2cSlotToRef(slot, sensor, n)) continue;
    sensor_restart::State& state = sSensorRestart[slot];
    // Read order: an auto-disable site reports before it clears its running
    // flag, so "not running" read here means its report is already visible.
    const bool running = sensorIsRunning(sensor, n);
    const uint8_t byHand = sSensorRestartByHand[slot].exchange(kRestartHandNone);
    bool noted = sSensorRestartNoted[slot].exchange(false);
    // A close by hand wins over a report from the same second: the sensor
    // stays off. After an open by hand the report stands: the open voided any
    // earlier one, so this one is about the sensor that open started.
    if (byHand == kRestartHandClosed) noted = false;
    if (state.phase == sensor_restart::Phase::Idle && !noted) continue;

    const char* name = deviceTypeDisplayName(sensor, n);
    const bool gateOpen = sensorRestartGateOpen(sensor, n);
    if (byHand != kRestartHandNone && state.phase != sensor_restart::Phase::Idle) {
      // A close or open by hand ends the episode that was running.
      sensor_restart::cancel(state);
      ramFlushClearAutostartFailed(sensor, n);
      logSystemEvent("SENSOR", "%s automatic restart ended: opened or closed by hand", name);
    }
    if (noted && gateOpen && sensor_restart::noteAutoDisabled(state, nowMs)) {
      // (With the gate closed the sensor is not set to start by itself: it
      // stays off, as before, and nothing is logged.)
      // Off because it failed, not because the user closed it: a RAM flush
      // taken now must not record it as switched off (as at boot).
      ramFlushMarkAutostartFailed(sensor, n);
      logSystemEvent("SENSOR", "%s automatic restart: next try in %lu s", name,
                     (unsigned long)(sensor_restart::delayForRung(state.rung) / 1000u));
    }

    const bool startPending = state.phase == sensor_restart::Phase::Starting &&
                              (sensorStartOutstanding(slot).load() || mgr->isInQueue(sensor, n));
    switch (sensor_restart::tick(state, nowMs, gateOpen, startPending, running)) {
      case sensor_restart::Tick::Attempt:
        sensorRestartAttempt(sensor, n, state, nowMs);
        break;
      case sensor_restart::Tick::GateClosed:
        ramFlushClearAutostartFailed(sensor, n);
        logSystemEvent("SENSOR", "%s automatic restart ended: auto-start off", name);
        break;
      case sensor_restart::Tick::Stopped:
        ramFlushClearAutostartFailed(sensor, n);
        break;
      case sensor_restart::Tick::RunProven:
      case sensor_restart::Tick::None:
        break;
    }
    sSensorRestartPending[slot].store(sensor_restart::pending(state));
  }
}

void sensorQueueProcessorTask(void* param) {
  DEBUG_CLIF("[QUEUE] Queue processor task started");
  static unsigned long lastSensorStartTime = 0;
  static I2cDeviceType lastI2cDeviceType = (I2cDeviceType)-1;
  
  I2cDeviceManager* mgr = I2cDeviceManager::getInstance();
  if (!mgr) {
    DEBUG_CLIF("[QUEUE] FATAL: I2C manager not initialized");
    vTaskDelete(nullptr);
    return;
  }

  while (true) {
    finishSensorStops();
    I2cDeviceRequest req;
    if (mgr->dequeueDeviceRequest(&req)) {
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
      finishSensorStops();
      DEBUG_CLIF("[QUEUE] Processing queued sensor: type=%d, queuedAt=%lu",
                 req.device, req.queuedAt);

      if (req.action == I2cDeviceRequestAction::Stop) {
        applySensorStop(req.device, req.n);
        continue;
      }
      // An admitted request always names an instance its type has.
      const uint8_t slot = i2cSlot(req.device, req.n);
      if (slot == kI2cNoSlot) continue;
      if (mgr->hasPendingDeviceStop(req.device, req.n)) continue;

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
      if (lastI2cDeviceType != (I2cDeviceType)-1) {
        switch (lastI2cDeviceType) {
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
          default: break;
        }
      }

      // Wait if a sensor was recently started (to let it complete init)
      if (lastSensorStartTime > 0 && requiredDelay > 0) {
        unsigned long elapsed = millis() - lastSensorStartTime;
        if (elapsed < requiredDelay) {
          unsigned long waitTime = requiredDelay - elapsed;
          DEBUG_CLIF("[QUEUE] Waiting for sensor (type=%d) to initialize", (int)lastI2cDeviceType);
          vTaskDelay(pdMS_TO_TICKS(waitTime));
        } else {
          DEBUG_CLIF("[QUEUE] Last sensor (type=%d) initialized - proceeding with sensor (type=%d)",
                     (int)lastI2cDeviceType, (int)req.device);
        }
      }

      // A close may arrive after dequeue or while waiting for the previous
      // device. Do not initialize a request that has since been cancelled.
      if (mgr->hasPendingDeviceStop(req.device, req.n)) continue;
      if (sensorRestartStartWithdrawn(req.device, req.n)) {
        sensorStartOutstanding(slot) = false;  // Settled: closed by hand.
        continue;
      }
      if (sensorIsRunning(req.device, req.n)) {
        sensorStartOutstanding(slot) = false;  // Settled: nothing to start.
        sensorStatusBumpWith("queue@already_running");
        continue;
      }
      if (sensorHasTask(req.device, req.n)) {
        sensorStartOutstanding(slot) = false;
        if (sensorRestartStartSettled(req.device, req.n, false)) {
          BROADCAST_PRINTF("Error: [%s] Previous task is still cleaning up; retry open shortly",
                           deviceTypeDisplayName(req.device, req.n));
          announceSensorStart(deviceTypeDisplayName(req.device, req.n), false);
        }
        continue;
      }

      finishSensorStops();  // Publish any old stop before announcing a fresh start.
      const uint8_t devAddr = i2cAddressForDeviceType(req.device, req.n);
      if (devAddr != 0) i2cResetGracePeriod(devAddr, i2cBusForDeviceType(req.device, req.n));

      const bool started = startSensorInternal(req.device, req.n);
      // An in-flight initialization cannot be interrupted safely. A close
      // received during it is the next request processed; suppress stale
      // success output and let that stop trigger owner-task cleanup.
      if (!mgr->hasPendingDeviceStop(req.device, req.n)) {
        sensorStartOutstanding(slot) = false;  // Settled by its outcome.
        // A repeated failure of an automatic restart is logged, not announced.
        if (sensorRestartStartSettled(req.device, req.n, started)) {
          announceSensorStart(deviceTypeDisplayName(req.device, req.n), started);
        }
        if (started) broadcastStartedSensor(req.device, req.n);
      }

      if (isDebugFlagSet(DEBUG_MEMORY)) {
        UBaseType_t hwmBytes = uxTaskGetStackHighWaterMark(NULL);
        INFO_MEMORYF("[STACK][QUEUE] after  start type=%d hwm=%u bytes",
                      (int)req.device, (unsigned)hwmBytes);
      }

      lastSensorStartTime = millis();
      lastI2cDeviceType = req.device;  // Track this sensor for next iteration's delay

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

      } while (mgr->dequeueDeviceRequest(&req)); // drain entire batch

      // Resume sensor polling after ALL queued sensors are initialized.
      pollResume();
      INFO_I2C_AUTOSTARTF("Resumed sensor polling after initialization batch");
    } else {
      // Queue empty: see to sensors that switched themselves off, then sleep.
      sensorRestartTick(mgr);
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
  // Per-device bus rows. The <sensor>bus COMMANDS now live in each sensor's own
  // (build-gated) .cpp, so these rows are guarded on the same flags — a row whose
  // cmdKey names a command that is not compiled would leave the OLED settings
  // editor firing an unknown command (setSettingValue routes through cmdKey with
  // no fallback). Rows stay in THIS table rather than moving to the sensor files
  // so their JSON identity — and therefore the persisted settings keys — is
  // unchanged.
  { "oledBus",     SETTING_INT, &gSettings.oledBus,     OLED_BUS_DEFAULT, 0, nullptr, 0, 1, "OLED bus (reboot required)",            "0|I2C1,1|I2C2", false, nullptr, "oledbus" },
  { "inputBus",    SETTING_INT, &gSettings.inputBus,    0, 0, nullptr, 0, 1, "Input device bus (reboot required)",     "0|I2C1,1|I2C2", false, nullptr, "inputbus" },
#if ENABLE_GPS_SENSOR
  { "gpsBus",      SETTING_INT, &gSettings.gpsBus,      0, 0, nullptr, 0, 1, "GPS bus (reboot required)",             "0|I2C1,1|I2C2", false, nullptr, "gpsbus" },
#endif
#if ENABLE_RTC_SENSOR
  { "rtcBus",      SETTING_INT, &gSettings.rtcBus,      0, 0, nullptr, 0, 1, "RTC bus (reboot required)",             "0|I2C1,1|I2C2", false, nullptr, "rtcbus" },
#endif
#if ENABLE_FM_RADIO
  { "fmRadioBus",  SETTING_INT, &gSettings.fmRadioBus,  0, 0, nullptr, 0, 1, "FM radio bus (reboot required)",        "0|I2C1,1|I2C2", false, nullptr, "fmradiobus" },
#endif
#if ENABLE_PRESENCE_SENSOR
  { "presenceBus", SETTING_INT, &gSettings.presenceBus, 0, 0, nullptr, 0, 1, "Presence bus (reboot required)",        "0|I2C1,1|I2C2", false, nullptr, "presencebus" },
#endif
#if ENABLE_IMU_SENSOR
  { "imuBus",      SETTING_INT, &gSettings.imuBus,      0, 0, nullptr, 0, 1, "IMU bus (reboot required)",             "0|I2C1,1|I2C2", false, nullptr, "imubus" },
#endif
#if ENABLE_THERMAL_SENSOR
  { "thermalBus",  SETTING_INT, &gSettings.thermalBus,  0, 0, nullptr, 0, 1, "Thermal bus (reboot required)",         "0|I2C1,1|I2C2", false, nullptr, "thermalbus" },
#endif
#if ENABLE_TOF_SENSOR
  { "tofBus",      SETTING_INT, &gSettings.tofBus,      0, 0, nullptr, 0, 1, "ToF bus (reboot required)",             "0|I2C1,1|I2C2", false, nullptr, "tofbus" },
#endif
#if ENABLE_APDS_SENSOR
  { "apdsBus",     SETTING_INT, &gSettings.apdsBus,     0, 0, nullptr, 0, 1, "APDS bus (reboot required)",            "0|I2C1,1|I2C2", false, nullptr, "apdsbus" },
#endif
#if ENABLE_SERVO
  { "servoBus",    SETTING_INT, &gSettings.servoBus,    0, 0, nullptr, 0, 1, "Servo bus (reboot required)",           "0|I2C1,1|I2C2", false, nullptr, "servobus" },
#endif
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

// Instance n of a type was found by the last scan at its own (address, bus).
// isSensorConnected() answers for the type (any board of it found), so it
// cannot tell a second instance from the first.
static bool isInstancePresent(I2cDeviceType deviceType, uint8_t n) {
  const uint8_t addr = i2cAddressForDeviceType(deviceType, n);
  if (addr == 0) return false;
  const uint8_t bus = i2cBusForDeviceType(deviceType, n);
  for (int i = 0; i < connectedDeviceCount; i++) {
    if (connectedDevices[i].isConnected && connectedDevices[i].address == addr &&
        connectedDevices[i].bus == bus) {
      return true;
    }
  }
  return false;
}

// Check if a sensor is available for auto-start:
// 1. Fast path: check connectedDevices[] registry (populated by discoverI2cDevices)
// 2. Fallback: live I2C ping in case the scan missed the device (transient bus issue)
// Instance 1 asks the registry by its module name as before; an instance
// n >= 2 by its own (address, bus).
static bool isSensorAvailableForAutoStart(I2cDeviceType deviceType, uint8_t n = 1) {
  const char* moduleName = i2cModuleNameForDeviceType(deviceType);
  if (!moduleName) moduleName = "?";
  if (n == 1 ? isSensorConnected(moduleName) : isInstancePresent(deviceType, n)) return true;
  // The id the lines below name: the module name, or "encoder2" for instance 2.
  char instanceId[16];
  const char* label = moduleName;
  if (n != 1) label = i2cInstanceId(deviceType, n, instanceId, sizeof(instanceId));
  // Registry miss — try a direct I2C ping as fallback
  uint8_t addr = i2cAddressForDeviceType(deviceType, n);
  const uint8_t bus = i2cBusForDeviceType(deviceType, n);
  if (addr != 0 && i2cPingAddress(addr, 100000, 50, bus)) {
    INFO_I2C_AUTOSTARTF("[AutoStart] %s not in registry but responds to I2C ping", label);
    return true;
  }
  // Reached only when the sensor's autostart flag is set (every caller gates on
  // it), so a false return here means "enabled in settings but not detected" —
  // durable so a silently-absent sensor is attributable after reboot.
  logSystemEvent("SENSOR", "%s autostart skipped: enabled in settings but not detected on I2C bus", label);
  systemEventPost(SYSEVT_SENSOR_START_FAILED, n == 1 ? moduleName : deviceTypeDisplayName(deviceType, n),
                  "not detected on bus");

  // Tell ramflush this sensor is off because it wasn't there, not because the user
  // turned it off. Without this, an unplugged sensor reads live=false against
  // intent=true and the next capture records "user turned it off" — which would
  // suppress the configured autostart on every later boot, with replugging the
  // sensor doing nothing to undo it. Per instance: a missing second board does
  // not mark the first as failed.
  ramFlushMarkAutostartFailed(deviceType, n);
  return false;
}

// Queue an auto-start. The ring holds one start per instance slot, so a
// refusal means a stop is pending or the manager is gone; it is reported like
// a sensor that was not found (one log line, a start-failed event, and the
// RAM-flush mark, so a capture does not read it as switched off).
static bool autoStartEnqueue(I2cDeviceType deviceType, uint8_t n) {
  if (enqueueDeviceStart(deviceType, n)) return true;
  const char* name = deviceTypeDisplayName(deviceType, n);
  ERROR_I2CF("[AutoStart] Could not queue %s (queue unavailable/full or stop pending)", name);
  logSystemEvent("SENSOR", "%s autostart skipped: could not queue the start", name);
  systemEventPost(SYSEVT_SENSOR_START_FAILED, name, "start not queued");
  ramFlushMarkAutostartFailed(deviceType, n);
  return false;
}

void processAutoStartSensors() {
  // Debug: Print I2C flags to diagnose auto-start issues
  DEBUG_I2C_AUTOSTARTF("[AutoStart] I2C check: i2cBus=%d", gSettings.i2cEnabled ? 1 : 0);
  
  if (!gSettings.i2cEnabled) {
    INFO_I2C_AUTOSTARTF("[AutoStart] I2C bus disabled, skipping sensor auto-start");
    return;
  }

 #if ENABLE_I2C_SENSOR_QUEUE
  if (!queueProcessorTask) {
    const uint32_t queueStackWords = SENSOR_QUEUE_STACK_WORDS;
    // Pin to Core 1 (I2C_SENSOR_CORE) — same rationale as the primary create site
    // in HardwareOne.cpp: this task runs the I2C device-init transactions and must
    // not float onto Wi-Fi-saturated Core 0 (starve → bus-storm → panic(4)).
    if (xTaskCreateLogged(sensorQueueProcessorTask, "sensor_queue", queueStackWords, nullptr, TASK_PRIORITY_LOW, &queueProcessorTask, "sensor.queue", I2C_SENSOR_CORE) != pdPASS) {
      ERROR_I2CF("[I2C_SENSORS] Failed to create sensor queue processor task (late init)");
      queueProcessorTask = nullptr;
      return;
    }
    INFO_I2C_DISCOVERYF("[I2C_SENSORS] Queue processor task created (late init)");
  }
 #endif
  
  INFO_I2C_AUTOSTARTF("[AutoStart] Flags: thermal=%d tof=%d imu=%d gps=%d fmradio=%d apds=%d gamepad=%d rtc=%d presence=%d encoder=%d",
            gSettings.thermalAutoStart ? 1 : 0,
            gSettings.tofAutoStart ? 1 : 0,
            gSettings.imuAutoStart ? 1 : 0,
            gSettings.gpsAutoStart ? 1 : 0,
            gSettings.fmRadioAutoStart ? 1 : 0,
            gSettings.apdsAutoStart ? 1 : 0,
            gSettings.inputAutoStart ? 1 : 0,
            gSettings.rtcAutoStart ? 1 : 0,
            gSettings.presenceAutoStart ? 1 : 0,
            encoderAutoStartOf(1) ? 1 : 0);
  INFO_I2C_AUTOSTARTF("[AutoStart] Processing sensor auto-start settings...");
  int autoStartQueued = 0;
  
  #if ENABLE_THERMAL_SENSOR
  if (ramFlushResolve(RF_THERMAL, gSettings.thermalAutoStart)) {
    if (isSensorAvailableForAutoStart(I2C_DEVICE_THERMAL)) {
      INFO_I2C_AUTOSTARTF("[AutoStart] Queuing thermal sensor");
      if (autoStartEnqueue(I2C_DEVICE_THERMAL, 1)) autoStartQueued++;
    } else {
      INFO_I2C_AUTOSTARTF("[AutoStart] Skipping thermal sensor (not detected on I2C bus)");
    }
  }
  #endif
  
  #if ENABLE_TOF_SENSOR
  if (ramFlushResolve(RF_TOF, gSettings.tofAutoStart)) {
    if (isSensorAvailableForAutoStart(I2C_DEVICE_TOF)) {
      INFO_I2C_AUTOSTARTF("[AutoStart] Queuing ToF sensor");
      if (autoStartEnqueue(I2C_DEVICE_TOF, 1)) autoStartQueued++;
    } else {
      INFO_I2C_AUTOSTARTF("[AutoStart] Skipping ToF sensor (not detected on I2C bus)");
    }
  }
  #endif
  
  #if ENABLE_IMU_SENSOR
  if (ramFlushResolve(RF_IMU, gSettings.imuAutoStart)) {
    if (isSensorAvailableForAutoStart(I2C_DEVICE_IMU)) {
      INFO_I2C_AUTOSTARTF("[AutoStart] Queuing IMU sensor");
      if (autoStartEnqueue(I2C_DEVICE_IMU, 1)) autoStartQueued++;
    } else {
      INFO_I2C_AUTOSTARTF("[AutoStart] Skipping IMU sensor (not detected on I2C bus)");
    }
  }
  #endif
  
  #if ENABLE_GPS_SENSOR
  if (ramFlushResolve(RF_GPS, gSettings.gpsAutoStart)) {
    if (isSensorAvailableForAutoStart(I2C_DEVICE_GPS)) {
      INFO_I2C_AUTOSTARTF("[AutoStart] Queuing GPS sensor");
      if (autoStartEnqueue(I2C_DEVICE_GPS, 1)) autoStartQueued++;
    } else {
      INFO_I2C_AUTOSTARTF("[AutoStart] Skipping GPS sensor (not detected on I2C bus)");
    }
  }
  #endif
  
  #if ENABLE_FM_RADIO
  if (ramFlushResolve(RF_FMRADIO, gSettings.fmRadioAutoStart)) {
    if (isSensorAvailableForAutoStart(I2C_DEVICE_FMRADIO)) {
      INFO_I2C_AUTOSTARTF("[AutoStart] Queuing FM Radio sensor");
      if (autoStartEnqueue(I2C_DEVICE_FMRADIO, 1)) autoStartQueued++;
    } else {
      INFO_I2C_AUTOSTARTF("[AutoStart] Skipping FM Radio sensor (not detected on I2C bus)");
    }
  }
  #endif
  
  #if ENABLE_APDS_SENSOR
  if (ramFlushResolve(RF_APDS, gSettings.apdsAutoStart)) {
    if (isSensorAvailableForAutoStart(I2C_DEVICE_APDS)) {
      INFO_I2C_AUTOSTARTF("[AutoStart] Queuing APDS sensor");
      if (autoStartEnqueue(I2C_DEVICE_APDS, 1)) autoStartQueued++;
    } else {
      INFO_I2C_AUTOSTARTF("[AutoStart] Skipping APDS sensor (not detected on I2C bus)");
    }
  }
  #endif
  
  #if ENABLE_GAMEPAD_SENSOR || ENABLE_GT911_TOUCH
  if (ramFlushResolve(RF_INPUT, gSettings.inputAutoStart)) {
    // The device type is the identity. Its module name comes from its
    // i2cSensors[] row, so the registry lookup and the ramflush id cannot
    // disagree. (A hand-typed "gamepad" here once missed the registry, fell
    // through to the raw 0x50 ping and poisoned the ramflush bit for a
    // gamepad on the alt address 0x49.)
    if (isSensorAvailableForAutoStart(I2C_DEVICE_INPUT)) {
      INFO_I2C_AUTOSTARTF("[AutoStart] Queuing Gamepad sensor");
      if (autoStartEnqueue(I2C_DEVICE_INPUT, 1)) autoStartQueued++;
    } else {
      INFO_I2C_AUTOSTARTF("[AutoStart] Skipping Gamepad sensor (not detected on I2C bus)");
    }
  }
  #endif
  
  #if ENABLE_RTC_SENSOR
  if (ramFlushResolve(RF_RTC, gSettings.rtcAutoStart)) {
    if (isSensorAvailableForAutoStart(I2C_DEVICE_RTC)) {
      INFO_I2C_AUTOSTARTF("[AutoStart] Queuing RTC sensor");
      if (autoStartEnqueue(I2C_DEVICE_RTC, 1)) autoStartQueued++;
    } else {
      INFO_I2C_AUTOSTARTF("[AutoStart] Skipping RTC sensor (not detected on I2C bus)");
    }
  }
  #endif
  
  #if ENABLE_PRESENCE_SENSOR
  if (ramFlushResolve(RF_PRESENCE, gSettings.presenceAutoStart)) {
    if (isSensorAvailableForAutoStart(I2C_DEVICE_PRESENCE)) {
      INFO_I2C_AUTOSTARTF("[AutoStart] Queuing Presence sensor");
      if (autoStartEnqueue(I2C_DEVICE_PRESENCE, 1)) autoStartQueued++;
    } else {
      INFO_I2C_AUTOSTARTF("[AutoStart] Skipping Presence sensor (not detected on I2C bus)");
    }
  }
  #endif

  #if ENABLE_ENCODER
  if (ramFlushResolve(RF_ENCODER, gEncoderInst[0].autoStart)) {
    // Missing at boot: one "autostart skipped" line and boot goes on; an
    // LED command to the encoder target reconnects it later.
    if (isSensorAvailableForAutoStart(I2C_DEVICE_ENCODER)) {
      INFO_I2C_AUTOSTARTF("[AutoStart] Queuing encoder board");
      if (autoStartEnqueue(I2C_DEVICE_ENCODER, 1)) autoStartQueued++;
    } else {
      INFO_I2C_AUTOSTARTF("[AutoStart] Skipping encoder board (not detected on I2C bus)");
    }
  }
  #endif
  
  // Instances 2..max of the types that have more than one (System_I2CInstanceCore.h).
  // Instance 1 of each was handled above; each extra instance has its own
  // start-at-boot switch, presence check and failure mark.
  for (uint8_t slot = I2C_DEVICE_COUNT; slot < kI2cSlotCount; ++slot) {
    I2cDeviceType type = I2C_DEVICE_COUNT;
    uint8_t n = 1;
    if (!i2cSlotToRef(slot, type, n)) continue;
    if (!ramFlushResolve(ramFlushIdForDevice(type), sensorAutoStartFor(type, n))) continue;
    if (i2cAddressForDeviceType(type, n) == 0) {
      // Start-at-boot on, but no address set: nothing to look for.
      INFO_I2C_AUTOSTARTF("[AutoStart] Skipping %s (no address set)", deviceTypeDisplayName(type, n));
      continue;
    }
    if (isSensorAvailableForAutoStart(type, n)) {
      INFO_I2C_AUTOSTARTF("[AutoStart] Queuing %s", deviceTypeDisplayName(type, n));
      if (autoStartEnqueue(type, n)) autoStartQueued++;
    } else {
      INFO_I2C_AUTOSTARTF("[AutoStart] Skipping %s (not detected on I2C bus)", deviceTypeDisplayName(type, n));
    }
  }

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
