/**
 * Sensor Logging System - Data logging for sensor readings
 * 
 * Provides configurable logging of sensor data to files with:
 * - Selectable sensors (thermal, tof, imu, gamepad, apds, gps)
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
#include "System_Notifications.h"
#include "System_Settings.h"
#include "System_Filesystem.h"
#include "System_VFS.h"
#include <LittleFS.h>

// Conditional sensor includes (same approach as main .ino)
#include "System_BuildConfig.h"
#if ENABLE_THERMAL_SENSOR
  #include "i2csensor_mlx90640.h"
#endif
#if ENABLE_IMU_SENSOR
  #include "i2csensor_bno055.h"
#endif
#if ENABLE_TOF_SENSOR
  #include "i2csensor_vl53l4cx.h"
#endif
#if ENABLE_OLED_INPUT
  #include "HAL_Input.h"   // gInputCache, gInputEnabled/Connected — populated by either driver
#endif
#if ENABLE_APDS_SENSOR
  #include "i2csensor_apds9960.h"
#endif
#if ENABLE_GPS_SENSOR
  #include "i2csensor_pa1010d.h"
  #include <Adafruit_GPS.h>
#endif
#if ENABLE_PRESENCE_SENSOR
  #include "i2csensor_sths34pf80.h"
#endif
#include "System_SensorStubs.h"  // Provides stubs for disabled sensors

// External dependencies
extern void getTimestampPrefixMsCached(char* out, size_t outSize);

// Modular sensor caches (conditionally available based on enabled sensors)

// ============================================================================
// Sensor Logging State Variables
// ============================================================================

bool gSensorLoggingEnabled = false;
String gSensorLogPath = "";
unsigned long gSensorLogLastWrite = 0;
uint32_t gSensorLogIntervalMs = 5000;
size_t gSensorLogMaxSize = 250 * 1024;
SensorLogFormat gSensorLogFormat = SENSOR_LOG_TEXT;
uint8_t gSensorLogMaxRotations = 3;
uint8_t gSensorLogMask = 0x00;

// ============================================================================
// Sensor Logging Tick (called from main loop — no dedicated task needed)
// ============================================================================

void sensorLogTick() {
  if (!gSensorLoggingEnabled) return;

  static unsigned long lastTickMs = 0;
  unsigned long nowMs = millis();
  if (lastTickMs != 0 && (long)(nowMs - lastTickMs) < (long)gSensorLogIntervalMs) return;
  lastTickMs = nowMs;

  // Diagnostics counters
  static uint32_t log_writes = 0;
  static uint32_t log_open_fail = 0;
  static uint32_t log_lock_fail = 0;
  static uint32_t log_idle_skips = 0;
  static uint32_t log_trunc = 0;
  static unsigned long lastSummaryMs = 0;
  static size_t approxSizeBytes = 0;
  static unsigned long lastTruncateMs = 0;
  static uint32_t writeCount = 0;
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
    if ((gSensorLogMask & LOG_THERMAL) && s.gThermalEnabled && s.gThermalConnected && s.thermalValid && remaining > 0) {
      written = snprintf(pos, remaining, "thermal: min=%dC avg=%dC max=%dC | ",
                         (int)s.thermalMin, (int)s.thermalAvg, (int)s.thermalMax);
      pos += written;
      remaining -= written;
    }

    // ToF (only if enabled in mask)
    if ((gSensorLogMask & LOG_TOF) && s.gTofEnabled && s.gTofConnected && s.tofValid && remaining > 0) {
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
    if ((gSensorLogMask & LOG_IMU) && s.gImuEnabled && s.gImuConnected && remaining > 0) {
      written = snprintf(pos, remaining, "imu: yaw=%.1f pitch=%.1f roll=%.1f accel=(%.2f,%.2f,%.2f) temp=%.1fC | ",
                         s.yaw, s.pitch, s.roll, s.ax, s.ay, s.az, s.imuTemp);
      pos += written;
      remaining -= written;
    }

    // Input device (only if enabled in mask). The "input:" prefix is generic
    // because the source could be either a gamepad joystick or the ANO encoder's
    // synthesized state — the format is the same shape either way.
    if ((gSensorLogMask & LOG_GAMEPAD) && s.gInputEnabled && s.gInputConnected && s.inputValid && remaining > 0) {
      written = snprintf(pos, remaining, "input: x=%d y=%d btns=0x%lX | ",
                         s.joyX, s.joyY, (unsigned long)s.buttons);
      pos += written;
      remaining -= written;
    }

    // APDS (only if enabled in mask)
    if ((gSensorLogMask & LOG_APDS) && s.gApdsConnected && s.apdsValid && remaining > 0) {
      written = snprintf(pos, remaining, "apds: r=%u g=%u b=%u c=%u prox=%u gest=%u | ",
                         s.apdsRed, s.apdsGreen, s.apdsBlue, s.apdsClear, s.apdsProximity, s.apdsGesture);
      pos += written;
      remaining -= written;
    }

    // GPS (only if enabled in mask)
    if ((gSensorLogMask & LOG_GPS) && s.gGpsEnabled && s.gGpsConnected && remaining > 0) {
      if (s.gpsFix) {
        written = snprintf(pos, remaining, "gps: lat=%.6f lon=%.6f alt=%.1fm speed=%.1fkn sats=%d q=%d | ",
                           s.gpsLatitude, s.gpsLongitude, s.gpsAltitude, s.gpsSpeed,
                           (int)s.gpsSatellites, (int)s.gpsFixQuality);
      } else {
        written = snprintf(pos, remaining, "gps: no_fix sats=%d q=%d | ",
                           (int)s.gpsSatellites, (int)s.gpsFixQuality);
      }
      pos += written;
      remaining -= written;
    }

    // Presence (only if enabled in mask)
    if ((gSensorLogMask & LOG_PRESENCE) && s.gPresenceEnabled && s.gPresenceConnected && remaining > 0) {
      written = snprintf(pos, remaining, "presence: amb=%.1fC pres=%d%s mot=%d%s | ",
                         s.presenceAmbientTemp, (int)s.presenceValue,
                         s.presenceDetected ? "[DET]" : "",
                         (int)s.motionValue,
                         s.motionDetected ? "[DET]" : "");
      pos += written;
      remaining -= written;
    }

    int len = strlen(buf);
    bool hasSensorData = (len >= 3 && strcmp(buf + len - 3, " | ") == 0);
    if (hasSensorData) buf[len - 3] = '\0';
    else if (remaining > 0) snprintf(pos, remaining, "(no data from selected sensors)");
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

    if ((gSensorLogMask & LOG_GAMEPAD) && s.inputValid && remaining > 0) {
      written = snprintf(pos, remaining, ",%d,%d,%lu",
                         s.joyX, s.joyY, (unsigned long)s.buttons);
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

    if ((gSensorLogMask & LOG_GPS) && remaining > 0) {
      written = snprintf(pos, remaining, ",%d,%.6f,%.6f,%.1f,%.1f,%d,%d",
                         s.gpsFix ? 1 : 0, s.gpsLatitude, s.gpsLongitude,
                         s.gpsAltitude, s.gpsSpeed,
                         (int)s.gpsSatellites, (int)s.gpsFixQuality);
      pos += written;
      remaining -= written;
    }

    if ((gSensorLogMask & LOG_PRESENCE) && remaining > 0) {
      written = snprintf(pos, remaining, ",%.1f,%d,%d,%d,%d",
                         s.presenceAmbientTemp, (int)s.presenceValue,
                         s.presenceDetected ? 1 : 0,
                         (int)s.motionValue, s.motionDetected ? 1 : 0);
      pos += written;
      remaining -= written;
    }

    return buf;
  };

  // Track format state (signal loss dedup) — must be static to persist across ticks
  static uint32_t trackSignalLostCount = 0;
  static bool trackWasConnected = false;

  // Track format builder — GPS-only compact: time,lat,lon,alt,speed,sats
  auto buildTrackFromSnap = [](const SensorCacheSnapshot& s) -> const char* {
    static char buf[128];
    
    // Format timestamp from GPS time or millis fallback
    char ts[12];
    if (s.gpsHasTime) {
      snprintf(ts, sizeof(ts), "%02d:%02d:%02d", s.gpsHour, s.gpsMinute, s.gpsSecond);
    } else {
      unsigned long secs = millis() / 1000;
      snprintf(ts, sizeof(ts), "%02lu:%02lu:%02lu",
               (secs / 3600) % 24, (secs / 60) % 60, secs % 60);
    }

    if (!s.gpsFix) {
      trackSignalLostCount++;
      if (trackSignalLostCount == 1 && trackWasConnected) {
        snprintf(buf, sizeof(buf), "%s,---,SIGNAL_LOST", ts);
        trackWasConnected = false;
        return buf;
      }
      trackWasConnected = false;
      return nullptr;  // suppress duplicate signal-loss lines
    }

    // Signal regained after loss
    if (trackSignalLostCount > 0) {
      if (trackSignalLostCount > 1) {
        // Emit regained line first — caller will write this, then the data line on next tick
        snprintf(buf, sizeof(buf), "%s,~~~,SIGNAL_REGAINED (lost %lu intervals)",
                 ts, (unsigned long)trackSignalLostCount);
        trackSignalLostCount = 0;
        trackWasConnected = true;
        return buf;
      }
      trackSignalLostCount = 0;
    }
    trackWasConnected = true;

    // NOTE: Live track (GPSTrackManager::appendPoint) is fed directly by
    // gpsTask in i2csensor_pa1010d.cpp, independent of sensor logging.

    snprintf(buf, sizeof(buf), "%s,%.6f,%.6f,%.1f,%.1f,%d",
             ts, s.gpsLatitude, s.gpsLongitude, s.gpsAltitude, s.gpsSpeed, (int)s.gpsSatellites);
    return buf;
  };

    SensorCacheSnapshot snap = {};
    const uint8_t mask = gSensorLogMask;
    
    // Only lock caches for sensors selected in the mask
    if (mask & LOG_THERMAL) {
      if (lockThermalCache(pdMS_TO_TICKS(10))) {
        snap.gThermalEnabled = gThermalEnabled;
        snap.gThermalConnected = gThermalConnected;
        snap.thermalValid = gThermalCache.thermalDataValid;
        snap.thermalMin = gThermalCache.thermalMinTemp;
        snap.thermalAvg = gThermalCache.thermalAvgTemp;
        snap.thermalMax = gThermalCache.thermalMaxTemp;
        unlockThermalCache();
      }
    }
    
#if ENABLE_TOF_SENSOR
    if (mask & LOG_TOF) {
      SensorCacheGuard g(gTofCache.mutex, pdMS_TO_TICKS(10), "sensorLog.tofSnapshot");
      if (g.held) {
        snap.gTofEnabled = gTofEnabled;
        snap.gTofConnected = gTofConnected;
        snap.tofValid = gTofCache.tofDataValid;
        snap.tofTotal = gTofCache.tofTotalObjects;
        for (int i = 0; i < 4; i++) {
          snap.tof[i].valid = gTofCache.tofObjects[i].valid;
          snap.tof[i].detected = gTofCache.tofObjects[i].detected;
          snap.tof[i].distance_mm = gTofCache.tofObjects[i].distance_mm;
          snap.tof[i].status = gTofCache.tofObjects[i].status;
        }
      }
    }
#endif

#if ENABLE_IMU_SENSOR
    if (mask & LOG_IMU) {
      SensorCacheGuard g(gImuCache.mutex, pdMS_TO_TICKS(10), "sensorLog.imuSnapshot");
      if (g.held) {
        snap.gImuEnabled = gImuEnabled;
        snap.gImuConnected = gImuConnected;
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
    }
#endif

#if ENABLE_OLED_INPUT  // gInputCache populated by either gamepad or ANO driver
    if (mask & LOG_GAMEPAD) {
      SensorCacheGuard g(gInputCache.mutex, pdMS_TO_TICKS(10), "sensorLog.inputSnapshot");
      if (g.held) {
        snap.gInputEnabled = gInputEnabled;
        snap.gInputConnected = gInputConnected;
        snap.inputValid = gInputCache.dataValid;
        // Stored in the cache's native bit layout (per-device, active-low).
        // Replay code that crosses devices should pass this through
        // inputButtonsToLogical() — buttons field is the device-native form.
        snap.buttons = gInputCache.buttons;
        snap.joyX = gInputCache.joyX;
        snap.joyY = gInputCache.joyY;
      }
    }
#endif

#if ENABLE_APDS_SENSOR
    if (mask & LOG_APDS) {
      SensorCacheGuard g(gApdsCache.mutex, pdMS_TO_TICKS(10), "sensorLog.apdsSnapshot");
      if (g.held) {
        snap.gApdsColorEnabled = gApdsColorEnabled;
        snap.gApdsProximityEnabled = gApdsProximityEnabled;
        snap.gApdsGestureEnabled = gApdsGestureEnabled;
        snap.gApdsConnected = gApdsConnected;
        snap.apdsValid = gApdsCache.apdsDataValid;
        snap.apdsRed = gApdsCache.apdsRed;
        snap.apdsGreen = gApdsCache.apdsGreen;
        snap.apdsBlue = gApdsCache.apdsBlue;
        snap.apdsClear = gApdsCache.apdsClear;
        snap.apdsProximity = gApdsCache.apdsProximity;
        snap.apdsGesture = gApdsCache.apdsGesture;
      }
    }
#endif

#if ENABLE_GPS_SENSOR
    if (mask & LOG_GPS) {
      snap.gGpsEnabled = gGpsEnabled;
      snap.gGpsConnected = gGpsConnected;
      SensorCacheGuard g(gGpsCache.mutex, pdMS_TO_TICKS(10), "sensorLog.gpsSnapshot");
      if (g.held) {
        if (gGpsCache.dataValid && gGpsCache.hasFix) {
          snap.gpsFix = true;
          snap.gpsLatitude = gGpsCache.latitude;
          snap.gpsLongitude = gGpsCache.longitude;
          snap.gpsAltitude = gGpsCache.altitude;
          snap.gpsSpeed = gGpsCache.speed;
          snap.gpsSatellites = gGpsCache.satellites;
          snap.gpsFixQuality = gGpsCache.fixQuality;
          snap.gpsHour = gGpsCache.hour;
          snap.gpsMinute = gGpsCache.minute;
          snap.gpsSecond = gGpsCache.second;
          snap.gpsHasTime = true;
        } else {
          snap.gpsFix = false;
          snap.gpsHasTime = false;
        }
      }
    }
#endif

#if ENABLE_PRESENCE_SENSOR
    if (mask & LOG_PRESENCE) {
      snap.gPresenceEnabled = gPresenceEnabled;
      snap.gPresenceConnected = gPresenceConnected;
      SensorCacheGuard g(gPresenceCache.mutex, pdMS_TO_TICKS(10), "sensorLog.presenceSnapshot");
      if (g.held) {
        snap.presenceAmbientTemp = gPresenceCache.ambientTemp;
        snap.presenceValue = gPresenceCache.presenceValue;
        snap.motionValue = gPresenceCache.motionValue;
        snap.presenceDetected = gPresenceCache.presenceDetected;
        snap.motionDetected = gPresenceCache.motionDetected;
      }
    }
#endif

    // Check if any selected sensor has active data
    bool hasSelectedData = false;
    if ((gSensorLogMask & LOG_THERMAL) && snap.gThermalEnabled && snap.gThermalConnected) hasSelectedData = true;
    if ((gSensorLogMask & LOG_TOF) && snap.gTofEnabled && snap.gTofConnected) hasSelectedData = true;
    if ((gSensorLogMask & LOG_IMU) && snap.gImuEnabled && snap.gImuConnected) hasSelectedData = true;
    if ((gSensorLogMask & LOG_GAMEPAD) && snap.gInputEnabled && snap.gInputConnected) hasSelectedData = true;
    if ((gSensorLogMask & LOG_APDS) && snap.gApdsConnected) hasSelectedData = true;
    if ((gSensorLogMask & LOG_GPS) && snap.gGpsEnabled && snap.gGpsConnected) hasSelectedData = true;
    if ((gSensorLogMask & LOG_PRESENCE) && snap.gPresenceEnabled && snap.gPresenceConnected) hasSelectedData = true;

    // Suppress idle lines when no selected sensor has data
    static unsigned long lastHeartbeatMs = 0;
    const unsigned long heartbeatMs = 5000;
    if (!hasSelectedData) {
      if (lastHeartbeatMs != 0 && (long)(nowMs - lastHeartbeatMs) < (long)heartbeatMs) {
        log_idle_skips++;
        if (isDebugFlagSet(DEBUG_LOGGER)) {
          DEBUG_LOGGERF("logger: idle skip #%u (dt=%lums)", (unsigned)log_idle_skips, (nowMs - lastHeartbeatMs));
        }
        return;
      }
      lastHeartbeatMs = nowMs;
      if (isDebugFlagSet(DEBUG_LOGGER)) {
        DEBUG_LOGGERF("logger: heartbeat at %lu ms", nowMs);
      }
    } else {
      lastHeartbeatMs = nowMs;
    }

    // Choose format
    const char* line = nullptr;
    if (gSensorLogFormat == SENSOR_LOG_TRACK) {
      line = buildTrackFromSnap(snap);
    } else if (gSensorLogFormat == SENSOR_LOG_CSV) {
      line = buildCSVFromSnap(snap);
    } else {
      line = buildFromSnap(snap);
    }
    if (line && line[0] != '\0') {
      fsLock("sensorlog.append");
      // Route to LittleFS primary, or /sd overflow mirror if flash is full.
      // The active path for this write cycle is resolved per-write so a hot
      // card plug-in can start working immediately, but the overflow latch
      // itself is one-way per reboot (see VFS::resolveOverflowPath).
      char activePath[128];
      bool onSd = VFS::resolveOverflowPath(gSensorLogPath.c_str(),
                                           (size_t)gSensorLogMaxSize,
                                           activePath, sizeof(activePath));
      File f = VFS::openGuarded(String(activePath), "a", VFS::systemAuth("senlog.append"), true);
      if (f) {
        size_t len = strlen(line);
        f.write((const uint8_t*)line, len);
        f.write('\n');
        f.close();
        gSensorLogLastWrite = millis();
        writeCount++;
        log_writes++;
        approxSizeBytes += (len + 1);

        // Handle log rotation. Rotation runs on whichever tier we're writing
        // to. When overflow is active we rotate inside `/sd/...` and the
        // LittleFS copies are preserved (never touched again).
        if (approxSizeBytes > gSensorLogMaxSize && (lastTruncateMs == 0 || (long)(millis() - lastTruncateMs) >= (long)truncateCooldownMs)) {
          lastTruncateMs = millis();
          writeCount = 0;

          if (gSensorLogMaxRotations > 0) {
            if (gSensorLogMaxRotations > 1) {
              char oldestFile[128];
              snprintf(oldestFile, sizeof(oldestFile), "%s.%d", activePath, gSensorLogMaxRotations);
              if (VFS::existsGuarded(String(oldestFile), VFS::systemAuth("senlog.rotate"))) {
                VFS::removeGuarded(String(oldestFile), VFS::systemAuth("senlog.rotate"));
              }
            }

            // Shift existing rotations up: .i -> .(i+1) for i = N-1..1. The base
            // file is moved to .1 separately AFTER this loop, so the loop must
            // operate on the numbered files only — never on the base path.
            for (int i = gSensorLogMaxRotations - 1; i >= 1; i--) {
              char fromFile[128], toFile[128];
              snprintf(fromFile, sizeof(fromFile), "%s.%d", activePath, i);
              snprintf(toFile, sizeof(toFile), "%s.%d", activePath, i + 1);
              if (VFS::existsGuarded(String(fromFile), VFS::systemAuth("senlog.rotate"))) {
                VFS::renameGuarded(String(fromFile), String(toFile), VFS::systemAuth("senlog.rotate"));
              }
            }

            if (VFS::existsGuarded(String(activePath), VFS::systemAuth("senlog.rotate"))) {
              String rotatedFile = String(activePath) + ".1";
              VFS::renameGuarded(String(activePath), rotatedFile, VFS::systemAuth("senlog.rotate"));
            }
          } else {
            VFS::removeGuarded(String(activePath), VFS::systemAuth("senlog.rotate"));
          }

          approxSizeBytes = 0;
          log_trunc++;
          if (isDebugFlagSet(DEBUG_STORAGE)) {
            DEBUGF_BROADCAST(DEBUG_STORAGE, "Sensor log: rotated file on %s (max size=%u bytes)",
                             onSd ? "SD" : "LittleFS", (unsigned)gSensorLogMaxSize);
          }
          if (isDebugFlagSet(DEBUG_LOGGER)) {
            DEBUG_LOGGERF("logger: rotated at approxSize=%u tier=%s",
                          (unsigned)gSensorLogMaxSize, onSd ? "sd" : "little");
          }
        }
        
        if (isDebugFlagSet(DEBUG_STORAGE)) {
          DEBUGF_BROADCAST(DEBUG_STORAGE, "Sensor log: wrote %d bytes", (int)len);
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

// ============================================================================
// Command Handler
// ============================================================================

const char* cmd_sensorlog(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  CommandArgs a(argsInput);
  if (a.count() == 0) {
    return "Usage: sensorlog <start|stop|status|format|maxsize|rotations|sensors|autostart> [args...]\n"
           "  start <filepath> [interval_ms]: Begin logging (default 5000ms)\n"
           "  stop: Stop logging\n"
           "  status: Show current logging status\n"
           "  format <text|csv|track>: Set log format (default: text)\n"
           "    track = GPS-only compact format with signal loss dedup\n"
           "  maxsize <bytes>: Set max file size before rotation (default: 256000)\n"
           "  rotations <count>: Set number of old logs to keep (0-9, default: 3)\n"
           "  sensors <thermal|tof|imu|gamepad|apds|gps|presence|all|none>: Select sensors to log\n"
           "  autostart [on|off]: Auto-start logging on boot with last-used parameters";
  }
  String subCmd = a.arg(0);
  subCmd.toLowerCase();

  // Handle 'status' subcommand
  if (subCmd == "status") {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    // Build sensors string
    String sensors = "";
    if (gSensorLogMask & LOG_THERMAL) sensors += "thermal ";
    if (gSensorLogMask & LOG_TOF) sensors += "tof ";
    if (gSensorLogMask & LOG_IMU) sensors += "imu ";
    if (gSensorLogMask & LOG_GAMEPAD) sensors += "gamepad ";
    if (gSensorLogMask & LOG_APDS) sensors += "apds ";
    if (gSensorLogMask & LOG_GPS) sensors += "gps ";
    if (gSensorLogMask & LOG_PRESENCE) sensors += "presence ";
    if (sensors.length() == 0) sensors = "(none)";
    
    const char* fmtName = (gSensorLogFormat == SENSOR_LOG_CSV) ? "CSV" :
                           (gSensorLogFormat == SENSOR_LOG_TRACK) ? "TRACK" : "TEXT";
    char* buf = getDebugBuffer();
    if (gSensorLoggingEnabled) {
      snprintf(buf, 1024,
        "Sensor logging ACTIVE\n"
        "  File: %s\n"
        "  Interval: %lums\n"
        "  Format: %s\n"
        "  Max size: %u bytes\n"
        "  Rotations: %u\n"
        "  Sensors: %s\n"
        "  Auto-start: %s\n"
        "  Last write: %lus ago",
        gSensorLogPath.c_str(),
        (unsigned long)gSensorLogIntervalMs,
        fmtName,
        (unsigned)gSensorLogMaxSize,
        (unsigned)gSensorLogMaxRotations,
        sensors.c_str(),
        gSettings.sensorLogAutoStart ? "ON" : "OFF",
        (millis() - gSensorLogLastWrite) / 1000);
    } else {
      snprintf(buf, 1024,
        "Sensor logging is INACTIVE\n"
        "  Format: %s\n"
        "  Max size: %u bytes\n"
        "  Rotations: %u\n"
        "  Sensors: %s\n"
        "  Auto-start: %s",
        fmtName,
        (unsigned)gSensorLogMaxSize,
        (unsigned)gSensorLogMaxRotations,
        sensors.c_str(),
        gSettings.sensorLogAutoStart ? "ON" : "OFF");
    }
    return buf;
  }

  // Handle 'stop' subcommand
  if (subCmd == "stop") {
    if (!gSensorLoggingEnabled) {
      return "Sensor logging is not running";
    }
    gSensorLoggingEnabled = false;
    notifySensorStopped("Logging");
    broadcastOutput("Sensor logging stop requested; will stop safely");
    return "SUCCESS: Sensor logging stop requested; will stop safely";
  }

  // Handle 'start' subcommand
  if (subCmd == "start") {
    if (gSensorLoggingEnabled) {
      return "Sensor logging already running. Use 'sensorlog stop' first.";
    }

    if (!a.has(1)) {
      return "Usage: sensorlog start <filepath> [interval_ms]\n"
             "Example: sensorlog start /logs/sensors/sensors.txt 1000";
    }

    String filepath = a.arg(1);
    uint32_t interval = gSensorLogIntervalMs;

    if (a.has(2)) {
      interval = a.argInt(2, (int)gSensorLogIntervalMs);
      if (interval < 100) interval = 100;
      if (interval > 3600000) interval = 3600000;
    }

    if (filepath.length() == 0 || filepath.charAt(0) != '/') {
      return "Error: Filepath must start with / (e.g., /logs/sensors/sensors.txt)";
    }

    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

    // Ensure directory exists (mkdir is non-recursive, create parents first)
    int lastSlash = filepath.lastIndexOf('/');
    if (lastSlash > 0) {
      String dir = filepath.substring(0, lastSlash);
      if (!VFS::existsGuarded(dir, VFS::systemAuth("senlog.setup_mkdir"))) {
        // Create parent directories iteratively via VFS so the sensor log's
        // write path and directory creation use the same dispatcher.
        for (int i = 1; i <= (int)dir.length(); i++) {
          if (i == (int)dir.length() || dir.charAt(i) == '/') {
            String parent = dir.substring(0, i);
            if (parent.length() > 0 && !VFS::existsGuarded(parent, VFS::systemAuth("senlog.setup_mkdir"))) {
              VFS::mkdirGuarded(parent, VFS::systemAuth("senlog.setup_mkdir"));
            }
          }
        }
        if (!VFS::existsGuarded(dir, VFS::systemAuth("senlog.setup_mkdir"))) {
          snprintf(getDebugBuffer(), 1024, "Error: Failed to create directory: %s", dir.c_str());
          return getDebugBuffer();
        }
        broadcastOutput("Created directory: " + dir);
      }
    }

    // Create file if needed
    if (!VFS::existsGuarded(filepath, VFS::systemAuth("senlog.setup_create"))) {
      File f = VFS::openGuarded(filepath, "w", VFS::systemAuth("senlog.setup_create"), true);
      if (!f) {
        snprintf(getDebugBuffer(), 1024, "Error: Failed to create file: %s", filepath.c_str());
        return getDebugBuffer();
      }

      if (gSensorLogFormat == SENSOR_LOG_CSV) {
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
        if (gSensorLogMask & LOG_GPS) csvHeader += ",gps_fix,gps_lat,gps_lon,gps_alt,gps_speed,gps_sats,gps_quality";
        if (gSensorLogMask & LOG_PRESENCE) csvHeader += ",presence_ambient,presence_value,presence_detected,motion_value,motion_detected";
        csvHeader += "\n";
        f.write((const uint8_t*)csvHeader.c_str(), csvHeader.length());
      } else if (gSensorLogFormat == SENSOR_LOG_TRACK) {
        const char* trackHeader = "# GPS Track Log\n"
                                  "# time,lat,lon,alt_m,speed_kn,satellites\n"
                                  "# Signal loss: time,---,SIGNAL_LOST\n"
                                  "# Signal regained: time,~~~,SIGNAL_REGAINED (lost N intervals)\n";
        f.write((const uint8_t*)trackHeader, strlen(trackHeader));
      }
      f.close();
      broadcastOutput("Created log file: " + filepath);
    }

    if (ESP.getFreeHeap() < 8192) {
      return "Error: Insufficient memory (need 8KB free)";
    }

    // Check filesystem space — need at least gSensorLogMaxSize free on
    // LittleFS OR SD card (if mounted) to allow start. Writes will route
    // transparently to whichever tier has room via the overflow system.
    {
      extern void fsLock(const char* tag);
      extern void fsUnlock();
      fsLock("sensorlog.spacecheck");
      size_t totalBytes = LittleFS.totalBytes();
      size_t usedBytes = LittleFS.usedBytes();
      fsUnlock();
      size_t freeBytes = (totalBytes > usedBytes) ? (totalBytes - usedBytes) : 0;
      bool littleFsOk = (freeBytes >= gSensorLogMaxSize);
      bool sdOk = false;
      if (!littleFsOk && VFS::isSDAvailable()) {
        uint64_t sdTotal = 0, sdUsed = 0, sdFree = 0;
        if (VFS::getStats(VFS::SDCARD, sdTotal, sdUsed, sdFree)) {
          sdOk = (sdFree >= (uint64_t)gSensorLogMaxSize);
        }
      }
      if (!littleFsOk && !sdOk) {
        char errMsg[120];
        snprintf(errMsg, sizeof(errMsg),
                 "Not enough space for log (need %uKB, flash has %uKB, no usable SD overflow)",
                 (unsigned)(gSensorLogMaxSize / 1024), (unsigned)(freeBytes / 1024));
        notifySensorStarted("Logging", false);
        snprintf(getDebugBuffer(), 1024, "Error: %s", errMsg);
        return getDebugBuffer();
      }
    }

    gSensorLogPath = filepath;
    gSensorLogIntervalMs = interval;
    gSensorLoggingEnabled = true;
    gSensorLogLastWrite = millis();

    // Persist last-used parameters for auto-start
    setSetting(gSettings.sensorLogPath, filepath);
    setSetting(gSettings.sensorLogIntervalMs, (int)interval);
    setSetting(gSettings.sensorLogMask, (int)gSensorLogMask);
    setSetting(gSettings.sensorLogFormat, (int)gSensorLogFormat);

    notifySensorStarted("Logging", true);
    snprintf(getDebugBuffer(), 1024, "SUCCESS: Sensor logging started\n  File: %s\n  Interval: %lums",
             filepath.c_str(), (unsigned long)interval);
    broadcastOutput(getDebugBuffer());
    return getDebugBuffer();
  }

  // Handle 'format' subcommand
  if (subCmd == "format") {
    if (!a.has(1)) {
      const char* fmtName = (gSensorLogFormat == SENSOR_LOG_CSV) ? "csv" :
                             (gSensorLogFormat == SENSOR_LOG_TRACK) ? "track" : "text";
      snprintf(getDebugBuffer(), 1024, "Current format: %s\nUsage: sensorlog format <text|csv|track>\n"
               "  text: Human-readable sensor data\n"
               "  csv: Structured CSV data\n"
               "  track: GPS-only compact track (time,lat,lon,alt,speed,sats) with signal loss dedup",
               fmtName);
      return getDebugBuffer();
    }

    String formatType = a.arg(1);
    formatType.toLowerCase();

    if (formatType == "csv") {
      gSensorLogFormat = SENSOR_LOG_CSV;
      setSetting(gSettings.sensorLogFormat, (int)SENSOR_LOG_CSV);
      return "Log format set to CSV (applies to next 'sensorlog start')";
    } else if (formatType == "text") {
      gSensorLogFormat = SENSOR_LOG_TEXT;
      setSetting(gSettings.sensorLogFormat, (int)SENSOR_LOG_TEXT);
      return "Log format set to TEXT (applies to next 'sensorlog start')";
    } else if (formatType == "track") {
      gSensorLogFormat = SENSOR_LOG_TRACK;
      gSensorLogMask = LOG_GPS;  // Track format is GPS-only
      setSetting(gSettings.sensorLogFormat, (int)SENSOR_LOG_TRACK);
      setSetting(gSettings.sensorLogMask, (int)gSensorLogMask);
      return "Log format set to TRACK (GPS-only, applies to next 'sensorlog start')";
    } else {
      return "Error: Format must be 'text', 'csv', or 'track'";
    }
  }

  // Handle 'maxsize' subcommand
  if (subCmd == "maxsize") {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    if (!a.has(1)) {
      snprintf(getDebugBuffer(), 1024, "Current max size: %u bytes\nUsage: sensorlog maxsize <bytes>",
               (unsigned)gSensorLogMaxSize);
      return getDebugBuffer();
    }

    size_t newSize = a.argInt(1, 0);

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
    if (!a.has(1)) {
      snprintf(getDebugBuffer(), 1024, "Current rotations: %u\nUsage: sensorlog rotations <count>\n"
                                   "Set to 0 to disable rotation (delete old logs)",
               (unsigned)gSensorLogMaxRotations);
      return getDebugBuffer();
    }

    int count = a.argInt(1, 0);

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
    if (!a.has(1)) {
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
      n = snprintf(p, remaining, "  %s GPS\n", (gSensorLogMask & LOG_GPS) ? "☑" : "☐");
      p += n; remaining -= n;
      n = snprintf(p, remaining, "  %s Presence\n", (gSensorLogMask & LOG_PRESENCE) ? "☑" : "☐");
      p += n; remaining -= n;
      snprintf(p, remaining, "\nUsage: sensorlog sensors <thermal|tof|imu|gamepad|apds|gps|presence|all|none>");
      return getDebugBuffer();
    }

    String sensorList = a.remaining(0);
    sensorList.toLowerCase();

    if (sensorList == "all") {
      gSensorLogMask = LOG_THERMAL | LOG_TOF | LOG_IMU | LOG_GAMEPAD | LOG_APDS | LOG_GPS | LOG_PRESENCE;
      setSetting(gSettings.sensorLogMask, (int)gSensorLogMask);
      return "All sensors enabled for logging";
    }

    if (sensorList == "none") {
      gSensorLogMask = 0x00;
      setSetting(gSettings.sensorLogMask, (int)gSensorLogMask);
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
      else if (sensor == "gamepad" || sensor == "input") gSensorLogMask |= LOG_GAMEPAD;
      else if (sensor == "apds") gSensorLogMask |= LOG_APDS;
      else if (sensor == "gps") gSensorLogMask |= LOG_GPS;
      else if (sensor == "presence") gSensorLogMask |= LOG_PRESENCE;
      else {
        snprintf(getDebugBuffer(), 1024, "Error: Unknown sensor '%s'", sensor.c_str());
        return getDebugBuffer();
      }

      if (comma < 0) break;
      start = comma + 1;
    }

    setSetting(gSettings.sensorLogMask, (int)gSensorLogMask);
    snprintf(getDebugBuffer(), 1024, "Logging enabled for: %s%s%s%s%s%s%s",
             (gSensorLogMask & LOG_THERMAL) ? "thermal " : "",
             (gSensorLogMask & LOG_TOF) ? "tof " : "",
             (gSensorLogMask & LOG_IMU) ? "imu " : "",
             (gSensorLogMask & LOG_GAMEPAD) ? "gamepad " : "",
             (gSensorLogMask & LOG_APDS) ? "apds " : "",
             (gSensorLogMask & LOG_GPS) ? "gps " : "",
             (gSensorLogMask & LOG_PRESENCE) ? "presence " : "");
    return getDebugBuffer();
  }

  // Handle 'interval' subcommand
  if (subCmd == "interval") {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    if (!a.has(1)) {
      snprintf(getDebugBuffer(), 1024, "Current interval: %lums\nUsage: sensorlog interval <ms> (100-3600000)",
               (unsigned long)gSensorLogIntervalMs);
      return getDebugBuffer();
    }
    uint32_t ms = (uint32_t)a.argInt(1, (int)gSensorLogIntervalMs);
    if (ms < 100) ms = 100;
    if (ms > 3600000) ms = 3600000;
    gSensorLogIntervalMs = ms;
    setSetting(gSettings.sensorLogIntervalMs, (int)ms);
    snprintf(getDebugBuffer(), 1024, "Sensor log interval set to %lums", (unsigned long)ms);
    return getDebugBuffer();
  }

  // Handle 'autostart' subcommand
  if (subCmd == "autostart") {
    if (!a.has(1)) {
      // Toggle
      bool newVal = !gSettings.sensorLogAutoStart;
      setSetting(gSettings.sensorLogAutoStart, newVal);
      return newVal ? "Sensor logging auto-start ENABLED" : "Sensor logging auto-start DISABLED";
    }
    bool enable = a.argBool(1, false);
    setSetting(gSettings.sensorLogAutoStart, enable);
    return enable ? "Sensor logging auto-start ENABLED" : "Sensor logging auto-start DISABLED";
  }

  return "Error: Unknown subcommand. Use: start, stop, status, format, maxsize, rotations, sensors, or autostart";
}

// ============================================================================
// Command Registry
// ============================================================================

// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
const CommandEntry sensorLoggingCommands[] = {
  { "sensorlog", "Sensor data logging: start, stop, status, format, maxsize, rotations, sensors", false, cmd_sensorlog,
    "Usage: sensorlog <start|stop|status|format|maxsize|rotations|sensors|interval|autostart> [args...]\n"
    "  start <filepath> [interval_ms]: Begin logging (default 5000ms)\n"
    "  stop: Stop logging\n"
    "  status: Show current logging status\n"
    "  format <text|csv|track>: Set log format (default: text)\n"
    "    track = GPS-only compact format with signal loss dedup\n"
    "  maxsize <bytes>: Set max file size before rotation (default: 256000)\n"
    "  rotations <count>: Set number of old logs to keep (0-9, default: 3)\n"
    "  sensors <thermal|tof|imu|gamepad|apds|gps|presence|all|none>: Select sensors to log\n"
    "  interval <ms>: Set poll interval 100-3600000 (default 5000)\n"
    "  autostart [on|off]: Auto-start logging on boot (bare = toggle)" },
};

const size_t sensorLoggingCommandsCount = sizeof(sensorLoggingCommands) / sizeof(sensorLoggingCommands[0]);

// Registration handled by gCommandModules[] in System_Utils.cpp

// ============================================================================
// Settings Module Registration
// ============================================================================

// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry sensorLogSettingEntries[] = {
  { "sensorLogAutoStart",    SETTING_BOOL,   &gSettings.sensorLogAutoStart,    0, 0, nullptr, 0, 1,       "Auto-start logging after boot", nullptr, false, nullptr, "sensorlog autostart" },
  { "sensorLogPath", SETTING_STRING, &gSettings.sensorLogPath, 0, 0, "/logs/sensors/sensors.txt", 0, 0, "Log file path", nullptr, false, nullptr, nullptr },
  { "sensorLogIntervalMs", SETTING_INT, &gSettings.sensorLogIntervalMs, 5000, 0, nullptr, 100, 3600000, "Poll interval (ms)", nullptr, false, nullptr, nullptr },
  { "sensorLogMask", SETTING_INT, &gSettings.sensorLogMask, 0, 0, nullptr, 0, 255, "Sensor bitmask", nullptr, false, nullptr, nullptr },
  { "sensorLogFormat", SETTING_INT, &gSettings.sensorLogFormat, 0, 0, nullptr, 0, 2, "Format (0=text,1=csv,2=track)", "0|Text,1|CSV,2|Track", false, nullptr, nullptr }
};

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule sensorLogSettingsModule = {
  "sensorlog",
  "logging.sensorlog",
  sensorLogSettingEntries,
  sizeof(sensorLogSettingEntries) / sizeof(sensorLogSettingEntries[0]),
  nullptr,
  "Sensor data logging auto-start and parameters"
};

// ============================================================================
// Auto-Start (called from boot after sensors are initialized)
// ============================================================================

void sensorLogAutoStart() {
  if (!gSettings.sensorLogAutoStart) return;
  if (gSensorLoggingEnabled) return;  // Already running

  // Restore persisted parameters
  if (gSettings.sensorLogMask > 0) gSensorLogMask = (uint8_t)gSettings.sensorLogMask;
  if (gSettings.sensorLogFormat >= 0 && gSettings.sensorLogFormat <= 2) gSensorLogFormat = (SensorLogFormat)gSettings.sensorLogFormat;
  if (gSettings.sensorLogIntervalMs >= 100) gSensorLogIntervalMs = gSettings.sensorLogIntervalMs;

  if (gSensorLogMask == 0) {
    broadcastOutput("[sensorlog] Auto-start skipped: no sensors selected");
    return;
  }

  String path = gSettings.sensorLogPath;
  if (path.length() == 0 || path.charAt(0) != '/') {
    path = "/logs/sensors/sensors.txt";
  }

  // Append timestamp to filename to prevent appending to old logs
  // Extract base path and extension
  int lastSlash = path.lastIndexOf('/');
  int lastDot = path.lastIndexOf('.');
  String dir = (lastSlash >= 0) ? path.substring(0, lastSlash + 1) : "/logs/sensors/";
  String baseName = (lastSlash >= 0) ? path.substring(lastSlash + 1) : path;
  String ext = "";
  
  if (lastDot > lastSlash && lastDot > 0) {
    ext = path.substring(lastDot);
    baseName = baseName.substring(0, baseName.lastIndexOf('.'));
  }
  
  // Strip any existing timestamp suffixes from baseName to prevent double-timestamping.
  // Timestamps start with "-YYYY-" (e.g. "-2026-04-04T...") or "-boot".
  // Scan forward for the first such pattern rather than looking at the last segment,
  // because the last segment is often just the seconds ("02") which has no T or boot.
  {
    int stripAt = -1;
    for (int i = 0; i < (int)baseName.length() - 5; i++) {
      if (baseName.charAt(i) == '-') {
        // Check for "-YYYY-" (4 digits followed by another dash)
        if (isdigit(baseName.charAt(i+1)) && isdigit(baseName.charAt(i+2)) &&
            isdigit(baseName.charAt(i+3)) && isdigit(baseName.charAt(i+4)) &&
            baseName.charAt(i+5) == '-') {
          stripAt = i;
          break;
        }
        // Check for "-boot"
        if (baseName.length() > (size_t)(i + 4) &&
            baseName.charAt(i+1) == 'b' && baseName.charAt(i+2) == 'o' &&
            baseName.charAt(i+3) == 'o' && baseName.charAt(i+4) == 't') {
          stripAt = i;
          break;
        }
      }
    }
    if (stripAt > 0) baseName = baseName.substring(0, stripAt);
  }
  
  // Get current time for timestamp (fallback to boot counter + ms if no RTC/NTP)
  time_t now = time(nullptr);
  char timestamp[32];
  extern uint32_t gBootCounter;
  
  if (now > 1609459200) {  // Valid if after 2021-01-01 (epoch 1609459200)
    struct tm* timeinfo = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H-%M-%S", timeinfo);
  } else {
    // No valid time - use boot counter and milliseconds for tracking power cycles
    snprintf(timestamp, sizeof(timestamp), "boot%lu-%lu", (unsigned long)gBootCounter, millis());
  }
  
  // Build timestamped path: /logs/sensors/sensors-2026-02-17T14-30-00.txt or /logs/sensors/sensors-boot12345.txt
  char pathBuf[256];
  snprintf(pathBuf, sizeof(pathBuf), "%s%s-%s%s", dir.c_str(), baseName.c_str(), timestamp, ext.c_str());
  path = pathBuf;

  // Ensure /logs/sensors directory exists before starting (mkdir is non-recursive).
  // Uses VFS::mkdirGuarded for consistency with the overflow-capable write path.
  if (!VFS::existsGuarded("/logs", VFS::systemAuth("senlog.autostart_mkdir"))) {
    VFS::mkdirGuarded("/logs", VFS::systemAuth("senlog.autostart_mkdir"));
  }
  if (!VFS::existsGuarded("/logs/sensors", VFS::systemAuth("senlog.autostart_mkdir"))) {
    if (!VFS::mkdirGuarded("/logs/sensors", VFS::systemAuth("senlog.autostart_mkdir"))) {
      broadcastOutput("[sensorlog] Auto-start failed: Could not create /logs/sensors directory");
      return;
    }
    broadcastOutput("[sensorlog] Created /logs/sensors directory");
  }

  // Build and execute the CLI command so all validation/space checks run
  char cmd[320];
  snprintf(cmd, sizeof(cmd), "sensorlog start %s %d", path.c_str(), (int)gSensorLogIntervalMs);
  BROADCAST_PRINTF("[sensorlog] Auto-start: %s", cmd);
  const char* result = cmd_sensorlog(String(cmd).substring(10));  // Pass everything after "sensorlog " (10 chars)
  if (result && strncmp(result, "SUCCESS", 7) != 0) {
    // Command failed - broadcast the error
    BROADCAST_PRINTF("[sensorlog] Auto-start failed: %s", result);
  }
}
