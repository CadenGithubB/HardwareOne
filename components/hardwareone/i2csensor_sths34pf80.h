#ifndef I2CSENSOR_STHS34PF80_H
#define I2CSENSOR_STHS34PF80_H

#include "System_BuildConfig.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Presence sensor cache structure (always available for type-safe references)
struct PresenceCache {
  SemaphoreHandle_t mutex = nullptr;
  float ambientTemp = 0.0f;
  int16_t objectTemp = 0;
  float compObjectTemp = 0.0f;
  int16_t presenceValue = 0;
  int16_t motionValue = 0;
  int16_t tempShockValue = 0;
  bool presenceDetected = false;
  bool motionDetected = false;
  bool tempShockDetected = false;
  unsigned long lastUpdate = 0;
  bool dataValid = false;
};

#if ENABLE_PRESENCE_SENSOR

// Forward declarations
class String;

extern PresenceCache gPresenceCache;
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

// JSON building for ESP-NOW streaming
int presenceBuildDataJSON(char* buf, size_t bufSize);

// Command registry (for system_utils.cpp module list)
struct CommandEntry;
extern const CommandEntry presenceCommands[];
extern const size_t presenceCommandsCount;

#endif // ENABLE_PRESENCE_SENSOR
#endif // I2CSENSOR_STHS34PF80_H
