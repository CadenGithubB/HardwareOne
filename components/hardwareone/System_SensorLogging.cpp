/**
 * Sensor Logging System - Data logging for sensor readings
 * 
 * Provides configurable logging of sensor data to files with:
 * - Selectable sensors (thermal, tof, imu, gamepad, apds)
 * - Configurable intervals and file sizes
 * - Text and CSV output formats
 * - Log rotation support
 */

#include "System_SensorLogging.h"
#include "System_Utils.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_Mutex.h"
#include "System_I2C.h"
#include "System_MemUtil.h"
#include "System_TaskUtils.h"
#include <LittleFS.h>

// Conditional sensor includes (same approach as main .ino)
#include "System_BuildConfig.h"
#if ENABLE_THERMAL_SENSOR
  #include "i2csensor-mlx90640.h"
#endif
#if ENABLE_IMU_SENSOR
  #include "i2csensor-bno055.h"
#endif
#if ENABLE_TOF_SENSOR
  #include "i2csensor-vl53l4cx.h"
#endif
#if ENABLE_GAMEPAD_SENSOR
  #include "i2csensor-seesaw.h"
#endif
#if ENABLE_APDS_SENSOR
  #include "i2csensor-apds9960.h"
#endif
#include "System_SensorStubs.h"  // Provides stubs for disabled sensors

// External dependencies
extern bool ensureDebugBuffer();
extern void broadcastOutput(const String& msg);
extern void broadcastOutput(const char* s);
extern void getTimestampPrefixMsCached(char* out, size_t outSize);

// Modular sensor caches (conditionally available based on enabled sensors)

// Validation macro
#define RETURN_VALID_IF_VALIDATE_CSTR() \
  do { \
    extern bool gCLIValidateOnly; \
    if (gCLIValidateOnly) return "VALID"; \
  } while(0)

// ============================================================================
// Sensor Logging State Variables
// ============================================================================

bool gSensorLoggingEnabled = false;
String gSensorLogPath = "";
unsigned long gSensorLogLastWrite = 0;
uint32_t gSensorLogIntervalMs = 5000;
size_t gSensorLogMaxSize = 250 * 1024;
TaskHandle_t gSensorLogTaskHandle = nullptr;
bool gSensorLogFormatCSV = false;
uint8_t gSensorLogMaxRotations = 3;
uint8_t gSensorLogMask = 0x00;

// ============================================================================
// Log Line Builders
// ============================================================================

const char* buildSensorLogLine() {
  static char* logBuffer = nullptr;
  if (!logBuffer) {
    logBuffer = (char*)ps_alloc(512, AllocPref::PreferPSRAM, "sensor.log.buf");
    if (!logBuffer) return "";
  }

  char* pos = logBuffer;
  int remaining = 512;
  int written = 0;

  // Timestamp prefix
  char tsPrefix[48];
  getTimestampPrefixMsCached(tsPrefix, sizeof(tsPrefix));
  if (tsPrefix[0]) {
    written = snprintf(pos, remaining, "%s", tsPrefix);
  } else {
    written = snprintf(pos, remaining, "[BOOT ms=%lu] | ", millis());
  }
  pos += written;
  remaining -= written;

  // Debug: Show sensor states (conditional compilation)
  if (isDebugFlagSet(DEBUG_STORAGE)) {
    String sensorStates = "Sensor states: ";
#if ENABLE_THERMAL_SENSOR
    sensorStates += String("thermal(en=") + (thermalEnabled ? "1" : "0") + 
                   " conn=" + (thermalConnected ? "1" : "0") + 
                   " valid=" + (gThermalCache.thermalDataValid ? "1" : "0") + ") ";
#endif
#if ENABLE_TOF_SENSOR
    sensorStates += String("tof(en=") + (tofEnabled ? "1" : "0") + 
                   " conn=" + (tofConnected ? "1" : "0") + 
                   " valid=" + (gTofCache.tofDataValid ? "1" : "0") + ") ";
#endif
#if ENABLE_IMU_SENSOR
    sensorStates += String("imu(en=") + (imuEnabled ? "1" : "0") + 
                   " conn=" + (imuConnected ? "1" : "0") + ") ";
#endif
    if (sensorStates == "Sensor states: ") {
      sensorStates += "no sensors enabled";
    }
    DEBUGF_BROADCAST(DEBUG_STORAGE, "%s", sensorStates.c_str());
  }

#if ENABLE_THERMAL_SENSOR
  // Thermal data
  if (thermalEnabled && thermalConnected && gThermalCache.thermalDataValid && remaining > 0) {
    written = snprintf(pos, remaining, "thermal: min=%.1fC avg=%.1fC max=%.1fC | ",
                       gThermalCache.thermalMinTemp, gThermalCache.thermalAvgTemp, gThermalCache.thermalMaxTemp);
    pos += written;
    remaining -= written;
  }
#endif

#if ENABLE_TOF_SENSOR
  // ToF data
  if (tofEnabled && tofConnected && gTofCache.tofDataValid && remaining > 0) {
    written = snprintf(pos, remaining, "tof: ");
    pos += written;
    remaining -= written;

    for (int i = 0; i < gTofCache.tofTotalObjects && i < 4 && remaining > 0; i++) {
      if (gTofCache.tofObjects[i].valid) {
        written = snprintf(pos, remaining, "obj%d=%dmm ", i, (int)gTofCache.tofObjects[i].distance_mm);
        pos += written;
        remaining -= written;
      }
    }

    if (remaining > 2) {
      written = snprintf(pos, remaining, "| ");
      pos += written;
      remaining -= written;
    }
  }
#endif

#if ENABLE_IMU_SENSOR
  // IMU data
  if (imuEnabled && imuConnected && remaining > 0) {
    written = snprintf(pos, remaining, "imu: yaw=%.1f pitch=%.1f roll=%.1f accel=(%.2f,%.2f,%.2f) | ",
                       gImuCache.oriYaw, gImuCache.oriPitch, gImuCache.oriRoll,
                       gImuCache.accelX, gImuCache.accelY, gImuCache.accelZ);
    pos += written;
    remaining -= written;
  }
#endif

  // Check if any sensor data was added (line ends with " | ")
  int len = strlen(logBuffer);
  bool hasSensorData = (len >= 3 && strcmp(logBuffer + len - 3, " | ") == 0);

  if (hasSensorData) {
    // Remove trailing " | "
    logBuffer[len - 3] = '\0';
  } else {
    // No sensor data - add message
    if (remaining > 0) {
      snprintf(pos, remaining, " no active sensors");
    }
  }

  return logBuffer;
}

// ============================================================================
// Sensor Logging Task
// ============================================================================

void sensorLogTask(void* parameter) {
  // Use DEBUG_SYSTEMF for consistent output routing
  DEBUG_SYSTEMF("Sensor log task started: path=%s interval=%lums",
                gSensorLogPath.c_str(), (unsigned long)gSensorLogIntervalMs);

  // Monitor stack usage (only when debug enabled)
  if (isDebugFlagSet(DEBUG_SYSTEM)) {
    UBaseType_t stackHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
    DEBUG_SYSTEMF("Sensor log task initial stack watermark: %d words", stackHighWaterMark);
  }

  uint32_t writeCount = 0;
  TickType_t lastWake = xTaskGetTickCount();
  unsigned long lastStackLog = 0;

  // Diagnostics counters
  static uint32_t log_writes = 0;
  static uint32_t log_open_fail = 0;
  static uint32_t log_lock_fail = 0;
  static uint32_t log_idle_skips = 0;
  static uint32_t log_trunc = 0;
  static unsigned long lastSummaryMs = 0;
  static size_t approxSizeBytes = 0;
  static unsigned long lastTruncateMs = 0;
  const unsigned long truncateCooldownMs = 5000;

  // Local builder - respects sensor selection mask
  auto buildFromSnap = [](const SensorCacheSnapshot& s) -> const char* {
    static char* buf = nullptr;
    if (!buf) {
      buf = (char*)ps_alloc(512, AllocPref::PreferPSRAM, "sensor.log.buf.snap");
      if (!buf) return "";
    }
    char* pos = buf;
    int remaining = 512;
    int written = 0;
    char tsPrefix[48];
    getTimestampPrefixMsCached(tsPrefix, sizeof(tsPrefix));
    if (tsPrefix[0]) written = snprintf(pos, remaining, "%s", tsPrefix);
    else written = snprintf(pos, remaining, "[BOOT ms=%lu] | ", millis());
    pos += written;
    remaining -= written;

    // Thermal (only if enabled in mask)
    if ((gSensorLogMask & LOG_THERMAL) && s.thermalEnabled && s.thermalConnected && s.thermalValid && remaining > 0) {
      written = snprintf(pos, remaining, "thermal: min=%dC avg=%dC max=%dC | ",
                         (int)s.thermalMin, (int)s.thermalAvg, (int)s.thermalMax);
      pos += written;
      remaining -= written;
    }

    // ToF (only if enabled in mask)
    if ((gSensorLogMask & LOG_TOF) && s.tofEnabled && s.tofConnected && s.tofValid && remaining > 0) {
      written = snprintf(pos, remaining, "tof: ");
      pos += written;
      remaining -= written;
      for (int i = 0; i < s.tofTotal && i < 4 && remaining > 0; i++) {
        if (s.tof[i].valid) {
          written = snprintf(pos, remaining, "obj%d=%dmm(st=%d) ", i, s.tof[i].distance_mm, s.tof[i].status);
          pos += written;
          remaining -= written;
        }
      }
      if (remaining > 0) {
        written = snprintf(pos, remaining, "| ");
        pos += written;
        remaining -= written;
      }
    }

    // IMU (only if enabled in mask)
    if ((gSensorLogMask & LOG_IMU) && s.imuEnabled && s.imuConnected && remaining > 0) {
      written = snprintf(pos, remaining, "imu: yaw=%.1f pitch=%.1f roll=%.1f accel=(%.2f,%.2f,%.2f) temp=%.1fC | ",
                         s.yaw, s.pitch, s.roll, s.ax, s.ay, s.az, s.imuTemp);
      pos += written;
      remaining -= written;
    }

    // Gamepad (only if enabled in mask)
    if ((gSensorLogMask & LOG_GAMEPAD) && s.gamepadEnabled && s.gamepadConnected && s.gamepadValid && remaining > 0) {
      written = snprintf(pos, remaining, "gamepad: x=%d y=%d btns=0x%lX | ",
                         s.gamepadX, s.gamepadY, (unsigned long)s.gamepadButtons);
      pos += written;
      remaining -= written;
    }

    // APDS (only if enabled in mask)
    if ((gSensorLogMask & LOG_APDS) && s.apdsConnected && s.apdsValid && remaining > 0) {
      written = snprintf(pos, remaining, "apds: r=%u g=%u b=%u c=%u prox=%u gest=%u | ",
                         s.apdsRed, s.apdsGreen, s.apdsBlue, s.apdsClear, s.apdsProximity, s.apdsGesture);
      pos += written;
      remaining -= written;
    }

    int len = strlen(buf);
    bool hasSensorData = (len >= 3 && strcmp(buf + len - 3, " | ") == 0);
    if (hasSensorData) buf[len - 3] = '\0';
    else if (remaining > 0) snprintf(pos, remaining, " no sensors selected");
    len = strlen(buf);
    pos = buf + len;
    remaining = 512 - len;
    if (remaining > 0) {
      snprintf(pos, remaining, " | THERM[e=%d c=%d v=%d] TOF[e=%d c=%d v=%d] IMU[e=%d c=%d]",
               s.thermalEnabled ? 1 : 0, s.thermalConnected ? 1 : 0, s.thermalValid ? 1 : 0,
               s.tofEnabled ? 1 : 0, s.tofConnected ? 1 : 0, s.tofValid ? 1 : 0,
               s.imuEnabled ? 1 : 0, s.imuConnected ? 1 : 0);
    }
    return buf;
  };

  // CSV builder for structured data logging
  auto buildCSVFromSnap = [](const SensorCacheSnapshot& s) -> const char* {
    static char* buf = nullptr;
    if (!buf) {
      buf = (char*)ps_alloc(1024, AllocPref::PreferPSRAM, "sensor.log.csv");
      if (!buf) return "";
    }

    unsigned long timestamp = millis();
    char* pos = buf;
    int remaining = 1024;

    int written = snprintf(pos, remaining, "%lu", timestamp);
    pos += written;
    remaining -= written;

    if ((gSensorLogMask & LOG_THERMAL) && s.thermalValid && remaining > 0) {
      written = snprintf(pos, remaining, ",%d,%d,%d",
                         (int)s.thermalMin, (int)s.thermalMax, (int)s.thermalAvg);
      pos += written;
      remaining -= written;
    }

    if ((gSensorLogMask & LOG_TOF) && s.tofValid && remaining > 0) {
      written = snprintf(pos, remaining, ",%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
                         s.tof[0].distance_mm, s.tof[0].valid ? 1 : 0, s.tof[0].status,
                         s.tof[1].distance_mm, s.tof[1].valid ? 1 : 0, s.tof[1].status,
                         s.tof[2].distance_mm, s.tof[2].valid ? 1 : 0, s.tof[2].status,
                         s.tof[3].distance_mm, s.tof[3].valid ? 1 : 0, s.tof[3].status);
      pos += written;
      remaining -= written;
    }

    if ((gSensorLogMask & LOG_IMU) && remaining > 0) {
      written = snprintf(pos, remaining, ",%.1f,%.1f,%.1f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.1f",
                         s.yaw, s.pitch, s.roll,
                         s.ax, s.ay, s.az,
                         s.gx, s.gy, s.gz,
                         s.imuTemp);
      pos += written;
      remaining -= written;
    }

    if ((gSensorLogMask & LOG_GAMEPAD) && s.gamepadValid && remaining > 0) {
      written = snprintf(pos, remaining, ",%d,%d,%lu",
                         s.gamepadX, s.gamepadY, (unsigned long)s.gamepadButtons);
      pos += written;
      remaining -= written;
    }

    if ((gSensorLogMask & LOG_APDS) && s.apdsValid && remaining > 0) {
      written = snprintf(pos, remaining, ",%u,%u,%u,%u,%u,%u",
                         s.apdsRed, s.apdsGreen, s.apdsBlue, s.apdsClear,
                         s.apdsProximity, s.apdsGesture);
      pos += written;
      remaining -= written;
    }

    return buf;
  };

  while (gSensorLoggingEnabled) {
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(gSensorLogIntervalMs));

    // Periodic stack/heap monitoring (every 30 seconds)
    unsigned long nowMs = millis();
    if ((nowMs - lastStackLog) >= 30000) {
      lastStackLog = nowMs;
      if (isDebugFlagSet(DEBUG_PERFORMANCE)) {
        UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
        DEBUG_PERFORMANCEF("[STACK] sensor_log watermark=%u words", (unsigned)watermark);
      }
      if (isDebugFlagSet(DEBUG_MEMORY)) {
        DEBUG_MEMORYF("[HEAP] sensor_log: free=%u min=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
      }
    }

    SensorCacheSnapshot snap = {};
    bool got = false;
    
    // Lock thermal cache separately
    if (lockThermalCache(pdMS_TO_TICKS(10))) {
      snap.thermalEnabled = thermalEnabled;
      snap.thermalConnected = thermalConnected;
      snap.thermalValid = gThermalCache.thermalDataValid;
      snap.thermalMin = gThermalCache.thermalMinTemp;
      snap.thermalAvg = gThermalCache.thermalAvgTemp;
      snap.thermalMax = gThermalCache.thermalMaxTemp;
      unlockThermalCache();
    }
    
    // Lock each modular sensor cache individually
    bool tofLocked = false, imuLocked = false, gamepadLocked = false, apdsLocked = false;
    
#if ENABLE_TOF_SENSOR
    if (gTofCache.mutex && xSemaphoreTake(gTofCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      tofLocked = true;
      snap.tofEnabled = tofEnabled;
      snap.tofConnected = tofConnected;
      snap.tofValid = gTofCache.tofDataValid;
      snap.tofTotal = gTofCache.tofTotalObjects;
      for (int i = 0; i < 4; i++) {
        snap.tof[i].valid = gTofCache.tofObjects[i].valid;
        snap.tof[i].detected = gTofCache.tofObjects[i].detected;
        snap.tof[i].distance_mm = gTofCache.tofObjects[i].distance_mm;
        snap.tof[i].status = gTofCache.tofObjects[i].status;
      }
    }
#endif

#if ENABLE_IMU_SENSOR
    if (gImuCache.mutex && xSemaphoreTake(gImuCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      imuLocked = true;
      snap.imuEnabled = imuEnabled;
      snap.imuConnected = imuConnected;
      snap.yaw = gImuCache.oriYaw;
      snap.pitch = gImuCache.oriPitch;
      snap.roll = gImuCache.oriRoll;
      snap.ax = gImuCache.accelX;
      snap.ay = gImuCache.accelY;
      snap.az = gImuCache.accelZ;
      snap.gx = gImuCache.gyroX;
      snap.gy = gImuCache.gyroY;
      snap.gz = gImuCache.gyroZ;
      snap.imuTemp = gImuCache.imuTemp;
    }
#endif

#if ENABLE_GAMEPAD_SENSOR
    if (gControlCache.mutex && xSemaphoreTake(gControlCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      gamepadLocked = true;
      snap.gamepadEnabled = gamepadEnabled;
      snap.gamepadConnected = gamepadConnected;
      snap.gamepadValid = gControlCache.gamepadDataValid;
      snap.gamepadButtons = gControlCache.gamepadButtons;
      snap.gamepadX = gControlCache.gamepadX;
      snap.gamepadY = gControlCache.gamepadY;
    }
#endif

#if ENABLE_APDS_SENSOR
    if (gPeripheralCache.mutex && xSemaphoreTake(gPeripheralCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      apdsLocked = true;
      snap.apdsColorEnabled = apdsColorEnabled;
      snap.apdsProximityEnabled = apdsProximityEnabled;
      snap.apdsGestureEnabled = apdsGestureEnabled;
      snap.apdsConnected = apdsConnected;
      snap.apdsValid = gPeripheralCache.apdsDataValid;
      snap.apdsRed = gPeripheralCache.apdsRed;
      snap.apdsGreen = gPeripheralCache.apdsGreen;
      snap.apdsBlue = gPeripheralCache.apdsBlue;
      snap.apdsClear = gPeripheralCache.apdsClear;
      snap.apdsProximity = gPeripheralCache.apdsProximity;
      snap.apdsGesture = gPeripheralCache.apdsGesture;
    }
#endif

    // Release all locks in reverse order
#if ENABLE_APDS_SENSOR
    if (apdsLocked) xSemaphoreGive(gPeripheralCache.mutex);
#endif
#if ENABLE_GAMEPAD_SENSOR
    if (gamepadLocked) xSemaphoreGive(gControlCache.mutex);
#endif
#if ENABLE_IMU_SENSOR
    if (imuLocked) xSemaphoreGive(gImuCache.mutex);
#endif
#if ENABLE_TOF_SENSOR
    if (tofLocked) xSemaphoreGive(gTofCache.mutex);
#endif

    got = true;

    if (!got) {
      log_lock_fail++;
      if (isDebugFlagSet(DEBUG_LOGGER)) {
        WARN_LOGGINGF("Cache lock failed (count=%u)", (unsigned)log_lock_fail);
      }
      continue;
    }

    // Suppress idle-only lines
    static unsigned long lastHeartbeatMs = 0;
    const unsigned long heartbeatMs = 5000;
    bool allIdle = (!snap.thermalEnabled && !snap.thermalConnected && !snap.tofEnabled && !snap.tofConnected && !snap.imuEnabled && !snap.imuConnected);
    if (allIdle) {
      if (lastHeartbeatMs != 0 && (long)(nowMs - lastHeartbeatMs) < (long)heartbeatMs) {
        log_idle_skips++;
        if (isDebugFlagSet(DEBUG_LOGGER)) {
          DEBUG_LOGGERF("logger: idle skip #%u (dt=%lums)", (unsigned)log_idle_skips, (nowMs - lastHeartbeatMs));
        }
        continue;
      }
      lastHeartbeatMs = nowMs;
      if (isDebugFlagSet(DEBUG_LOGGER)) {
        DEBUG_LOGGERF("logger: heartbeat at %lu ms", nowMs);
      }
    } else {
      lastHeartbeatMs = nowMs;
    }

    // Choose format: CSV or text
    const char* line = gSensorLogFormatCSV ? buildCSVFromSnap(snap) : buildFromSnap(snap);
    if (line && line[0] != '\0') {
      fsLock("sensorlog.append");
      File f = LittleFS.open(gSensorLogPath.c_str(), "a");
      if (f) {
        size_t len = strlen(line);
        f.write((const uint8_t*)line, len);
        f.write('\n');
        f.close();
        gSensorLogLastWrite = millis();
        writeCount++;
        log_writes++;
        approxSizeBytes += (len + 1);
        
        // Handle log rotation
        if (approxSizeBytes > gSensorLogMaxSize && (lastTruncateMs == 0 || (long)(millis() - lastTruncateMs) >= (long)truncateCooldownMs)) {
          lastTruncateMs = millis();
          writeCount = 0;

          if (gSensorLogMaxRotations > 0) {
            if (gSensorLogMaxRotations > 1) {
              String oldestFile = gSensorLogPath + "." + String(gSensorLogMaxRotations);
              if (LittleFS.exists(oldestFile)) {
                LittleFS.remove(oldestFile.c_str());
              }
            }

            for (int i = gSensorLogMaxRotations - 1; i >= 1; i--) {
              String fromFile = (i == 1) ? gSensorLogPath : (gSensorLogPath + "." + String(i));
              String toFile = gSensorLogPath + "." + String(i + 1);
              if (LittleFS.exists(fromFile)) {
                LittleFS.rename(fromFile.c_str(), toFile.c_str());
              }
            }

            if (LittleFS.exists(gSensorLogPath)) {
              String rotatedFile = gSensorLogPath + ".1";
              LittleFS.rename(gSensorLogPath.c_str(), rotatedFile.c_str());
            }
          } else {
            LittleFS.remove(gSensorLogPath.c_str());
          }

          approxSizeBytes = 0;
          log_trunc++;
          if (isDebugFlagSet(DEBUG_STORAGE)) {
            DEBUGF_BROADCAST(DEBUG_STORAGE, "Sensor log: rotated file (max size=%u bytes)", (unsigned)gSensorLogMaxSize);
          }
          if (isDebugFlagSet(DEBUG_LOGGER)) {
            DEBUG_LOGGERF("logger: rotated at approxSize=%u", (unsigned)gSensorLogMaxSize);
          }
        }
        
        if (isDebugFlagSet(DEBUG_STORAGE)) {
          UBaseType_t stackHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
          DEBUGF_BROADCAST(DEBUG_STORAGE, "Sensor log: wrote %d bytes, stack watermark: %d words",
                           (int)len, (int)stackHighWaterMark);
        }
        if (isDebugFlagSet(DEBUG_LOGGER)) {
          DEBUG_LOGGERF("logger: wrote %dB, approxSize=%uB, writes=%u", (int)len, (unsigned)approxSizeBytes, (unsigned)log_writes);
        }
      } else {
        if (isDebugFlagSet(DEBUG_STORAGE)) {
          DEBUGF_BROADCAST(DEBUG_STORAGE, "Sensor log: failed to open file");
        }
        log_open_fail++;
        if (isDebugFlagSet(DEBUG_LOGGER)) {
          DEBUG_LOGGERF("logger: open fail #%u", (unsigned)log_open_fail);
        }
      }
      fsUnlock();
    }

    // Periodic summary
    unsigned long now2 = millis();
    if (isDebugFlagSet(DEBUG_LOGGER) && (lastSummaryMs == 0 || (long)(now2 - lastSummaryMs) >= 5000)) {
      lastSummaryMs = now2;
      DEBUG_LOGGERF("logger: summary | writes=%u open_fail=%u lock_fail=%u idle_skips=%u trunc=%u",
                    (unsigned)log_writes, (unsigned)log_open_fail, (unsigned)log_lock_fail,
                    (unsigned)log_idle_skips, (unsigned)log_trunc);
    }
  }

  if (isDebugFlagSet(DEBUG_SYSTEM)) {
    DEBUGF_BROADCAST(DEBUG_SYSTEM, "Sensor log task stopping");
  }
  // NOTE: Do NOT clear gSensorLogTaskHandle here - let create function use eTaskGetState()
  // to detect stale handles. Clearing here creates a race condition window.
  vTaskDelete(NULL);
}

// ============================================================================
// Command Handler
// ============================================================================

const char* cmd_sensorlog(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  int sp1 = originalCmd.indexOf(' ');
  if (sp1 < 0) {
    return "Usage: sensorlog <start|stop|status|format|maxsize|rotations|sensors> [args...]\n"
           "  start <filepath> [interval_ms]: Begin logging (default 5000ms)\n"
           "  stop: Stop logging\n"
           "  status: Show current logging status\n"
           "  format <text|csv>: Set log format (default: text)\n"
           "  maxsize <bytes>: Set max file size before rotation (default: 256000)\n"
           "  rotations <count>: Set number of old logs to keep (0-9, default: 3)\n"
           "  sensors <thermal|tof|imu|gamepad|apds|all|none>: Select sensors to log";
  }

  String action = originalCmd.substring(sp1 + 1);
  action.trim();
  int sp2 = action.indexOf(' ');
  String subCmd = (sp2 >= 0) ? action.substring(0, sp2) : action;
  subCmd.toLowerCase();

  // Handle 'status' subcommand
  if (subCmd == "status") {
    if (!ensureDebugBuffer()) return "Sensor logging status unavailable";
    if (gSensorLoggingEnabled && gSensorLogTaskHandle) {
      char* p = getDebugBuffer();
      size_t remaining = 1024;
      int n;

      n = snprintf(p, remaining, "Sensor logging ACTIVE\n");
      p += n; remaining -= n;
      n = snprintf(p, remaining, "  File: %s\n", gSensorLogPath.c_str());
      p += n; remaining -= n;
      n = snprintf(p, remaining, "  Interval: %lums\n", (unsigned long)gSensorLogIntervalMs);
      p += n; remaining -= n;
      n = snprintf(p, remaining, "  Format: %s\n", gSensorLogFormatCSV ? "CSV" : "TEXT");
      p += n; remaining -= n;
      n = snprintf(p, remaining, "  Max size: %u bytes\n", (unsigned)gSensorLogMaxSize);
      p += n; remaining -= n;
      n = snprintf(p, remaining, "  Rotations: %u\n", (unsigned)gSensorLogMaxRotations);
      p += n; remaining -= n;
      n = snprintf(p, remaining, "  Sensors: %s%s%s%s%s\n",
                   (gSensorLogMask & LOG_THERMAL) ? "thermal " : "",
                   (gSensorLogMask & LOG_TOF) ? "tof " : "",
                   (gSensorLogMask & LOG_IMU) ? "imu " : "",
                   (gSensorLogMask & LOG_GAMEPAD) ? "gamepad " : "",
                   (gSensorLogMask & LOG_APDS) ? "apds " : "");
      p += n; remaining -= n;
      n = snprintf(p, remaining, "  Last write: %lus ago", (millis() - gSensorLogLastWrite) / 1000);
      return getDebugBuffer();
    } else {
      snprintf(getDebugBuffer(), 1024, "Sensor logging is INACTIVE\nCurrent format: %s\nMax size: %u bytes\nRotations: %u\nSensors: %s%s%s%s%s",
               gSensorLogFormatCSV ? "CSV" : "TEXT", (unsigned)gSensorLogMaxSize, (unsigned)gSensorLogMaxRotations,
               (gSensorLogMask & LOG_THERMAL) ? "thermal " : "",
               (gSensorLogMask & LOG_TOF) ? "tof " : "",
               (gSensorLogMask & LOG_IMU) ? "imu " : "",
               (gSensorLogMask & LOG_GAMEPAD) ? "gamepad " : "",
               (gSensorLogMask & LOG_APDS) ? "apds " : "");
      return getDebugBuffer();
    }
  }

  // Handle 'stop' subcommand
  if (subCmd == "stop") {
    if (!gSensorLoggingEnabled) {
      return "Sensor logging is not running";
    }
    gSensorLoggingEnabled = false;
    broadcastOutput("Sensor logging stop requested; will stop safely");
    return "SUCCESS: Sensor logging stop requested; will stop safely";
  }

  // Handle 'start' subcommand
  if (subCmd == "start") {
    if (gSensorLoggingEnabled) {
      return "Sensor logging already running. Use 'sensorlog stop' first.";
    }

    if (sp2 < 0) {
      return "Usage: sensorlog start <filepath> [interval_ms]\n"
             "Example: sensorlog start /logs/sensors.txt 1000";
    }

    String args = action.substring(sp2 + 1);
    args.trim();
    int sp3 = args.indexOf(' ');

    String filepath = (sp3 >= 0) ? args.substring(0, sp3) : args;
    uint32_t interval = gSensorLogIntervalMs;

    if (sp3 >= 0) {
      String intervalStr = args.substring(sp3 + 1);
      intervalStr.trim();
      interval = intervalStr.toInt();
      if (interval < 100) interval = 100;
      if (interval > 3600000) interval = 3600000;
    }

    if (filepath.length() == 0 || filepath.charAt(0) != '/') {
      return "Error: Filepath must start with / (e.g., /logs/sensors.txt)";
    }

    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

    // Ensure directory exists
    int lastSlash = filepath.lastIndexOf('/');
    if (lastSlash > 0) {
      String dir = filepath.substring(0, lastSlash);
      if (!LittleFS.exists(dir)) {
        if (!LittleFS.mkdir(dir)) {
          snprintf(getDebugBuffer(), 1024, "Error: Failed to create directory: %s", dir.c_str());
          return getDebugBuffer();
        }
        broadcastOutput("Created directory: " + dir);
      }
    }

    // Create file if needed
    if (!LittleFS.exists(filepath)) {
      File f = LittleFS.open(filepath, "w");
      if (!f) {
        snprintf(getDebugBuffer(), 1024, "Error: Failed to create file: %s", filepath.c_str());
        return getDebugBuffer();
      }

      if (gSensorLogFormatCSV) {
        String csvHeader = "timestamp_ms";
        if (gSensorLogMask & LOG_THERMAL) csvHeader += ",thermal_min,thermal_max,thermal_avg";
        if (gSensorLogMask & LOG_TOF) {
          csvHeader += ",tof_obj0_dist,tof_obj0_valid,tof_obj0_status";
          csvHeader += ",tof_obj1_dist,tof_obj1_valid,tof_obj1_status";
          csvHeader += ",tof_obj2_dist,tof_obj2_valid,tof_obj2_status";
          csvHeader += ",tof_obj3_dist,tof_obj3_valid,tof_obj3_status";
        }
        if (gSensorLogMask & LOG_IMU) {
          csvHeader += ",imu_yaw,imu_pitch,imu_roll";
          csvHeader += ",imu_accel_x,imu_accel_y,imu_accel_z";
          csvHeader += ",imu_gyro_x,imu_gyro_y,imu_gyro_z";
          csvHeader += ",imu_temp";
        }
        if (gSensorLogMask & LOG_GAMEPAD) csvHeader += ",gamepad_x,gamepad_y,gamepad_buttons";
        if (gSensorLogMask & LOG_APDS) csvHeader += ",apds_red,apds_green,apds_blue,apds_clear,apds_proximity,apds_gesture";
        csvHeader += "\n";
        f.write((const uint8_t*)csvHeader.c_str(), csvHeader.length());
      }
      f.close();
      broadcastOutput("Created log file: " + filepath);
    }

    if (ESP.getFreeHeap() < 20480) {
      return "Error: Insufficient memory (need 20KB free)";
    }

    gSensorLogPath = filepath;
    gSensorLogIntervalMs = interval;
    gSensorLoggingEnabled = true;
    gSensorLogLastWrite = millis();

    // Check for stale task handle (task deleted itself but handle not cleared)
    if (gSensorLogTaskHandle != nullptr) {
      eTaskState state = eTaskGetState(gSensorLogTaskHandle);
      if (state == eDeleted || state == eInvalid) {
        gSensorLogTaskHandle = nullptr;
      }
    }
    
    const uint32_t stackWords = 16384 / 4;
    BaseType_t result = xTaskCreateLogged(
      sensorLogTask, "sensor_log", stackWords, nullptr, 1, &gSensorLogTaskHandle, "sensor.log");

    if (result != pdPASS) {
      gSensorLoggingEnabled = false;
      gSensorLogPath = "";
      return "Error: Failed to create logging task";
    }

    snprintf(getDebugBuffer(), 1024, "SUCCESS: Sensor logging started\n  File: %s\n  Interval: %lums",
             filepath.c_str(), (unsigned long)interval);
    broadcastOutput(getDebugBuffer());
    return getDebugBuffer();
  }

  // Handle 'format' subcommand
  if (subCmd == "format") {
    if (sp2 < 0) {
      snprintf(getDebugBuffer(), 1024, "Current format: %s\nUsage: sensorlog format <text|csv>",
               gSensorLogFormatCSV ? "csv" : "text");
      return getDebugBuffer();
    }

    String formatType = action.substring(sp2 + 1);
    formatType.trim();
    formatType.toLowerCase();

    if (formatType == "csv") {
      gSensorLogFormatCSV = true;
      return "Log format set to CSV (applies to next 'sensorlog start')";
    } else if (formatType == "text") {
      gSensorLogFormatCSV = false;
      return "Log format set to TEXT (applies to next 'sensorlog start')";
    } else {
      return "Error: Format must be 'text' or 'csv'";
    }
  }

  // Handle 'maxsize' subcommand
  if (subCmd == "maxsize") {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    if (sp2 < 0) {
      snprintf(getDebugBuffer(), 1024, "Current max size: %u bytes\nUsage: sensorlog maxsize <bytes>",
               (unsigned)gSensorLogMaxSize);
      return getDebugBuffer();
    }

    String sizeStr = action.substring(sp2 + 1);
    sizeStr.trim();
    size_t newSize = sizeStr.toInt();

    if (newSize < 10240) return "Error: Max size must be at least 10240 bytes (10KB)";
    if (newSize > 10485760) return "Error: Max size cannot exceed 10485760 bytes (10MB)";

    gSensorLogMaxSize = newSize;
    snprintf(getDebugBuffer(), 1024, "Max log size set to %u bytes (applies to active logging)",
             (unsigned)gSensorLogMaxSize);
    return getDebugBuffer();
  }

  // Handle 'rotations' subcommand
  if (subCmd == "rotations") {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    if (sp2 < 0) {
      snprintf(getDebugBuffer(), 1024, "Current rotations: %u\nUsage: sensorlog rotations <count>\n"
                                   "Set to 0 to disable rotation (delete old logs)",
               (unsigned)gSensorLogMaxRotations);
      return getDebugBuffer();
    }

    String countStr = action.substring(sp2 + 1);
    countStr.trim();
    int count = countStr.toInt();

    if (count < 0 || count > 9) return "Error: Rotation count must be 0-9";

    gSensorLogMaxRotations = (uint8_t)count;
    if (count == 0) {
      return "Rotation disabled - old logs will be deleted";
    } else {
      snprintf(getDebugBuffer(), 1024, "Will keep up to %u old log file%s (.1, .2, etc.)",
               count, count > 1 ? "s" : "");
      return getDebugBuffer();
    }
  }

  // Handle 'sensors' subcommand
  if (subCmd == "sensors") {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    if (sp2 < 0) {
      char* p = getDebugBuffer();
      size_t remaining = 1024;
      int n = snprintf(p, remaining, "Selected sensors:\n");
      p += n; remaining -= n;

      n = snprintf(p, remaining, "  %s Thermal\n", (gSensorLogMask & LOG_THERMAL) ? "☑" : "☐");
      p += n; remaining -= n;
      n = snprintf(p, remaining, "  %s ToF\n", (gSensorLogMask & LOG_TOF) ? "☑" : "☐");
      p += n; remaining -= n;
      n = snprintf(p, remaining, "  %s IMU\n", (gSensorLogMask & LOG_IMU) ? "☑" : "☐");
      p += n; remaining -= n;
      n = snprintf(p, remaining, "  %s Gamepad\n", (gSensorLogMask & LOG_GAMEPAD) ? "☑" : "☐");
      p += n; remaining -= n;
      n = snprintf(p, remaining, "  %s APDS\n", (gSensorLogMask & LOG_APDS) ? "☑" : "☐");
      p += n; remaining -= n;
      snprintf(p, remaining, "\nUsage: sensorlog sensors <thermal|tof|imu|gamepad|apds|all|none>");
      return getDebugBuffer();
    }

    String sensorList = action.substring(sp2 + 1);
    sensorList.trim();
    sensorList.toLowerCase();

    if (sensorList == "all") {
      gSensorLogMask = LOG_THERMAL | LOG_TOF | LOG_IMU | LOG_GAMEPAD | LOG_APDS;
      return "All sensors enabled for logging";
    }

    if (sensorList == "none") {
      gSensorLogMask = 0x00;
      return "All sensors disabled for logging";
    }

    gSensorLogMask = 0x00;
    int start = 0;
    while (start < (int)sensorList.length()) {
      int comma = sensorList.indexOf(',', start);
      String sensor = (comma >= 0) ? sensorList.substring(start, comma) : sensorList.substring(start);
      sensor.trim();

      if (sensor == "thermal") gSensorLogMask |= LOG_THERMAL;
      else if (sensor == "tof") gSensorLogMask |= LOG_TOF;
      else if (sensor == "imu") gSensorLogMask |= LOG_IMU;
      else if (sensor == "gamepad") gSensorLogMask |= LOG_GAMEPAD;
      else if (sensor == "apds") gSensorLogMask |= LOG_APDS;
      else {
        snprintf(getDebugBuffer(), 1024, "Error: Unknown sensor '%s'", sensor.c_str());
        return getDebugBuffer();
      }

      if (comma < 0) break;
      start = comma + 1;
    }

    snprintf(getDebugBuffer(), 1024, "Logging enabled for: %s%s%s%s%s",
             (gSensorLogMask & LOG_THERMAL) ? "thermal " : "",
             (gSensorLogMask & LOG_TOF) ? "tof " : "",
             (gSensorLogMask & LOG_IMU) ? "imu " : "",
             (gSensorLogMask & LOG_GAMEPAD) ? "gamepad " : "",
             (gSensorLogMask & LOG_APDS) ? "apds " : "");
    return getDebugBuffer();
  }

  return "Error: Unknown subcommand. Use: start, stop, status, format, maxsize, rotations, or sensors";
}

// ============================================================================
// Command Registry
// ============================================================================

const CommandEntry sensorLoggingCommands[] = {
  { "sensorlog", "Sensor data logging: start, stop, status, format, maxsize, rotations, sensors", false, cmd_sensorlog,
    "Usage: sensorlog <start|stop|status|format|maxsize|rotations|sensors> [args...]\n"
    "  start <filepath> [interval_ms]: Begin logging (default 5000ms)\n"
    "  stop: Stop logging\n"
    "  status: Show current logging status\n"
    "  format <text|csv>: Set log format (default: text)\n"
    "  maxsize <bytes>: Set max file size before rotation (default: 256000)\n"
    "  rotations <count>: Set number of old logs to keep (0-9, default: 3)\n"
    "  sensors <thermal|tof|imu|gamepad|apds|all|none>: Select sensors to log" },
};

const size_t sensorLoggingCommandsCount = sizeof(sensorLoggingCommands) / sizeof(sensorLoggingCommands[0]);

// Auto-register with command system
static CommandModuleRegistrar _sensorlog_cmd_registrar(sensorLoggingCommands, sensorLoggingCommandsCount, "sensorlog");
