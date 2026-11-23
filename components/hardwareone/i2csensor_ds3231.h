// i2csensor_ds3231.h - DS3231 Precision RTC sensor driver
// I2C Address: 0x68 (fixed)
// Features: Temperature-compensated RTC with battery backup

#ifndef I2CSENSOR_DS3231_H
#define I2CSENSOR_DS3231_H

#include "System_BuildConfig.h"
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stddef.h>

// RTC type definitions (always available for type-safe references)
struct RtcDateTime {
  uint16_t year;     // 2000-2099
  uint8_t month;     // 1-12
  uint8_t day;       // 1-31
  uint8_t hour;      // 0-23
  uint8_t minute;    // 0-59
  uint8_t second;    // 0-59
  uint8_t dayOfWeek; // 1-7 (1=Sunday)
};

struct RtcCache {
  RtcDateTime dateTime;
  float temperature;
  bool dataValid;
  unsigned long lastUpdate;
};

// Values only: callers own their copy; the live cache and lock stay in the driver.
#if ENABLE_RTC_SENSOR

// State variables
extern bool gRtcRunning;
extern bool gRtcConnected;
extern unsigned long gRtcLastStopTime;
extern TaskHandle_t gRtcTaskHandle;

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
void rtcStop();  // Lifecycle worker only; live tasks complete their own cleanup.
// createRtcTask() is declared in System_TaskUtils.h
bool rtcStartInternal();  // For sensor queue processor

// Read/Write functions
bool rtcReadDateTime(RtcDateTime* dt);
bool rtcWriteDateTime(const RtcDateTime* dt);
// rtcReadTemperature() returns this when the register read failed.
constexpr float kRtcTemperatureUnavailable = -999.0f;
float rtcReadTemperature();

// Time sync with ESP32 system time
// Called at boot before NTP: sets the system time from the chip when the chip
// is trusted (this device set it, gSettings.rtcTimeHasBeenSet, and its
// oscillator has not stopped since). False with *whyNot = an "Error: ..." line
// when a chip answered but its time was not taken, nullptr when no chip did.
bool rtcEarlyBootSync(const char** whyNot = nullptr);
// A command passes `whyNot` and gets the failure as its reply text; without it
// the reason is printed on the output stream. Both go through Clock::step
// (the rank table). `operatorAsked` ('rtcsync to'): a chip this device has not
// set is taken as well, and is marked set; automatic callers leave it false.
bool rtcSyncToSystem(const char** whyNot = nullptr, bool operatorAsked = false);  // RTC -> ESP32 system time
bool rtcSyncFromSystem(const char** whyNot = nullptr);    // ESP32 system time -> RTC

// Utility functions
uint32_t rtcToUnixTime(const RtcDateTime* dt);
void unixTimeToRtc(uint32_t unixTime, RtcDateTime* dt);
String rtcDateTimeToString(const RtcDateTime* dt);

// Returns a copy of utc with gSettings.tzOffsetMinutes applied.
// Handles date rollover (midnight crossing in either direction).
// Use this before any human-facing display of RTC time.
RtcDateTime rtcLocalTime(const RtcDateTime* utc);

// JSON building for ESP-NOW streaming
int rtcBuildDataJson(char* buf, size_t bufSize);

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

// Copy one reading; false clears out when uninitialized or the lock is busy.
// Success only means a copy was available; check dataValid where required.
bool rtcCacheSnapshot(RtcCache& out, uint32_t timeoutMs = 10);

#else
// The RTC is not in this build. Its state reads as stopped and a start
// request fails, so callers need no build condition.
inline bool gRtcRunning = false;
inline bool gRtcConnected = false;
inline unsigned long gRtcLastStopTime = 0;
inline TaskHandle_t gRtcTaskHandle = nullptr;
inline bool rtcStartInternal() { return false; }
inline bool rtcCacheSnapshot(RtcCache& out, uint32_t timeoutMs = 10) {
  (void)timeoutMs;
  out = {};
  return false;
}
inline int rtcBuildDataJson(char* buf, size_t bufSize) {
  if (buf && bufSize) buf[0] = '\0';
  return 0;
}
#endif // ENABLE_RTC_SENSOR

#endif // I2CSENSOR_DS3231_H
