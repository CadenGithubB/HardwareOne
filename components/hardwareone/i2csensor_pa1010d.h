#ifndef I2CSENSOR_PA1010D_H
#define I2CSENSOR_PA1010D_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "System_BuildConfig.h"

// GPS data cache structure (always available for type-safe references)
struct GPSCache {
  SemaphoreHandle_t mutex = nullptr;
  float latitude;
  float longitude;
  float altitude;
  float speed;
  float angle;
  bool hasFix;
  uint8_t fixQuality;    // 0=invalid, 1=GPS, 2=DGPS
  uint8_t satellites;
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  bool dataValid;
  unsigned long lastUpdate;
};

#if ENABLE_GPS_SENSOR

// Forward declarations
class Adafruit_GPS;

extern Adafruit_GPS* gPA1010D;

extern bool gGpsRunning;
extern bool gGpsConnected;
extern unsigned long gGpsLastStopTime;
extern TaskHandle_t gGpsTaskHandle;

extern GPSCache gGpsCache;

// Build GPS JSON snapshot from gGpsCache. Safe to call from any task —
// acquires gGpsCache.mutex. Returns bytes written or 0 on failure.
int gpsBuildDataJSON(char* buf, size_t bufSize);

// Thread-safe snapshot of the GPS cache. Every consumer (CLI, OLED, map,
// accessors) reads GPS data through this instead of touching gPA1010D fields
// directly — the gps_task is the only reader of the device itself, exactly like
// the other sensors. Returns false if the cache mutex could not be taken within
// a short wait (treat as "no data").
bool gpsCacheSnapshot(GPSCache& out);

// GPS initialization (called by sensor queue processor)
bool gpsStartInternal();

// Command handlers
const char* cmd_gps(const String& argsInput);
const char* cmd_gpsstart(const String& argsInput);
const char* cmd_gpsstop(const String& argsInput);

// Command registry (for system_utils.cpp module list)
struct CommandEntry;
extern const CommandEntry gpsCommands[];
extern const size_t gpsCommandsCount;

#endif // ENABLE_GPS_SENSOR

#endif // I2CSENSOR_PA1010D_H
