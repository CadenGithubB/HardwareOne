// Flag for allowing queued debug messages during help-mode
#define DEBUG_MSG_FLAG_ALLOW_IN_HELP 0x01
#ifndef DEBUG_SYSTEM_H
#define DEBUG_SYSTEM_H

#include <Arduino.h>
#include "System_Debug_Manager.h"

// ============================================================================
// Debug System - Centralized debug output and ring buffer management
// ============================================================================

// Debug flags (bitmask)
#define DEBUG_AUTH            0x0001
#define DEBUG_HTTP            0x0002
#define DEBUG_SSE             0x0004
#define DEBUG_CLI             0x0008
#define DEBUG_SENSORS_FRAME   0x0010
#define DEBUG_SENSORS_DATA    0x0020
#define DEBUG_SENSORS         0x0040
#define DEBUG_FMRADIO         0x0080  // FM Radio operations and I2C debugging
#define DEBUG_I2C             0x0100  // I2C bus operations, transactions, clock changes, mutex
#define DEBUG_WIFI            0x0200
#define DEBUG_STORAGE         0x8000  // File operations (moved from 0x0100 to avoid collision)
#define DEBUG_PERFORMANCE     0x0400
#define DEBUG_SYSTEM          0x4000
#define DEBUG_USERS           0x2000
#define DEBUG_AUTOMATIONS     0x10000000  // Parent flag (legacy - kept for backward compat)
#define DEBUG_LOGGER          0x20000
#define DEBUG_ESPNOW_STREAM   0x400000
#define DEBUG_MEMORY          0x40000
#define DEBUG_CMD_FLOW        0x1000
#define DEBUG_COMMAND_SYSTEM  0x800000  // Modular command registry operations
#define DEBUG_SETTINGS_SYSTEM 0x1000000 // Settings module registration and validation
// Additional flags used across the codebase (aligned with main .ino)
#define DEBUG_SECURITY        0x8000
#define DEBUG_ESPNOW_CORE     0x10000
#define DEBUG_ESPNOW_ROUTER   0x80000
#define DEBUG_ESPNOW_MESH     0x100000
#define DEBUG_ESPNOW_TOPO     0x200000
#define DEBUG_ESPNOW_ENCRYPTION 0x80000000  // Bit 31 (was 0x20000000 which collided)
#define DEBUG_AUTO_SCHEDULER  0x40000000    // Bit 30
#define DEBUG_AUTO_EXEC       0x2000000     // Bit 25
#define DEBUG_AUTO_CONDITION  0x4000000     // Bit 26
#define DEBUG_AUTO_TIMING     0x8000000     // Bit 27
#define DEBUG_CAMERA          0x20000000    // Bit 29 - Camera operations
#define DEBUG_MICROPHONE      0x0800        // Bit 11 - Microphone operations (was 0x10000000 which conflicted with AUTOMATIONS)

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
#define DEBUG_QUEUE_SIZE 64        // Number of messages that can be queued
#define DEBUG_MSG_SIZE 256         // Max size of each debug message

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
#ifndef OUTPUT_TFT
#define OUTPUT_TFT    0x02
#endif
#ifndef OUTPUT_WEB
#define OUTPUT_WEB    0x04
#endif
#ifndef OUTPUT_FILE
#define OUTPUT_FILE   0x08
#endif

// Global output routing flags (runtime). Persisted settings are in settings.cpp.
// Use these flags to decide which sinks (serial/web/tft/file) are currently enabled.
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
extern uint32_t gDebugFlags;
extern DebugSubFlags gDebugSubFlags;
extern char* gDebugBuffer;
extern QueueHandle_t gDebugOutputQueue;
extern QueueHandle_t gDebugFreeQueue;
extern volatile unsigned long gDebugDropped;

// Accessor functions - use these instead of direct global access
inline uint32_t getDebugFlags() { return DEBUG_MANAGER.getDebugFlags(); }
inline void setDebugFlags(uint32_t flags) { DEBUG_MANAGER.setDebugFlags(flags); }
inline void setDebugFlag(uint32_t flag) { setDebugFlags(getDebugFlags() | flag); }
inline void clearDebugFlag(uint32_t flag) { setDebugFlags(getDebugFlags() & ~flag); }
inline bool isDebugFlagSet(uint32_t flag) { return (getDebugFlags() & flag) != 0; }

// Sub-flag accessor functions
inline DebugSubFlags& getDebugSubFlags() { return gDebugSubFlags; }

// Helper to update parent flag when sub-flags change
inline void updateParentDebugFlag(uint32_t parentFlag, bool anyChildEnabled) {
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
inline uint8_t getLogLevel() { return DEBUG_MANAGER.getLogLevel(); }
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
#define DEBUG_CAMERAF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_CAMERA, fmt, ##__VA_ARGS__)
#define DEBUG_MICF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_MICROPHONE, fmt, ##__VA_ARGS__)
#define DEBUG_FRAMEF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_SENSORS_FRAME, fmt, ##__VA_ARGS__)
#define DEBUG_DATAF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_SENSORS_DATA, fmt, ##__VA_ARGS__)
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

const char* cmd_outtft(const String& cmd);
const char* cmd_debugauthcookies(const String& cmd);
const char* cmd_debughttp(const String& cmd);
const char* cmd_debugsse(const String& cmd);
const char* cmd_debugcli(const String& cmd);
const char* cmd_debugsensorsframe(const String& cmd);
const char* cmd_debugsensorsdata(const String& cmd);
const char* cmd_debugsensorsgeneral(const String& cmd);
const char* cmd_debugcamera(const String& cmd);
const char* cmd_debugmicrophone(const String& cmd);
const char* cmd_debugwifi(const String& cmd);
const char* cmd_debugstorage(const String& cmd);
const char* cmd_debuglogger(const String& cmd);
const char* cmd_debugautomations(const String& cmd);
const char* cmd_debugperformance(const String& cmd);
const char* cmd_debugdatetime(const String& cmd);
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

// Helper: Get category name from debug flag
const char* getDebugCategoryName(uint32_t flag);

#endif // DEBUG_SYSTEM_H
