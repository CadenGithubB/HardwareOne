// Flag for allowing queued debug messages during help-mode
#define DEBUG_MSG_FLAG_ALLOW_IN_HELP 0x01
#ifndef DEBUG_SYSTEM_H
#define DEBUG_SYSTEM_H

#include <Arduino.h>
#include "System_Debug_Manager.h"

// ============================================================================
// Debug System - Centralized debug output and ring buffer management
// ============================================================================
// This header provides: ensureDebugBuffer(), getDebugBuffer(), broadcastOutput(),
// BROADCAST_PRINTF, DEBUG_*F macros, and all debug flag constants.
// Include this header instead of using 'extern' declarations for these functions.

// Debug flags (bitmask) - 64-bit mask
// Bits 0-31: Core system and infrastructure
#define DEBUG_AUTH            0x0001ULL
#define DEBUG_HTTP            0x0002ULL
#define DEBUG_SSE             0x0004ULL
#define DEBUG_CLI             0x0008ULL
// Bits 4-5: REMOVED - Legacy DEBUG_SENSORS_FRAME and DEBUG_SENSORS_DATA (replaced by per-sensor flags)
#define DEBUG_SENSORS         0x0040ULL
#define DEBUG_FMRADIO         0x0080ULL  // FM Radio operations and I2C debugging
#define DEBUG_I2C             0x0100ULL  // I2C bus operations, transactions, clock changes, mutex
#define DEBUG_WIFI            0x0200ULL
#define DEBUG_PERFORMANCE     0x0400ULL
#define DEBUG_MICROPHONE      0x0800ULL        // Bit 11 - Microphone operations
#define DEBUG_CMD_FLOW        0x1000ULL
#define DEBUG_USERS           0x2000ULL
#define DEBUG_SYSTEM          0x4000ULL
#define DEBUG_STORAGE         0x8000ULL  // File operations (NOTE: Also used by DEBUG_SECURITY and DEBUG_G2 - collision acceptable as they're related)
#define DEBUG_SECURITY        0x8000ULL  // Bit 15 - Security (shares bit with STORAGE/G2)
#define DEBUG_G2              0x8000ULL  // Bit 15 - G2 smart glasses BLE operations (shares bit with STORAGE/SECURITY)
#define DEBUG_ESPNOW_CORE     0x10000ULL
#define DEBUG_LOGGER          0x20000ULL
#define DEBUG_MEMORY          0x40000ULL
#define DEBUG_ESPNOW_ROUTER   0x80000ULL
#define DEBUG_ESPNOW_MESH     0x100000ULL
#define DEBUG_ESPNOW_TOPO     0x200000ULL
#define DEBUG_ESPNOW_STREAM   0x400000ULL
#define DEBUG_COMMAND_SYSTEM  0x800000ULL  // Modular command registry operations
#define DEBUG_SETTINGS_SYSTEM 0x1000000ULL // Settings module registration and validation
#define DEBUG_AUTO_EXEC       0x2000000ULL     // Bit 25
#define DEBUG_AUTO_CONDITION  0x4000000ULL     // Bit 26
#define DEBUG_AUTO_TIMING     0x8000000ULL     // Bit 27
#define DEBUG_AUTOMATIONS     0x10000000ULL  // Parent flag (legacy - kept for backward compat)
#define DEBUG_CAMERA          0x20000000ULL    // Bit 29 - Camera operations
#define DEBUG_AUTO_SCHEDULER  0x40000000ULL    // Bit 30
#define DEBUG_ESPNOW_ENCRYPTION 0x80000000ULL  // Bit 31

// Bits 32-39: Individual I2C sensor debug flags
#define DEBUG_GPS             0x100000000ULL  // Bit 32 - GPS (PA1010D)
#define DEBUG_RTC             0x200000000ULL  // Bit 33 - RTC (DS3231)
#define DEBUG_IMU             0x400000000ULL  // Bit 34 - IMU (BNO055)
#define DEBUG_THERMAL         0x800000000ULL  // Bit 35 - Thermal (MLX90640)
#define DEBUG_TOF             0x1000000000ULL // Bit 36 - ToF (VL53L4CX)
#define DEBUG_GAMEPAD         0x2000000000ULL // Bit 37 - Gamepad (Seesaw)
#define DEBUG_APDS            0x4000000000ULL // Bit 38 - APDS (APDS9960)
#define DEBUG_PRESENCE        0x8000000000ULL // Bit 39 - Presence (STHS34PF80)

// Bits 40-47: Per-sensor frame/data debug flags (granular timing and data processing)
#define DEBUG_THERMAL_FRAME   0x10000000000ULL  // Bit 40 - Thermal frame timing, capture, FPS
#define DEBUG_THERMAL_DATA    0x20000000000ULL  // Bit 41 - Thermal data interpolation, processing
#define DEBUG_TOF_FRAME       0x40000000000ULL  // Bit 42 - ToF frame capture, object detection
#define DEBUG_GAMEPAD_FRAME   0x80000000000ULL  // Bit 43 - Gamepad frame timing, connection
#define DEBUG_GAMEPAD_DATA    0x100000000000ULL // Bit 44 - Gamepad button press/release events
#define DEBUG_IMU_FRAME       0x200000000000ULL // Bit 45 - IMU frame timing, cache operations
#define DEBUG_IMU_DATA        0x400000000000ULL // Bit 46 - IMU data updates
#define DEBUG_APDS_FRAME      0x800000000000ULL // Bit 47 - APDS frame timing, connection
#define DEBUG_ESPNOW_METADATA 0x1000000000000ULL // Bit 48 - ESP-NOW metadata exchange (REQ/RESP/PUSH/store)

// Debug sub-flags structure for granular control
// The parent flags (DEBUG_AUTH, DEBUG_HTTP, etc.) are set when ANY child is enabled
// This structure tracks which specific sub-categories are enabled
struct DebugSubFlags {
  // Auth sub-flags
  bool authSessions;    // Session creation, validation, expiration
  bool authCookies;     // Cookie parsing, setting, validation
  bool authLogin;       // Login attempts, authentication
  bool authBootId;      // Boot ID validation, session invalidation
  
  // HTTP sub-flags
  bool httpHandlers;    // Handler entry/exit, page serving
  bool httpRequests;    // Request parsing, query parameters
  bool httpResponses;   // Response building, JSON serialization
  bool httpStreaming;   // Chunked response streaming, buffer usage
  
  // WiFi sub-flags
  bool wifiConnection;  // Connect/disconnect, status changes
  bool wifiConfig;      // Credential setup, encryption/decryption
  bool wifiScanning;    // Network scanning, SSID discovery
  bool wifiDriver;      // ESP-IDF API calls, low-level operations
  
  // Storage sub-flags
  bool storageFiles;    // File read/write/delete operations
  bool storageJson;     // JSON parsing, serialization, validation
  bool storageSettings; // Settings load/save, module registration
  bool storageMigration;// Filesystem migrations, directory creation
  
  // System sub-flags
  bool systemBoot;      // Boot sequence, initialization
  bool systemConfig;    // Settings application, encryption
  bool systemTasks;     // Task creation, stack monitoring
  bool systemHardware;  // Hardware initialization
  
  // Users sub-flags
  bool usersMgmt;       // User CRUD operations
  bool usersRegister;   // Pending user requests, validation
  bool usersQuery;      // User list, user info retrieval
  
  // CLI sub-flags
  bool cliExecution;    // Command execution, handler invocation
  bool cliQueue;        // Command queue processing
  bool cliValidation;   // Command validation, authorization
  
  // Performance sub-flags
  bool perfStack;       // Task stack watermarks
  bool perfHeap;        // Heap/PSRAM usage tracking
  bool perfTiming;      // Execution timing
  
  // SSE sub-flags
  bool sseConnection;   // SSE connection establishment
  bool sseEvents;       // Event sending, notice queue
  bool sseBroadcast;    // Broadcast routing, targeted messages
  
  // Command Flow sub-flags
  bool cmdflowRouting;  // Command routing, handler lookup
  bool cmdflowQueue;    // Queue operations, sync/async execution
  bool cmdflowContext;  // Context management, origin tracking
};

// Alias for legacy code that referenced DEBUG_DATETIME
#define DEBUG_DATETIME        DEBUG_SYSTEM

// Debug output queue configuration
#define DEBUG_QUEUE_SIZE_MIN 64    // Minimum queue size (internal RAM only)
#define DEBUG_QUEUE_SIZE_MAX 128   // Maximum queue size (with PSRAM)
#define DEBUG_MSG_SIZE 256         // Max size of each debug message

// Runtime queue size (set during init based on PSRAM availability)
extern int gDebugQueueSize;

// Debug message structure
struct DebugMessage {
  unsigned long timestamp;
  uint32_t flags;  // Changed from uint8_t to store full flag value
  char text[DEBUG_MSG_SIZE];
};

// ============================================================================
// Centralized Output Routing Flags
// ============================================================================
// Single source of truth for OUTPUT_* bit positions used across the codebase.
// Include this header wherever OUTPUT_* flags are needed.
#ifndef OUTPUT_SERIAL
#define OUTPUT_SERIAL 0x01
#endif
#ifndef OUTPUT_DISPLAY
#define OUTPUT_DISPLAY 0x02
#endif
#ifndef OUTPUT_WEB
#define OUTPUT_WEB    0x04
#endif
#ifndef OUTPUT_FILE
#define OUTPUT_FILE   0x08
#endif
#ifndef OUTPUT_G2
#define OUTPUT_G2     0x10  // Even Realities G2 glasses display
#endif

// Global output routing flags (runtime). Persisted settings are in settings.cpp.
// Use these flags to decide which sinks (serial/web/display/file) are currently enabled.
extern volatile uint32_t gOutputFlags;

// System logging state
extern String gSystemLogPath;
extern bool gSystemLogEnabled;
extern unsigned long gSystemLogLastWrite;
extern bool gSystemLogCategoryTags;  // Enable/disable category tags in log output

// ============================================================================
// Debug System Functions
// ============================================================================

// Initialize debug system (call from setup())
void initDebugSystem();

// Ensure debug buffer is allocated
bool ensureDebugBuffer();

// Internal globals - required for inline accessor functions below
// DO NOT access directly - use accessor functions instead
extern uint64_t gDebugFlags;  // 64-bit debug flags
extern DebugSubFlags gDebugSubFlags;
extern char* gDebugBuffer;
extern QueueHandle_t gDebugOutputQueue;
extern QueueHandle_t gDebugFreeQueue;
extern volatile unsigned long gDebugDropped;
extern volatile bool gDebugVerbose;

// Accessor functions - use these instead of direct global access
inline uint64_t getDebugFlags() { return DEBUG_MANAGER.getDebugFlags(); }
inline void setDebugFlags(uint64_t flags) { DEBUG_MANAGER.setDebugFlags(flags); }
inline void setDebugFlag(uint64_t flag) { setDebugFlags(getDebugFlags() | flag); }
inline void clearDebugFlag(uint64_t flag) { setDebugFlags(getDebugFlags() & ~flag); }
inline bool isDebugFlagSet(uint64_t flag) { return gDebugVerbose || ((getDebugFlags() & flag) != 0); }

// Sub-flag accessor functions
inline DebugSubFlags& getDebugSubFlags() { return gDebugSubFlags; }

// Helper to update parent flag when sub-flags change
inline void updateParentDebugFlag(uint64_t parentFlag, bool anyChildEnabled) {
  if (anyChildEnabled) setDebugFlag(parentFlag);
  else clearDebugFlag(parentFlag);
}
inline char* getDebugBuffer() { return DEBUG_MANAGER.getDebugBuffer(); }
inline QueueHandle_t getDebugQueue() { return DEBUG_MANAGER.getDebugQueue(); }
inline QueueHandle_t getDebugFreeQueue() { return DEBUG_MANAGER.getDebugFreeQueue(); }
inline void incrementDebugDropped() { DEBUG_MANAGER.incrementDebugDropped(); }

// Broadcast output functions
void broadcastOutput(const String& s);
void broadcastOutput(const char* s);

// Forward declaration for CommandContext (defined in main .ino)
struct CommandContext;
void broadcastOutput(const String& s, const CommandContext& ctx);

void debugQueuePrintf(uint32_t flag, const char* fmt, ...);

// Print summary (and tail) of output suppressed during help; resets counters
void helpSuppressedPrintAndReset();
void helpSuppressedTailDump();

// Print to web buffer helper
void printToWeb(const String& s);

// ============================================================================
// Severity-Based Logging Levels
// ============================================================================
// ERROR - Always visible, critical failures only (cannot be disabled)
// WARN  - Always visible, recoverable issues (cannot be disabled)
// INFO  - Optional, normal operations (controlled by debug flags)
// DEBUG - Optional, verbose details (controlled by debug flags)

#define LOG_LEVEL_ERROR 0
#define LOG_LEVEL_WARN  1
#define LOG_LEVEL_INFO  2
#define LOG_LEVEL_DEBUG 3

// Global log level (default: show everything)
extern uint8_t gLogLevel;
inline uint8_t getLogLevel() { return gDebugVerbose ? LOG_LEVEL_DEBUG : DEBUG_MANAGER.getLogLevel(); }
// ============================================================================
// Debug Macros - Safe for use from any context
// ============================================================================

// Queue-based debug output - thread-safe with mutex protection
// Checks sensor enabled flags to prevent crashes during task deletion
// CRITICAL: Do NOT call from sensor tasks when disabled - causes crashes
// OPTIMIZED: Allocates DebugMessage on heap instead of stack (saves 256 bytes per call)
// PERFORMANCE: Lazy evaluation - check flag BEFORE evaluating expensive arguments
#define DEBUGF_QUEUE(flag, fmt, ...) \
  do { \
    if ((flag) == 0xFFFFFFFF || isDebugFlagSet(flag)) { \
      debugQueuePrintf(flag, fmt, ##__VA_ARGS__); \
    } \
  } while (0)

// All debug output now uses queue - no direct broadcast
// This ensures thread-safe, ordered output from all sources
#define DEBUGF_BROADCAST(flag, fmt, ...) DEBUGF_QUEUE(flag, fmt, ##__VA_ARGS__)

#define DEBUGF_QUEUE_DEBUG(flag, fmt, ...) \
  do { \
    if (getLogLevel() >= LOG_LEVEL_DEBUG) { \
      DEBUGF_QUEUE(flag, fmt, ##__VA_ARGS__); \
    } \
  } while (0)

// Convenience macros for specific subsystems
#define DEBUG_AUTHF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_AUTH, fmt, ##__VA_ARGS__)
#define DEBUG_HTTPF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_HTTP, fmt, ##__VA_ARGS__)
#define DEBUG_SSEF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_SSE, fmt, ##__VA_ARGS__)
#define DEBUG_CLIF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_CLI, fmt, ##__VA_ARGS__)
#define DEBUG_I2CF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_I2C, fmt, ##__VA_ARGS__)
#define DEBUG_CMD_FLOWF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_CMD_FLOW, fmt, ##__VA_ARGS__)
#define DEBUG_SENSORSF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_SENSORS, fmt, ##__VA_ARGS__)
#define DEBUG_FMRADIOF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_FMRADIO, fmt, ##__VA_ARGS__)
#define DEBUG_G2F(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_G2, fmt, ##__VA_ARGS__)
#define DEBUG_CAMERAF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_CAMERA, fmt, ##__VA_ARGS__)
#define DEBUG_MICF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_MICROPHONE, fmt, ##__VA_ARGS__)
// Legacy DEBUG_FRAMEF and DEBUG_DATAF removed - use per-sensor macros instead
#define DEBUG_THERMAL_FRAMEF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_THERMAL_FRAME, fmt, ##__VA_ARGS__)
#define DEBUG_THERMAL_DATAF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_THERMAL_DATA, fmt, ##__VA_ARGS__)
#define DEBUG_TOF_FRAMEF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_TOF_FRAME, fmt, ##__VA_ARGS__)
#define DEBUG_GAMEPAD_FRAMEF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_GAMEPAD_FRAME, fmt, ##__VA_ARGS__)
#define DEBUG_GAMEPAD_DATAF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_GAMEPAD_DATA, fmt, ##__VA_ARGS__)
#define DEBUG_IMU_FRAMEF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_IMU_FRAME, fmt, ##__VA_ARGS__)
#define DEBUG_IMU_DATAF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_IMU_DATA, fmt, ##__VA_ARGS__)
#define DEBUG_APDS_FRAMEF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_APDS_FRAME, fmt, ##__VA_ARGS__)
#define DEBUG_ESPNOW_METADATAF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_ESPNOW_METADATA, fmt, ##__VA_ARGS__)
#define DEBUG_WIFIF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_WIFI, fmt, ##__VA_ARGS__)
#define DEBUG_STORAGEF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_STORAGE, fmt, ##__VA_ARGS__)
#define DEBUG_PERFORMANCEF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_PERFORMANCE, fmt, ##__VA_ARGS__)
#define DEBUG_SYSTEMF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_SYSTEM, fmt, ##__VA_ARGS__)
#define DEBUG_USERSF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_USERS, fmt, ##__VA_ARGS__)
#define DEBUG_AUTOMATIONSF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_AUTOMATIONS, fmt, ##__VA_ARGS__)
#define DEBUG_LOGGERF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_LOGGER, fmt, ##__VA_ARGS__)
#define DEBUG_MEMORYF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_MEMORY, fmt, ##__VA_ARGS__)
#define DEBUG_ESPNOW_STREAMF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_ESPNOW_STREAM, fmt, ##__VA_ARGS__)
#define DEBUG_ESPNOWF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_ESPNOW_CORE, fmt, ##__VA_ARGS__)  // General ESP-NOW debug
#define DEBUG_COMMAND_SYSTEMF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_COMMAND_SYSTEM, fmt, ##__VA_ARGS__)
#define DEBUG_SETTINGS_SYSTEMF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_SETTINGS_SYSTEM, fmt, ##__VA_ARGS__)

// DateTime debug flag doesn't exist yet - map to SYSTEM for now
#define DEBUG_DATETIMEF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_SYSTEM, fmt, ##__VA_ARGS__)

// Individual I2C sensor debug macros
#define DEBUG_GPSF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_GPS, fmt, ##__VA_ARGS__)
#define DEBUG_RTCF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_RTC, fmt, ##__VA_ARGS__)
#define DEBUG_IMUF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_IMU, fmt, ##__VA_ARGS__)
#define DEBUG_THERMALF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_THERMAL, fmt, ##__VA_ARGS__)
#define DEBUG_TOFF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_TOF, fmt, ##__VA_ARGS__)
#define DEBUG_GAMEPADF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_GAMEPAD, fmt, ##__VA_ARGS__)
#define DEBUG_APDSF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_APDS, fmt, ##__VA_ARGS__)
#define DEBUG_PRESENCEF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_PRESENCE, fmt, ##__VA_ARGS__)

// Legacy compatibility macro
#define DEBUGF(flag, fmt, ...) DEBUGF_QUEUE_DEBUG(flag, fmt, ##__VA_ARGS__)

// ============================================================================
// Severity-Based Logging Macros
// ============================================================================

// ERROR macros - Always visible (cannot be disabled)
#define ERROR_SENSORSF(fmt, ...) DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][SENSORS] " fmt, ##__VA_ARGS__)
#define ERROR_I2CF(fmt, ...) DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][I2C] " fmt, ##__VA_ARGS__)
#define ERROR_ESPNOWF(fmt, ...) DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][ESPNOW] " fmt, ##__VA_ARGS__)
#define ERROR_AUTOMATIONF(fmt, ...) DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][AUTO] " fmt, ##__VA_ARGS__)
#define ERROR_SESSIONF(fmt, ...) DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][SESSION] " fmt, ##__VA_ARGS__)
#define ERROR_USERF(fmt, ...) DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][USER] " fmt, ##__VA_ARGS__)
#define ERROR_LOGGINGF(fmt, ...) DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][LOG] " fmt, ##__VA_ARGS__)
#define ERROR_WEBF(fmt, ...) DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][WEB] " fmt, ##__VA_ARGS__)
#define ERROR_COMMANDF(fmt, ...) DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][CMD] " fmt, ##__VA_ARGS__)
#define ERROR_SYSTEMF(fmt, ...) DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][SYS] " fmt, ##__VA_ARGS__)
#define ERROR_STORAGEF(fmt, ...) DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][STORAGE] " fmt, ##__VA_ARGS__)
#define ERROR_WIFIF(fmt, ...) DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][WIFI] " fmt, ##__VA_ARGS__)
#define ERROR_MEMORYF(fmt, ...) DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][MEM] " fmt, ##__VA_ARGS__)

// WARN macros - Always visible (cannot be disabled)
#define WARN_SENSORSF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(0xFFFFFFFF, "[WARN][SENSORS] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_I2CF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(0xFFFFFFFF, "[WARN][I2C] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_ESPNOWF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(0xFFFFFFFF, "[WARN][ESPNOW] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_AUTOMATIONF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(0xFFFFFFFF, "[WARN][AUTO] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_SESSIONF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(0xFFFFFFFF, "[WARN][SESSION] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_USERF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(0xFFFFFFFF, "[WARN][USER] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_LOGGINGF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(0xFFFFFFFF, "[WARN][LOG] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_WEBF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(0xFFFFFFFF, "[WARN][WEB] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_COMMANDF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(0xFFFFFFFF, "[WARN][CMD] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_SYSTEMF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(0xFFFFFFFF, "[WARN][SYS] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_STORAGEF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(0xFFFFFFFF, "[WARN][STORAGE] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_WIFIF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(0xFFFFFFFF, "[WARN][WIFI] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_MEMORYF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(0xFFFFFFFF, "[WARN][MEM] " fmt, ##__VA_ARGS__); } while (0)

// INFO macros - Optional (controlled by debug flags)
#define INFO_SENSORSF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_SENSORS, "[INFO][SENSORS] " fmt, ##__VA_ARGS__); } while (0)
#define INFO_I2CF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_I2C, "[INFO][I2C] " fmt, ##__VA_ARGS__); } while (0)
#define INFO_ESPNOWF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_ESPNOW_CORE, "[INFO][ESPNOW] " fmt, ##__VA_ARGS__); } while (0)
#define INFO_AUTOMATIONF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_AUTOMATIONS, "[INFO][AUTO] " fmt, ##__VA_ARGS__); } while (0)
#define INFO_SESSIONF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_AUTH, "[INFO][SESSION] " fmt, ##__VA_ARGS__); } while (0)
#define INFO_USERF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_USERS, "[INFO][USER] " fmt, ##__VA_ARGS__); } while (0)
#define INFO_LOGGINGF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_LOGGER, "[INFO][LOG] " fmt, ##__VA_ARGS__); } while (0)
#define INFO_WEBF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_HTTP, "[INFO][WEB] " fmt, ##__VA_ARGS__); } while (0)
#define INFO_COMMANDF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_CLI, "[INFO][CMD] " fmt, ##__VA_ARGS__); } while (0)
#define INFO_SYSTEMF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_SYSTEM, "[INFO][SYS] " fmt, ##__VA_ARGS__); } while (0)
#define INFO_STORAGEF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_STORAGE, "[INFO][STORAGE] " fmt, ##__VA_ARGS__); } while (0)
#define INFO_WIFIF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_WIFI, "[INFO][WIFI] " fmt, ##__VA_ARGS__); } while (0)
#define INFO_MEMORYF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_MEMORY, "[INFO][MEM] " fmt, ##__VA_ARGS__); } while (0)

// ============================================================================
// Broadcast Printf Macros (for memory-efficient output)
// ============================================================================

// Forward declarations for broadcast functions
extern bool ensureDebugBuffer();
extern char* gDebugBuffer;
extern void broadcastOutput(const char* msg);
extern void broadcastOutput(const String& msg);

// MEMORY OPTIMIZATION: Printf-style broadcastOutput using stack-local buffer
// Thread-safe: each caller uses its own stack for formatting (no shared gDebugBuffer)
// PERFORMANCE: Conditional execution - check output flags BEFORE allocating stack/formatting
#define BROADCAST_PRINTF(fmt, ...) \
  do { \
    if (gOutputFlags & (OUTPUT_SERIAL | OUTPUT_WEB | OUTPUT_FILE)) { \
      char _bpBuf[256]; \
      snprintf(_bpBuf, sizeof(_bpBuf), fmt, ##__VA_ARGS__); \
      broadcastOutput(_bpBuf); \
    } \
  } while (0)

// Category-tagged variant: embeds [CATEGORY] prefix so the log file writer and
// log viewer parser both see a proper category without a debug flag.
// Use instead of BROADCAST_PRINTF when the output should be filterable.
// Example: BROADCAST_PRINTF_CAT("SYSTEM", "Boot complete in %lums", millis())
#define BROADCAST_PRINTF_CAT(cat, fmt, ...) \
  do { \
    if (gOutputFlags & (OUTPUT_SERIAL | OUTPUT_WEB | OUTPUT_FILE)) { \
      char _bpBuf[256]; \
      snprintf(_bpBuf, sizeof(_bpBuf), "[" cat "] " fmt, ##__VA_ARGS__); \
      broadcastOutput(_bpBuf); \
    } \
  } while (0)

// Context-aware version for commands that need user/source attribution
// Note: Requires CommandContext to be defined
// PERFORMANCE: Conditional execution - check output flags BEFORE allocating stack/formatting
#define BROADCAST_PRINTF_CTX(ctx, fmt, ...) \
  do { \
    if (gOutputFlags & (OUTPUT_SERIAL | OUTPUT_WEB | OUTPUT_FILE)) { \
      char _bpBuf[256]; \
      snprintf(_bpBuf, sizeof(_bpBuf), fmt, ##__VA_ARGS__); \
      broadcastOutput(_bpBuf, ctx); \
    } \
  } while (0)

// Security debug always on - uses broadcastOutput with explicit prefix
#define DEBUG_SECURITYF(fmt, ...) \
  do { \
    if (ensureDebugBuffer()) { \
      snprintf(getDebugBuffer(), 1024, "[SECURITY] " fmt, ##__VA_ARGS__); \
      broadcastOutput(getDebugBuffer()); \
    } \
  } while (0)

// ==========================================================================
// Streaming Debug Instrumentation (centralized)
// ==========================================================================
// Records chunked HTTP streaming metrics per response for a concise summary
void streamDebugReset(const char* tag);
void streamDebugRecord(size_t sz, size_t chunkLimit);
void streamDebugFlush();

// ============================================================================
// Debug Command Registry
// ============================================================================

// CommandEntry is defined in system_utils.h (included by files that need it)
// Forward declare here for header-only usage
struct CommandEntry;

// Debug command registry
extern const CommandEntry debugCommands[];
extern const size_t debugCommandsCount;

// ============================================================================
// Debug Command Handlers (implemented in debug_system.cpp)
// ============================================================================

const char* cmd_outdisplay(const String& cmd);
const char* cmd_debugauthcookies(const String& cmd);
const char* cmd_debughttp(const String& cmd);
const char* cmd_debugsse(const String& cmd);
const char* cmd_debugcli(const String& cmd);
const char* cmd_debugsensorsgeneral(const String& cmd);
const char* cmd_debugcamera(const String& cmd);
const char* cmd_debugmicrophone(const String& cmd);
const char* cmd_debugwifi(const String& cmd);
const char* cmd_debugstorage(const String& cmd);
const char* cmd_debuglogger(const String& cmd);
const char* cmd_debugautomations(const String& cmd);
const char* cmd_debugperformance(const String& cmd);
const char* cmd_debugauth(const String& cmd);
const char* cmd_debugsensors(const String& cmd);
const char* cmd_debugespnow(const String& cmd);
const char* cmd_debugdatetime(const String& cmd);
const char* cmd_debuggps(const String& cmd);
const char* cmd_debugrtc(const String& cmd);
const char* cmd_debugimu(const String& cmd);
const char* cmd_debugthermal(const String& cmd);
const char* cmd_debugtof(const String& cmd);
const char* cmd_debuggamepad(const String& cmd);
const char* cmd_debugapds(const String& cmd);
const char* cmd_debugpresence(const String& cmd);
const char* cmd_debugverbose(const String& cmd);
const char* cmd_debugbuffer(const String& cmd);
const char* cmd_debugcommandflow(const String& cmd);
const char* cmd_debugusers(const String& cmd);
const char* cmd_debugsystem(const String& cmd);
const char* cmd_debugespnowstream(const String& cmd);
const char* cmd_debugespnowcore(const String& cmd);
const char* cmd_debugespnowrouter(const String& cmd);
const char* cmd_debugespnowmesh(const String& cmd);
const char* cmd_debugespnowtopo(const String& cmd);
const char* cmd_debugespnowencryption(const String& cmd);
const char* cmd_debugespnowmetadata(const String& cmd);
const char* cmd_debugautoscheduler(const String& cmd);
const char* cmd_debugautoexec(const String& cmd);
const char* cmd_debugautocondition(const String& cmd);
const char* cmd_debugautotiming(const String& cmd);
const char* cmd_debugmemory(const String& cmd);
const char* cmd_debugauthsessions(const String& cmd);
const char* cmd_debugauthcookies(const String& cmd);
const char* cmd_debugauthlogin(const String& cmd);
const char* cmd_debugauthbootid(const String& cmd);
const char* cmd_debughttphandlers(const String& cmd);
const char* cmd_debughttprequests(const String& cmd);
const char* cmd_debughttpresponses(const String& cmd);
const char* cmd_debughttpstreaming(const String& cmd);
const char* cmd_debugwificonnection(const String& cmd);
const char* cmd_debugwificonfig(const String& cmd);
const char* cmd_debugwifiscanning(const String& cmd);
const char* cmd_debugwifidriver(const String& cmd);
const char* cmd_debugstoragefiles(const String& cmd);
const char* cmd_debugstoragejson(const String& cmd);
const char* cmd_debugstoragesettings(const String& cmd);
const char* cmd_debugstoragemigration(const String& cmd);
const char* cmd_debugsystemboot(const String& cmd);
const char* cmd_debugsystemconfig(const String& cmd);
const char* cmd_debugsystemtasks(const String& cmd);
const char* cmd_debugsystemhardware(const String& cmd);
const char* cmd_debugusersmgmt(const String& cmd);
const char* cmd_debugusersregister(const String& cmd);
const char* cmd_debugusersquery(const String& cmd);
const char* cmd_debugcliexecution(const String& cmd);
const char* cmd_debugcliqueue(const String& cmd);
const char* cmd_debugclivalidation(const String& cmd);
const char* cmd_debugperfstack(const String& cmd);
const char* cmd_debugperfheap(const String& cmd);
const char* cmd_debugperftiming(const String& cmd);
const char* cmd_debugsseconnection(const String& cmd);
const char* cmd_debugsseevents(const String& cmd);
const char* cmd_debugssebroadcast(const String& cmd);
const char* cmd_debugcmdflowrouting(const String& cmd);
const char* cmd_debugcmdflowqueue(const String& cmd);
const char* cmd_debugcmdflowcontext(const String& cmd);
const char* cmd_commandmodulesummary(const String& cmd);
const char* cmd_settingsmodulesummary(const String& cmd);

// System logging commands
const char* cmd_log(const String& cmd);

// System log auto-start (called from boot)
void systemLogAutoStart();

// Helper: Get category name from debug flag
const char* getDebugCategoryName(uint32_t flag);

#endif // DEBUG_SYSTEM_H
