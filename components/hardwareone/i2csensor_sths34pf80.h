#ifndef I2CSENSOR_STHS34PF80_H
#define I2CSENSOR_STHS34PF80_H

#include "System_BuildConfig.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// A copied STHS34PF80 reading. The live cache and its lock stay in the owner.
// Preserve the existing register scaling here; callers do not apply it again.
struct PresenceCache {
  float ambientTemp = 0.0f;       // Displayed Celsius: ambient register / 100
  int16_t objectTemp = 0;         // Raw object register counts
  float compObjectTemp = 0.0f;    // Compensated object register / 100
  int16_t presenceValue = 0;      // Raw presence signal counts
  int16_t motionValue = 0;        // Raw motion signal counts
  int16_t tempShockValue = 0;     // Raw ambient-shock signal counts
  bool presenceDetected = false;
  bool motionDetected = false;
  bool tempShockDetected = false;
  unsigned long lastUpdate = 0;  // millis() at the latest reading publication
  bool dataValid = false;        // A reading was published in this active run
};

#if ENABLE_PRESENCE_SENSOR

// Forward declarations
class String;

extern bool gPresenceRunning;
extern bool gPresenceConnected;
extern unsigned long gPresenceLastStopTime;
extern TaskHandle_t gPresenceTaskHandle;

// Command handlers
const char* cmd_presencestart(const String& argsInput);
const char* cmd_presencestop(const String& argsInput);
const char* cmd_presenceread(const String& argsInput);
const char* cmd_presencestatus(const String& argsInput);

// Presence sensor functions
bool presenceStartInternal();  // Called by queue processor
bool presenceInit();
bool presencePoll();

// Copy a consistent reading, waiting at most timeoutMs for the cache lock.
// Success means the copy was available; dataValid is a separate reading flag.
// Until the cache exists or on timeout, returns false and clears out.
bool presenceCacheSnapshot(PresenceCache& out, uint32_t timeoutMs = 10);

// Build the existing sensor envelope from one snapshot, formatting after the
// lock is released. A 50 ms lock timeout produces a not-ready envelope.
// Returns bytes written (excluding NUL), or 0 for an invalid/insufficient buffer.
int presenceBuildDataJSON(char* buf, size_t bufSize);

// Command registry (for System_CommandRegistry.cpp module list)
struct CommandEntry;
extern const CommandEntry presenceCommands[];
extern const size_t presenceCommandsCount;

#else

inline bool presenceCacheSnapshot(PresenceCache& out, uint32_t timeoutMs = 10) {
  (void)timeoutMs;
  out = {};
  return false;
}

inline int presenceBuildDataJSON(char* buf, size_t bufSize) {
  if (buf && bufSize) buf[0] = '\0';
  return 0;
}

#endif // ENABLE_PRESENCE_SENSOR
#endif // I2CSENSOR_STHS34PF80_H
