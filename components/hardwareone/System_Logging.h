#ifndef SYSTEM_LOGGING_H
#define SYSTEM_LOGGING_H

#include <Arduino.h>

// ============================================================================
// Centralized Logging System
// ============================================================================
// Centralized logging utilities and file path definitions
//
// ─── Which logging mechanism should I use? ──────────────────────────────────
// This codebase has three ways to emit log output. Pick by audience:
//
//   1. ESP_LOGE / ESP_LOGW / ESP_LOGI / ESP_LOGD   (from "esp_log.h")
//        For low-level / driver / IDF-facing diagnostics. Tagged, colored,
//        runtime-filterable via esp_log_level_set("TAG", level).
//        Use for:  hardware init failures, driver errors, IDF subsystem events.
//
//   2. System_Debug  (DEBUG_PRINT, ERROR_PRINT, etc. from "System_Debug.h")
//        For user-visible application events. Timestamped, queued to PSRAM,
//        mirrored to the web log viewer, and persisted to LOG_ERROR_FILE
//        when emitted at ERROR level.
//        Use for:  login events, command execution, WiFi/BT state changes,
//                  anything an admin watching the web UI should see.
//
//   3. Raw printf / Serial.print
//        Avoid. No timestamp, no level, no tag, not mirrored to web.
//        Only acceptable inside first-time setup wizards that run before
//        System_Debug is initialized.
//
//   4. logSystemEvent()  (from "System_Debug.h")
//        For durable system lifecycle events: boot decisions, FS format /
//        file deletions, settings load/save failures, WiFi connects.
//        Always-on (independent of debug flags and log level), persisted to
//        LOG_EVENTS_FILE, safe to call before the debug system exists.
//        Keep it LOW VOLUME — discrete state changes only.
//
// Level guidance (applies to both ESP_LOG* and System_Debug ERROR/WARN/INFO):
//   ERROR — something failed that a user/admin needs to know about
//   WARN  — recoverable issue (sensor timeout, client disconnect mid-stream)
//   INFO  — state changes (boot, connect, command OK)
//   DEBUG — verbose per-iteration data; off by default in release builds
// ────────────────────────────────────────────────────────────────────────────

// Log file paths
extern const char* LOG_OK_FILE;      // Successful login events
extern const char* LOG_FAIL_FILE;    // Failed login attempts
extern const char* LOG_I2C_FILE;     // I2C device errors
extern const char* LOG_ERROR_FILE;   // ERROR_* macro lines from debug queue ([ERROR]...)
extern const char* LOG_EVENTS_FILE;  // System lifecycle events ([EVENT]...) — see logSystemEvent()

// Log file caps (bytes)
constexpr size_t LOG_CAP_BYTES = 696969;  // ~680 KB (for login logs)
constexpr size_t LOG_I2C_CAP = 64 * 1024;  // 64KB (for I2C errors)
constexpr size_t LOG_ERROR_CAP = 256 * 1024;  // 256KB ring for application errors
constexpr size_t LOG_EVENTS_CAP = 256 * 1024;  // 256KB ring for system events

// Time sync marker flag
extern bool gTimeSyncedMarkerWritten;

// Time sync logging
void logTimeSyncedMarkerIfReady();

// Always-on per-boot orientation divider, written to the login/i2c/error logs
// regardless of NTP. system-events.log is skipped (it gets [EVENT][BOOT]). Call
// once early in boot; pass the reset-reason string (resetReasonName()).
void logBootAnchorToLogs(const char* resetReason);

// I2C-specific logging
void logI2CError(uint8_t address, const char* deviceName, int consecutiveErrors, int totalErrors, bool nowDegraded);
void logI2CRecovery(uint8_t address, const char* deviceName, int totalErrors);

// Generic logging utility (wraps appendLineWithCap)
void logToFile(const char* path, const String& line, size_t capBytes);

#endif // SYSTEM_LOGGING_H
