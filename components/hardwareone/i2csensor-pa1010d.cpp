#include "i2csensor-pa1010d.h"
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
#include "System_MemoryMonitor.h"
#include "System_Settings.h"
#include "System_TaskUtils.h"

// Task handle (owned by this module)
TaskHandle_t gpsTaskHandle = nullptr;

// GPS module object (owned by this module)
Adafruit_GPS* gPA1010D = nullptr;

// External dependencies provided by System_I2C.h:
// sensorStatusBumpWith, gSensorPollingPaused, i2cMutex, drainDebugRing

// GPS sensor state (definition - matching pattern of thermal/tof/imu/gamepad sensors)
bool gpsEnabled = false;
bool gpsConnected = false;
unsigned long gpsLastStopTime = 0;

// GPS cache for thread-safe data access (mutex created in setup())
GPSCache gGPSCache = {
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
void startGPSInternal() {
  INFO_SENSORSF("Starting GPS initialization...");
  
  if (gpsEnabled) {
    DEBUG_SENSORSF("[GPS_INIT] GPS already started (enabled=1)");
    return;
  }

  // Check memory before creating GPS task
  if (!checkMemoryAvailable("gps", nullptr)) {
    ERROR_SENSORSF("[GPS_INIT] Insufficient memory for GPS sensor");
    return;
  }
  
  // Initialize GPS module if not already done
  if (!gpsConnected || gPA1010D == nullptr) {
    DEBUG_SENSORSF("[GPS_INIT] Allocating Adafruit_GPS object on Wire1...");
    gPA1010D = new Adafruit_GPS(&Wire1);
    if (!gPA1010D) {
      ERROR_SENSORSF("Failed to allocate GPS module");
      return;
    }
    DEBUG_SENSORSF("[GPS_INIT] GPS object allocated at %p", gPA1010D);
    
    DEBUG_SENSORSF("[GPS_INIT] Calling gPA1010D->begin(0x10)...");
    
    // Retry GPS initialization with delays (GPS needs time after power-on)
    bool initSuccess = false;
    for (int retry = 0; retry < 3; retry++) {
      if (retry > 0) {
        DEBUG_SENSORSF("[GPS_INIT] Retry %d/3 after 200ms delay...", retry);
        delay(200);
      }
      if (gPA1010D->begin(0x10)) {
        initSuccess = true;
        break;
      }
    }
    
    if (!initSuccess) {
      delete gPA1010D;
      gPA1010D = nullptr;
      gpsConnected = false;
      ERROR_SENSORSF("Failed to initialize GPS module at 0x10 after 3 attempts");
      return;
    }
    INFO_SENSORSF("GPS module initialized successfully at I2C address 0x10");
    
    // Configure GPS module
    DEBUG_SENSORSF("[GPS_INIT] Configuring GPS: RMC+GGA sentences, 1Hz update rate");
    gPA1010D->sendCommand(PMTK_SET_NMEA_OUTPUT_RMCGGA);  // RMC + GGA sentences
    gPA1010D->sendCommand(PMTK_SET_NMEA_UPDATE_1HZ);     // 1 Hz update rate
    gPA1010D->sendCommand(PGCMD_ANTENNA);                // Enable antenna status info
    DEBUG_SENSORSF("[GPS_INIT] GPS configuration commands sent");
    
    gpsConnected = true;
    DEBUG_SENSORSF("[GPS_INIT] gpsConnected set to true");
    
  }
  
  gpsEnabled = true;
  DEBUG_SENSORSF("[GPS_INIT] gpsEnabled set to true");
  
  // Start GPS polling task if not already running
  // Check for stale task handle (task deleted itself but handle not cleared)
  if (gpsTaskHandle != nullptr) {
    eTaskState state = eTaskGetState(gpsTaskHandle);
    if (state == eDeleted || state == eInvalid) {
      gpsTaskHandle = nullptr;
    }
  }
  if (gpsTaskHandle == nullptr) {
    DEBUG_SENSORSF("[GPS_INIT] Creating GPS polling task...");
    BaseType_t result = xTaskCreatePinnedToCore(
      gpsTask,
      "gps_task",
      GPS_STACK_WORDS,  // Stack size in words; ~12KB (reduced from 16KB - peak usage ~5KB)
      nullptr,
      1,  // Priority
      &gpsTaskHandle,
      1  // Core 1 (same as other sensor tasks)
    );
    if (result != pdPASS) {
      ERROR_SENSORSF("Failed to create GPS task (result=%d)", result);
      gpsEnabled = false;
      gpsConnected = false;
      delete gPA1010D;
      gPA1010D = nullptr;
      return;
    }
    DEBUG_SENSORSF("[GPS_INIT] GPS polling task created successfully (handle=%p)", gpsTaskHandle);
  } else {
    DEBUG_SENSORSF("[GPS_INIT] GPS task already running (handle=%p)", gpsTaskHandle);
  }
  
  sensorStatusBumpWith("opengps@queue");
  DEBUG_SENSORSF("[GPS_INIT] GPS module initialization complete - task is now polling");
  
  // Broadcast sensor status to ESP-NOW master
#if ENABLE_ESPNOW
  broadcastSensorStatus(REMOTE_SENSOR_GPS, true);
#endif
}

// ============================================================================
// GPS Sensor Command Handlers
// ============================================================================

const char* cmd_gpsstart(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (gpsEnabled) {
    return "[GPS] Sensor already running";
  }
  
  if (isInQueue(I2C_DEVICE_GPS)) {
    if (!ensureDebugBuffer()) return "[GPS] Already queued";
    int pos = getQueuePosition(I2C_DEVICE_GPS);
    snprintf(getDebugBuffer(), 1024, "[GPS] Already queued (position %d)", pos);
    return getDebugBuffer();
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

const char* cmd_gpsstop(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  DEBUG_SENSORSF("[GPS_STOP] GPS stop command called (current enabled=%d)", gpsEnabled ? 1 : 0);
  
  handleDeviceStopped(I2C_DEVICE_GPS);
  return "[GPS] Close requested; cleanup will complete asynchronously";
}

const char* cmd_gps(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  DEBUG_SENSORSF("[GPS_CMD] Reading GPS data (enabled=%d, task=%p)...", 
                 gpsEnabled ? 1 : 0, gpsTaskHandle);
  
  if (!gpsConnected || gPA1010D == nullptr) {
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
// Lifecycle: Created by cmd_gpsstart, deleted when gpsEnabled=false
// Polling: Configurable via gpsDevicePollMs (default 1000ms) | I2C Clock: 100kHz
//
// Cleanup Strategy:
//   1. Check gpsEnabled flag at loop start
//   2. Acquire i2cMutex to prevent race conditions during cleanup
//   3. Delete GPS module object
//   4. Release mutex and delete task
// ============================================================================

void gpsTask(void* parameter) {
  INFO_SENSORSF("[GPS] Task started (handle=%p, stack=%u words)", 
                (void*)xTaskGetCurrentTaskHandle(), 
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  INFO_SENSORSF("[MODULAR] gpsTask() running from Sensor_GPS_PA1010D.cpp");
  unsigned long lastStackLog = 0;
  unsigned long lastStatusLog = 0;
  unsigned long lastGPSRead = 0;
  bool wasPolling = false;
  
  while (true) {
    // CRITICAL: Check enabled flag FIRST for graceful shutdown
    if (!gpsEnabled) {
      gpsConnected = false;
      if (gPA1010D != nullptr) {
        delete gPA1010D;
        gPA1010D = nullptr;
      }
      INFO_SENSORSF("[GPS] Task disabled - cleaning up and deleting");
      // NOTE: Do NOT clear gpsTaskHandle here - let create function use eTaskGetState()
      // to detect stale handles. Clearing here creates a race condition window.
      vTaskDelete(nullptr);
    }

    // Stack watermark tracking + safety bailout
    unsigned long nowMs = millis();
    if ((nowMs - lastStackLog) >= 30000) {
      lastStackLog = nowMs;
      if (checkTaskStackSafety("gps", GPS_STACK_WORDS, &gpsEnabled)) break;
      if (gpsEnabled && isDebugFlagSet(DEBUG_PERFORMANCE)) {
        UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
        DEBUG_PERFORMANCEF("[STACK] gps_task watermark=%u words", (unsigned)watermark);
      }
      if (gpsEnabled && isDebugFlagSet(DEBUG_MEMORY)) {
        DEBUG_MEMORYF("[HEAP] gps_task: free=%u min=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
      }
    }
    
    if (gpsEnabled && gpsConnected && gPA1010D != nullptr && !gSensorPollingPaused) {
      unsigned long gpsPollMs = (gSettings.gpsDevicePollMs > 0) ? (unsigned long)gSettings.gpsDevicePollMs : 1000;
      
      if (!wasPolling) {
        DEBUG_SENSORSF("[GPS_TASK] Started active polling - reading NMEA data every %lums", gpsPollMs);
        wasPolling = true;
        lastStatusLog = nowMs;
      }
      
      if ((nowMs - lastStatusLog) >= 30000) {
        DEBUG_SENSORSF("[GPS_TASK] Active polling - fix=%d sats=%d quality=%d",
                       gPA1010D->fix ? 1 : 0, (int)gPA1010D->satellites, (int)gPA1010D->fixquality);
        lastStatusLog = nowMs;
      }
      
      if ((nowMs - lastGPSRead) >= gpsPollMs) {
        // GPS reads ~10ms at 100kHz; fail fast and retry next poll rather than blocking 1000ms
        auto result = i2cTaskWithTimeout(I2C_ADDR_GPS, 100000, 100, [&]() -> bool {
          if ((nowMs - lastGPSRead) >= gpsPollMs) {
            gPA1010D->read();
            if (gPA1010D->newNMEAreceived()) {
              gPA1010D->parse(gPA1010D->lastNMEA());
            }
            lastGPSRead = nowMs;
            
            // Update GPS cache for thread-safe access from web/OLED
            if (gGPSCache.mutex && xSemaphoreTake(gGPSCache.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
              gGPSCache.latitude = gPA1010D->latitudeDegrees;
              gGPSCache.longitude = gPA1010D->longitudeDegrees;
              gGPSCache.altitude = gPA1010D->altitude;
              gGPSCache.speed = gPA1010D->speed;
              gGPSCache.angle = gPA1010D->angle;
              gGPSCache.hasFix = gPA1010D->fix;
              gGPSCache.fixQuality = gPA1010D->fixquality;
              gGPSCache.satellites = gPA1010D->satellites;
              gGPSCache.year = 2000 + gPA1010D->year;
              gGPSCache.month = gPA1010D->month;
              gGPSCache.day = gPA1010D->day;
              gGPSCache.hour = gPA1010D->hour;
              gGPSCache.minute = gPA1010D->minute;
              gGPSCache.second = gPA1010D->seconds;
              gGPSCache.dataValid = true;
              gGPSCache.lastUpdate = nowMs;
              xSemaphoreGive(gGPSCache.mutex);
            }
            
            // Stream data to ESP-NOW master if enabled (worker devices only)
#if ENABLE_ESPNOW
            // Check mesh mode (worker role) OR bond mode (worker role)
            bool shouldStream = false;
            if (meshEnabled() && gSettings.meshRole != MESH_ROLE_MASTER) {
              shouldStream = true;
            }
#if ENABLE_BONDED_MODE
            if (gSettings.bondModeEnabled && gSettings.bondRole == 0) {
              shouldStream = true;  // Bond mode worker
            }
#endif
            
            if (shouldStream) {
              char gpsJson[256];
              int jsonLen;
              if (gPA1010D->fix) {
                jsonLen = snprintf(gpsJson, sizeof(gpsJson),
                                   "{\"val\":1,\"fix\":%d,\"quality\":%d,\"sats\":%d,\"lat\":%.6f,\"lon\":%.6f,\"alt\":%.2f,\"speed\":%.2f}",
                                   1, (int)gPA1010D->fixquality, (int)gPA1010D->satellites,
                                   gPA1010D->latitudeDegrees, gPA1010D->longitudeDegrees,
                                   gPA1010D->altitude, gPA1010D->speed);
              } else {
                // No fix yet - stream status so master knows GPS is active but acquiring
                jsonLen = snprintf(gpsJson, sizeof(gpsJson),
                                   "{\"val\":1,\"fix\":0,\"quality\":0,\"sats\":%d,\"lat\":0,\"lon\":0,\"alt\":0,\"speed\":0}",
                                   (int)gPA1010D->satellites);
              }
              if (jsonLen > 0 && jsonLen < 256) {
                sendSensorDataUpdate(REMOTE_SENSOR_GPS, String(gpsJson));
              }
            }
#endif
          }
          return true;  // Assume success for void operation
        });
        
        lastGPSRead = nowMs;
        
        // Mark OLED dirty if GPS page is active (enables real-time display updates)
        if (result && currentOLEDMode == OLED_GPS_DATA) {
          oledMarkDirty();
        }
        
        // Auto-disable if too many consecutive failures
        if (!result) {
          if (i2cShouldAutoDisable(I2C_ADDR_GPS, 5)) {
            ERROR_SENSORSF("Too many consecutive GPS failures - auto-disabling");
            gpsEnabled = false;
            sensorStatusBumpWith("gps@auto_disabled");
          }
        }
      }
      
      vTaskDelay(pdMS_TO_TICKS(10));
      drainDebugRing();
    } else {
      if (wasPolling && (!gpsEnabled || !gpsConnected || gPA1010D == nullptr)) {
        // Only log stop when sensor is actually disabled/disconnected,
        // not for brief gSensorPollingPaused toggles from web requests
        DEBUG_SENSORSF("[GPS_TASK] Stopped active polling - entering idle mode");
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
static const SettingEntry gpsSettingEntries[] = {
  { "gpsAutoStart",    SETTING_BOOL, &gSettings.gpsAutoStart,    0, 0, nullptr, 0, 1, "Auto-start after boot", nullptr },
  { "gpsDevicePollMs", SETTING_INT,  &gSettings.gpsDevicePollMs, 1000, 0, nullptr, 100, 10000, "Poll Interval (ms)", nullptr }
};

static bool isGPSConnected() {
  return gpsConnected;
}

extern const SettingsModule gpsSettingsModule = {
  "gps",
  "gps_pa1010d",
  gpsSettingEntries,
  sizeof(gpsSettingEntries) / sizeof(gpsSettingEntries[0]),
  isGPSConnected,
  "PA1010D GPS sensor settings"
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp

// ============================================================================
// GPS Command Registry
// ============================================================================

const char* cmd_gpsautostart(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = args; arg.trim();
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

const CommandEntry gpsCommands[] = {
  // 3-level voice: "sensor" -> "GPS" -> "open/close"
  { "opengps", "Start PA1010D GPS module.", false, cmd_gpsstart, nullptr, "sensor", "GPS", "open" },
  { "closegps", "Stop PA1010D GPS module.", false, cmd_gpsstop, nullptr, "sensor", "GPS", "close" },
  { "gpsread", "Read GPS location and time data.", false, cmd_gps },
  
  // Auto-start
  { "gpsautostart", "Enable/disable GPS auto-start after boot [on|off]", false, cmd_gpsautostart, "Usage: gpsautostart [on|off]" },
};

const size_t gpsCommandsCount = sizeof(gpsCommands) / sizeof(gpsCommands[0]);

// Auto-register with command system
static CommandModuleRegistrar _gps_cmd_registrar(gpsCommands, gpsCommandsCount, "gps");

// ============================================================================
// GPS OLED Mode (Display Function + Registration)
// ============================================================================
#if DISPLAY_TYPE > 0
#include "i2csensor-pa1010d-oled.h"
#endif

#endif // ENABLE_GPS_SENSOR
