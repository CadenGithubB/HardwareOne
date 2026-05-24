#include "i2csensor_pa1010d.h"
#include "System_Utils.h"

#if ENABLE_GPS_SENSOR

#include <Adafruit_GPS.h>
#include <Arduino.h>
#include <Wire.h>

#include "OLED_Display.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_ESPNow.h"
#include "System_ESPNow_Sensors.h"
#include "System_I2C.h"
#include "System_Maps.h"
#include "System_MemoryMonitor.h"
#include "System_SensorLogging.h"
#include "System_Settings.h"
#include "System_TaskUtils.h"

// Task handle (owned by this module)
TaskHandle_t gGpsTaskHandle = nullptr;

// GPS module object (owned by this module)
Adafruit_GPS* gPA1010D = nullptr;

// External dependencies provided by System_I2C.h:
// sensorStatusBumpWith, gSensorPollingPaused, drainDebugRing

// GPS sensor state (definition - matching pattern of thermal/tof/imu/gamepad sensors)
bool gGpsEnabled = false;
bool gGpsConnected = false;
unsigned long gGpsLastStopTime = 0;

// GPS cache for thread-safe data access (mutex created in setup())
GPSCache gGpsCache = {
  .mutex = nullptr,
  .latitude = 0.0f,
  .longitude = 0.0f,
  .altitude = 0.0f,
  .speed = 0.0f,
  .angle = 0.0f,
  .hasFix = false,
  .fixQuality = 0,
  .satellites = 0,
  .year = 0,
  .month = 0,
  .day = 0,
  .hour = 0,
  .minute = 0,
  .second = 0,
  .dataValid = false,
  .lastUpdate = 0
};

// Helper function to start GPS internal (called by queue processor)
// Moved from HardwareOne.ino to consolidate GPS initialization logic
bool gpsStartInternal() {
  INFO_GPS_LIFECYCLEF("Starting GPS initialization...");

  if (gGpsEnabled) {
    DEBUG_GPS_LIFECYCLEF("[GPS_INIT] GPS already started (enabled=1)");
    return true;
  }

  // Check memory before creating GPS task
  if (!checkMemoryAvailable("gps", nullptr)) {
    ERROR_GPSF("[GPS_INIT] Insufficient memory for GPS sensor");
    return false;
  }

  // Create cache mutex if not already created
  if (!gGpsCache.mutex) {
    gGpsCache.mutex = xSemaphoreCreateMutex();
    if (!gGpsCache.mutex) {
      ERROR_GPSF("Failed to create cache mutex");
      return false;
    }
    DEBUG_GPS_LIFECYCLEF("[GPS] Cache mutex created");
  }

  // Clean up any stale cache from previous run BEFORE starting
  {
    SensorCacheGuard g(gGpsCache.mutex, pdMS_TO_TICKS(100), "gps.cleanStaleCache");
    if (g.held) {
      gGpsCache.dataValid = false;
      gGpsCache.latitude = 0.0f;
      gGpsCache.longitude = 0.0f;
      gGpsCache.altitude = 0.0f;
      gGpsCache.speed = 0.0f;
      gGpsCache.angle = 0.0f;
      gGpsCache.hasFix = false;
      gGpsCache.fixQuality = 0;
      gGpsCache.satellites = 0;
      DEBUG_GPS_LIFECYCLEF("[GPS] Cleaned up stale cache from previous run");
    }
  }
  
  // Initialize GPS module if not already done
  if (!gGpsConnected || gPA1010D == nullptr) {
    DEBUG_GPS_LIFECYCLEF("[GPS_INIT] Allocating Adafruit_GPS object on Wire1...");
    gPA1010D = new Adafruit_GPS(&Wire1);
    if (!gPA1010D) {
      ERROR_GPSF("Failed to allocate GPS module");
      return false;
    }
    DEBUG_GPS_LIFECYCLEF("[GPS_INIT] GPS object allocated at %p", gPA1010D);
    
    DEBUG_GPS_LIFECYCLEF("[GPS_INIT] Calling gPA1010D->begin(0x%02X)...", I2C_ADDR_GPS);
    
    // Retry GPS initialization with delays (GPS needs time after power-on)
    bool initSuccess = false;
    for (int retry = 0; retry < 3; retry++) {
      if (retry > 0) {
        DEBUG_GPS_LIFECYCLEF("[GPS_INIT] Retry %d/3 after 200ms delay...", retry);
        delay(200);
      }
      bool began = i2cDeviceTransaction(I2C_ADDR_GPS, 100000, 500, [&]() -> bool {
        return gPA1010D->begin(I2C_ADDR_GPS);
      });
      if (began) {
        initSuccess = true;
        break;
      }
    }
    
    if (!initSuccess) {
      delete gPA1010D;
      gPA1010D = nullptr;
      gGpsConnected = false;
      ERROR_GPSF("Failed to initialize GPS module at 0x%02X after 3 attempts", I2C_ADDR_GPS);
      return false;
    }
    INFO_GPS_LIFECYCLEF("GPS module initialized successfully at I2C address 0x%02X", I2C_ADDR_GPS);
    
    // Configure GPS module (wrapped for mutex/clock management)
    DEBUG_GPS_LIFECYCLEF("[GPS_INIT] Configuring GPS: RMC+GGA sentences, 1Hz update rate");
    i2cDeviceTransactionVoid(I2C_ADDR_GPS, 100000, 500, [&]() {
      gPA1010D->sendCommand(PMTK_SET_NMEA_OUTPUT_RMCGGA);  // RMC + GGA sentences
      gPA1010D->sendCommand(PMTK_SET_NMEA_UPDATE_1HZ);     // 1 Hz update rate
      gPA1010D->sendCommand(PGCMD_ANTENNA);                // Enable antenna status info
    });
    DEBUG_GPS_LIFECYCLEF("[GPS_INIT] GPS configuration commands sent");
    
    gGpsConnected = true;
    DEBUG_GPS_LIFECYCLEF("[GPS_INIT] gGpsConnected set to true");
    
  }
  
  gGpsEnabled = true;
  DEBUG_GPS_LIFECYCLEF("[GPS_INIT] gGpsEnabled set to true");
  
  // Create GPS task using centralized helper
  if (!createGPSTask()) {
    ERROR_GPSF("Failed to create GPS task");
    gGpsEnabled = false;
    gGpsConnected = false;
    delete gPA1010D;
    gPA1010D = nullptr;
    return false;
  }

  sensorStatusBumpWith("opengps@queue");
  DEBUG_GPS_LIFECYCLEF("[GPS_INIT] GPS module initialization complete - task is now polling");

  // Broadcast sensor status to ESP-NOW master
#if ENABLE_ESPNOW
  broadcastSensorStatus(REMOTE_SENSOR_GPS, true);
#endif
  return true;
}

// ============================================================================
// GPS Sensor Command Handlers
// ============================================================================

const char* cmd_gpsstart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (gGpsEnabled) {
    return "[GPS] Sensor already running";
  }
  
  if (isInQueue(I2C_DEVICE_GPS)) {
    if (!ensureDebugBuffer()) return "[GPS] Already queued";
    int pos = getQueuePosition(I2C_DEVICE_GPS);
    snprintf(getDebugBuffer(), 1024, "[GPS] Already queued (position %d)", pos);
    return getDebugBuffer();
  }

  if (!i2cPingAddress(I2C_ADDR_GPS, 100000, 50)) {
    return "[GPS] Not detected on I2C bus";
  }

  if (enqueueDeviceStart(I2C_DEVICE_GPS)) {
    sensorStatusBumpWith("opengps@enqueue");
    if (!ensureDebugBuffer()) return "[GPS] Sensor queued for open";
    int pos = getQueuePosition(I2C_DEVICE_GPS);
    snprintf(getDebugBuffer(), 1024, "[GPS] Sensor queued for open (position %d)", pos);
    return getDebugBuffer();
  }
  
  return "[GPS] Error: Failed to enqueue open (queue full)";
}

const char* cmd_gpsstop(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  DEBUG_GPS_LIFECYCLEF("[GPS_STOP] GPS stop command called (current enabled=%d)", gGpsEnabled ? 1 : 0);
  
  handleDeviceStopped(I2C_DEVICE_GPS);
  return "[GPS] Close requested; cleanup will complete asynchronously";
}

const char* cmd_gps(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  DEBUG_GPS_POLLINGF("[GPS_CMD] Reading GPS data (enabled=%d, task=%p)...",
                 gGpsEnabled ? 1 : 0, gGpsTaskHandle);
  
  if (!gGpsConnected || gPA1010D == nullptr) {
    return "[GPS] Error: Module not connected or initialized";
  }
  
  if (!ensureDebugBuffer()) {
    return "[GPS] Error: Debug buffer unavailable";
  }
  
  // Use BROADCAST_PRINTF for each line (zero String churn)
  broadcastOutput("GPS Data:");
  broadcastOutput("=========");
  
  // Fix status
  BROADCAST_PRINTF("Fix: %s", gPA1010D->fix ? "YES" : "NO");
  BROADCAST_PRINTF("Quality: %d", (int)gPA1010D->fixquality);
  BROADCAST_PRINTF("Satellites: %d", (int)gPA1010D->satellites);
  
  if (gPA1010D->fix) {
    // Location
    float latitude = gPA1010D->latitudeDegrees;
    float longitude = gPA1010D->longitudeDegrees;
    BROADCAST_PRINTF("Latitude: %.6f %c", latitude >= 0 ? latitude : -latitude, gPA1010D->lat);
    BROADCAST_PRINTF("Longitude: %.6f %c", longitude >= 0 ? longitude : -longitude, gPA1010D->lon);
    BROADCAST_PRINTF("Altitude: %.2f m", gPA1010D->altitude);
    BROADCAST_PRINTF("Speed: %.2f knots", gPA1010D->speed);
    BROADCAST_PRINTF("Angle: %.2f°", gPA1010D->angle);
    
    // Time
    BROADCAST_PRINTF("Time: %02d:%02d:%02d", gPA1010D->hour, gPA1010D->minute, gPA1010D->seconds);
    BROADCAST_PRINTF("Date: %02d/%02d/20%02d", gPA1010D->day, gPA1010D->month, gPA1010D->year);
  } else {
    broadcastOutput("No GPS fix - waiting for satellites...");
  }
  
  // Build compact return string for web interface (uses gDebugBuffer)
  if (gPA1010D->fix) {
    snprintf(getDebugBuffer(), 1024, "GPS Data:\n=========\nFix: YES\nQuality: %d\nSatellites: %d\nLatitude: %.6f %c\nLongitude: %.6f %c\nAltitude: %.2f m\nSpeed: %.2f knots\nAngle: %.2f°\nTime: %02d:%02d:%02d\nDate: %02d/%02d/20%02d",
             (int)gPA1010D->fixquality, (int)gPA1010D->satellites,
             gPA1010D->latitudeDegrees >= 0 ? gPA1010D->latitudeDegrees : -gPA1010D->latitudeDegrees, gPA1010D->lat,
             gPA1010D->longitudeDegrees >= 0 ? gPA1010D->longitudeDegrees : -gPA1010D->longitudeDegrees, gPA1010D->lon,
             gPA1010D->altitude, gPA1010D->speed, gPA1010D->angle,
             gPA1010D->hour, gPA1010D->minute, gPA1010D->seconds,
             gPA1010D->day, gPA1010D->month, gPA1010D->year);
  } else {
    snprintf(getDebugBuffer(), 1024, "GPS Data:\n=========\nFix: NO\nQuality: %d\nSatellites: %d\nNo GPS fix - waiting for satellites...",
             (int)gPA1010D->fixquality, (int)gPA1010D->satellites);
  }
  
  return getDebugBuffer();
}

// ============================================================================
// GPS Task Implementation (moved from i2c_system.cpp for full modularization)
// ============================================================================

// ============================================================================
// GPS Task - FreeRTOS Task Function
// ============================================================================
// Purpose: Continuously reads NMEA data from PA1010D GPS module
// Stack: 4608 words (~18KB) | Priority: 1 | Core: Any
// Lifecycle: Created by cmd_gpsstart, deleted when gGpsEnabled=false
// Polling: Configurable via gpsDevicePollMs (default 200ms) | I2C Clock: 100kHz
//
// Cleanup Strategy:
//   1. Check gGpsEnabled flag at loop start
//   2. Acquire bus mutex via I2CDeviceManager to prevent race conditions during cleanup
//   3. Delete GPS module object
//   4. Release mutex and delete task
// ============================================================================

// Build GPS JSON directly into buffer from cache. Safe to call from any task —
// only reads gGpsCache under its own mutex. Returns bytes written (excluding \0)
// or 0 on cache-lock timeout / invalid buffer.
int gpsBuildDataJSON(char* buf, size_t bufSize) {
  if (!buf || bufSize == 0) return 0;

  SensorCacheGuard g(gGpsCache.mutex, pdMS_TO_TICKS(50), "gps.buildJSON");
  if (!g.held) return 0;

  if (gGpsCache.hasFix) {
    return snprintf(buf, bufSize,
                    "{\"val\":1,\"fix\":1,\"quality\":%d,\"sats\":%d,"
                    "\"lat\":%.6f,\"lon\":%.6f,\"alt\":%.2f,\"speed\":%.2f}",
                    (int)gGpsCache.fixQuality, (int)gGpsCache.satellites,
                    gGpsCache.latitude, gGpsCache.longitude,
                    gGpsCache.altitude, gGpsCache.speed);
  }
  return snprintf(buf, bufSize,
                  "{\"val\":1,\"fix\":0,\"quality\":0,\"sats\":%d,"
                  "\"lat\":0,\"lon\":0,\"alt\":0,\"speed\":0}",
                  (int)gGpsCache.satellites);
}

void gpsTask(void* parameter) {
  INFO_GPS_LIFECYCLEF("Task started (handle=%p, stack=%u words)",
                (void*)xTaskGetCurrentTaskHandle(),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  INFO_GPS_LIFECYCLEF("[MODULAR] gpsTask() running from Sensor_GPS_PA1010D.cpp");
  unsigned long lastStackLog = 0;
  unsigned long lastStatusLog = 0;
  unsigned long lastGPSRead = 0;
  bool wasPolling = false;
  
  while (true) {
    // CRITICAL: Check enabled flag FIRST for graceful shutdown
    if (!gGpsEnabled) {
      gGpsConnected = false;
      if (gPA1010D != nullptr) {
        delete gPA1010D;
        gPA1010D = nullptr;
      }
      SENSOR_TASK_EXIT(GPS);
    }

    // Stack watermark tracking + safety bailout
    unsigned long nowMs = millis();
    if ((nowMs - lastStackLog) >= 30000) {
      lastStackLog = nowMs;
      if (checkTaskStackSafety("gps", GPS_STACK_WORDS, &gGpsEnabled)) break;
      if (gGpsEnabled && isDebugFlagSet(DEBUG_PERFORMANCE)) {
        UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
        DEBUG_PERFORMANCEF("[STACK] gps_task watermark=%u words", (unsigned)watermark);
      }
      if (gGpsEnabled && isDebugFlagSet(DEBUG_MEMORY)) {
        DEBUG_MEMORY_HEAPF("[HEAP] gps_task: free=%u min=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
      }
    }
    
    if (gGpsEnabled && gGpsConnected && gPA1010D != nullptr && !gSensorPollingPaused) {
      // gpsPollMs gates only the I2C health probe — NOT the NMEA read.
      // Adafruit_GPS is designed for read() to be called once per loop iteration
      // (~1ms cadence ideally). Our 10ms vTaskDelay approximates that.
      const unsigned long gpsPollMs = (gSettings.gpsDevicePollMs > 0) ? (unsigned long)gSettings.gpsDevicePollMs : 200;

      if (!wasPolling) {
        DEBUG_GPS_LIFECYCLEF("[GPS_TASK] Started active polling (probe every %lums, read every tick)", gpsPollMs);
        wasPolling = true;
        lastStatusLog = nowMs;
      }

      if ((nowMs - lastStatusLog) >= 30000) {
        DEBUG_GPS_VALUESF("[GPS_TASK] Active polling - fix=%d sats=%d quality=%d",
                       gPA1010D->fix ? 1 : 0, (int)gPA1010D->satellites, (int)gPA1010D->fixquality);
        lastStatusLog = nowMs;
      }

      // ── I2C health probe (rate-limited) ──────────────────────────────────
      // Separate from reading so the health/auto-disable system still works.
      if ((nowMs - lastGPSRead) >= gpsPollMs) {
        lastGPSRead = nowMs;
        auto probeResult = i2cTaskWithTimeout(I2C_ADDR_GPS, 100000, 100, [&]() -> bool {
          Wire1.beginTransmission(I2C_ADDR_GPS);
          return Wire1.endTransmission() == 0;
        });
        if (!probeResult) {
          if (i2cShouldAutoDisable(I2C_ADDR_GPS)) {
            ERROR_GPSF("Too many consecutive GPS failures - auto-disabling");
            gGpsEnabled = false;
            sensorStatusBumpWith("gps@auto_disabled");
          }
        }
      }

      // ── NMEA read — once per tick (Adafruit's "call once per loop" pattern) ──
      // In I2C mode available() always returns 1 regardless of actual data, so
      // do NOT use while(available()) — it never terminates. Instead call read()
      // once; it returns 0 when the PA1010D has only filler bytes (no real data).
      bool parsedNewSentence = false;
      char c = gPA1010D->read();
      if (c && gPA1010D->newNMEAreceived()) {
        gPA1010D->parse(gPA1010D->lastNMEA());
        parsedNewSentence = true;
      }

      // Only update GPS cache when a new NMEA sentence was actually parsed.
      // Previously dataValid was set unconditionally, causing the sensor logger to
      // write the same stale position repeatedly between real fixes.
      if (parsedNewSentence) {
        SensorCacheGuard g(gGpsCache.mutex, pdMS_TO_TICKS(50), "gps.pollWrite");
        if (g.held) {
          gGpsCache.latitude = gPA1010D->latitudeDegrees;
          gGpsCache.longitude = gPA1010D->longitudeDegrees;
          gGpsCache.altitude = gPA1010D->altitude;
          gGpsCache.speed = gPA1010D->speed;
          gGpsCache.angle = gPA1010D->angle;
          gGpsCache.hasFix = gPA1010D->fix;
          gGpsCache.fixQuality = gPA1010D->fixquality;
          gGpsCache.satellites = gPA1010D->satellites;
          gGpsCache.year = 2000 + gPA1010D->year;
          gGpsCache.month = gPA1010D->month;
          gGpsCache.day = gPA1010D->day;
          gGpsCache.hour = gPA1010D->hour;
          gGpsCache.minute = gPA1010D->minute;
          gGpsCache.second = gPA1010D->seconds;
          gGpsCache.dataValid = true;
          gGpsCache.lastUpdate = nowMs;

          // Feed live track directly from GPS task (independent of sensor logging)
#if ENABLE_MAPS
          if (gPA1010D->fix && GPSTrackManager::isLiveTracking()) {
            GPSTrackManager::appendPoint(gPA1010D->latitudeDegrees, gPA1010D->longitudeDegrees);
          }
#endif
        }

        // ESP-NOW broadcaster reads gpsBuildDataJSON() on demand from gGpsCache.

        // Mark OLED dirty if GPS page is active (enables real-time display updates)
#if ENABLE_OLED_DISPLAY
        if (currentOLEDMode == OLED_GPS_DATA) {
          oledMarkDirty();
        }
#endif
      }

      vTaskDelay(pdMS_TO_TICKS(10));
      drainDebugRing();
    } else {
      if (wasPolling && (!gGpsEnabled || !gGpsConnected || gPA1010D == nullptr)) {
        // Only log stop when sensor is actually disabled/disconnected,
        // not for brief gSensorPollingPaused toggles from web requests
        DEBUG_GPS_LIFECYCLEF("[GPS_TASK] Stopped active polling - entering idle mode");
        wasPolling = false;
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      drainDebugRing();
    }
  }
}

// ============================================================================
// GPS Accessor Functions (for MQTT and other modules)
// ============================================================================

bool hasGPSFix() {
  return gPA1010D && gPA1010D->fix;
}

float getGPSLatitude() {
  if (!gPA1010D || !gPA1010D->fix) return 0.0f;
  float lat = gPA1010D->latitudeDegrees;
  if (gPA1010D->lat == 'S') lat = -lat;
  return lat;
}

float getGPSLongitude() {
  if (!gPA1010D || !gPA1010D->fix) return 0.0f;
  float lon = gPA1010D->longitudeDegrees;
  if (gPA1010D->lon == 'W') lon = -lon;
  return lon;
}

float getGPSAltitude() {
  if (!gPA1010D || !gPA1010D->fix) return 0.0f;
  return gPA1010D->altitude;
}

float getGPSSpeed() {
  if (!gPA1010D || !gPA1010D->fix) return 0.0f;
  return gPA1010D->speed * 1.852f;  // Convert knots to km/h
}

int getGPSSatellites() {
  if (!gPA1010D) return 0;
  return (int)gPA1010D->satellites;
}

// ============================================================================
// GPS Modular Settings Registration (for safety and reliability)
// ============================================================================

// GPS settings entries
// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry gpsSettingEntries[] = {
  { "gpsAutoStart", SETTING_BOOL, &gSettings.gpsAutoStart, 0, 0, nullptr, 0, 1, "Auto-start after boot", nullptr, false, nullptr, nullptr },
  { "gpsDevicePollMs", SETTING_INT, &gSettings.gpsDevicePollMs, 200, 0, nullptr, 50, 10000, "Poll Interval (ms)", nullptr, false, nullptr, nullptr }
};

static bool isGPSConnected() {
  return gGpsConnected;
}

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule gpsSettingsModule = {
  "gps",
  "hardware.sensors.gps",
  gpsSettingEntries,
  sizeof(gpsSettingEntries) / sizeof(gpsSettingEntries[0]),
  isGPSConnected,
  "PA1010D GPS receiver"
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp

// ============================================================================
// GPS Command Registry
// ============================================================================

const char* cmd_gpsautostart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput; arg.trim();
  if (arg.length() == 0) {
    return gSettings.gpsAutoStart ? "[GPS] Auto-start: enabled" : "[GPS] Auto-start: disabled";
  }
  arg.toLowerCase();
  if (arg == "on" || arg == "true" || arg == "1") {
    setSetting(gSettings.gpsAutoStart, true);
    return "[GPS] Auto-start enabled";
  } else if (arg == "off" || arg == "false" || arg == "0") {
    setSetting(gSettings.gpsAutoStart, false);
    return "[GPS] Auto-start disabled";
  }
  return "Usage: gpsautostart [on|off]";
}

// One-shot command: persist all GPS logging settings AND start everything immediately.
// Usage: gpslog [interval_ms]   (default 1000ms)
const char* cmd_gpslog(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // Parse optional interval argument
  String arg = argsInput;
  arg.trim();
  uint32_t intervalMs = 1000;
  if (arg.length() > 0) {
    long parsed = arg.toInt();
    if (parsed >= 100 && parsed <= 3600000) {
      intervalMs = (uint32_t)parsed;
    } else {
      return "Usage: gpslog [interval_ms]  (default 1000, min 100)";
    }
  }

  // ── 1. Persist all settings via their own command handlers ─────────────
  // Using command-in-command (not raw setSetting) so validation and any
  // side-effects in each handler run exactly as they would for a user call.
  // There is no recursion: none of these handlers call back to cmd_gpslog.
  cmd_gpsautostart("on");           // persists gSettings.gpsAutoStart
  cmd_sensorlog("autostart on");    // persists gSettings.sensorLogAutoStart

  // "format track" already forces gSensorLogMask = LOG_GPS internally,
  // so a separate "sensors gps" call would be redundant.
  cmd_sensorlog("format track");

  // No "interval" subcommand exists in sensorlog, so set both the persisted
  // setting and the live runtime variable directly.
  setSetting(gSettings.sensorLogIntervalMs, (int)intervalMs);
  gSensorLogIntervalMs = intervalMs;

  // ── 2. Start GPS sensor now if not already running ───────────────────────
  bool gpsWasRunning = gGpsEnabled;
  if (!gpsWasRunning) {
    cmd_gpsstart("");
  }

  // ── 3. Start sensor logging now for this boot ────────────────────────────
  // sensorLogAutoStart() creates the timestamped file + directories and
  // calls cmd_sensorlog("start ...") internally — identical to what would
  // happen on the next boot automatically.
  if (!gSensorLoggingEnabled) {
    sensorLogAutoStart();
  }

  EXT_RAM_BSS_ATTR static char result[300];
  snprintf(result, sizeof(result),
    "[gpslog] Active — interval=%lums, autostart=on\n"
    "  GPS:    %s\n"
    "  Log:    %s",
    (unsigned long)intervalMs,
    gpsWasRunning ? "was already running" : "started now",
    gSensorLoggingEnabled ? gSensorLogPath.c_str() : "FAILED to start — check serial output"
  );
  return result;
}

// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
const CommandEntry gpsCommands[] = {
  // 3-level voice: "sensor" -> "GPS" -> "open/close"
  { "opengps", "Start PA1010D GPS module.", false, cmd_gpsstart, nullptr, "sensor", "GPS", "open" },
  { "closegps", "Stop PA1010D GPS module.", false, cmd_gpsstop, nullptr, "sensor", "GPS", "close" },
  { "gpsread", "Read GPS location and time data.", false, cmd_gps },
  
  // Auto-start
  { "gpsautostart", "Enable/disable GPS auto-start after boot [on|off]", false, cmd_gpsautostart, "Usage: gpsautostart [on|off]" },

  // One-shot GPS logging setup: persists all settings AND starts immediately
  { "gpslog", "Set up and start GPS track logging now (persists across boots). Usage: gpslog [interval_ms]", false, cmd_gpslog,
    "Usage: gpslog [interval_ms]\n"
    "  Sets gpsAutoStart, sensorlog format=track, sensors=gps, and autostart,\n"
    "  then starts both the GPS sensor and sensor logging immediately.\n"
    "  interval_ms: log interval in ms (default 1000, min 100)\n"
    "  Example: gpslog        (1-second logging)\n"
    "           gpslog 500   (500ms logging)" },
};

const size_t gpsCommandsCount = sizeof(gpsCommands) / sizeof(gpsCommands[0]);

// Registration handled by gCommandModules[] in System_Utils.cpp

// ============================================================================
// GPS OLED Mode (Display Function + Registration)
// ============================================================================
#if DISPLAY_TYPE > 0
#include "i2csensor_pa1010d_oled.h"
#endif

#endif // ENABLE_GPS_SENSOR
