// Output sink bits — the single bit vocabulary for output routing.
// Bits 0-5 name the sinks; bit 6 is a modifier. Three masks share these
// positions (same bit = same sink everywhere, no translation layers):
//   msg->routing   (DebugMessage)    — which sinks this message targets
//   gOutputFlags   (global)          — which lanes are currently open:
//                    SERIAL is the one persisted lane (outSerial, applied in
//                    applySettings); WEB tracks the HTTP server lifecycle
//                    (raised in startHttpServer, cleared by httpstop/
//                    closewifi); FILE follows `log start`/`log stop`; BLE
//                    and G2 are per-session opt-in streams (outble/outg2).
//                    OLED is never set here — see below.
//   ctx.outputMask (CommandContext)  — which sinks a command's output
//                    targets; passed through to routing in broadcastOutput.
// Delivery: the drain requires BOTH masks for SERIAL, FILE, BLE and G2:
//   (msg->routing & MSG_ROUTE_X) && (gOutputFlags & MSG_ROUTE_X)
// Two sinks are deliberately routing-gated only (no gOutputFlags check):
//   WEB  — the web CLI mirror is always populated while the buffer exists,
//          so history is there whenever a browser attaches. The WEB bit in
//          gOutputFlags only feeds the BROADCAST_PRINTF early-out below.
//   OLED — the OLED console screen is its own opt-in (the user must open
//          it), so command output is always routed there (broadcastOutput
//          force-adds OLED|G2 to command routes).
// Persisted web/display/g2 lane settings were removed pre-1.0 once an audit
// showed delivery had never honored them.
#define MSG_ROUTE_SERIAL  0x01
#define MSG_ROUTE_WEB     0x02
#define MSG_ROUTE_FILE    0x04
#define MSG_ROUTE_OLED    0x08
#define MSG_ROUTE_BLE     0x10
#define MSG_ROUTE_G2      0x20  // Even Realities G2 glasses display
#define MSG_ROUTE_ALLOW_IN_HELP  0x40
#define MSG_ROUTE_ALL     0x3F  // all 6 sinks (no ALLOW_IN_HELP)
#ifndef DEBUG_SYSTEM_H
#define DEBUG_SYSTEM_H

#include <Arduino.h>
#include <stdint.h>

// ============================================================================
// Debug System - Centralized debug output and ring buffer management
// ============================================================================
// This header provides: ensureDebugBuffer(), getDebugBuffer(), broadcastOutput(),
// BROADCAST_PRINTF, DEBUG_*F macros, and all debug flag constants.
// Include this header instead of using 'extern' declarations for these functions.

// 256-bit debug flag mask. xtensa-esp-elf-g++ is a 32-bit toolchain and
// does not provide __uint128_t, so we hand-roll a four-uint64 mask with
// the operators the codebase uses (|, &, ~, <<, >>, ==, !=, contextual
// bool). All ops are constexpr so DEBUG_BIT(n) and the DEBUG_* constants
// fold at compile time.
struct DebugFlagMask {
  uint64_t w0;  // bits 0-63
  uint64_t w1;  // bits 64-127
  uint64_t w2;  // bits 128-191
  uint64_t w3;  // bits 192-255

  constexpr DebugFlagMask() : w0(0), w1(0), w2(0), w3(0) {}
  constexpr DebugFlagMask(uint64_t v) : w0(v), w1(0), w2(0), w3(0) {}   // implicit: lets ((DebugFlagMask)0x1ULL) and `mask = 0` work
  constexpr DebugFlagMask(uint64_t a, uint64_t b, uint64_t c, uint64_t d) : w0(a), w1(b), w2(c), w3(d) {}

  constexpr explicit operator bool() const { return (w0 | w1 | w2 | w3) != 0; }
  constexpr explicit operator uint64_t() const { return w0; }           // low 64 bits — combine with >>64/128/192 to print the rest

  constexpr DebugFlagMask operator|(DebugFlagMask o) const { return {w0 | o.w0, w1 | o.w1, w2 | o.w2, w3 | o.w3}; }
  constexpr DebugFlagMask operator&(DebugFlagMask o) const { return {w0 & o.w0, w1 & o.w1, w2 & o.w2, w3 & o.w3}; }
  constexpr DebugFlagMask operator^(DebugFlagMask o) const { return {w0 ^ o.w0, w1 ^ o.w1, w2 ^ o.w2, w3 ^ o.w3}; }
  constexpr DebugFlagMask operator~() const { return {~w0, ~w1, ~w2, ~w3}; }

  DebugFlagMask& operator|=(DebugFlagMask o) { w0 |= o.w0; w1 |= o.w1; w2 |= o.w2; w3 |= o.w3; return *this; }
  DebugFlagMask& operator&=(DebugFlagMask o) { w0 &= o.w0; w1 &= o.w1; w2 &= o.w2; w3 &= o.w3; return *this; }
  DebugFlagMask& operator^=(DebugFlagMask o) { w0 ^= o.w0; w1 ^= o.w1; w2 ^= o.w2; w3 ^= o.w3; return *this; }

  constexpr bool operator==(DebugFlagMask o) const { return w0 == o.w0 && w1 == o.w1 && w2 == o.w2 && w3 == o.w3; }
  constexpr bool operator!=(DebugFlagMask o) const { return !(*this == o); }

  // Shifts: word/bit split. The bs != 0 guards avoid the UB of shifting a
  // uint64_t by 64 (shift count >= width) when n is a multiple of 64.
  constexpr DebugFlagMask operator<<(int n) const {
    if (n <= 0)   return *this;
    if (n >= 256) return {0, 0, 0, 0};
    const uint64_t in[4] = {w0, w1, w2, w3};
    uint64_t out[4] = {0, 0, 0, 0};
    const int ws = n >> 6, bs = n & 63;
    for (int i = 3; i >= ws; --i) {
      out[i] = in[i - ws] << bs;
      if (bs != 0 && i - ws - 1 >= 0) out[i] |= in[i - ws - 1] >> (64 - bs);
    }
    return {out[0], out[1], out[2], out[3]};
  }
  constexpr DebugFlagMask operator>>(int n) const {
    if (n <= 0)   return *this;
    if (n >= 256) return {0, 0, 0, 0};
    const uint64_t in[4] = {w0, w1, w2, w3};
    uint64_t out[4] = {0, 0, 0, 0};
    const int ws = n >> 6, bs = n & 63;
    for (int i = 0; i + ws <= 3; ++i) {
      out[i] = in[i + ws] >> bs;
      if (bs != 0 && i + ws + 1 <= 3) out[i] |= in[i + ws + 1] << (64 - bs);
    }
    return {out[0], out[1], out[2], out[3]};
  }
};

#include "System_Debug_Manager.h"
#include "System_BuildConfig.h"

// Helper: construct a single-bit mask (bit 0-255). Shifts past word
// boundaries are well-defined via the operators above.
#define DEBUG_BIT(n)          (((DebugFlagMask)1) << (n))

// ============================================================================
// Debug flags — generated from the X-macro table in System_DebugFlags.h.
// That table is the single source of truth for every flag's bit, bank,
// parent link, and writer-side tag: it emits the DEBUG_* mask constants,
// the DbgBank/DbgFlagIdx index enums, the kDbg* parallel columns, and the
// static_asserts that hold the map's invariants. It is a fragment of THIS
// header — it needs DEBUG_BIT above and must never be included directly.
// ============================================================================
#include "System_DebugFlags.h"

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
  bool storageFiles;       // File read/write/delete operations
  bool storageJson;        // JSON parsing, serialization, validation
  bool storageSettings;    // Settings load/save, module registration
  bool storageMigration;   // Filesystem migrations, directory creation
  bool storagePermissions; // [PERM] DENY audit lines from VFS::*Guarded
  
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

  // NTP / DateTime sub-flags
  bool ntpSync;         // DNS validation, wait loop, per-iteration polling, success/timeout
  bool ntpSetup;        // setupNTP() / configTime() calls, server & offset config
  bool ntpAnchor;       // Boot anchor write/read/cleanup, loadAndIncrementBootSeq
  bool ntpResolve;      // resolvePendingUserCreationTimes(), [resolve] messages

  // ESP-SR sub-flags (parent: gSettings.debugSr -> DEBUG_SR)
  bool srWake;          // Wake word events (HiLexin etc.)
  bool srCommand;       // MultiNet command recognition / matching
  bool srAfe;           // AFE chain — VAD, noise suppression, gain
  bool srLifecycle;     // Init / start / stop verbose
  bool srTuning;        // Auto-tune sweeps, confidence threshold changes
};

// Debug output queue configuration
#define DEBUG_QUEUE_SIZE_MIN 64    // Minimum queue size (internal RAM only)
#define DEBUG_QUEUE_SIZE_MAX 192   // Maximum queue size (with PSRAM)
#define DEBUG_POOL_SIZE_INITIAL_PSRAM 96  // Grow to DEBUG_QUEUE_SIZE_MAX under pressure
#define DEBUG_MSG_SIZE 256         // Max size of each debug message

// Queue capacity and currently allocated message slots. On PSRAM builds the
// queues keep full capacity while the expensive message pool starts at 96 and
// grows once to 192 before pressure can exhaust it.
extern int gDebugQueueSize;
extern int gDebugPoolSize;

// Debug message structure
struct DebugMessage {
  unsigned long timestamp;
  DebugFlagMask category;  // Debug category (DEBUG_WIFI, DEBUG_AUTH, etc.) — 0 for broadcast messages
  uint8_t  routing;    // MSG_ROUTE_* sink mask — which sinks receive this message
  char text[DEBUG_MSG_SIZE];
};

// Global output lane gates, using the MSG_ROUTE_* sink bits declared at the
// top of this header. (The former OUTPUT_* constants were a second, value-
// misaligned copy of the same sinks; unified 2026-07.) Persisted lanes come
// from named settings booleans in applySettings; FILE/BLE are runtime lanes.
extern volatile uint32_t gOutputFlags;

// System logging state
extern String gSystemLogPath;
extern bool gSystemLogRunning;
extern unsigned long gSystemLogLastWrite;
extern bool gSystemLogCategoryTags;  // Enable/disable category tags in log output

// ============================================================================
// Debug System Functions
// ============================================================================

// Initialize debug system (call from setup())
void initDebugSystem();

// Record an always-on system lifecycle event: boot decisions, FS format /
// file deletions, settings load/save failures, WiFi connects. Emits an
// "[EVENT][<category>] ..." line independent of debug flags and log level;
// the output task persists it to LOG_EVENTS_FILE and mirrors it to the
// normal sinks. Safe to call before initDebugSystem() — early-boot events
// echo to Serial immediately and are ring-buffered until the queue exists.
// Keep it LOW VOLUME: discrete state changes only, never per-iteration data.
void logSystemEvent(const char* category, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Enqueue one pre-formatted line into the debug output queue with an explicit
// route mask. Used by the event-stream file sink (systemEventLogTick):
// routing MSG_ROUTE_ALLOW_IN_HELP alone means "no display sinks" — the line
// exists only for the [EVLOG] file tee in debugOutputTask. Returns false when
// the queue is saturated (the drop is counted in gDebugDropped).
bool debugQueueLine(const char* text, uint8_t routing);

// Ensure debug buffer is allocated
bool ensureDebugBuffer();

// Internal globals - required for inline accessor functions below
// DO NOT access directly - use accessor functions instead
extern DebugFlagMask gDebugFlags;  // 128-bit debug flags
extern DebugSubFlags gDebugSubFlags;
extern char* gDebugBuffer;
// gDebugBuffer is the shared scratch space for cmd_* return strings.
// Two classes of user: (1) short status strings that stream long output
// line-by-line via broadcastOutput() (capped at DEBUG_MSG_SIZE 256 B) and
// return a one-liner — the historical case, for which 1 KB was already a
// no-op upper bound; (2) commands that return a single machine-readable JSON
// object DIRECTLY as the result (memreport/taskstats/perftop json, etc.),
// which is NOT subject to the 256 B broadcast cap — it flows through the
// command result path up to the transport's own limit (~4 KB over BLE).
// Sized 4096 to hold a full task-table JSON snapshot in one object; this
// matches the ~4 KB BLE reply ceiling, so a larger buffer wouldn't help over
// BLE anyway. Only PSRAM (lazy ps_alloc), never a stack array — grep confirms
// no `char x[GLOBAL_DEBUG_BUFFER_SIZE]` exists — so the bump costs nothing on
// any task stack. Existing `snprintf(gDebugBuffer, 1024, ...)` callsites stay
// correct (self-capping); only the JSON producers pass the full size.
constexpr size_t GLOBAL_DEBUG_BUFFER_SIZE = 4096;
extern QueueHandle_t gDebugOutputQueue;
extern QueueHandle_t gDebugFreeQueue;

// Yield inside a bursty debug-output loop until the free-list pool drains back
// above `minFree` slots (bounded by `maxWaitMs`). Prevents a producer loop from
// overflowing the pool and dropping the tail of large boot dumps. No-op when
// the pool is already healthy, so it's free when nothing is being emitted.
void debugQueueBackpressure(int minFree = 32, uint32_t maxWaitMs = 200);

// Bounded wait for the output queue itself to drain to empty. Used by the
// serial result writer so a directly-written command result cannot overtake
// lines the command streamed through the queue during execution.
void debugWaitOutputDrained(uint32_t maxWaitMs);
extern volatile unsigned long gDebugDropped;
extern volatile bool gDebugVerbose;

// ── Low-level stack/heap diagnostic trace (runtime-only) ────────────────────
// Emits directly to Serial — bypasses the debug_out queue and the broadcast
// fan-out. The point is to stay visible when the thing you're diagnosing IS
// the debug subsystem (queue saturation, task stack overflow, LittleFS deep
// recursion, etc).
//
// Toggle via CLI:   debugstack on  |  debugstack off
// Not persisted.    Turn it off when you're done; Serial.printf is synchronous
//                   and will slow heavy paths if left on.
extern volatile bool gDebugStackTraceEnabled;

// hwm is in BYTES on this port (StackType_t is uint8_t) — no word->byte
// scaling. See System_TaskUtils.h.
#define STACK_TRACEF(fmt, ...) do { \
  if (gDebugStackTraceEnabled) { \
    UBaseType_t _hwm_bytes = uxTaskGetStackHighWaterMark(nullptr); \
    Serial.printf("[STK %s hwm=%u heap=%u ms=%lu] " fmt "\n", \
                  pcTaskGetName(nullptr), \
                  (unsigned)_hwm_bytes, \
                  (unsigned)ESP.getFreeHeap(), \
                  (unsigned long)millis(), \
                  ##__VA_ARGS__); \
  } \
} while (0)

// Accessor functions - use these instead of direct global access
// Read the global directly. DebugManager::getDebugFlags() is literally
// `return gDebugFlags;`, but routing through the singleton makes this an
// un-inlinable cross-TU call (getInstance() + getter, returning a 32-byte mask
// by value) at every DEBUGF guard site. gDebugFlags is extern above and
// static-init'd independent of the manager, so a direct read is equivalent and
// lets the compiler fold the mask math for single-bit flags.
inline DebugFlagMask getDebugFlags() { return gDebugFlags; }
inline void setDebugFlags(DebugFlagMask flags) { DEBUG_MANAGER.setDebugFlags(flags); }
inline void setDebugFlag(DebugFlagMask flag) { setDebugFlags(getDebugFlags() | flag); }
inline void clearDebugFlag(DebugFlagMask flag) { setDebugFlags(getDebugFlags() & ~flag); }
inline bool isDebugFlagSet(DebugFlagMask flag) { return gDebugVerbose || ((getDebugFlags() & flag) != (DebugFlagMask)0); }

// Apply the ESP-IDF log-level for the TLS/HTTPS framework tags (esp-tls-mbedtls,
// esp_https_server, httpd, httpd_txrx). verbose=false → ESP_LOG_NONE (quiet the
// benign browser-disconnect flood); verbose=true → ESP_LOG_DEBUG. Driven by the
// DEBUG_HTTPS flag; called at boot from applySettings() and on `debughttps`.
void applyHttpsLogLevels(bool verbose);

// Sub-flag accessor functions
inline DebugSubFlags& getDebugSubFlags() { return gDebugSubFlags; }

// Helper to update parent flag when sub-flags change
inline void updateParentDebugFlag(DebugFlagMask parentFlag, bool anyChildEnabled) {
  if (anyChildEnabled) setDebugFlag(parentFlag);
  else clearDebugFlag(parentFlag);
}

// Recompute ONE aggregated family's parent bit from its charter terms
// (DBG_AGG_FAMILY_LIST / DBG_SUBBOOL_LIST / the settingsField column — see
// System_Debug.cpp). Structural no-op for every flag outside the agg list:
// explicit master switches (G2, MQTT, sensors, ...) are never rederived.
// CLI handlers must call this single-family form only — a cross-family sweep
// would clobber temp-set parent bits in unrelated families. Family PARENT
// handlers must not call it either (SR arrangement): the terms exclude the
// runtime parent bit, so a recompute would immediately undo a temp parent
// toggle. Sub handlers recompute; parent handlers own their bit outright.
void dbgRecomputeParent(DbgFlagIdx root);

// Recompute every aggregated family. applySettings()-only: the full sweep is
// safe there and only there because the mask was just rebuilt from gSettings,
// so no temp-set bits exist to clobber — and bit==setting for every mapped
// row, which is what reduces the SETTINGS_BITS runtime terms (BT/SR child
// bits) to the settings-only expressions the boot path historically used.
void dbgRecomputeAllParents();
// Direct global reads — same rationale as getDebugFlags()/getLogLevel() above.
// The DebugManager getters are pure pass-throughs (return gDebugOutputQueue etc.),
// so routing through the singleton only adds an un-inlinable cross-TU hop. This
// path matters: debugQueuePrintf() calls getDebugQueue()/getDebugFreeQueue() on
// every single log line.
inline char* getDebugBuffer() { return gDebugBuffer; }
inline QueueHandle_t getDebugQueue() { return gDebugOutputQueue; }
inline QueueHandle_t getDebugFreeQueue() { return gDebugFreeQueue; }
inline void incrementDebugDropped() { gDebugDropped = gDebugDropped + 1; }

// Broadcast output functions
void broadcastOutput(const String& s);
void broadcastOutput(const char* s);

// CLI next-step hint (see System_Debug.cpp). Text form prints "Hint: <text>";
// JSON command output sets a top-level "hint" string field instead. Use only
// where output is a genuine dead end or could be misread — not everywhere.
void cliHint(const char* text);
void cliHintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Convenience for the common "this output is a listing, not a reading" case.
void emitListingTrailer(const char* what, const char* readHint, const char* extraNote = nullptr);

// Log de-spam primitive: returns true at most once per windowMs for a given
// by-ref last-timestamp (first call always returns true). Use to throttle a
// repeating WARN/log line from a flapping peer/condition. Generalized from the
// hand-rolled notifyCooldownOk / logBondAuthFailure idiom. millis()-based.
bool logCooldownOk(uint32_t& lastMs, uint32_t windowMs);

// Forward declaration for CommandContext (defined in main .ino)
struct CommandContext;
void broadcastOutput(const String& s, const CommandContext& ctx);

// Transport-completion delivery for a command's return value (OUTPUT CONTRACT
// channel 2). Serial-origin results are written to the console directly —
// byte-exact, no [origin] decoration, no 256 B chunking — and every other
// sink keeps the queued mirror it gets today. Non-serial origins fall through
// to broadcastOutput(s, ctx) unchanged. Completion sites should call this,
// not broadcastOutput, with the return blob.
void deliverCommandResult(const String& result, const CommandContext& ctx);

// Output capture is now per-task (System_AuthIdentity.h). Use
// setCaptureBuffer() / clearCaptureBuffer() / currentCaptureState() instead
// of the previous gCmdCaptureBuf/Len/Cap globals — those routed cross-task
// broadcasts into whichever buffer cmd_exec_task had set.

void debugQueuePrintf(DebugFlagMask flag, const char* fmt, ...);

// Print summary (and tail) of output suppressed during help; resets counters
void helpSuppressedPrintAndReset();
void helpSuppressedTailDump();

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
// Direct global read for the same reason as getDebugFlags() above:
// DebugManager::getLogLevel() just returns gLogLevel (extern above).
inline uint8_t getLogLevel() { return gDebugVerbose ? LOG_LEVEL_DEBUG : gLogLevel; }
// ============================================================================
// Debug Macros - Safe for use from any context
// ============================================================================

// Queue-based debug output - thread-safe with mutex protection
// Checks sensor enabled flags to prevent crashes during task deletion
// CRITICAL: Do NOT call from sensor tasks when disabled - causes crashes
// OPTIMIZED: Allocates DebugMessage on heap instead of stack (saves 256 bytes per call)
// PERFORMANCE: Lazy evaluation - check flag BEFORE evaluating expensive arguments
// DEBUG_ALWAYS anywhere in the mask emits unconditionally; the producer's own
// flag rides along so the message still tags as its real subsystem.
#define DEBUGF_QUEUE(flag, fmt, ...) \
  do { \
    if (((flag) & DEBUG_ALWAYS) != (DebugFlagMask)0 || isDebugFlagSet(flag)) { \
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
// I2C sub-flag macros — parent OR sub gating so the I2C "All" toggle stays a master.
#define DEBUG_I2C_BUSF(fmt, ...)       DEBUGF_QUEUE_DEBUG(DEBUG_I2C | DEBUG_I2C_BUS,       fmt, ##__VA_ARGS__)
#define DEBUG_I2C_DISCOVERYF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_I2C | DEBUG_I2C_DISCOVERY, fmt, ##__VA_ARGS__)
#define DEBUG_I2C_AUTOSTARTF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_I2C | DEBUG_I2C_AUTOSTART, fmt, ##__VA_ARGS__)
// MQTT debug macros — parent + 4 sub-flag-aware variants.
#define DEBUG_MQTTF(fmt, ...)             DEBUGF_QUEUE_DEBUG(DEBUG_MQTT, fmt, ##__VA_ARGS__)
#define DEBUG_MQTT_CONNECTIONF(fmt, ...)  DEBUGF_QUEUE_DEBUG(DEBUG_MQTT | DEBUG_MQTT_CONNECTION, fmt, ##__VA_ARGS__)
#define DEBUG_MQTT_PUBSUBF(fmt, ...)      DEBUGF_QUEUE_DEBUG(DEBUG_MQTT | DEBUG_MQTT_PUBSUB,     fmt, ##__VA_ARGS__)
#define DEBUG_MQTT_DISCOVERYF(fmt, ...)   DEBUGF_QUEUE_DEBUG(DEBUG_MQTT | DEBUG_MQTT_DISCOVERY,  fmt, ##__VA_ARGS__)
#define DEBUG_MQTT_COMMANDSF(fmt, ...)    DEBUGF_QUEUE_DEBUG(DEBUG_MQTT | DEBUG_MQTT_COMMANDS,   fmt, ##__VA_ARGS__)
#define DEBUG_CMD_FLOWF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_CMD_FLOW, fmt, ##__VA_ARGS__)
#define DEBUG_FMRADIOF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_FMRADIO, fmt, ##__VA_ARGS__)
#define DEBUG_G2F(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_G2, fmt, ##__VA_ARGS__)
// G2 sub-flag macros. Each gate ORs the parent DEBUG_G2 with the
// sub-flag, so DEBUG_G2 acts as a true master switch ("show everything
// G2") while individual sub-flags grant granular control without
// implicitly enabling the parent. Settings does NOT aggregate sub-flags
// up to the parent — toggling just "Lifecycle" gives only lifecycle
// messages, which is the point of the granular split.
#define DEBUG_G2_LIFECYCLEF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_G2 | DEBUG_G2_LIFECYCLE, fmt, ##__VA_ARGS__)
#define DEBUG_G2_PROTOCOLF(fmt, ...)  DEBUGF_QUEUE_DEBUG(DEBUG_G2 | DEBUG_G2_PROTOCOL,  fmt, ##__VA_ARGS__)
#define DEBUG_G2_EVENTSF(fmt, ...)    DEBUGF_QUEUE_DEBUG(DEBUG_G2 | DEBUG_G2_EVENTS,    fmt, ##__VA_ARGS__)
#define DEBUG_G2_PAGESF(fmt, ...)     DEBUGF_QUEUE_DEBUG(DEBUG_G2 | DEBUG_G2_PAGES,     fmt, ##__VA_ARGS__)
#define DEBUG_G2_HEARTBEATF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_G2 | DEBUG_G2_HEARTBEAT, fmt, ##__VA_ARGS__)
#define DEBUG_G2_DUMPF(fmt, ...)      DEBUGF_QUEUE_DEBUG(DEBUG_G2 | DEBUG_G2_DUMP,      fmt, ##__VA_ARGS__)
#define DEBUG_CAMERAF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_CAMERA, fmt, ##__VA_ARGS__)
// Camera sub-flag macros — gate on parent OR sub-flag so the Camera "All"
// toggle still acts as a master switch. Mirrors DEBUG_G2_*F.
#define DEBUG_CAMERA_LIFECYCLEF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_CAMERA | DEBUG_CAMERA_LIFECYCLE, fmt, ##__VA_ARGS__)
#define DEBUG_CAMERA_CAPTUREF(fmt, ...)   DEBUGF_QUEUE_DEBUG(DEBUG_CAMERA | DEBUG_CAMERA_CAPTURE,   fmt, ##__VA_ARGS__)
#define DEBUG_CAMERA_SETTINGSF(fmt, ...)  DEBUGF_QUEUE_DEBUG(DEBUG_CAMERA | DEBUG_CAMERA_SETTINGS,  fmt, ##__VA_ARGS__)
#define DEBUG_CAMERA_VIDEOF(fmt, ...)     DEBUGF_QUEUE_DEBUG(DEBUG_CAMERA | DEBUG_CAMERA_VIDEO,     fmt, ##__VA_ARGS__)
#define DEBUG_MICF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_MICROPHONE, fmt, ##__VA_ARGS__)
// Per-sensor sub-flag macros (Lifecycle / Polling / Values).
// Each gates on parent OR sub so master "All <Sensor>" toggle still acts as
// a master switch. Mirrors DEBUG_CAMERA_*F / DEBUG_G2_*F.
#define DEBUG_THERMAL_LIFECYCLEF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_THERMAL    | DEBUG_THERMAL_LIFECYCLE,  fmt, ##__VA_ARGS__)
#define DEBUG_THERMAL_POLLINGF(fmt, ...)   DEBUGF_QUEUE_DEBUG(DEBUG_THERMAL    | DEBUG_THERMAL_POLLING,    fmt, ##__VA_ARGS__)
#define DEBUG_THERMAL_VALUESF(fmt, ...)    DEBUGF_QUEUE_DEBUG(DEBUG_THERMAL    | DEBUG_THERMAL_VALUES,     fmt, ##__VA_ARGS__)
#define DEBUG_TOF_LIFECYCLEF(fmt, ...)     DEBUGF_QUEUE_DEBUG(DEBUG_TOF        | DEBUG_TOF_LIFECYCLE,      fmt, ##__VA_ARGS__)
#define DEBUG_TOF_POLLINGF(fmt, ...)       DEBUGF_QUEUE_DEBUG(DEBUG_TOF        | DEBUG_TOF_POLLING,        fmt, ##__VA_ARGS__)
#define DEBUG_TOF_VALUESF(fmt, ...)        DEBUGF_QUEUE_DEBUG(DEBUG_TOF        | DEBUG_TOF_VALUES,         fmt, ##__VA_ARGS__)
#define DEBUG_INPUT_LIFECYCLEF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_INPUT    | DEBUG_INPUT_LIFECYCLE,  fmt, ##__VA_ARGS__)
#define DEBUG_INPUT_POLLINGF(fmt, ...)   DEBUGF_QUEUE_DEBUG(DEBUG_INPUT    | DEBUG_INPUT_POLLING,    fmt, ##__VA_ARGS__)
#define DEBUG_INPUT_VALUESF(fmt, ...)    DEBUGF_QUEUE_DEBUG(DEBUG_INPUT    | DEBUG_INPUT_VALUES,     fmt, ##__VA_ARGS__)
#define DEBUG_ANO_ENCODER_LIFECYCLEF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_ANO_ENCODER | DEBUG_ANO_ENCODER_LIFECYCLE, fmt, ##__VA_ARGS__)
#define DEBUG_ANO_ENCODER_POLLINGF(fmt, ...)   DEBUGF_QUEUE_DEBUG(DEBUG_ANO_ENCODER | DEBUG_ANO_ENCODER_POLLING,   fmt, ##__VA_ARGS__)
#define DEBUG_ANO_ENCODER_VALUESF(fmt, ...)    DEBUGF_QUEUE_DEBUG(DEBUG_ANO_ENCODER | DEBUG_ANO_ENCODER_VALUES,    fmt, ##__VA_ARGS__)
#define DEBUG_IMU_LIFECYCLEF(fmt, ...)     DEBUGF_QUEUE_DEBUG(DEBUG_IMU        | DEBUG_IMU_LIFECYCLE,      fmt, ##__VA_ARGS__)
#define DEBUG_IMU_POLLINGF(fmt, ...)       DEBUGF_QUEUE_DEBUG(DEBUG_IMU        | DEBUG_IMU_POLLING,        fmt, ##__VA_ARGS__)
#define DEBUG_IMU_VALUESF(fmt, ...)        DEBUGF_QUEUE_DEBUG(DEBUG_IMU        | DEBUG_IMU_VALUES,         fmt, ##__VA_ARGS__)
#define DEBUG_APDS_LIFECYCLEF(fmt, ...)    DEBUGF_QUEUE_DEBUG(DEBUG_APDS       | DEBUG_APDS_LIFECYCLE,     fmt, ##__VA_ARGS__)
#define DEBUG_APDS_POLLINGF(fmt, ...)      DEBUGF_QUEUE_DEBUG(DEBUG_APDS       | DEBUG_APDS_POLLING,       fmt, ##__VA_ARGS__)
#define DEBUG_APDS_VALUESF(fmt, ...)       DEBUGF_QUEUE_DEBUG(DEBUG_APDS       | DEBUG_APDS_VALUES,        fmt, ##__VA_ARGS__)
#define DEBUG_GPS_LIFECYCLEF(fmt, ...)     DEBUGF_QUEUE_DEBUG(DEBUG_GPS        | DEBUG_GPS_LIFECYCLE,      fmt, ##__VA_ARGS__)
#define DEBUG_GPS_POLLINGF(fmt, ...)       DEBUGF_QUEUE_DEBUG(DEBUG_GPS        | DEBUG_GPS_POLLING,        fmt, ##__VA_ARGS__)
#define DEBUG_GPS_VALUESF(fmt, ...)        DEBUGF_QUEUE_DEBUG(DEBUG_GPS        | DEBUG_GPS_VALUES,         fmt, ##__VA_ARGS__)
#define DEBUG_RTC_LIFECYCLEF(fmt, ...)     DEBUGF_QUEUE_DEBUG(DEBUG_RTC        | DEBUG_RTC_LIFECYCLE,      fmt, ##__VA_ARGS__)
#define DEBUG_RTC_POLLINGF(fmt, ...)       DEBUGF_QUEUE_DEBUG(DEBUG_RTC        | DEBUG_RTC_POLLING,        fmt, ##__VA_ARGS__)
#define DEBUG_RTC_VALUESF(fmt, ...)        DEBUGF_QUEUE_DEBUG(DEBUG_RTC        | DEBUG_RTC_VALUES,         fmt, ##__VA_ARGS__)
#define DEBUG_FMRADIO_LIFECYCLEF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_FMRADIO    | DEBUG_FMRADIO_LIFECYCLE,  fmt, ##__VA_ARGS__)
#define DEBUG_FMRADIO_POLLINGF(fmt, ...)   DEBUGF_QUEUE_DEBUG(DEBUG_FMRADIO    | DEBUG_FMRADIO_POLLING,    fmt, ##__VA_ARGS__)
#define DEBUG_FMRADIO_VALUESF(fmt, ...)    DEBUGF_QUEUE_DEBUG(DEBUG_FMRADIO    | DEBUG_FMRADIO_VALUES,     fmt, ##__VA_ARGS__)
#define DEBUG_MIC_LIFECYCLEF(fmt, ...)     DEBUGF_QUEUE_DEBUG(DEBUG_MICROPHONE | DEBUG_MIC_LIFECYCLE,      fmt, ##__VA_ARGS__)
#define DEBUG_MIC_POLLINGF(fmt, ...)       DEBUGF_QUEUE_DEBUG(DEBUG_MICROPHONE | DEBUG_MIC_POLLING,        fmt, ##__VA_ARGS__)
#define DEBUG_MIC_VALUESF(fmt, ...)        DEBUGF_QUEUE_DEBUG(DEBUG_MICROPHONE | DEBUG_MIC_VALUES,         fmt, ##__VA_ARGS__)
#define DEBUG_PRESENCE_LIFECYCLEF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_PRESENCE  | DEBUG_PRESENCE_LIFECYCLE, fmt, ##__VA_ARGS__)
#define DEBUG_PRESENCE_POLLINGF(fmt, ...)  DEBUGF_QUEUE_DEBUG(DEBUG_PRESENCE   | DEBUG_PRESENCE_POLLING,   fmt, ##__VA_ARGS__)
#define DEBUG_PRESENCE_VALUESF(fmt, ...)   DEBUGF_QUEUE_DEBUG(DEBUG_PRESENCE   | DEBUG_PRESENCE_VALUES,    fmt, ##__VA_ARGS__)
#define DEBUG_ESPNOW_METADATAF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_ESPNOW_METADATA, fmt, ##__VA_ARGS__)
#define DEBUG_MAPSF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_MAPS, fmt, ##__VA_ARGS__)
#define DEBUG_MAPS_LOADINGF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_MAPS_LOADING, fmt, ##__VA_ARGS__)
#define DEBUG_MAPS_RENDERINGF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_MAPS_RENDERING, fmt, ##__VA_ARGS__)
#define DEBUG_MAPS_PERFF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_MAPS_PERF, fmt, ##__VA_ARGS__)
#define DEBUG_LLMF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_LLM, fmt, ##__VA_ARGS__)
#define DEBUG_LLM_LOADF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_LLM_LOAD, fmt, ##__VA_ARGS__)
#define DEBUG_LLM_TOKENIZERF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_LLM_TOKENIZER, fmt, ##__VA_ARGS__)
#define DEBUG_LLM_FORWARDF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_LLM_FORWARD, fmt, ##__VA_ARGS__)
#define DEBUG_LLM_GENERATEF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_LLM_GENERATE, fmt, ##__VA_ARGS__)
#define DEBUG_LLM_MEMORYF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_LLM_MEMORY, fmt, ##__VA_ARGS__)
#define DEBUG_WIFIF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_WIFI, fmt, ##__VA_ARGS__)
#define DEBUG_NTPF(fmt, ...)          DEBUGF_QUEUE_DEBUG(DEBUG_NTP, fmt, ##__VA_ARGS__)
#define DEBUG_NTP_SYNCF(fmt, ...)     DEBUGF_QUEUE_DEBUG(DEBUG_NTP, fmt, ##__VA_ARGS__)
#define DEBUG_NTP_SETUPF(fmt, ...)    DEBUGF_QUEUE_DEBUG(DEBUG_NTP, fmt, ##__VA_ARGS__)
#define DEBUG_NTP_ANCHORF(fmt, ...)   DEBUGF_QUEUE_DEBUG(DEBUG_NTP, fmt, ##__VA_ARGS__)
#define DEBUG_NTP_RESOLVEF(fmt, ...)  DEBUGF_QUEUE_DEBUG(DEBUG_NTP, fmt, ##__VA_ARGS__)
#define DEBUG_STORAGEF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_STORAGE, fmt, ##__VA_ARGS__)
#define DEBUG_PERFORMANCEF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_PERFORMANCE, fmt, ##__VA_ARGS__)
#define DEBUG_SYSTEMF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_SYSTEM, fmt, ##__VA_ARGS__)
#define DEBUG_USERSF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_USERS, fmt, ##__VA_ARGS__)
#define DEBUG_AUTOMATIONSF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_AUTOMATIONS, fmt, ##__VA_ARGS__)
#define DEBUG_LOGGERF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_LOGGER, fmt, ##__VA_ARGS__)
#define DEBUG_MEMORYF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_MEMORY, fmt, ##__VA_ARGS__)
// Memory sub-flag macros — gate on parent OR sub-flag so the Memory "All"
// toggle still acts as a master switch. Mirrors DEBUG_CAMERA_*F / DEBUG_G2_*F.
#define DEBUG_MEMORY_HEAPF(fmt, ...)    DEBUGF_QUEUE_DEBUG(DEBUG_MEMORY | DEBUG_MEMORY_HEAP,    fmt, ##__VA_ARGS__)
#define DEBUG_MEMORY_STACKF(fmt, ...)   DEBUGF_QUEUE_DEBUG(DEBUG_MEMORY | DEBUG_MEMORY_STACK,   fmt, ##__VA_ARGS__)
#define DEBUG_MEMORY_BUFFERSF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_MEMORY | DEBUG_MEMORY_BUFFERS, fmt, ##__VA_ARGS__)
#define DEBUG_ESPNOW_STREAMF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_ESPNOW_STREAM, fmt, ##__VA_ARGS__)
#define DEBUG_ESPNOWF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_ESPNOW_CORE, fmt, ##__VA_ARGS__)  // General ESP-NOW debug
#define DEBUG_COMMAND_SYSTEMF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_COMMAND_SYSTEM, fmt, ##__VA_ARGS__)


// Individual I2C sensor debug macros
#define DEBUG_GPSF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_GPS, fmt, ##__VA_ARGS__)
#define DEBUG_RTCF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_RTC, fmt, ##__VA_ARGS__)
#define DEBUG_IMUF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_IMU, fmt, ##__VA_ARGS__)
#define DEBUG_THERMALF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_THERMAL, fmt, ##__VA_ARGS__)
#define DEBUG_TOFF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_TOF, fmt, ##__VA_ARGS__)
#define DEBUG_INPUTF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_INPUT, fmt, ##__VA_ARGS__)
#define DEBUG_ANO_ENCODERF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_ANO_ENCODER, fmt, ##__VA_ARGS__)
#define DEBUG_APDSF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_APDS, fmt, ##__VA_ARGS__)
#define DEBUG_PRESENCEF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_PRESENCE, fmt, ##__VA_ARGS__)
#define DEBUG_DISPLAYF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_DISPLAY, fmt, ##__VA_ARGS__)
#define DEBUG_NOTIFICATIONSF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_NOTIFICATIONS, fmt, ##__VA_ARGS__)

// Legacy compatibility macro
#define DEBUGF(flag, fmt, ...) DEBUGF_QUEUE_DEBUG(flag, fmt, ##__VA_ARGS__)

// ============================================================================
// Severity-Based Logging Macros
// ============================================================================

// ERROR macros - Always visible (cannot be disabled)
#define ERROR_I2CF(fmt, ...) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_I2C, "[ERROR][I2C] " fmt, ##__VA_ARGS__)
#define ERROR_ESPNOWF(fmt, ...) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_ESPNOW_CORE, "[ERROR][ESP-NOW] " fmt, ##__VA_ARGS__)
#define ERROR_AUTOMATIONF(fmt, ...) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_AUTOMATIONS, "[ERROR][AUTO] " fmt, ##__VA_ARGS__)
#define ERROR_SESSIONF(fmt, ...) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_AUTH, "[ERROR][SESSION] " fmt, ##__VA_ARGS__)
#define ERROR_USERF(fmt, ...) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_USERS, "[ERROR][USER] " fmt, ##__VA_ARGS__)
#define ERROR_LOGGINGF(fmt, ...) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_LOGGER, "[ERROR][LOG] " fmt, ##__VA_ARGS__)
#define ERROR_WEBF(fmt, ...) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_HTTP, "[ERROR][WEB] " fmt, ##__VA_ARGS__)
#define ERROR_COMMANDF(fmt, ...) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_CLI, "[ERROR][CMD] " fmt, ##__VA_ARGS__)
#define ERROR_SYSTEMF(fmt, ...) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_SYSTEM, "[ERROR][SYS] " fmt, ##__VA_ARGS__)
#define ERROR_STORAGEF(fmt, ...) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_STORAGE, "[ERROR][STORAGE] " fmt, ##__VA_ARGS__)
#define ERROR_WIFIF(fmt, ...) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_WIFI, "[ERROR][WIFI] " fmt, ##__VA_ARGS__)
#define ERROR_MEMORYF(fmt, ...) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_MEMORY, "[ERROR][MEM] " fmt, ##__VA_ARGS__)
#define ERROR_LLMF(fmt, ...) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_LLM, "[ERROR][LLM] " fmt, ##__VA_ARGS__)
// Per-sensor / display ERROR macros — always visible (DEBUG_ALWAYS).
// Added during DEBUG_SENSORS umbrella removal so error tags match their
// actual subsystem instead of all reading "[ERROR][SENSORS]".
#define ERROR_CAMERAF(fmt, ...)       DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_CAMERA,       "[ERROR][CAMERA] "    fmt, ##__VA_ARGS__)
#define ERROR_THERMALF(fmt, ...)      DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_THERMAL,      "[ERROR][THERMAL] "   fmt, ##__VA_ARGS__)
#define ERROR_TOFF(fmt, ...)          DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_TOF,          "[ERROR][TOF] "       fmt, ##__VA_ARGS__)
#define ERROR_IMUF(fmt, ...)          DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_IMU,          "[ERROR][IMU] "       fmt, ##__VA_ARGS__)
#define ERROR_INPUTF(fmt, ...)        DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_INPUT,        "[ERROR][INPUT] "     fmt, ##__VA_ARGS__)
#define ERROR_ANO_ENCODERF(fmt, ...)  DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_ANO_ENCODER,  "[ERROR][ANO] "       fmt, ##__VA_ARGS__)
#define ERROR_APDSF(fmt, ...)         DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_APDS,         "[ERROR][APDS] "      fmt, ##__VA_ARGS__)
#define ERROR_PRESENCEF(fmt, ...)     DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_PRESENCE,     "[ERROR][PRESENCE] "  fmt, ##__VA_ARGS__)
#define ERROR_GPSF(fmt, ...)          DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_GPS,          "[ERROR][GPS] "       fmt, ##__VA_ARGS__)
#define ERROR_RTCF(fmt, ...)          DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_RTC,          "[ERROR][RTC] "       fmt, ##__VA_ARGS__)
#define ERROR_FMRADIOF(fmt, ...)      DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_FMRADIO,      "[ERROR][FMRADIO] "   fmt, ##__VA_ARGS__)
#define ERROR_MAPSF(fmt, ...)         DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_MAPS,         "[ERROR][MAPS] "      fmt, ##__VA_ARGS__)
#define ERROR_MICF(fmt, ...)          DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_MICROPHONE,   "[ERROR][MIC] "       fmt, ##__VA_ARGS__)
#define ERROR_DISPLAYF(fmt, ...)      DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_DISPLAY,      "[ERROR][DISPLAY] "   fmt, ##__VA_ARGS__)
#define ERROR_MQTTF(fmt, ...)         DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_MQTT,         "[ERROR][MQTT] "      fmt, ##__VA_ARGS__)

// WARN macros - Always visible (cannot be disabled)
#define WARN_I2CF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_I2C, "[WARN][I2C] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_ESPNOWF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_ESPNOW_CORE, "[WARN][ESP-NOW] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_AUTOMATIONF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_AUTOMATIONS, "[WARN][AUTO] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_SESSIONF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_AUTH, "[WARN][SESSION] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_USERF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_USERS, "[WARN][USER] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_LOGGINGF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_LOGGER, "[WARN][LOG] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_WEBF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_HTTP, "[WARN][WEB] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_COMMANDF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_CLI, "[WARN][CMD] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_SYSTEMF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_SYSTEM, "[WARN][SYS] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_STORAGEF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_STORAGE, "[WARN][STORAGE] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_WIFIF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_WIFI, "[WARN][WIFI] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_MEMORYF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_MEMORY, "[WARN][MEM] " fmt, ##__VA_ARGS__); } while (0)
// Per-sensor WARN macros — added during DEBUG_SENSORS umbrella removal.
// Only added for subsystems that actually use WARN_SENSORSF today.
#define WARN_INPUTF(fmt, ...)        do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_INPUT,        "[WARN][INPUT] "  fmt, ##__VA_ARGS__); } while (0)
#define WARN_ANO_ENCODERF(fmt, ...)  do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_ANO_ENCODER,  "[WARN][ANO] "    fmt, ##__VA_ARGS__); } while (0)
#define WARN_MAPSF(fmt, ...)         do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_MAPS,         "[WARN][MAPS] "   fmt, ##__VA_ARGS__); } while (0)
#define WARN_IMUF(fmt, ...)          do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_IMU,          "[WARN][IMU] "    fmt, ##__VA_ARGS__); } while (0)
#define WARN_MQTTF(fmt, ...)         do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_MQTT,         "[WARN][MQTT] "   fmt, ##__VA_ARGS__); } while (0)
#define WARN_BLUETOOTHF(fmt, ...)    do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_BLUETOOTH,    "[WARN][BT] "     fmt, ##__VA_ARGS__); } while (0)

// INFO macros - Optional (controlled by debug flags)
#define INFO_CAMERAF(fmt, ...)       do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_CAMERA | DEBUG_CAMERA_LIFECYCLE, "[INFO][CAMERA] " fmt, ##__VA_ARGS__); } while (0)
#define INFO_CAMERA_VIDEOF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_CAMERA | DEBUG_CAMERA_VIDEO,     "[INFO][VIDEO] "  fmt, ##__VA_ARGS__); } while (0)
// Per-sensor INFO macros — added during DEBUG_SENSORS umbrella removal.
// Each gates on its own per-sensor flag so the sensor's UI toggle actually
// controls its INFO output (previously these all funneled through DEBUG_SENSORS).
#define INFO_THERMALF(fmt, ...)  do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_THERMAL,    "[INFO][THERMAL] "  fmt, ##__VA_ARGS__); } while (0)
#define INFO_TOFF(fmt, ...)      do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_TOF,        "[INFO][TOF] "      fmt, ##__VA_ARGS__); } while (0)
#define INFO_IMUF(fmt, ...)      do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_IMU,        "[INFO][IMU] "      fmt, ##__VA_ARGS__); } while (0)
#define INFO_INPUTF(fmt, ...)  do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_INPUT,    "[INFO][INPUT] "  fmt, ##__VA_ARGS__); } while (0)
#define INFO_ANO_ENCODERF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_ANO_ENCODER, "[INFO][ANO] " fmt, ##__VA_ARGS__); } while (0)
#define INFO_APDSF(fmt, ...)     do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_APDS,       "[INFO][APDS] "     fmt, ##__VA_ARGS__); } while (0)
#define INFO_PRESENCEF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_PRESENCE,   "[INFO][PRESENCE] " fmt, ##__VA_ARGS__); } while (0)
#define INFO_GPSF(fmt, ...)      do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_GPS,        "[INFO][GPS] "      fmt, ##__VA_ARGS__); } while (0)
#define INFO_RTCF(fmt, ...)      do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_RTC,        "[INFO][RTC] "      fmt, ##__VA_ARGS__); } while (0)
#define INFO_FMRADIOF(fmt, ...)  do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_FMRADIO,    "[INFO][FMRADIO] "  fmt, ##__VA_ARGS__); } while (0)
#define INFO_MAPSF(fmt, ...)     do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_MAPS,       "[INFO][MAPS] "     fmt, ##__VA_ARGS__); } while (0)
#define INFO_MICF(fmt, ...)      do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_MICROPHONE, "[INFO][MIC] "      fmt, ##__VA_ARGS__); } while (0)
// Per-sensor INFO sub-flag macros (Option β). Mirror the DEBUG_*_LIFECYCLEF /
// _POLLINGF / _VALUESF sub-flag-aware variants but at INFO severity. Each gates
// on (parent_flag | sub_flag) so the user can:
//   - enable the parent ("All GPS") → see every INFO_GPS_* line
//   - enable just the sub-flag ("GPS Lifecycle") → see only LIFE-tagged INFO
// Tag uses short suffix (LIFE / POLL / VAL) to keep log columns tight, matching
// the existing MQTT/I2C INFO sub-flag tag style (e.g. [INFO][MQTT_CONN]).
#define INFO_THERMAL_LIFECYCLEF(fmt, ...)  do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_THERMAL    | DEBUG_THERMAL_LIFECYCLE,  "[INFO][THERMAL_LIFE] "  fmt, ##__VA_ARGS__); } while (0)
#define INFO_THERMAL_POLLINGF(fmt, ...)    do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_THERMAL    | DEBUG_THERMAL_POLLING,    "[INFO][THERMAL_POLL] "  fmt, ##__VA_ARGS__); } while (0)
#define INFO_THERMAL_VALUESF(fmt, ...)     do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_THERMAL    | DEBUG_THERMAL_VALUES,     "[INFO][THERMAL_VAL] "   fmt, ##__VA_ARGS__); } while (0)
#define INFO_TOF_LIFECYCLEF(fmt, ...)      do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_TOF        | DEBUG_TOF_LIFECYCLE,      "[INFO][TOF_LIFE] "      fmt, ##__VA_ARGS__); } while (0)
#define INFO_TOF_POLLINGF(fmt, ...)        do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_TOF        | DEBUG_TOF_POLLING,        "[INFO][TOF_POLL] "      fmt, ##__VA_ARGS__); } while (0)
#define INFO_TOF_VALUESF(fmt, ...)         do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_TOF        | DEBUG_TOF_VALUES,         "[INFO][TOF_VAL] "       fmt, ##__VA_ARGS__); } while (0)
#define INFO_IMU_LIFECYCLEF(fmt, ...)      do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_IMU        | DEBUG_IMU_LIFECYCLE,      "[INFO][IMU_LIFE] "      fmt, ##__VA_ARGS__); } while (0)
#define INFO_IMU_POLLINGF(fmt, ...)        do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_IMU        | DEBUG_IMU_POLLING,        "[INFO][IMU_POLL] "      fmt, ##__VA_ARGS__); } while (0)
#define INFO_IMU_VALUESF(fmt, ...)         do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_IMU        | DEBUG_IMU_VALUES,         "[INFO][IMU_VAL] "       fmt, ##__VA_ARGS__); } while (0)
#define INFO_INPUT_LIFECYCLEF(fmt, ...)  do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_INPUT    | DEBUG_INPUT_LIFECYCLE,  "[INFO][INPUT_LIFE] "  fmt, ##__VA_ARGS__); } while (0)
#define INFO_INPUT_POLLINGF(fmt, ...)    do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_INPUT    | DEBUG_INPUT_POLLING,    "[INFO][INPUT_POLL] "  fmt, ##__VA_ARGS__); } while (0)
#define INFO_INPUT_VALUESF(fmt, ...)     do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_INPUT    | DEBUG_INPUT_VALUES,     "[INFO][INPUT_VAL] "   fmt, ##__VA_ARGS__); } while (0)
#define INFO_ANO_ENCODER_LIFECYCLEF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_ANO_ENCODER | DEBUG_ANO_ENCODER_LIFECYCLE, "[INFO][ANO_LIFE] " fmt, ##__VA_ARGS__); } while (0)
#define INFO_ANO_ENCODER_POLLINGF(fmt, ...)   do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_ANO_ENCODER | DEBUG_ANO_ENCODER_POLLING,   "[INFO][ANO_POLL] " fmt, ##__VA_ARGS__); } while (0)
#define INFO_ANO_ENCODER_VALUESF(fmt, ...)    do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_ANO_ENCODER | DEBUG_ANO_ENCODER_VALUES,    "[INFO][ANO_VAL] "  fmt, ##__VA_ARGS__); } while (0)
#define INFO_APDS_LIFECYCLEF(fmt, ...)     do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_APDS       | DEBUG_APDS_LIFECYCLE,     "[INFO][APDS_LIFE] "     fmt, ##__VA_ARGS__); } while (0)
#define INFO_APDS_POLLINGF(fmt, ...)       do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_APDS       | DEBUG_APDS_POLLING,       "[INFO][APDS_POLL] "     fmt, ##__VA_ARGS__); } while (0)
#define INFO_APDS_VALUESF(fmt, ...)        do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_APDS       | DEBUG_APDS_VALUES,        "[INFO][APDS_VAL] "      fmt, ##__VA_ARGS__); } while (0)
#define INFO_PRESENCE_LIFECYCLEF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_PRESENCE   | DEBUG_PRESENCE_LIFECYCLE, "[INFO][PRESENCE_LIFE] " fmt, ##__VA_ARGS__); } while (0)
#define INFO_PRESENCE_POLLINGF(fmt, ...)   do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_PRESENCE   | DEBUG_PRESENCE_POLLING,   "[INFO][PRESENCE_POLL] " fmt, ##__VA_ARGS__); } while (0)
#define INFO_PRESENCE_VALUESF(fmt, ...)    do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_PRESENCE   | DEBUG_PRESENCE_VALUES,    "[INFO][PRESENCE_VAL] "  fmt, ##__VA_ARGS__); } while (0)
#define INFO_GPS_LIFECYCLEF(fmt, ...)      do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_GPS        | DEBUG_GPS_LIFECYCLE,      "[INFO][GPS_LIFE] "      fmt, ##__VA_ARGS__); } while (0)
#define INFO_GPS_POLLINGF(fmt, ...)        do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_GPS        | DEBUG_GPS_POLLING,        "[INFO][GPS_POLL] "      fmt, ##__VA_ARGS__); } while (0)
#define INFO_GPS_VALUESF(fmt, ...)         do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_GPS        | DEBUG_GPS_VALUES,         "[INFO][GPS_VAL] "       fmt, ##__VA_ARGS__); } while (0)
#define INFO_RTC_LIFECYCLEF(fmt, ...)      do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_RTC        | DEBUG_RTC_LIFECYCLE,      "[INFO][RTC_LIFE] "      fmt, ##__VA_ARGS__); } while (0)
#define INFO_RTC_POLLINGF(fmt, ...)        do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_RTC        | DEBUG_RTC_POLLING,        "[INFO][RTC_POLL] "      fmt, ##__VA_ARGS__); } while (0)
#define INFO_RTC_VALUESF(fmt, ...)         do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_RTC        | DEBUG_RTC_VALUES,         "[INFO][RTC_VAL] "       fmt, ##__VA_ARGS__); } while (0)
#define INFO_FMRADIO_LIFECYCLEF(fmt, ...)  do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_FMRADIO    | DEBUG_FMRADIO_LIFECYCLE,  "[INFO][FMRADIO_LIFE] "  fmt, ##__VA_ARGS__); } while (0)
#define INFO_FMRADIO_POLLINGF(fmt, ...)    do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_FMRADIO    | DEBUG_FMRADIO_POLLING,    "[INFO][FMRADIO_POLL] "  fmt, ##__VA_ARGS__); } while (0)
#define INFO_FMRADIO_VALUESF(fmt, ...)     do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_FMRADIO    | DEBUG_FMRADIO_VALUES,     "[INFO][FMRADIO_VAL] "   fmt, ##__VA_ARGS__); } while (0)
#define INFO_MIC_LIFECYCLEF(fmt, ...)      do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_MICROPHONE | DEBUG_MIC_LIFECYCLE,      "[INFO][MIC_LIFE] "      fmt, ##__VA_ARGS__); } while (0)
#define INFO_MIC_POLLINGF(fmt, ...)        do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_MICROPHONE | DEBUG_MIC_POLLING,        "[INFO][MIC_POLL] "      fmt, ##__VA_ARGS__); } while (0)
#define INFO_MIC_VALUESF(fmt, ...)         do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_MICROPHONE | DEBUG_MIC_VALUES,         "[INFO][MIC_VAL] "       fmt, ##__VA_ARGS__); } while (0)
#define INFO_DISPLAYF(fmt, ...)  do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_DISPLAY,    "[INFO][DISPLAY] "  fmt, ##__VA_ARGS__); } while (0)
#define INFO_I2CF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_I2C, "[INFO][I2C] " fmt, ##__VA_ARGS__); } while (0)
#define INFO_I2C_BUSF(fmt, ...)       do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_I2C | DEBUG_I2C_BUS,       "[INFO][I2C_BUS] "       fmt, ##__VA_ARGS__); } while (0)
#define INFO_I2C_DISCOVERYF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_I2C | DEBUG_I2C_DISCOVERY, "[INFO][I2C_DISCOVERY] " fmt, ##__VA_ARGS__); } while (0)
#define INFO_I2C_AUTOSTARTF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_I2C | DEBUG_I2C_AUTOSTART, "[INFO][I2C_AUTOSTART] " fmt, ##__VA_ARGS__); } while (0)
// MQTT INFO macros — parent + 4 sub-flag-aware variants.
#define INFO_MQTTF(fmt, ...)            do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_MQTT,                       "[INFO][MQTT] "            fmt, ##__VA_ARGS__); } while (0)
#define INFO_MQTT_CONNECTIONF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_MQTT | DEBUG_MQTT_CONNECTION, "[INFO][MQTT_CONN] "      fmt, ##__VA_ARGS__); } while (0)
#define INFO_MQTT_PUBSUBF(fmt, ...)     do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_MQTT | DEBUG_MQTT_PUBSUB,     "[INFO][MQTT_PUBSUB] "    fmt, ##__VA_ARGS__); } while (0)
#define INFO_MQTT_DISCOVERYF(fmt, ...)  do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_MQTT | DEBUG_MQTT_DISCOVERY,  "[INFO][MQTT_DISCOVERY] " fmt, ##__VA_ARGS__); } while (0)
#define INFO_MQTT_COMMANDSF(fmt, ...)   do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_MQTT | DEBUG_MQTT_COMMANDS,   "[INFO][MQTT_CMD] "       fmt, ##__VA_ARGS__); } while (0)
#define INFO_ESPNOWF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_ESPNOW_CORE, "[INFO][ESP-NOW] " fmt, ##__VA_ARGS__); } while (0)
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

// MEMORY OPTIMIZATION: Printf-style broadcastOutput using stack-local buffer
// Thread-safe: each caller uses its own stack for formatting (no shared gDebugBuffer)
// PERFORMANCE: Conditional execution - check output flags BEFORE allocating stack/formatting
#define BROADCAST_PRINTF(fmt, ...) \
  do { \
    if (gOutputFlags & (MSG_ROUTE_SERIAL | MSG_ROUTE_WEB | MSG_ROUTE_FILE | MSG_ROUTE_BLE)) { \
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
    if (gOutputFlags & (MSG_ROUTE_SERIAL | MSG_ROUTE_WEB | MSG_ROUTE_FILE | MSG_ROUTE_BLE)) { \
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
    if (gOutputFlags & (MSG_ROUTE_SERIAL | MSG_ROUTE_WEB | MSG_ROUTE_FILE | MSG_ROUTE_BLE)) { \
      char _bpBuf[256]; \
      snprintf(_bpBuf, sizeof(_bpBuf), fmt, ##__VA_ARGS__); \
      broadcastOutput(_bpBuf, ctx); \
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

const char* cmd_debugauthcookies(const String& argsInput);
const char* cmd_debughttp(const String& argsInput);
const char* cmd_debugsse(const String& argsInput);
const char* cmd_debugcli(const String& argsInput);
const char* cmd_debugcamera(const String& argsInput);
const char* cmd_debugcameralifecycle(const String& a);
const char* cmd_debugcameracapture(const String& a);
const char* cmd_debugcamerasettings(const String& a);
const char* cmd_debugcameravideo(const String& a);
const char* cmd_debugdisplay(const String& a);
const char* cmd_debugmicrophone(const String& argsInput);
const char* cmd_debugwifi(const String& argsInput);
const char* cmd_debugstorage(const String& argsInput);
const char* cmd_debuglogger(const String& argsInput);
const char* cmd_debugautomations(const String& argsInput);
const char* cmd_debugperformance(const String& argsInput);
const char* cmd_debugauth(const String& argsInput);
const char* cmd_debugdatetime(const String& argsInput);
const char* cmd_debugdatetimesync(const String& argsInput);
const char* cmd_debugdatetimesetup(const String& argsInput);
const char* cmd_debugdatetimeanchor(const String& argsInput);
const char* cmd_debugdatetimeresolve(const String& argsInput);
const char* cmd_debuggps(const String& argsInput);
const char* cmd_debugrtc(const String& argsInput);
const char* cmd_debugimu(const String& argsInput);
const char* cmd_debugthermal(const String& argsInput);
const char* cmd_debugtof(const String& argsInput);
const char* cmd_debuginput(const String& argsInput);
const char* cmd_debugapds(const String& argsInput);
const char* cmd_debugpresence(const String& argsInput);
const char* cmd_debugverbose(const String& argsInput);
const char* cmd_debugbuffer(const String& argsInput);
const char* cmd_debugflags(const String& argsInput);
const char* cmd_debugcommandflow(const String& argsInput);
const char* cmd_debugusers(const String& argsInput);
const char* cmd_debugsystem(const String& argsInput);
const char* cmd_debugespnowstream(const String& argsInput);
const char* cmd_debugespnowcore(const String& argsInput);
const char* cmd_debugespnowrouter(const String& argsInput);
const char* cmd_debugespnowmesh(const String& argsInput);
const char* cmd_debugespnowtopo(const String& argsInput);
const char* cmd_debugespnowmetadata(const String& argsInput);
const char* cmd_debugautoscheduler(const String& argsInput);
const char* cmd_debugautoexec(const String& argsInput);
const char* cmd_debugautocondition(const String& argsInput);
const char* cmd_debugautotiming(const String& argsInput);
const char* cmd_debugmemory(const String& argsInput);
const char* cmd_debugmemoryheap(const String& a);
const char* cmd_debugmemorystack(const String& a);
const char* cmd_debugmemorybuffers(const String& a);
const char* cmd_debugauthsessions(const String& argsInput);
const char* cmd_debugauthcookies(const String& argsInput);
const char* cmd_debugauthlogin(const String& argsInput);
const char* cmd_debugauthbootid(const String& argsInput);
const char* cmd_debughttphandlers(const String& argsInput);
const char* cmd_debughttprequests(const String& argsInput);
const char* cmd_debughttpresponses(const String& argsInput);
const char* cmd_debughttpstreaming(const String& argsInput);
const char* cmd_debugwificonnection(const String& argsInput);
const char* cmd_debugwificonfig(const String& argsInput);
const char* cmd_debugwifiscanning(const String& argsInput);
const char* cmd_debugwifidriver(const String& argsInput);
const char* cmd_debugstoragefiles(const String& argsInput);
const char* cmd_debugstoragejson(const String& argsInput);
const char* cmd_debugstoragesettings(const String& argsInput);
const char* cmd_debugstoragemigration(const String& argsInput);
const char* cmd_debugstoragepermissions(const String& argsInput);
const char* cmd_debugsystemboot(const String& argsInput);
const char* cmd_debugsystemconfig(const String& argsInput);
const char* cmd_debugsystemtasks(const String& argsInput);
const char* cmd_debugsystemhardware(const String& argsInput);
const char* cmd_debugusersmgmt(const String& argsInput);
const char* cmd_debugusersregister(const String& argsInput);
const char* cmd_debugusersquery(const String& argsInput);
const char* cmd_debugcliexecution(const String& argsInput);
const char* cmd_debugcliqueue(const String& argsInput);
const char* cmd_debugclivalidation(const String& argsInput);
const char* cmd_debugperfstack(const String& argsInput);
const char* cmd_debugperfheap(const String& argsInput);
const char* cmd_debugperftiming(const String& argsInput);
const char* cmd_debugsseconnection(const String& argsInput);
const char* cmd_debugsseevents(const String& argsInput);
const char* cmd_debugssebroadcast(const String& argsInput);
const char* cmd_debugcmdflowrouting(const String& argsInput);
const char* cmd_debugcmdflowqueue(const String& argsInput);
const char* cmd_debugcmdflowcontext(const String& argsInput);
const char* cmd_commandmodulesummary(const String& argsInput);
const char* cmd_settingsmodulesummary(const String& argsInput);
const char* cmd_debugmaps(const String& argsInput);
const char* cmd_debugmapsloading(const String& argsInput);
const char* cmd_debugmapsrendering(const String& argsInput);
const char* cmd_debugmapsperf(const String& argsInput);
#if ENABLE_ONDEVICE_LLM
const char* cmd_debugllm(const String& argsInput);
const char* cmd_debugllmload(const String& argsInput);
const char* cmd_debugllmtokenizer(const String& argsInput);
const char* cmd_debugllmforward(const String& argsInput);
const char* cmd_debugllmgenerate(const String& argsInput);
const char* cmd_debugllmmemory(const String& argsInput);
#endif

// ESP-SR debug commands (System_ESPSR — parent + 5 sub-flags)
const char* cmd_debugsr(const String& argsInput);
const char* cmd_debugsrwake(const String& argsInput);
const char* cmd_debugsrcommand(const String& argsInput);
const char* cmd_debugsrafe(const String& argsInput);
const char* cmd_debugsrlifecycle(const String& argsInput);
const char* cmd_debugsrtuning(const String& argsInput);

// System logging commands
const char* cmd_log(const String& argsInput);

// System log auto-start (called from boot)
void systemLogAutoStart();

// Parse / format the persisted systemLogFlags hex mask (also used by Settings UI save).
bool parseSystemLogFlags(const String& flagsStr, DebugFlagMask& out);
String formatSystemLogFlags(DebugFlagMask mask);
bool systemLogApplyPersistedFlags();  // gSettings.systemLogFlags → gDebugFlags

// Helper: Get category name from debug flag
const char* getDebugCategoryName(DebugFlagMask flag);

#endif // DEBUG_SYSTEM_H
