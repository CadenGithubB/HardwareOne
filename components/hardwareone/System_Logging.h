#ifndef LOGGING_SYSTEM_H
#define LOGGING_SYSTEM_H

#include <Arduino.h>

// ============================================================================
// Centralized Logging System
// ============================================================================
// Centralized logging utilities and file path definitions

// Log file paths
extern const char* LOG_OK_FILE;      // Successful login events
extern const char* LOG_FAIL_FILE;    // Failed login attempts
extern const char* LOG_I2C_FILE;     // I2C device errors

// Log file caps (bytes)
constexpr size_t LOG_CAP_BYTES = 696969;  // ~680 KB (for login logs)
constexpr size_t LOG_I2C_CAP = 64 * 1024;  // 64KB (for I2C errors)

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
