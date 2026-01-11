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
