#ifndef LOGGING_SYSTEM_H
#define LOGGING_SYSTEM_H

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

// Log file caps (bytes)
constexpr size_t LOG_CAP_BYTES = 696969;  // ~680 KB (for login logs)
constexpr size_t LOG_I2C_CAP = 64 * 1024;  // 64KB (for I2C errors)
constexpr size_t LOG_ERROR_CAP = 256 * 1024;  // 256KB ring for application errors

// Time sync marker flag
extern bool gTimeSyncedMarkerWritten;

// Time sync logging
void logTimeSyncedMarkerIfReady();

// I2C-specific logging
void logI2CError(uint8_t address, const char* deviceName, int consecutiveErrors, int totalErrors, bool nowDegraded);
void logI2CRecovery(uint8_t address, const char* deviceName, int totalErrors);

// Generic logging utility (wraps appendLineWithCap)
void logToFile(const char* path, const String& line, size_t capBytes);

#endif // LOGGING_SYSTEM_H
