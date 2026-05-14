// i2csensor_ds3231.h - DS3231 Precision RTC sensor driver
// I2C Address: 0x68 (fixed)
// Features: Temperature-compensated RTC with battery backup

#ifndef I2CSENSOR_DS3231_H
#define I2CSENSOR_DS3231_H

#include "System_BuildConfig.h"
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// RTC type definitions (always available for type-safe references)
struct RTCDateTime {
  uint16_t year;     // 2000-2099
  uint8_t month;     // 1-12
  uint8_t day;       // 1-31
  uint8_t hour;      // 0-23
  uint8_t minute;    // 0-59
  uint8_t second;    // 0-59
  uint8_t dayOfWeek; // 1-7 (1=Sunday)
};

struct RTCCache {
  SemaphoreHandle_t mutex = nullptr;
  RTCDateTime dateTime;
  float temperature;
  bool dataValid;
  unsigned long lastUpdate;
};

#if ENABLE_RTC_SENSOR

#include "System_I2C.h"

// State variables
extern RTCCache gRtcCache;
extern bool gRtcEnabled;
extern bool gRtcConnected;
extern unsigned long gRtcLastStopTime;
extern TaskHandle_t gRtcTaskHandle;

// Command registry
struct CommandEntry;
extern const CommandEntry rtcCommands[];
extern const size_t rtcCommandsCount;

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
bool rtcInit();
// createRTCTask() is declared in System_TaskUtils.h
void rtcStop();
bool rtcStartInternal();  // For sensor queue processor

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

// Returns a copy of utc with gSettings.tzOffsetMinutes applied.
// Handles date rollover (midnight crossing in either direction).
// Use this before any human-facing display of RTC time.
RTCDateTime rtcLocalTime(const RTCDateTime* utc);

// JSON building for ESP-NOW streaming
int rtcBuildDataJSON(char* buf, size_t bufSize);

// Accessor functions (for MQTT and other modules)
int rtcGetYear();
int rtcGetMonth();
int rtcGetDay();
int rtcGetHour();
int rtcGetMinute();
int rtcGetSecond();
float rtcGetTemperature();

// Command handlers
const char* cmd_rtc(const String& argsInput);
const char* cmd_rtcset(const String& argsInput);
const char* cmd_rtcsync(const String& argsInput);
const char* cmd_rtcstart(const String& argsInput);
const char* cmd_rtcstop(const String& argsInput);

#endif // ENABLE_RTC_SENSOR

#endif // I2CSENSOR_DS3231_H
