/**
 * System Utilities Header
 * 
 * Shared utility functions used across multiple subsystems
 * Including the centralized command registry system
 */

#ifndef SYSTEM_UTILS_H
#define SYSTEM_UTILS_H

#include <Arduino.h>

// ============================================================================
// Task Execution Performance Monitoring
// ============================================================================

// Task execution metrics (separate from I2C bus metrics for better architecture)
struct TaskExecutionMetrics {
  uint32_t totalOperations;    // Total sensor task operations tracked
  uint32_t timeoutCount;       // Operations exceeding timeout threshold
  uint32_t avgExecutionMs;     // EWMA of task execution times
  uint32_t maxExecutionMs;     // Peak operation execution time
  uint32_t lastResetMs;        // When metrics were last reset
};

extern TaskExecutionMetrics gTaskMetrics;

// Task performance tracking API
void taskOperationStart();
void taskOperationComplete(uint32_t elapsedMs, uint32_t timeoutThresholdMs);
void resetTaskMetrics();

// ============================================================================
// Security Utilities
// ============================================================================

// Securely clear a String's internal buffer before releasing memory
// Prevents plaintext passwords from lingering in RAM after use
void secureClearString(String& s);

// Bring in command/context definitions for shared use
#include "System_BuildConfig.h"
#if ENABLE_HTTP_SERVER
  #include "WebServer_Server.h"
#endif
#include "System_SensorStubs.h"
// ============================================================================
// Command Registry System
// ============================================================================

// Command entry structure - used by all modules to define their commands
// Voice hierarchy: voiceCategory -> voiceSubCategory (optional) -> voiceTarget
// Examples:
//   2-level: { ..., "camera", nullptr, "open" }      -> "camera" -> "open"
//   3-level: { ..., "sensor", "thermal", "open" }    -> "sensor" -> "thermal" -> "open"
struct CommandEntry {
  const char* name;                           // canonical command name
  const char* help;                           // short help text
  bool requiresAdmin;                         // whether admin is required
  const char* (*handler)(const String& cmd);  // function pointer to command handler
  const char* usage;                          // optional longer usage string (may be nullptr)
  const char* voiceCategory;                  // 1st level: category phrase (may be nullptr)
  const char* voiceSubCategory;               // 2nd level: sub-category phrase (may be nullptr for 2-level)
  const char* voiceTarget;                    // final level: action phrase (may be nullptr)

  constexpr CommandEntry(const char* name_,
                         const char* help_,
                         bool requiresAdmin_,
                         const char* (*handler_)(const String& cmd_),
                         const char* usage_ = nullptr,
                         const char* voiceCategory_ = nullptr,
                         const char* voiceTarget_ = nullptr)
      : name(name_),
        help(help_),
        requiresAdmin(requiresAdmin_),
        handler(handler_),
        usage(usage_),
        voiceCategory(voiceCategory_),
        voiceSubCategory(nullptr),
        voiceTarget(voiceTarget_) {}

  // 3-level constructor with sub-category
  constexpr CommandEntry(const char* name_,
                         const char* help_,
                         bool requiresAdmin_,
                         const char* (*handler_)(const String& cmd_),
                         const char* usage_,
                         const char* voiceCategory_,
                         const char* voiceSubCategory_,
                         const char* voiceTarget_)
      : name(name_),
        help(help_),
        requiresAdmin(requiresAdmin_),
        handler(handler_),
        usage(usage_),
        voiceCategory(voiceCategory_),
        voiceSubCategory(voiceSubCategory_),
        voiceTarget(voiceTarget_) {}
};

// Command module flags
#define CMD_MODULE_SENSOR    0x01  // Module controls a sensor/peripheral
#define CMD_MODULE_CORE      0x02  // Core system module (skip in help menu)
#define CMD_MODULE_ADMIN     0x04  // Module requires admin access
#define CMD_MODULE_NETWORK   0x08  // Network-related module

// Command module structure - each module registers its command table
struct CommandModule {
  const char* name;                           // module name (for help categories)
  const char* description;                    // module description for help (can be nullptr)
  const CommandEntry* commands;               // pointer to command array
  size_t count;                               // number of commands in array
  uint8_t flags;                              // module flags (CMD_MODULE_*)
  bool (*isConnected)();                      // optional connection check (can be nullptr)
};

// Get the global module registry (defined in system_utils.cpp)
// Returns pointer to array, sets count via output parameter
const CommandModule* getCommandModules(size_t& count);

// Find a command entry by name (searches all modules, returns longest match)
// Returns nullptr if not found
const CommandEntry* findCommand(const String& cmdLine);

// Check if a command requires admin privileges
bool commandRequiresAdmin(const String& cmdLine);

// Execute a command through the registry (returns result string)
// Note: This is a lower-level function; executeCommand() in .ino handles auth context
const char* dispatchCommand(const String& cmd);

// HTTP server control (lowercase 'h' matches implementation in .ino)
void startHttpServer();

// ============================================================================
// NTP Time Synchronization
// ============================================================================
void setupNTP();
bool syncNTPAndResolve();  // Synchronous - runs in calling task's stack
time_t nowEpoch();

// Note: BROADCAST_PRINTF is defined as a macro in debug_system.h, not a function

// ============================================================================
// Forward declaration for AuthContext (defined in user_system.h)
// ============================================================================
struct AuthContext;

// File I/O functions
bool readText(const char* path, String& out);
bool writeText(const char* path, const String& content);

// Settings persistence
// NOTE: Use writeSettingsJson() from .ino instead of saveUnifiedSettings()

// Automation logging
bool appendAutoLogEntry(const char* type, const String& message);

// Command audit logging (always-on, lightweight)
void logCommandExecution(const AuthContext& ctx, const char* cmd, bool success, const char* result);

// Command execution (implemented in main .ino)
bool executeCommand(AuthContext& ctx, const char* cmd, char* out, size_t outSize);

// Audit / Redaction utilities
// Centralized redaction for audit logs and debug output
String redactCmdForAudit(const String& cmd);
String redactOutputForLog(const String& output);

// CLI validation macro - returns "VALID" early when gCLIValidateOnly is set
// Use in command handlers to short-circuit during command validation pass
#define RETURN_VALID_IF_VALIDATE_CSTR() \
  do { \
    extern bool gCLIValidateOnly; \
    if (gCLIValidateOnly) return "VALID"; \
  } while(0)

// ============================================================================
// Base64 Encoding/Decoding
// ============================================================================

String base64Encode(const uint8_t* data, size_t len);
String base64Decode(const String& input);

// ============================================================================
// JSON Parsing Helpers
// ============================================================================

bool parseJsonBool(const String& src, const char* key, bool& out);
bool parseJsonInt(const String& src, const char* key, int& out);
bool parseJsonFloat(const String& src, const char* key, float& out);
bool parseJsonU16(const String& src, const char* key, uint16_t& out);
bool parseJsonString(const String& src, const char* key, String& out);
bool extractObjectByKey(const String& src, const char* key, String& outObj);
bool extractArrayByKey(const String& src, const char* key, String& outArray);
bool extractArrayItem(const String& arrayStr, int& pos, String& outItem);

// ============================================================================
// URL Encoding/Decoding Utilities
// ============================================================================

String urlEncode(const char* s);
String urlDecode(const String& s);
String extractFormField(const String& body, const String& key);
String jsonEscape(const String& in);

// Serialize JsonArray to buffer with self-repair: removes oldest entries if overflow
// Returns number of entries removed (0 if no overflow)
// Usage: int removed = serializeJsonArrayWithRepair(arr, buf, bufSize, "context");
// Note: Requires ArduinoJson.h - included here for the declaration
#include <ArduinoJson.h>
int serializeJsonArrayWithRepair(JsonArray& arr, char* buf, size_t bufSize, const char* context);

// ============================================================================
// Date/Time Formatting Utilities
// ============================================================================

String formatDateTime(time_t timestamp);

// ============================================================================
// Serial Input Helpers
// ============================================================================

String waitForSerialInput(unsigned long timeoutMs);
String waitForSerialInputBlocking();

// ============================================================================
// Time Sync Functions
// ============================================================================

// Call when SNTP/RTC time becomes valid or changes significantly
void timeSyncUpdateBootEpoch();

// Returns a ms-precision prefix like "[YYYY-MM-DD HH:MM:SS.mmm] | "
// Writes empty string if epoch time invalid
void getTimestampPrefixMsCached(char* out, size_t outSize);

// ============================================================================
// Memory Reporting & System Diagnostics
// ============================================================================

// Print breakdown of connected device libraries (returns total via outTotal)
void printConnectedDevicesLibraries(size_t& outTotal);

// Calculate total estimated memory usage for all sensor systems
size_t calculateSensorSystemMemory();

// System diagnostic commands - migrated from main .ino
void printMemoryReport();
const char* cmd_memreport(const String& cmd);
const char* cmd_taskstats(const String& cmd);

// Note: Command execution functions (executeCommand, submitAndExecuteSync, etc.) 
// are implemented in system_utils.cpp but declared in main .ino where the 
// Command/CommandContext types are defined to avoid circular dependencies

// ============================================================================
// Icon System - Unified PNG-based icons for OLED/Web/TFT
// ============================================================================

// Forward declaration for OLED display
class Adafruit_SSD1306;

// Icon system initialization (creates /icons/ directory if needed)
bool initIconSystem();

// Load icon from PNG file and draw to OLED display
// Icons are 32x32 PNG files stored in /icons/ directory
bool drawIcon(Adafruit_SSD1306* display, const char* name, int x, int y, uint16_t color = 1);

// Draw icon with scaling (0.5 = half size, 2.0 = double size)
// Optimized path for 0.5x scale (32→16) uses 2×2 block sampling
bool drawIconScaled(Adafruit_SSD1306* display, const char* name, int x, int y, uint16_t color, float scale);

// Load raw icon bitmap data (for custom rendering)
// Buffer must be at least 128 bytes for 32x32 monochrome bitmap
bool loadIconData(const char* name, uint8_t* buffer, size_t bufferSize, uint8_t& width, uint8_t& height);

// Get full path to icon file
String getIconPath(const char* name);

// Check if icon exists
bool iconExists(const char* name);

// Get icon name for file extension (modular mapping)
// Returns static string that is valid for the duration of the program
const char* getIconNameForExtension(const char* ext);

// ============================================================================
// Input Abstraction Layer
// ============================================================================
// Input handling is now defined in HAL_Input.h for hardware abstraction.
// This allows switching between different controller types at compile time
// by changing INPUT_TYPE in System_BuildConfig.h.
#include "HAL_Input.h"

#endif // SYSTEM_UTILS_H
