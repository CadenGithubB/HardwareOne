#ifndef I2CSENSOR_STHS34PF80_H
#define I2CSENSOR_STHS34PF80_H

#include "System_BuildConfig.h"

#if ENABLE_PRESENCE_SENSOR

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Forward declarations
class String;

// STHS34PF80 presence sensor cache
struct PresenceCache {
  SemaphoreHandle_t mutex = nullptr;
  float ambientTemp = 0.0f;        // Ambient temperature in °C
  int16_t objectTemp = 0;          // Raw object temperature
  float compObjectTemp = 0.0f;     // Compensated object temperature
  int16_t presenceValue = 0;       // Presence detection value
  int16_t motionValue = 0;         // Motion detection value
  int16_t tempShockValue = 0;      // Temperature shock value
  bool presenceDetected = false;   // Presence flag
  bool motionDetected = false;     // Motion flag
  bool tempShockDetected = false;  // Temperature shock flag
  unsigned long lastUpdate = 0;
  bool dataValid = false;
};

extern PresenceCache gPresenceCache;
extern bool presenceEnabled;
extern bool presenceConnected;
extern unsigned long presenceLastStopTime;
extern TaskHandle_t presenceTaskHandle;

// Command handlers
const char* cmd_presencestart(const String& cmd);
const char* cmd_presencestop(const String& cmd);
const char* cmd_presenceread(const String& cmd);
const char* cmd_presencestatus(const String& cmd);

// Presence sensor functions
bool startPresenceSensorInternal();  // Called by queue processor
bool initPresenceSensor();
bool readPresenceData();

// JSON building for ESP-NOW streaming
int buildPresenceDataJSON(char* buf, size_t bufSize);

// Command registry (for system_utils.cpp module list)
struct CommandEntry;
extern const CommandEntry presenceCommands[];
extern const size_t presenceCommandsCount;

#endif // ENABLE_PRESENCE_SENSOR
#endif // I2CSENSOR_STHS34PF80_H
