#ifndef I2CSENSOR_APDS9960_H
#define I2CSENSOR_APDS9960_H

#include "System_BuildConfig.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// APDS sensor cache structure (always available for type-safe references)
struct APDSCache {
  SemaphoreHandle_t mutex = nullptr;
  uint16_t apdsRed = 0, apdsGreen = 0, apdsBlue = 0, apdsClear = 0;
  uint8_t apdsProximity = 0;
  uint8_t apdsGesture = 0;
  unsigned long apdsLastUpdate = 0;
  bool apdsDataValid = false;
};

#if ENABLE_APDS_SENSOR

// Forward declarations
class String;

extern APDSCache gApdsCache;
extern bool gApdsColorEnabled;
extern bool gApdsProximityEnabled;
extern bool gApdsGestureEnabled;
extern bool gApdsConnected;
extern unsigned long gApdsLastStopTime;
extern TaskHandle_t gApdsTaskHandle;

// APDS sensor object
class Adafruit_APDS9960;
extern Adafruit_APDS9960* gAPDS9960;

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
void apdsColorPoll();
void apdsProximityPoll();
void apdsGesturePoll();

// Accessor functions (for MQTT and other modules)
uint8_t apdsGetProximity();
uint16_t apdsGetColorR();
uint16_t apdsGetColorG();
uint16_t apdsGetColorB();
uint16_t apdsGetColorC();
extern bool gApdsEnabled;

// Command registry (for system_utils.cpp module list)
struct CommandEntry;
extern const CommandEntry apdsCommands[];
extern const size_t apdsCommandsCount;

#endif // ENABLE_APDS_SENSOR
#endif // APDS_SENSOR_H
