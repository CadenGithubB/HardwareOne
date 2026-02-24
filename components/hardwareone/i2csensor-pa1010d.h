#ifndef PA1010D_GPS_SENSOR_H
#define PA1010D_GPS_SENSOR_H

#include <Arduino.h>

#include "System_BuildConfig.h"

#if ENABLE_GPS_SENSOR

// Forward declarations
class Adafruit_GPS;

// GPS module object (defined in gps_sensor.cpp)
extern Adafruit_GPS* gPA1010D;

// GPS sensor state
extern bool gpsEnabled;
extern bool gpsConnected;
extern unsigned long gpsLastStopTime;
extern TaskHandle_t gpsTaskHandle;

// GPS data cache for thread-safe access from web/OLED
struct GPSCache {
  SemaphoreHandle_t mutex;
  
  // Position data
  float latitude;        // Decimal degrees
  float longitude;       // Decimal degrees
  float altitude;        // Meters
  float speed;           // Knots
  float angle;           // Degrees
  
  // Fix status
  bool hasFix;
  uint8_t fixQuality;    // 0=invalid, 1=GPS, 2=DGPS
  uint8_t satellites;
  
  // Time data (UTC)
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  
  // Metadata
  bool dataValid;
  unsigned long lastUpdate;  // millis() timestamp
};

extern GPSCache gGPSCache;

// GPS initialization (called by sensor queue processor)
void startGPSInternal();

// Command handlers
const char* cmd_gps(const String& cmd);
const char* cmd_gpsstart(const String& cmd);
const char* cmd_gpsstop(const String& cmd);

// Command registry (for system_utils.cpp module list)
struct CommandEntry;
extern const CommandEntry gpsCommands[];
extern const size_t gpsCommandsCount;

#endif // ENABLE_GPS_SENSOR

#endif // PA1010D_GPS_SENSOR_H
