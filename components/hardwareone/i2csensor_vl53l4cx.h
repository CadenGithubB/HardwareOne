#ifndef I2CSENSOR_VL53L4CX_H
#define I2CSENSOR_VL53L4CX_H

#include "System_BuildConfig.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ToF sensor cache structure (always available for type-safe references)
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

#if ENABLE_TOF_SENSOR

#include "System_Command.h"

#ifndef VL53L4CX_MAX_NB_OF_OBJECTS_PER_ROI
#define VL53L4CX_MAX_NB_OF_OBJECTS_PER_ROI 4
#endif

// Forward declarations
class String;
class VL53L4CX;

extern VL53L4CX* gVL53L4CX;
extern TofCache gTofCache;

// ToF watermark tracking
extern volatile UBaseType_t gTofWatermarkMin;
extern volatile UBaseType_t gTofWatermarkNow;

// ToF sensor command handlers
const char* cmd_tof(const String& argsInput);
const char* cmd_tofstart(const String& argsInput);
const char* cmd_tofstop(const String& argsInput);
const char* cmd_toftransitionms(const String& argsInput);
const char* cmd_tofmaxdistancemm(const String& argsInput);
const char* cmd_tofpollingms(const String& argsInput);
const char* cmd_tofstabilitythreshold(const String& argsInput);
const char* cmd_tofdevicepollms(const String& argsInput);

// ToF sensor state and control
extern bool gTofRunning;
extern bool gTofConnected;
extern uint32_t gTofLastStopTime;
extern TaskHandle_t gTofTaskHandle;

// Internal function called by queue processor
bool tofStartInternal();

// ToF sensor functions
bool tofInit();
bool tofPoll();

// JSON building
int tofBuildDataJSON(char* buf, size_t bufSize);

// ToF command registry (for help system compatibility)
extern const CommandEntry tofCommands[];
extern const size_t tofCommandsCount;

// ToF command registration (sensor-specific naming)
void registerTofVl53l4cxCommands();

#endif // ENABLE_TOF_SENSOR
#endif // TOF_SENSOR_H
