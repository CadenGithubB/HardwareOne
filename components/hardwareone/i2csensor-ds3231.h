// i2csensor-ds3231.h - DS3231 Precision RTC sensor driver
// I2C Address: 0x68 (fixed)
// Features: Temperature-compensated RTC with battery backup

#ifndef I2CSENSOR_DS3231_H
#define I2CSENSOR_DS3231_H

#include "System_BuildConfig.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// I2C address (fixed, cannot be changed)
#define I2C_ADDR_DS3231 0x68

// RTC DateTime structure (always defined for stubs)
struct RTCDateTime {
  uint16_t year;    // 2000-2099
  uint8_t month;    // 1-12
  uint8_t day;      // 1-31
  uint8_t hour;     // 0-23
  uint8_t minute;   // 0-59
  uint8_t second;   // 0-59
  uint8_t dayOfWeek; // 1-7 (1=Sunday)
};

// RTC Cache for thread-safe access (always defined for stubs)
struct RTCCache {
  SemaphoreHandle_t mutex;
  RTCDateTime dateTime;
  float temperature;       // DS3231 has built-in temperature sensor
  bool dataValid;
  unsigned long lastUpdate;
};

// State variables (always declared for stubs)
extern RTCCache gRTCCache;
extern bool rtcEnabled;
extern bool rtcConnected;
extern unsigned long rtcLastStopTime;
extern TaskHandle_t rtcTaskHandle;

// Command registry (always declared for stubs)
struct CommandEntry;
extern const CommandEntry rtcCommands[];
extern const size_t rtcCommandsCount;

#if ENABLE_RTC_SENSOR

#include <Arduino.h>

// DS3231 Register addresses
#define DS3231_REG_SECONDS    0x00
#define DS3231_REG_MINUTES    0x01
#define DS3231_REG_HOURS      0x02
#define DS3231_REG_DAY        0x03
#define DS3231_REG_DATE       0x04
#define DS3231_REG_MONTH      0x05
#define DS3231_REG_YEAR       0x06
#define DS3231_REG_CONTROL    0x0E
#define DS3231_REG_STATUS     0x0F
#define DS3231_REG_TEMP_MSB   0x11
#define DS3231_REG_TEMP_LSB   0x12

// Core functions
bool initRTCSensor();
bool createRTCTask();
void stopRTCSensor();
void startRTCSensorInternal();  // For sensor queue processor

// Read/Write functions
bool rtcReadDateTime(RTCDateTime* dt);
bool rtcWriteDateTime(const RTCDateTime* dt);
float rtcReadTemperature();

// Time sync with ESP32 system time
bool rtcEarlyBootSync();     // Called at boot before NTP - syncs system time from RTC
bool rtcSyncToSystem();      // RTC -> ESP32 system time
bool rtcSyncFromSystem();    // ESP32 system time -> RTC

// Utility functions
uint32_t rtcToUnixTime(const RTCDateTime* dt);
void unixTimeToRTC(uint32_t unixTime, RTCDateTime* dt);
String rtcDateTimeToString(const RTCDateTime* dt);

// JSON building for ESP-NOW streaming
int buildRTCDataJSON(char* buf, size_t bufSize);

// Command handlers
const char* cmd_rtc(const String& cmd);
const char* cmd_rtcset(const String& cmd);
const char* cmd_rtcsync(const String& cmd);
const char* cmd_rtcstart(const String& cmd);
const char* cmd_rtcstop(const String& cmd);

#endif // ENABLE_RTC_SENSOR

#endif // I2CSENSOR_DS3231_H
