// i2csensor-ds3231.cpp - DS3231 Precision RTC sensor driver
// Minimal IRAM footprint - uses standard I2C transactions

#include "i2csensor-ds3231.h"
#include "System_BuildConfig.h"

#if ENABLE_RTC_SENSOR

#include <Arduino.h>
#include <Wire.h>
#include <time.h>
#include <sys/time.h>

#include "System_Command.h"
#include "System_Debug.h"
#include "System_I2C.h"
#include "System_Settings.h"
#include "System_TaskUtils.h"
#include "System_Utils.h"

extern TwoWire Wire1;
extern Settings gSettings;
extern void broadcastOutput(const char* msg);
extern void broadcastOutput(const String& msg);

// RTC Cache
RTCCache gRTCCache = {nullptr, {0}, 0.0f, false, 0};

// Sensor state
bool rtcEnabled = false;
bool rtcConnected = false;
TaskHandle_t rtcTaskHandle = nullptr;

// Task stack watermark tracking
volatile UBaseType_t gRTCWatermarkMin = (UBaseType_t)0xFFFFFFFF;
volatile UBaseType_t gRTCWatermarkNow = (UBaseType_t)0;

// Macro for CLI validation
#define RETURN_VALID_IF_VALIDATE_CSTR() \
  do { \
    extern bool gCLIValidateOnly; \
    if (gCLIValidateOnly) return "VALID"; \
  } while(0)

// ============================================================================
// BCD Conversion Helpers
// ============================================================================

static uint8_t bcdToDec(uint8_t bcd) {
  return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

static uint8_t decToBcd(uint8_t dec) {
  return ((dec / 10) << 4) | (dec % 10);
}

// ============================================================================
// Low-level I2C Functions
// ============================================================================

static bool rtcWriteRegister(uint8_t reg, uint8_t value) {
  return i2cTransaction(100000, 100, [&]() -> bool {
    Wire1.beginTransmission(I2C_ADDR_DS3231);
    Wire1.write(reg);
    Wire1.write(value);
    return Wire1.endTransmission() == 0;
  });
}

static bool rtcReadRegisters(uint8_t startReg, uint8_t* buffer, uint8_t count) {
  return i2cTransaction(100000, 100, [&]() -> bool {
    Wire1.beginTransmission(I2C_ADDR_DS3231);
    Wire1.write(startReg);
    if (Wire1.endTransmission() != 0) return false;
    
    Wire1.requestFrom((uint8_t)I2C_ADDR_DS3231, count);
    for (uint8_t i = 0; i < count && Wire1.available(); i++) {
      buffer[i] = Wire1.read();
    }
    return true;
  });
}

// ============================================================================
// RTC Read/Write Functions
// ============================================================================

bool rtcReadDateTime(RTCDateTime* dt) {
  if (!dt) return false;
  
  uint8_t buffer[7];
  if (!rtcReadRegisters(DS3231_REG_SECONDS, buffer, 7)) {
    return false;
  }
  
  dt->second = bcdToDec(buffer[0] & 0x7F);
  dt->minute = bcdToDec(buffer[1]);
  dt->hour = bcdToDec(buffer[2] & 0x3F);  // 24-hour mode
  dt->dayOfWeek = buffer[3];
  dt->day = bcdToDec(buffer[4]);
  dt->month = bcdToDec(buffer[5] & 0x1F);
  dt->year = 2000 + bcdToDec(buffer[6]);
  
  return true;
}

bool rtcWriteDateTime(const RTCDateTime* dt) {
  if (!dt) return false;
  
  return i2cTransaction(100000, 100, [&]() -> bool {
    Wire1.beginTransmission(I2C_ADDR_DS3231);
    Wire1.write(DS3231_REG_SECONDS);
    Wire1.write(decToBcd(dt->second));
    Wire1.write(decToBcd(dt->minute));
    Wire1.write(decToBcd(dt->hour));  // 24-hour mode
    Wire1.write(dt->dayOfWeek);
    Wire1.write(decToBcd(dt->day));
    Wire1.write(decToBcd(dt->month));
    Wire1.write(decToBcd(dt->year - 2000));
    return Wire1.endTransmission() == 0;
  });
}

float rtcReadTemperature() {
  uint8_t buffer[2];
  if (!rtcReadRegisters(DS3231_REG_TEMP_MSB, buffer, 2)) {
    return -999.0f;
  }
  
  // Temperature is in 0.25°C resolution
  int16_t temp = ((int16_t)buffer[0] << 2) | (buffer[1] >> 6);
  if (buffer[0] & 0x80) {
    temp |= 0xFC00;  // Sign extend for negative temps
  }
  return temp * 0.25f;
}

// ============================================================================
// Time Sync Functions
// ============================================================================

bool rtcEarlyBootSync() {
  // Called early at boot to sync system time from RTC before NTP
  // This works even if the RTC sensor task isn't running yet
  // Just needs I2C to be initialized
  
  // Quick probe to check if RTC is present
  Wire1.beginTransmission(I2C_ADDR_DS3231);
  if (Wire1.endTransmission() != 0) {
    DEBUG_SENSORSF("[RTC] Early boot sync: RTC not detected at 0x%02X", I2C_ADDR_DS3231);
    return false;
  }
  
  RTCDateTime dt;
  if (!rtcReadDateTime(&dt)) {
    DEBUG_SENSORSF("[RTC] Early boot sync: Failed to read RTC");
    return false;
  }
  
  // Sanity check - make sure RTC has valid time (year >= 2020)
  if (dt.year < 2020 || dt.year > 2099) {
    DEBUG_SENSORSF("[RTC] Early boot sync: RTC time invalid (year=%d)", dt.year);
    return false;
  }
  
  struct tm timeinfo;
  timeinfo.tm_year = dt.year - 1900;
  timeinfo.tm_mon = dt.month - 1;
  timeinfo.tm_mday = dt.day;
  timeinfo.tm_hour = dt.hour;
  timeinfo.tm_min = dt.minute;
  timeinfo.tm_sec = dt.second;
  timeinfo.tm_isdst = -1;
  
  time_t t = mktime(&timeinfo);
  struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
  
  Serial.printf("[RTC] Early boot sync: System time set to %s\n", rtcDateTimeToString(&dt).c_str());
  return true;
}

bool rtcSyncToSystem() {
  RTCDateTime dt;
  if (!rtcReadDateTime(&dt)) {
    broadcastOutput("[RTC] Failed to read RTC for sync");
    return false;
  }
  
  struct tm timeinfo;
  timeinfo.tm_year = dt.year - 1900;
  timeinfo.tm_mon = dt.month - 1;
  timeinfo.tm_mday = dt.day;
  timeinfo.tm_hour = dt.hour;
  timeinfo.tm_min = dt.minute;
  timeinfo.tm_sec = dt.second;
  timeinfo.tm_isdst = -1;
  
  time_t t = mktime(&timeinfo);
  struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
  
  DEBUG_SENSORSF("[RTC] Synced system time from RTC: %s", rtcDateTimeToString(&dt).c_str());
  return true;
}

bool rtcSyncFromSystem() {
  time_t now;
  time(&now);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  
  RTCDateTime dt;
  dt.year = timeinfo.tm_year + 1900;
  dt.month = timeinfo.tm_mon + 1;
  dt.day = timeinfo.tm_mday;
  dt.hour = timeinfo.tm_hour;
  dt.minute = timeinfo.tm_min;
  dt.second = timeinfo.tm_sec;
  dt.dayOfWeek = timeinfo.tm_wday + 1;  // tm_wday is 0-6, RTC uses 1-7
  
  if (!rtcWriteDateTime(&dt)) {
    broadcastOutput("[RTC] Failed to write system time to RTC");
    return false;
  }
  
  DEBUG_SENSORSF("[RTC] Synced RTC from system time: %s", rtcDateTimeToString(&dt).c_str());
  return true;
}

// ============================================================================
// Utility Functions
// ============================================================================

uint32_t rtcToUnixTime(const RTCDateTime* dt) {
  struct tm timeinfo;
  timeinfo.tm_year = dt->year - 1900;
  timeinfo.tm_mon = dt->month - 1;
  timeinfo.tm_mday = dt->day;
  timeinfo.tm_hour = dt->hour;
  timeinfo.tm_min = dt->minute;
  timeinfo.tm_sec = dt->second;
  timeinfo.tm_isdst = -1;
  return (uint32_t)mktime(&timeinfo);
}

void unixTimeToRTC(uint32_t unixTime, RTCDateTime* dt) {
  time_t t = (time_t)unixTime;
  struct tm timeinfo;
  gmtime_r(&t, &timeinfo);
  
  dt->year = timeinfo.tm_year + 1900;
  dt->month = timeinfo.tm_mon + 1;
  dt->day = timeinfo.tm_mday;
  dt->hour = timeinfo.tm_hour;
  dt->minute = timeinfo.tm_min;
  dt->second = timeinfo.tm_sec;
  dt->dayOfWeek = timeinfo.tm_wday + 1;
}

String rtcDateTimeToString(const RTCDateTime* dt) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           dt->year, dt->month, dt->day,
           dt->hour, dt->minute, dt->second);
  return String(buf);
}

// ============================================================================
// RTC Task
// ============================================================================

static void rtcTask(void* pvParameters) {
  (void)pvParameters;
  
  DEBUG_SENSORSF("[RTC] Task started");
  
  // Check if system time is already valid (e.g., from NTP that ran before RTC started)
  struct tm timeinfo;
  bool systemTimeValid = false;
  if (getLocalTime(&timeinfo, 0)) {
    // System time is valid if year >= 2020 (tm_year is years since 1900)
    systemTimeValid = (timeinfo.tm_year >= 120);
  }
  
  if (systemTimeValid) {
    // System already has valid time (likely from NTP) - sync TO RTC
    if (rtcSyncFromSystem()) {
      broadcastOutput("[RTC] RTC updated from system time (NTP)");
      // Mark RTC as calibrated
      if (!gSettings.rtcTimeHasBeenSet) {
        gSettings.rtcTimeHasBeenSet = true;
        writeSettingsJson();
      }
    }
  } else {
    // No valid system time yet - sync FROM RTC to system
    if (rtcSyncToSystem()) {
      broadcastOutput("[RTC] System time synchronized from RTC");
    }
  }
  
  unsigned long lastCacheUpdate = 0;
  const unsigned long CACHE_UPDATE_INTERVAL = 1000;  // 1 second
  
  while (rtcEnabled) {
    unsigned long now = millis();
    
    // Update cache periodically
    if (now - lastCacheUpdate >= CACHE_UPDATE_INTERVAL) {
      RTCDateTime dt;
      float temp = rtcReadTemperature();
      
      if (rtcReadDateTime(&dt)) {
        if (gRTCCache.mutex && xSemaphoreTake(gRTCCache.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
          gRTCCache.dateTime = dt;
          gRTCCache.temperature = temp;
          gRTCCache.dataValid = true;
          gRTCCache.lastUpdate = now;
          xSemaphoreGive(gRTCCache.mutex);
        }
      }
      
      lastCacheUpdate = now;
    }
    
    // Track stack watermark
    UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
    gRTCWatermarkNow = watermark;
    if (watermark < gRTCWatermarkMin) {
      gRTCWatermarkMin = watermark;
    }
    
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  
  DEBUG_SENSORSF("[RTC] Task exiting");
  rtcTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

// ============================================================================
// Sensor Lifecycle
// ============================================================================

bool initRTCSensor() {
  DEBUG_SENSORSF("[RTC] Initializing DS3231...");
  
  // Check if device responds
  bool found = i2cPingAddress(I2C_ADDR_DS3231, 100000, 50);
  if (!found) {
    DEBUG_SENSORSF("[RTC] DS3231 not found at 0x%02X", I2C_ADDR_DS3231);
    return false;
  }
  
  // Initialize cache mutex
  if (!gRTCCache.mutex) {
    gRTCCache.mutex = xSemaphoreCreateMutex();
    if (!gRTCCache.mutex) {
      DEBUG_SENSORSF("[RTC] Failed to create cache mutex");
      return false;
    }
  }
  
  // Clear oscillator stop flag if set (indicates power loss)
  uint8_t status;
  if (rtcReadRegisters(DS3231_REG_STATUS, &status, 1)) {
    if (status & 0x80) {  // OSF bit
      DEBUG_SENSORSF("[RTC] Oscillator was stopped - RTC time may be invalid");
      rtcWriteRegister(DS3231_REG_STATUS, status & ~0x80);
    }
  }
  
  rtcConnected = true;
  DEBUG_SENSORSF("[RTC] DS3231 initialized successfully");
  return true;
}

bool createRTCTask() {
  if (rtcTaskHandle != nullptr) {
    DEBUG_SENSORSF("[RTC] Task already running");
    return true;
  }
  
  rtcEnabled = true;
  
  // Stack needs room for NTP sync check and settings write on startup
  BaseType_t result = xTaskCreatePinnedToCore(
    rtcTask,
    "rtc_task",
    4096,  // Increased for NTP sync logic
    nullptr,
    1,     // Low priority
    &rtcTaskHandle,
    1      // Core 1
  );
  
  if (result != pdPASS) {
    DEBUG_SENSORSF("[RTC] Failed to create task");
    rtcEnabled = false;
    return false;
  }
  
  DEBUG_SENSORSF("[RTC] Task created successfully");
  return true;
}

void stopRTCSensor() {
  rtcEnabled = false;
  
  // Wait for task to exit
  int timeout = 50;
  while (rtcTaskHandle != nullptr && timeout-- > 0) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  
  if (rtcTaskHandle != nullptr) {
    vTaskDelete(rtcTaskHandle);
    rtcTaskHandle = nullptr;
  }
  
  rtcConnected = false;
  DEBUG_SENSORSF("[RTC] Sensor stopped");
}

// Internal start function for sensor queue processor
void startRTCSensorInternal() {
  if (rtcEnabled && rtcConnected) {
    DEBUG_SENSORSF("[RTC] Already running");
    return;
  }
  
  if (!initRTCSensor()) {
    DEBUG_SENSORSF("[RTC] Failed to initialize");
    return;
  }
  
  if (!createRTCTask()) {
    DEBUG_SENSORSF("[RTC] Failed to create task");
    return;
  }
  
  DEBUG_SENSORSF("[RTC] Started successfully via queue");
}

// ============================================================================
// CLI Command Handlers
// ============================================================================

const char* cmd_rtc(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  static String response;
  response = "";
  
  String arg = cmd;
  arg.trim();
  
  if (arg.length() == 0 || arg == "status") {
    // Show RTC status and current time
    if (!rtcConnected) {
      response = "[RTC] Not connected. Use 'openrtc' to initialize.";
      return response.c_str();
    }
    
    RTCDateTime dt;
    if (rtcReadDateTime(&dt)) {
      float temp = rtcReadTemperature();
      response = "[RTC] " + rtcDateTimeToString(&dt);
      response += " | Temp: " + String(temp, 1) + "°C";
      response += " | Unix: " + String(rtcToUnixTime(&dt));
    } else {
      response = "[RTC] Failed to read time";
    }
    return response.c_str();
  }
  
  if (arg == "temp" || arg == "temperature") {
    if (!rtcConnected) {
      return "[RTC] Not connected";
    }
    float temp = rtcReadTemperature();
    response = "[RTC] Temperature: " + String(temp, 2) + "°C";
    return response.c_str();
  }
  
  response = "[RTC] Unknown command. Use: rtc [status|temp]";
  return response.c_str();
}

const char* cmd_rtcset(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  static String response;
  response = "";
  
  if (!rtcConnected) {
    return "[RTC] Not connected. Use 'openrtc' first.";
  }
  
  String arg = cmd;
  arg.trim();
  
  // Parse: YYYY-MM-DD HH:MM:SS or unix timestamp
  if (arg.length() == 0) {
    return "[RTC] Usage: rtcset YYYY-MM-DD HH:MM:SS  or  rtcset <unix_timestamp>";
  }
  
  RTCDateTime dt;
  
  // Check if it's a unix timestamp (all digits)
  bool isUnix = true;
  for (size_t i = 0; i < arg.length(); i++) {
    if (!isDigit(arg[i])) {
      isUnix = false;
      break;
    }
  }
  
  if (isUnix) {
    uint32_t unixTime = arg.toInt();
    unixTimeToRTC(unixTime, &dt);
  } else {
    // Parse YYYY-MM-DD HH:MM:SS
    int year, month, day, hour, minute, second;
    if (sscanf(arg.c_str(), "%d-%d-%d %d:%d:%d", 
               &year, &month, &day, &hour, &minute, &second) != 6) {
      return "[RTC] Invalid format. Use: YYYY-MM-DD HH:MM:SS";
    }
    
    dt.year = year;
    dt.month = month;
    dt.day = day;
    dt.hour = hour;
    dt.minute = minute;
    dt.second = second;
    
    // Calculate day of week
    struct tm timeinfo = {0};
    timeinfo.tm_year = year - 1900;
    timeinfo.tm_mon = month - 1;
    timeinfo.tm_mday = day;
    mktime(&timeinfo);
    dt.dayOfWeek = timeinfo.tm_wday + 1;
  }
  
  if (rtcWriteDateTime(&dt)) {
    response = "[RTC] Time set to: " + rtcDateTimeToString(&dt);
    // Mark RTC as calibrated so future boots trust RTC first
    if (!gSettings.rtcTimeHasBeenSet) {
      gSettings.rtcTimeHasBeenSet = true;
      extern bool writeSettingsJson();
      writeSettingsJson();
      response += "\n[RTC] Marked as calibrated for future boots";
    }
  } else {
    response = "[RTC] Failed to set time";
  }
  
  return response.c_str();
}

const char* cmd_rtcsync(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!rtcConnected) {
    return "[RTC] Not connected. Use 'openrtc' first.";
  }
  
  String arg = cmd;
  arg.trim();
  
  if (arg == "tosystem" || arg == "to" || arg.length() == 0) {
    // RTC -> System (default)
    if (rtcSyncToSystem()) {
      return "[RTC] System time updated from RTC";
    }
    return "[RTC] Sync failed";
  }
  
  if (arg == "fromsystem" || arg == "from") {
    // System -> RTC
    if (rtcSyncFromSystem()) {
      // Mark RTC as calibrated so future boots trust RTC first
      if (!gSettings.rtcTimeHasBeenSet) {
        gSettings.rtcTimeHasBeenSet = true;
        extern bool writeSettingsJson();
        writeSettingsJson();
      }
      return "[RTC] RTC updated from system time";
    }
    return "[RTC] Sync failed";
  }
  
  return "[RTC] Usage: rtcsync [to|from]  (to=RTC->system, from=system->RTC)";
}

const char* cmd_rtcstart(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)cmd;
  
  if (rtcEnabled && rtcConnected) {
    return "[RTC] Already running";
  }
  
  if (!initRTCSensor()) {
    return "[RTC] Failed to initialize - check wiring";
  }
  
  if (!createRTCTask()) {
    return "[RTC] Failed to create task";
  }
  
  return "[RTC] Opened successfully";
}

const char* cmd_rtcstop(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)cmd;
  
  if (!rtcEnabled) {
    return "[RTC] Not running";
  }
  
  stopRTCSensor();
  return "[RTC] Closed";
}

// ============================================================================
// RTC Modular Settings Registration
// ============================================================================

#include "System_Settings.h"

// RTC settings entries
static const SettingEntry rtcSettingEntries[] = {
  { "rtcAutoStart", SETTING_BOOL, &gSettings.rtcAutoStart, 0, 0, nullptr, 0, 1, "Auto-start after boot", nullptr },
  { "rtcTimeHasBeenSet", SETTING_BOOL, &gSettings.rtcTimeHasBeenSet, 0, 0, nullptr, 0, 1, "RTC time has been set (NTP/manual)", nullptr }
};

static bool isRTCConnectedSetting() {
  return rtcConnected;
}

extern const SettingsModule rtcSettingsModule = {
  "rtc",
  "rtc_ds3231",
  rtcSettingEntries,
  sizeof(rtcSettingEntries) / sizeof(rtcSettingEntries[0]),
  isRTCConnectedSetting,
  "DS3231 RTC sensor settings"
};

// ============================================================================
// Command Registry
// ============================================================================

#include "System_Utils.h"

const CommandEntry rtcCommands[] = {
  { "rtc",      "Show RTC status and current time.",           false, cmd_rtc,      "Usage: rtc [status|temp]" },
  { "openrtc", "Open DS3231 RTC sensor.",                    false, cmd_rtcstart, nullptr },
  { "closertc",  "Close DS3231 RTC sensor.",                     false, cmd_rtcstop,  nullptr },
  { "rtcset",   "Set RTC time.",                               false, cmd_rtcset,   "Usage: rtcset YYYY-MM-DD HH:MM:SS  or  rtcset <unix_timestamp>" },
  { "rtcsync",  "Sync time between RTC and system.",           false, cmd_rtcsync,  "Usage: rtcsync [to|from] (to=RTC->system, from=system->RTC)" },
};
const size_t rtcCommandsCount = sizeof(rtcCommands) / sizeof(rtcCommands[0]);

// Auto-register with command system
static CommandModuleRegistrar _rtc_cmd_registrar(rtcCommands, rtcCommandsCount, "rtc");

// ============================================================================
// RTC OLED Mode (Display Function + Registration)
// ============================================================================
#include "i2csensor-ds3231-oled.h"

#endif // ENABLE_RTC_SENSOR
