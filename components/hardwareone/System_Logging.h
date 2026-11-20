#ifndef SYSTEM_LOGGING_H
#define SYSTEM_LOGGING_H

#include <Arduino.h>

// ============================================================================
// Centralized Logging System
// ============================================================================
// Centralized logging utilities and file path definitions
//
// ─── Which logging mechanism should I use? ──────────────────────────────────
// The project log is the default. The other routes stay for stated reasons.
// CONTRIBUTING.md, "Logging", has the full rule; the host test `logging_routes`
// counts the calls of routes 3 and 4 per file.
//
//   1. The project log  (area macros from "System_Debug.h")
//        DEBUG_<AREA>F, INFO_<AREA>F, WARN_<AREA>F, ERROR_<AREA>F.
//        A line is formatted into a pooled message and queued without
//        blocking. It follows the debug flags and the log level and reaches
//        the serial port, the web log viewer and the log files; ERROR lines
//        are also kept in LOG_ERROR_FILE.
//        Use for:  everything that is not covered by 2 to 4.
//
//   2. logSystemEvent()  (from "System_Debug.h")
//        For durable system lifecycle events: boot decisions, FS format /
//        file deletions, settings load/save failures, WiFi connects.
//        Always-on (independent of debug flags and log level), persisted to
//        LOG_EVENTS_FILE, safe to call before the debug system exists.
//        Keep it LOW VOLUME — discrete state changes only.
//
//   3. ESP_LOGE / ESP_LOGW / ESP_LOGI / ESP_LOGD   (from "esp_log.h")
//        Serial port only, unless the `loglink` command routes the IDF log
//        into the project log. Three kinds of line use it:
//          - a bench record that a tool or a test reads from the console
//            ("STT: TL ...", BLE_COEX, RADIO_STATS, CAPTEST);
//          - a path where the project log is not running yet or cannot be
//            trusted: early boot, update safety, filesystem mount, memory
//            diagnostics;
//          - a driver unit that a host harness compiles without the project
//            log (HAL_Radio, the HAL display and camera files).
//
//   4. Serial.print / Serial.printf / esp_rom_printf
//        No timestamp, no level, no tag, not mirrored to web. Only in the
//        setup wizards, the serial console itself, the writer of the project
//        log, and fatal paths before the log exists.
//
// Level guidance (applies to both ESP_LOG* and the project log):
//   ERROR — something failed that a user/admin needs to know about
//   WARN  — recoverable issue (sensor timeout, client disconnect mid-stream)
//   INFO  — state changes (boot, connect, command OK)
//   DEBUG — verbose per-iteration data; off by default in release builds
// ────────────────────────────────────────────────────────────────────────────

// Log file paths
extern const char* const LOG_OK_FILE;      // Successful login events
extern const char* const LOG_FAIL_FILE;    // Failed login attempts
extern const char* const LOG_I2C_FILE;     // I2C device errors
extern const char* const LOG_ERROR_FILE;   // ERROR_* macro lines from debug queue ([ERROR]...)
extern const char* const LOG_EVENTS_FILE;  // System lifecycle events ([EVENT]...) — see logSystemEvent()
extern const char* const LOG_EVENT_STREAM_FILE;  // Structured event-ring history ([EVLOG]...) — see systemEventLogTick()

// Log file caps (bytes). These are the upper bounds: the capped-log writer
// scales the cap it is given to the LittleFS partition, so that all capped
// logs together use at most a quarter of it (UP-03, System_LogCapsCore.h;
// cappedLogScaledCap() gives the size in effect). On the P4X-EYE layout they
// come to about 82 % of these values. Change a value here and in
// System_LogCapsCore.h together; the host test log_caps compares them.
constexpr size_t LOG_CAP_BYTES = 696969;  // ~680 KB (for login logs)
constexpr size_t LOG_I2C_CAP = 64 * 1024;  // 64KB (for I2C errors)
constexpr size_t LOG_ERROR_CAP = 256 * 1024;  // 256KB ring for application errors
constexpr size_t LOG_EVENTS_CAP = 256 * 1024;  // 256KB ring for system events
constexpr size_t LOG_EVENT_STREAM_CAP = 256 * 1024;  // 256KB ring-derived structured event history

// Time sync marker flag
extern bool gTimeSyncedMarkerWritten;

// Time sync logging. `source` names who supplied the clock ("ntp", "rtc",
// "ring", "manual", "carryover") for the forensic marker line.
void logTimeSyncedMarkerIfReady(const char* source);

// Always-on per-boot orientation divider, written to the login/i2c/error logs
// regardless of NTP. system-events.log is skipped (it gets [EVENT][BOOT]). Call
// once early in boot; pass the reset-reason string (resetReasonName()).
void logBootAnchorToLogs(const char* resetReason, const char* detail = nullptr);

// I2C-specific logging
void logI2cError(uint8_t address, const char* deviceName, int consecutiveErrors, int totalErrors, bool nowDegraded);
void logI2cRecovery(uint8_t address, const char* deviceName, int totalErrors);

#endif // SYSTEM_LOGGING_H
