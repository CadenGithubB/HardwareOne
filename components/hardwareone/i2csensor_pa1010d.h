#ifndef I2CSENSOR_PA1010D_H
#define I2CSENSOR_PA1010D_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "System_BuildConfig.h"

// A copied GPS reading. The live cache, its lock and the driver belong to the
// implementation; callers may retain or modify this value independently.
struct GPSCache {
  float latitude;       // Signed decimal degrees (south is negative)
  float longitude;      // Signed decimal degrees (west is negative)
  float altitude;       // Metres above mean sea level
  float speed;          // Knots, as reported by the GPS
  float angle;          // Course over ground, degrees
  bool hasFix;          // Position/speed fields have a GPS fix
  uint8_t fixQuality;    // 0=invalid, 1=GPS, 2=DGPS
  uint8_t satellites;
  uint16_t year;        // UTC date and time (not guaranteed valid without a fix)
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  bool dataValid;       // NMEA data received in this run; does not imply a fix
  unsigned long lastUpdate; // millis() at publication, zero before first reading
};

#if ENABLE_GPS_SENSOR

extern bool gGpsRunning;
extern bool gGpsConnected;
extern unsigned long gGpsLastStopTime;
extern TaskHandle_t gGpsTaskHandle;

// Build the existing sensor envelope from one snapshot, formatting after the
// lock is released. A 50 ms lock timeout produces a not-ready envelope.
// Returns bytes written (excluding NUL), or 0 for an invalid/insufficient buffer.
int gpsBuildDataJSON(char* buf, size_t bufSize);

// Copy a consistent reading, waiting at most timeoutMs for the cache lock.
// Success means the copy was available: check dataValid/hasFix before use.
// Before initialization or on timeout, returns false and clears out.
bool gpsCacheSnapshot(GPSCache& out, uint32_t timeoutMs = 10);

// GPS initialization (called by sensor queue processor)
bool gpsStartInternal();

// Command handlers
const char* cmd_gps(const String& argsInput);
const char* cmd_gpsstart(const String& argsInput);
const char* cmd_gpsstop(const String& argsInput);

// Command registry (for System_CommandRegistry.cpp module list)
struct CommandEntry;
extern const CommandEntry gpsCommands[];
extern const size_t gpsCommandsCount;

#else

// Keep reading APIs available when GPS is omitted from the build.
inline bool gpsCacheSnapshot(GPSCache& out, uint32_t timeoutMs = 10) {
  (void)timeoutMs;
  out = {};
  return false;
}

inline int gpsBuildDataJSON(char* buf, size_t bufSize) {
  if (buf && bufSize) buf[0] = '\0';
  return 0;
}

#endif // ENABLE_GPS_SENSOR

#endif // I2CSENSOR_PA1010D_H
