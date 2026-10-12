#ifndef I2CSENSOR_APDS9960_H
#define I2CSENSOR_APDS9960_H

#include "System_BuildConfig.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stddef.h>

// APDS sensor cache structure (always available for type-safe references)
struct APDSCache {
  uint16_t apdsRed = 0, apdsGreen = 0, apdsBlue = 0, apdsClear = 0;
  uint8_t apdsProximity = 0;
  uint8_t apdsGesture = 0;
  unsigned long apdsLastUpdate = 0;
  bool apdsDataValid = false;
  // Which fields the polling task actually sampled in this cache entry.
  bool apdsColorValid = false;
  bool apdsProximityValid = false;
  bool apdsGestureValid = false;
  // Last non-zero swipe and when it was sampled; cleared when a session ends
  // or starts, so `apdsgesture` never reports a previous session's swipe.
  uint8_t apdsLastGesture = 0;
  unsigned long apdsLastGestureMs = 0;
};

// Values only: callers own their copy; the live cache and lock stay in the driver.
#if ENABLE_APDS_SENSOR

// Forward declarations
class String;

extern bool gApdsColorRunning;
extern bool gApdsProximityRunning;
extern bool gApdsGestureRunning;

// Shared sensor-data JSON builder (apdsread json / sensors json).
int apdsBuildDataJSON(char* buf, size_t bufSize);
extern bool gApdsConnected;
extern unsigned long gApdsLastStopTime;
extern TaskHandle_t gApdsTaskHandle;

// Command handlers
const char* cmd_apdscolor(const String& argsInput);
const char* cmd_apdsproximity(const String& argsInput);
const char* cmd_apdsgesture(const String& argsInput);
const char* cmd_apdsstart(const String& argsInput);
const char* cmd_apdsstop(const String& argsInput);
const char* cmd_apdsmode(const String& argsInput);


// APDS sensor functions
bool apdsStartInternal();  // Called by queue processor
bool apdsInit();

// Accessor functions (for MQTT and other modules)
uint8_t apdsGetProximity();
uint16_t apdsGetColorR();
uint16_t apdsGetColorG();
uint16_t apdsGetColorB();
uint16_t apdsGetColorC();
extern bool gApdsRunning;

// Command registry (for System_CommandRegistry.cpp module list)
struct CommandEntry;
extern const CommandEntry apdsCommands[];
extern const size_t apdsCommandsCount;

// Copy one reading; false clears out when uninitialized or the lock is busy.
// Success only means a copy was available; check apdsDataValid where required.
bool apdsCacheSnapshot(APDSCache& out, uint32_t timeoutMs = 10);
// Static cache data plus its mutex handle, for memory attribution (not RTOS allocation).
size_t apdsCacheStorageBytes();

#else
inline bool apdsCacheSnapshot(APDSCache& out, uint32_t timeoutMs = 10) {
  (void)timeoutMs;
  out = {};
  return false;
}
inline int apdsBuildDataJSON(char* buf, size_t bufSize) {
  if (buf && bufSize) buf[0] = '\0';
  return 0;
}
inline size_t apdsCacheStorageBytes() { return 0; }
#endif // ENABLE_APDS_SENSOR
#endif // APDS_SENSOR_H
