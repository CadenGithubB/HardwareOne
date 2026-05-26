// Per-message routing flags (set by producer, checked by debugOutputTask consumer).
// These control which sinks receive each individual message.
// Bits 0-5 are sink enables; bit 6 is a modifier.
#define MSG_ROUTE_SERIAL  0x01
#define MSG_ROUTE_WEB     0x02
#define MSG_ROUTE_FILE    0x04
#define MSG_ROUTE_OLED    0x08
#define MSG_ROUTE_BLE     0x10
#define MSG_ROUTE_G2      0x20
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

// 128-bit debug flag mask. xtensa-esp-elf-g++ is a 32-bit toolchain and
// does not provide __uint128_t, so we hand-roll a two-uint64 mask with
// the operators the codebase uses (|, &, ~, <<, >>, ==, !=, contextual
// bool). All ops are constexpr so DEBUG_BIT(n) and the DEBUG_* constants
// fold at compile time.
struct DebugFlagMask {
  uint64_t lo;
  uint64_t hi;

  constexpr DebugFlagMask() : lo(0), hi(0) {}
  constexpr DebugFlagMask(uint64_t v) : lo(v), hi(0) {}              // implicit: lets ((DebugFlagMask)0x1ULL) and `mask = 0` work
  constexpr DebugFlagMask(uint64_t l, uint64_t h) : lo(l), hi(h) {}

  constexpr explicit operator bool() const { return (lo | hi) != 0; }
  constexpr explicit operator uint64_t() const { return lo; }        // returns low 64 bits — used by log printing

  constexpr DebugFlagMask operator|(DebugFlagMask o) const { return {lo | o.lo, hi | o.hi}; }
  constexpr DebugFlagMask operator&(DebugFlagMask o) const { return {lo & o.lo, hi & o.hi}; }
  constexpr DebugFlagMask operator^(DebugFlagMask o) const { return {lo ^ o.lo, hi ^ o.hi}; }
  constexpr DebugFlagMask operator~() const { return {~lo, ~hi}; }

  DebugFlagMask& operator|=(DebugFlagMask o) { lo |= o.lo; hi |= o.hi; return *this; }
  DebugFlagMask& operator&=(DebugFlagMask o) { lo &= o.lo; hi &= o.hi; return *this; }
  DebugFlagMask& operator^=(DebugFlagMask o) { lo ^= o.lo; hi ^= o.hi; return *this; }

  constexpr bool operator==(DebugFlagMask o) const { return lo == o.lo && hi == o.hi; }
  constexpr bool operator!=(DebugFlagMask o) const { return !(*this == o); }

  // Shifts: the n==0 and n==64 special cases avoid the UB of `x << 64` /
  // `x >> 64` on a uint64_t (shift count >= width).
  constexpr DebugFlagMask operator<<(int n) const {
    if (n <= 0)   return *this;
    if (n >= 128) return {0, 0};
    if (n == 64)  return {0, lo};
    if (n > 64)   return {0, lo << (n - 64)};
    return {lo << n, (hi << n) | (lo >> (64 - n))};
  }
  constexpr DebugFlagMask operator>>(int n) const {
    if (n <= 0)   return *this;
    if (n >= 128) return {0, 0};
    if (n == 64)  return {hi, 0};
    if (n > 64)   return {hi >> (n - 64), 0};
    return {(lo >> n) | (hi << (64 - n)), hi >> n};
  }
};

#include "System_Debug_Manager.h"
#include "System_BuildConfig.h"

// Helper: construct a single-bit mask. `((DebugFlagMask)1) << 64` is
// well-defined via the shift operator above; use this for any flag past bit 63.
#define DEBUG_BIT(n)          (((DebugFlagMask)1) << (n))

// Bits 0-31: Core system and infrastructure
#define DEBUG_AUTH            ((DebugFlagMask)0x0001ULL)
#define DEBUG_HTTP            ((DebugFlagMask)0x0002ULL)
#define DEBUG_SSE             ((DebugFlagMask)0x0004ULL)
#define DEBUG_CLI             ((DebugFlagMask)0x0008ULL)
// Bits 4-5: Security and G2 (legacy sensor frame/data flags moved to bits 32-47)
#define DEBUG_MQTT            ((DebugFlagMask)0x0040ULL)  // Bit 6 (reclaimed from DEBUG_SENSORS) — MQTT parent flag
#define DEBUG_FMRADIO         ((DebugFlagMask)0x0080ULL)  // FM Radio operations and I2C debugging
#define DEBUG_I2C             ((DebugFlagMask)0x0100ULL)  // I2C bus operations, transactions, clock changes, mutex
#define DEBUG_WIFI            ((DebugFlagMask)0x0200ULL)
#define DEBUG_PERFORMANCE     ((DebugFlagMask)0x0400ULL)
#define DEBUG_MICROPHONE      ((DebugFlagMask)0x0800ULL)        // Bit 11 - Microphone operations
#define DEBUG_CMD_FLOW        ((DebugFlagMask)0x1000ULL)
#define DEBUG_USERS           ((DebugFlagMask)0x2000ULL)
#define DEBUG_SYSTEM          ((DebugFlagMask)0x4000ULL)
#define DEBUG_SECURITY        ((DebugFlagMask)0x0010ULL)  // Bit 4 - Security operations
#define DEBUG_G2              ((DebugFlagMask)0x0020ULL)  // Bit 5 - G2 smart glasses BLE (parent)
#define DEBUG_STORAGE         ((DebugFlagMask)0x8000ULL)  // Bit 15 - File operations
#define DEBUG_ESPNOW_CORE     ((DebugFlagMask)0x10000ULL)
#define DEBUG_LOGGER          ((DebugFlagMask)0x20000ULL)
#define DEBUG_MEMORY          ((DebugFlagMask)0x40000ULL)
#define DEBUG_ESPNOW_ROUTER   ((DebugFlagMask)0x80000ULL)
#define DEBUG_ESPNOW_MESH     ((DebugFlagMask)0x100000ULL)
#define DEBUG_ESPNOW_TOPO     ((DebugFlagMask)0x200000ULL)
#define DEBUG_ESPNOW_STREAM   ((DebugFlagMask)0x400000ULL)
#define DEBUG_COMMAND_SYSTEM  ((DebugFlagMask)0x800000ULL)  // Modular command registry operations
// Bit 24 (formerly DEBUG_SETTINGS_SYSTEM) — FREED; flag had zero callsites. Available for reuse.
#define DEBUG_AUTO_EXEC       ((DebugFlagMask)0x2000000ULL)     // Bit 25
#define DEBUG_AUTO_CONDITION  ((DebugFlagMask)0x4000000ULL)     // Bit 26
#define DEBUG_AUTO_TIMING     ((DebugFlagMask)0x8000000ULL)     // Bit 27
#define DEBUG_AUTOMATIONS     ((DebugFlagMask)0x10000000ULL)  // Parent flag (legacy - kept for backward compat)
#define DEBUG_CAMERA          ((DebugFlagMask)0x20000000ULL)    // Bit 29 - Camera operations
#define DEBUG_AUTO_SCHEDULER  ((DebugFlagMask)0x40000000ULL)    // Bit 30
#define DEBUG_ESPNOW_ENCRYPTION ((DebugFlagMask)0x80000000ULL)  // Bit 31

// Bits 32-39: Individual I2C sensor debug flags
#define DEBUG_GPS             ((DebugFlagMask)0x100000000ULL)  // Bit 32 - GPS (PA1010D)
#define DEBUG_RTC             ((DebugFlagMask)0x200000000ULL)  // Bit 33 - RTC (DS3231)
#define DEBUG_IMU             ((DebugFlagMask)0x400000000ULL)  // Bit 34 - IMU (BNO055)
#define DEBUG_THERMAL         ((DebugFlagMask)0x800000000ULL)  // Bit 35 - Thermal (MLX90640)
#define DEBUG_TOF             ((DebugFlagMask)0x1000000000ULL) // Bit 36 - ToF (VL53L4CX)
#define DEBUG_INPUT         ((DebugFlagMask)0x2000000000ULL) // Bit 37 - Gamepad (Seesaw)
#define DEBUG_APDS            ((DebugFlagMask)0x4000000000ULL) // Bit 38 - APDS (APDS9960)
#define DEBUG_PRESENCE        ((DebugFlagMask)0x8000000000ULL) // Bit 39 - Presence (STHS34PF80)

// Bits 40-47: Per-sensor frame/data debug flags (granular timing and data processing)
#define DEBUG_THERMAL_POLLING   ((DebugFlagMask)0x10000000000ULL)  // Bit 40 - Thermal frame timing, capture, FPS
#define DEBUG_THERMAL_VALUES    ((DebugFlagMask)0x20000000000ULL)  // Bit 41 - Thermal data interpolation, processing
#define DEBUG_TOF_POLLING       ((DebugFlagMask)0x40000000000ULL)  // Bit 42 - ToF frame capture, object detection
#define DEBUG_INPUT_POLLING   ((DebugFlagMask)0x80000000000ULL)  // Bit 43 - Gamepad frame timing, connection
#define DEBUG_INPUT_VALUES    ((DebugFlagMask)0x100000000000ULL) // Bit 44 - Gamepad button press/release events
#define DEBUG_IMU_POLLING       ((DebugFlagMask)0x200000000000ULL) // Bit 45 - IMU frame timing, cache operations
#define DEBUG_IMU_VALUES        ((DebugFlagMask)0x400000000000ULL) // Bit 46 - IMU data updates
#define DEBUG_APDS_POLLING      ((DebugFlagMask)0x800000000000ULL) // Bit 47 - APDS frame timing, connection
#define DEBUG_ESPNOW_METADATA ((DebugFlagMask)0x1000000000000ULL) // Bit 48 - ESP-NOW metadata exchange (REQ/RESP/PUSH/store)

// Bits 49-52: Maps debug flags
#define DEBUG_MAPS              ((DebugFlagMask)0x2000000000000ULL) // Bit 49 - Maps (parent flag)
#define DEBUG_MAPS_LOADING      ((DebugFlagMask)0x4000000000000ULL) // Bit 50 - Map file loading, tile directory parsing
#define DEBUG_MAPS_RENDERING    ((DebugFlagMask)0x8000000000000ULL) // Bit 51 - Map render pipeline, feature drawing, viewport
#define DEBUG_MAPS_PERF         ((DebugFlagMask)0x10000000000000ULL) // Bit 52 - Map performance timing (render ms, tile I/O, cache, FPS)

// Bits 53-56: Bluetooth debug flags
#define DEBUG_BLUETOOTH         ((DebugFlagMask)0x20000000000000ULL) // Bit 53 - Bluetooth (parent flag)
#define DEBUG_BLUETOOTH_CORE    ((DebugFlagMask)0x40000000000000ULL) // Bit 54 - BLE core lifecycle (init/connect/disconnect)
#define DEBUG_BLUETOOTH_GATT    ((DebugFlagMask)0x80000000000000ULL) // Bit 55 - BLE GATT operations (read/write/notify)
#define DEBUG_BLUETOOTH_DATA    ((DebugFlagMask)0x100000000000000ULL) // Bit 56 - BLE command/data transfer

// Bits 57-62: On-device LLM (llama2.c / System_LLM)
#define DEBUG_LLM               ((DebugFlagMask)0x200000000000000ULL)   // Bit 57 - parent (all LLM debug)
#define DEBUG_LLM_LOAD          ((DebugFlagMask)0x400000000000000ULL)   // Bit 58 - checkpoint load, header validation, weight mapping
#define DEBUG_LLM_TOKENIZER     ((DebugFlagMask)0x800000000000000ULL)   // Bit 59 - tokenizer file, BPE encode/decode
#define DEBUG_LLM_FORWARD       ((DebugFlagMask)0x1000000000000000ULL)  // Bit 60 - transformer forward (per-step; use sparingly)
#define DEBUG_LLM_GENERATE      ((DebugFlagMask)0x2000000000000000ULL)  // Bit 61 - generation loop, sampling, throughput
#define DEBUG_LLM_MEMORY        ((DebugFlagMask)0x4000000000000000ULL)  // Bit 62 - PSRAM estimates, context cap, allocations

// Bit 63: NTP / DateTime (repurposed from debugDateTime which previously aliased DEBUG_SYSTEM)
#define DEBUG_NTP               ((DebugFlagMask)0x8000000000000000ULL)  // Bit 63 - NTP sync, setup, anchors, timestamp resolution

// Bits 64-69: G2 smart glasses sub-flags (parent: DEBUG_G2 at bit 5)
// All gated through DEBUG_G2_*F macros so the parent toggle still works
// as a master switch — sub-flags refine *which* G2 noise gets through.
#define DEBUG_G2_LIFECYCLE      DEBUG_BIT(64)  // Scan, BLE connect/disconnect, MTU, service enumeration
#define DEBUG_G2_PROTOCOL       DEBUG_BIT(65)  // Envelope TX/RX, CRC, fragmentation, parse errors
#define DEBUG_G2_EVENTS         DEBUG_BIT(66)  // DevEvents, ListEvents, SysEvents, gestures, taps
#define DEBUG_G2_PAGES          DEBUG_BIT(67)  // Page-swap worker, hijack, CREATE-list/text, lens state
#define DEBUG_G2_HEARTBEAT      DEBUG_BIT(68)  // Heartbeat TX + HeartbeatAck (every ~5s; loud)
#define DEBUG_G2_DUMP           DEBUG_BIT(69)  // [G2-DUMP] diagnostic ring buffer dumps on errors

// Bits 70-75: ESP-SR speech recognition sub-flags (parent: DEBUG_SR at bit 70)
// Mirrors the legacy gSrDebugLevel (0/1/2/3) but with finer-grained control:
// DEBUG_SR alone matches legacy "level 1" (lifecycle + wake + commands).
// Add WAKE/COMMAND/AFE/LIFECYCLE/TUNING for selective verbosity that previously
// required raising the global level (which dragged in unrelated noise).
#define DEBUG_SR                DEBUG_BIT(70)  // Parent: any SR debug
#define DEBUG_SR_WAKE           DEBUG_BIT(71)  // Wake word detection events
#define DEBUG_SR_COMMAND        DEBUG_BIT(72)  // Command recognition + matching
#define DEBUG_SR_AFE            DEBUG_BIT(73)  // AFE/audio chain (VAD, noise, gain)
#define DEBUG_SR_LIFECYCLE      DEBUG_BIT(74)  // init / start / stop verbose
#define DEBUG_SR_TUNING         DEBUG_BIT(75)  // Auto-tune + confidence threshold

// Bits 76-79: Camera sub-flags (parent: DEBUG_CAMERA at bit 29)
// All gated through DEBUG_CAMERA_*F macros with `DEBUG_CAMERA | DEBUG_CAMERA_<sub>`,
// so the parent toggle still works as a master switch and sub-flags refine
// *which* camera noise gets through. Mirrors the G2 sub-flag pattern.
#define DEBUG_CAMERA_LIFECYCLE  DEBUG_BIT(76)  // initCamera(), stopCamera(), PWDN/RESET sequencing, GPIO state
#define DEBUG_CAMERA_CAPTURE    DEBUG_BIT(77)  // captureFrame(), JPEG validation, frame buffer, recovery path
#define DEBUG_CAMERA_SETTINGS   DEBUG_BIT(78)  // Runtime resolution / quality / sensor register changes
#define DEBUG_CAMERA_VIDEO      DEBUG_BIT(79)  // Video recording start/finalize, frame writing, encoder state

// Bit 80: Display subsystem (OLED init/probe/boot-animation/mode-transitions).
// Previously folded into DEBUG_SENSORS umbrella; broken out during the
// Sensors umbrella removal so display noise can be toggled independently.
#define DEBUG_DISPLAY           DEBUG_BIT(80)

// Bits 81-83: Memory sub-flags (parent: DEBUG_MEMORY at bit 18)
// Mirrors the Performance group's Stack/Heap/Timing split. Lets you isolate
// per-task heap noise from stack-watermark reports from buffer-sizing logs.
#define DEBUG_MEMORY_HEAP       DEBUG_BIT(81)  // [HEAP] per-task free/min/largest, [HEAP_MONITOR] DRAM low watermarks
#define DEBUG_MEMORY_STACK      DEBUG_BIT(82)  // [STACK] per-task watermark + peak reports
#define DEBUG_MEMORY_BUFFERS    DEBUG_BIT(83)  // [JSON_RESP_BUF], [COOKIE_BUF] sizing diagnostics

// Bits 84-86: I2C bus sub-flags (parent: DEBUG_I2C at bit 8)
// Bus enable/disable + sensor auto-start happen at runtime, not just boot,
// so being able to silence each independently matters.
#define DEBUG_I2C_BUS           DEBUG_BIT(84)  // [I2C] bus lifecycle, polling pause/resume, status bumps, raw transactions
#define DEBUG_I2C_DISCOVERY     DEBUG_BIT(85)  // [Discovery] / [I2C_REGISTRY] / [I2C_SENSORS] — device probing, registration, scan results
#define DEBUG_I2C_AUTOSTART     DEBUG_BIT(86)  // [AutoStart] sensor auto-start orchestration + per-sensor init result reporting

// Bits 87-90: MQTT sub-flags (parent: DEBUG_MQTT at bit 6)
// Mirrors the G2/Camera/Memory/I2C pattern. Lets you isolate connection
// noise from publish/subscribe flow from HA discovery from inbound commands.
#define DEBUG_MQTT_CONNECTION   DEBUG_BIT(87)  // connect/disconnect, TLS config, broker errors, client init
#define DEBUG_MQTT_PUBSUB       DEBUG_BIT(88)  // subscribe events, publish results, JSON buffer alloc, received messages
#define DEBUG_MQTT_DISCOVERY    DEBUG_BIT(89)  // Home Assistant auto-discovery configs, base topic generation
#define DEBUG_MQTT_COMMANDS     DEBUG_BIT(90)  // inbound MQTT command parsing, auth, response

// Bits 91-112: Per-sensor Lifecycle / Polling / Values sub-flags.
// Existing bits 40-47 host POLLING + VALUES for THERMAL / TOF / GAMEPAD /
// IMU / APDS (see above, kept for ABI stability). LIFECYCLE for those plus
// the full triplet for sensors that previously had no sub-flags
// (GPS / RTC / FMRADIO / MIC / PRESENCE) live here in the hi-half.
//   LIFECYCLE — init, connect/disconnect, recovery, error retries
//   POLLING   — poll/sample cadence, capture timing, FPS, frame events
//   VALUES    — parsed readings, value-change events, data processing
// Macros gate on parent-OR-sub like every other sub-flag pattern.
#define DEBUG_THERMAL_LIFECYCLE  DEBUG_BIT(91)
#define DEBUG_TOF_LIFECYCLE      DEBUG_BIT(92)
#define DEBUG_TOF_VALUES         DEBUG_BIT(93)
#define DEBUG_INPUT_LIFECYCLE  DEBUG_BIT(94)
#define DEBUG_IMU_LIFECYCLE      DEBUG_BIT(95)
#define DEBUG_APDS_LIFECYCLE     DEBUG_BIT(96)
#define DEBUG_APDS_VALUES        DEBUG_BIT(97)
#define DEBUG_GPS_LIFECYCLE      DEBUG_BIT(98)
#define DEBUG_GPS_POLLING        DEBUG_BIT(99)
#define DEBUG_GPS_VALUES         DEBUG_BIT(100)
#define DEBUG_RTC_LIFECYCLE      DEBUG_BIT(101)
#define DEBUG_RTC_POLLING        DEBUG_BIT(102)
#define DEBUG_RTC_VALUES         DEBUG_BIT(103)
#define DEBUG_FMRADIO_LIFECYCLE  DEBUG_BIT(104)
#define DEBUG_FMRADIO_POLLING    DEBUG_BIT(105)
#define DEBUG_FMRADIO_VALUES     DEBUG_BIT(106)
#define DEBUG_MIC_LIFECYCLE      DEBUG_BIT(107)
#define DEBUG_MIC_POLLING        DEBUG_BIT(108)
#define DEBUG_MIC_VALUES         DEBUG_BIT(109)
#define DEBUG_PRESENCE_LIFECYCLE DEBUG_BIT(110)
#define DEBUG_PRESENCE_POLLING   DEBUG_BIT(111)
#define DEBUG_PRESENCE_VALUES    DEBUG_BIT(112)

// Bits 113-116: ANO Rotary Encoder driver. Separate from DEBUG_INPUT* (which
// gates the shared input-abstraction layer) and from the seesaw gamepad's
// flags — toggling DEBUG_ANO_ENCODER only affects the ANO driver's internal
// logs (init, polling, chord state machine, axis toggle).
#define DEBUG_ANO_ENCODER           DEBUG_BIT(113)
#define DEBUG_ANO_ENCODER_LIFECYCLE DEBUG_BIT(114)
#define DEBUG_ANO_ENCODER_POLLING   DEBUG_BIT(115)
#define DEBUG_ANO_ENCODER_VALUES    DEBUG_BIT(116)

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
#define DEBUG_MSG_SIZE 256         // Max size of each debug message

// Runtime queue size (set during init based on PSRAM availability)
extern int gDebugQueueSize;

// Debug message structure
struct DebugMessage {
  unsigned long timestamp;
  DebugFlagMask category;  // Debug category (DEBUG_WIFI, DEBUG_AUTH, etc.) — 0 for broadcast messages
  uint8_t  routing;    // MSG_ROUTE_* sink mask — which sinks receive this message
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
#ifndef OUTPUT_BLE
#define OUTPUT_BLE    0x20  // Bluetooth Low Energy broadcast output
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
extern DebugFlagMask gDebugFlags;  // 128-bit debug flags
extern DebugSubFlags gDebugSubFlags;
extern char* gDebugBuffer;
// gDebugBuffer is the shared scratch space for cmd_* return strings.
// 1 KB is plenty: any single return string is capped at DEBUG_MSG_SIZE
// (256 B) by the message-queue pipeline anyway, so commands that need
// long output stream line-by-line via broadcastOutput() and return a
// short status string (see cmd_taskstats). The
// existing snprintf(gDebugBuffer, 1024, ...) callsites all produce
// status strings under 256 B in practice — the 1024 cap was already
// a no-op upper bound.
constexpr size_t GLOBAL_DEBUG_BUFFER_SIZE = 1024;
extern QueueHandle_t gDebugOutputQueue;
extern QueueHandle_t gDebugFreeQueue;
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

#define STACK_TRACEF(fmt, ...) do { \
  if (gDebugStackTraceEnabled) { \
    UBaseType_t _hwm_words = uxTaskGetStackHighWaterMark(nullptr); \
    Serial.printf("[STK %s hwm=%u heap=%u ms=%lu] " fmt "\n", \
                  pcTaskGetName(nullptr), \
                  (unsigned)(_hwm_words * 4), \
                  (unsigned)ESP.getFreeHeap(), \
                  (unsigned long)millis(), \
                  ##__VA_ARGS__); \
  } \
} while (0)

// Accessor functions - use these instead of direct global access
inline DebugFlagMask getDebugFlags() { return DEBUG_MANAGER.getDebugFlags(); }
inline void setDebugFlags(DebugFlagMask flags) { DEBUG_MANAGER.setDebugFlags(flags); }
inline void setDebugFlag(DebugFlagMask flag) { setDebugFlags(getDebugFlags() | flag); }
inline void clearDebugFlag(DebugFlagMask flag) { setDebugFlags(getDebugFlags() & ~flag); }
inline bool isDebugFlagSet(DebugFlagMask flag) { return gDebugVerbose || ((getDebugFlags() & flag) != (DebugFlagMask)0); }

// Sub-flag accessor functions
inline DebugSubFlags& getDebugSubFlags() { return gDebugSubFlags; }

// Helper to update parent flag when sub-flags change
inline void updateParentDebugFlag(DebugFlagMask parentFlag, bool anyChildEnabled) {
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

// Legacy compatibility macro
#define DEBUGF(flag, fmt, ...) DEBUGF_QUEUE_DEBUG(flag, fmt, ##__VA_ARGS__)

// ============================================================================
// Severity-Based Logging Macros
// ============================================================================

// ERROR macros - Always visible (cannot be disabled)
#define ERROR_I2CF(fmt, ...) DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][I2C] " fmt, ##__VA_ARGS__)
#define ERROR_ESPNOWF(fmt, ...) DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][ESP-NOW] " fmt, ##__VA_ARGS__)
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
#define ERROR_LLMF(fmt, ...) DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][LLM] " fmt, ##__VA_ARGS__)
// Per-sensor / display ERROR macros — always visible (gate=0xFFFFFFFF).
// Added during DEBUG_SENSORS umbrella removal so error tags match their
// actual subsystem instead of all reading "[ERROR][SENSORS]".
#define ERROR_CAMERAF(fmt, ...)   DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][CAMERA] "   fmt, ##__VA_ARGS__)
#define ERROR_THERMALF(fmt, ...)  DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][THERMAL] "  fmt, ##__VA_ARGS__)
#define ERROR_TOFF(fmt, ...)      DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][TOF] "      fmt, ##__VA_ARGS__)
#define ERROR_IMUF(fmt, ...)      DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][IMU] "      fmt, ##__VA_ARGS__)
#define ERROR_INPUTF(fmt, ...)  DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][INPUT] "  fmt, ##__VA_ARGS__)
#define ERROR_ANO_ENCODERF(fmt, ...)  DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][ANO] "  fmt, ##__VA_ARGS__)
#define ERROR_APDSF(fmt, ...)     DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][APDS] "     fmt, ##__VA_ARGS__)
#define ERROR_PRESENCEF(fmt, ...) DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][PRESENCE] " fmt, ##__VA_ARGS__)
#define ERROR_GPSF(fmt, ...)      DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][GPS] "      fmt, ##__VA_ARGS__)
#define ERROR_RTCF(fmt, ...)      DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][RTC] "      fmt, ##__VA_ARGS__)
#define ERROR_FMRADIOF(fmt, ...)  DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][FMRADIO] "  fmt, ##__VA_ARGS__)
#define ERROR_MAPSF(fmt, ...)     DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][MAPS] "     fmt, ##__VA_ARGS__)
#define ERROR_MICF(fmt, ...)      DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][MIC] "      fmt, ##__VA_ARGS__)
#define ERROR_DISPLAYF(fmt, ...)  DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][DISPLAY] "  fmt, ##__VA_ARGS__)
#define ERROR_MQTTF(fmt, ...)     DEBUGF_QUEUE(0xFFFFFFFF, "[ERROR][MQTT] "     fmt, ##__VA_ARGS__)

// WARN macros - Always visible (cannot be disabled)
#define WARN_I2CF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(0xFFFFFFFF, "[WARN][I2C] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_ESPNOWF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(0xFFFFFFFF, "[WARN][ESP-NOW] " fmt, ##__VA_ARGS__); } while (0)
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
// Per-sensor WARN macros — added during DEBUG_SENSORS umbrella removal.
// Only added for subsystems that actually use WARN_SENSORSF today.
#define WARN_INPUTF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(0xFFFFFFFF, "[WARN][INPUT] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_ANO_ENCODERF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(0xFFFFFFFF, "[WARN][ANO] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_MAPSF(fmt, ...)    do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(0xFFFFFFFF, "[WARN][MAPS] "    fmt, ##__VA_ARGS__); } while (0)
#define WARN_IMUF(fmt, ...)     do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(0xFFFFFFFF, "[WARN][IMU] "     fmt, ##__VA_ARGS__); } while (0)
#define WARN_MQTTF(fmt, ...)    do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(0xFFFFFFFF, "[WARN][MQTT] "    fmt, ##__VA_ARGS__); } while (0)
#define WARN_BLUETOOTHF(fmt, ...) do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(0xFFFFFFFF, "[WARN][BT] " fmt, ##__VA_ARGS__); } while (0)

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
    if (gOutputFlags & (OUTPUT_SERIAL | OUTPUT_WEB | OUTPUT_FILE | OUTPUT_BLE)) { \
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
    if (gOutputFlags & (OUTPUT_SERIAL | OUTPUT_WEB | OUTPUT_FILE | OUTPUT_BLE)) { \
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
    if (gOutputFlags & (OUTPUT_SERIAL | OUTPUT_WEB | OUTPUT_FILE | OUTPUT_BLE)) { \
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

const char* cmd_outdisplay(const String& argsInput);
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
const char* cmd_debugespnow(const String& argsInput);
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
const char* cmd_debugcommandflow(const String& argsInput);
const char* cmd_debugusers(const String& argsInput);
const char* cmd_debugsystem(const String& argsInput);
const char* cmd_debugespnowstream(const String& argsInput);
const char* cmd_debugespnowcore(const String& argsInput);
const char* cmd_debugespnowrouter(const String& argsInput);
const char* cmd_debugespnowmesh(const String& argsInput);
const char* cmd_debugespnowtopo(const String& argsInput);
const char* cmd_debugespnowencryption(const String& argsInput);
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

// Helper: Get category name from debug flag
const char* getDebugCategoryName(DebugFlagMask flag);

#endif // DEBUG_SYSTEM_H
