#ifndef I2CSENSOR_VL53L4CX_H
#define I2CSENSOR_VL53L4CX_H

#include "System_BuildConfig.h"

#if ENABLE_TOF_SENSOR

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "System_Command.h"

// VL53L4CX library constants (define if not already defined by library)
#ifndef VL53L4CX_MAX_NB_OF_OBJECTS_PER_ROI
#define VL53L4CX_MAX_NB_OF_OBJECTS_PER_ROI 4  // Maximum number of objects per region of interest
#endif

// Forward declarations
class String;
class VL53L4CX;

// VL53L4CX ToF sensor object (defined in tof_sensor.cpp)
extern VL53L4CX* gVL53L4CX;

// ToF sensor cache (distance sensing, 4Hz updates)
struct TofCache {
  SemaphoreHandle_t mutex = nullptr;
  struct TofObject {
    bool detected = false;
    bool valid = false;
    int distance_mm = 0;
    float distance_cm = 0.0;
    int status = 0;
    float smoothed_distance_mm = 0.0;
    float smoothed_distance_cm = 0.0;
    bool hasHistory = false;
  } tofObjects[4];
  int tofTotalObjects = 0;
  unsigned long tofLastUpdate = 0;
  bool tofDataValid = false;
  uint32_t tofSeq = 0;
};

// Global ToF cache (defined in tof_sensor.cpp)
extern TofCache gTofCache;

// ToF watermark tracking
extern volatile UBaseType_t gTofWatermarkMin;
extern volatile UBaseType_t gTofWatermarkNow;

// ToF sensor command handlers
const char* cmd_tof(const String& cmd);
const char* cmd_tofstart(const String& cmd);
const char* cmd_tofstop(const String& cmd);
const char* cmd_toftransitionms(const String& cmd);
const char* cmd_tofuimaxdistancemm(const String& cmd);
const char* cmd_tofpollingms(const String& cmd);
const char* cmd_tofstabilitythreshold(const String& cmd);
const char* cmd_tofdevicepollms(const String& cmd);

// ToF sensor state and control
extern bool tofEnabled;
extern bool tofConnected;
extern uint32_t tofLastStopTime;
extern TaskHandle_t tofTaskHandle;

// Internal function called by queue processor
bool startToFSensorInternal();

// ToF sensor functions
bool initToFSensor();
bool readToFObjects();

// JSON building
int buildToFDataJSON(char* buf, size_t bufSize);

// ToF command registry (for help system compatibility)
extern const CommandEntry tofCommands[];
extern const size_t tofCommandsCount;

// ToF command registration (sensor-specific naming)
void registerTofVl53l4cxCommands();

#endif // ENABLE_TOF_SENSOR
#endif // TOF_SENSOR_H
