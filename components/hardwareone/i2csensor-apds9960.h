#ifndef I2CSENSOR_APDS9960_H
#define I2CSENSOR_APDS9960_H

#include "System_BuildConfig.h"

#if ENABLE_APDS_SENSOR

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Forward declarations
class String;

// APDS sensor state
struct PeripheralCache {
  SemaphoreHandle_t mutex = nullptr;
  uint16_t apdsRed = 0, apdsGreen = 0, apdsBlue = 0, apdsClear = 0;
  uint8_t apdsProximity = 0;
  uint8_t apdsGesture = 0;
  unsigned long apdsLastUpdate = 0;
  bool apdsDataValid = false;
};

extern PeripheralCache gPeripheralCache;
extern bool apdsColorEnabled;
extern bool apdsProximityEnabled;
extern bool apdsGestureEnabled;
extern bool apdsConnected;
extern unsigned long apdsLastStopTime;
extern TaskHandle_t apdsTaskHandle;

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
bool startAPDSSensorInternal();  // Called by queue processor
bool initAPDS9960();
void readAPDSColor();
void readAPDSProximity();
void readAPDSGesture();

// Accessor functions (for MQTT and other modules)
uint8_t getAPDSProximity();
uint16_t getAPDSColorR();
uint16_t getAPDSColorG();
uint16_t getAPDSColorB();
uint16_t getAPDSColorC();
extern bool apdsEnabled;

// Command registry (for system_utils.cpp module list)
struct CommandEntry;
extern const CommandEntry apdsCommands[];
extern const size_t apdsCommandsCount;

#endif // ENABLE_APDS_SENSOR
#endif // APDS_SENSOR_H
