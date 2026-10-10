#ifndef I2CSENSOR_VL53L4CX_H
#define I2CSENSOR_VL53L4CX_H

#include "System_BuildConfig.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stddef.h>

// ToF sensor cache structure (always available for type-safe references)
struct TofCache {
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

// Values only: callers own their copy; the live cache and lock stay in the driver.
#if ENABLE_TOF_SENSOR

struct CommandEntry;

#ifndef VL53L4CX_MAX_NB_OF_OBJECTS_PER_ROI
#define VL53L4CX_MAX_NB_OF_OBJECTS_PER_ROI 4
#endif

// Forward declarations
class String;

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
// Owns its bus transaction, then publishes outside the bus lock. True means
// successful bus access, not necessarily a new sample; inspect the cache for
// freshness. Called by the owner task, which retains the driver until return.
bool tofPoll();

// JSON building
int tofBuildDataJSON(char* buf, size_t bufSize);

// ToF command registry (for help system compatibility)
extern const CommandEntry tofCommands[];
extern const size_t tofCommandsCount;

// ToF command registration (sensor-specific naming)
void registerTofVl53l4cxCommands();

// Copy one reading; false clears out when uninitialized or the lock is busy.
// Success only means a copy was available; check tofDataValid where required.
bool tofCacheSnapshot(TofCache& out, uint32_t timeoutMs = 10);
// Static cache data plus its mutex handle, for memory attribution (not RTOS allocation).
size_t tofCacheStorageBytes();

#else
inline bool tofCacheSnapshot(TofCache& out, uint32_t timeoutMs = 10) {
  (void)timeoutMs;
  out = {};
  return false;
}
inline int tofBuildDataJSON(char* buf, size_t bufSize) {
  if (buf && bufSize) buf[0] = '\0';
  return 0;
}
inline size_t tofCacheStorageBytes() { return 0; }
#endif // ENABLE_TOF_SENSOR
#endif // TOF_SENSOR_H
