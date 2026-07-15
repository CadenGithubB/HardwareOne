// Output sink bits — the single bit vocabulary for output routing.
// Bits 0-5 name the sinks; bit 6 is a modifier. Three masks share these
// positions (same bit = same sink everywhere, no translation layers):
//   msg->routing   (DebugMessage)    — which sinks this message targets
//   gOutputFlags   (global)          — which lanes are currently open.
//                    SERIAL/OLED/WEB/G2 are persisted lanes (outSerial etc.,
//                    applied in applySettings); FILE and BLE are runtime
//                    lanes (opened by `log start` / a BLE client connecting).
//   ctx.outputMask (CommandContext)  — which sinks a command's output
//                    targets; passed through to routing in broadcastOutput.
// A message reaches a sink only when BOTH masks have its bit:
//   (msg->routing & MSG_ROUTE_X) && (gOutputFlags & MSG_ROUTE_X)
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
// Debug flag map — 256 bits, one 8-bit bank per family (16 for the fast
// growers), each bank starting on a byte boundary so a printed hex mask
// reads family-per-byte. Spare bits belong to the bank they sit in — add
// new sub-flags there instead of appending at the end.
//
// Bit positions are internal-only: settings persist as per-flag booleans
// and are rebuilt by name in System_Settings.cpp, so renumbering only
// invalidates raw `log start flags=0x...` masks.
//
// Sub-flag convention: parent bit first, subs behind it. The parent is the
// master switch; DEBUG_*F macros gate on parent-OR-sub.
// ============================================================================

// ---- Word 0 (bits 0-63): core system --------------------------------------

// Bits 0-15: core singles (bank full — new subsystems get their own bank)
#define DEBUG_AUTH            DEBUG_BIT(0)
// bit 1 free (was DEBUG_SECURITY — removed pre-1.0: never gated on anywhere)
#define DEBUG_HTTP            DEBUG_BIT(2)   // Firmware's own request/handler logs
#define DEBUG_HTTPS           DEBUG_BIT(3)   // ESP-IDF TLS/server tag verbosity (esp-tls-mbedtls,
                                             // esp_https_server, httpd*) via applyHttpsLogLevels().
                                             // OFF silences the benign handshake-reject/conn-reset
                                             // flood browsers produce against a self-signed cert;
                                             // ON surfaces full TLS handshake detail.
#define DEBUG_SSE             DEBUG_BIT(4)
#define DEBUG_CLI             DEBUG_BIT(5)
#define DEBUG_CMD_FLOW        DEBUG_BIT(6)
#define DEBUG_COMMAND_SYSTEM  DEBUG_BIT(7)   // Modular command registry operations
#define DEBUG_USERS           DEBUG_BIT(8)
#define DEBUG_SYSTEM          DEBUG_BIT(9)
#define DEBUG_STORAGE         DEBUG_BIT(10)  // File operations
#define DEBUG_LOGGER          DEBUG_BIT(11)  // Sensor logger internals
#define DEBUG_PERFORMANCE     DEBUG_BIT(12)
#define DEBUG_WIFI            DEBUG_BIT(13)
#define DEBUG_NTP             DEBUG_BIT(14)  // NTP sync, setup, anchors, timestamp resolution
#define DEBUG_DISPLAY         DEBUG_BIT(15)  // OLED init/probe/boot-animation/mode-transitions
#define DEBUG_NOTIFICATIONS   DEBUG_BIT(16)  // Notification pipeline diagnostics: ring lag/skips,
                                             // stale/cooldown drops, SSE toast-queue saturation
                                             // (periodic [NOTIF] line + `notifstats` CLI)
// Bits 17-23: spare (future core singles)

// Bits 24-31: Memory. Mirrors the Performance group's Stack/Heap/Timing
// split — isolates per-task heap noise from stack watermarks from
// buffer-sizing logs.
#define DEBUG_MEMORY          DEBUG_BIT(24)  // Parent
#define DEBUG_MEMORY_HEAP     DEBUG_BIT(25)  // [HEAP] per-task free/min/largest, [HEAP_MONITOR] DRAM low watermarks
#define DEBUG_MEMORY_STACK    DEBUG_BIT(26)  // [STACK] per-task watermark + peak reports
#define DEBUG_MEMORY_BUFFERS  DEBUG_BIT(27)  // [JSON_RESP_BUF], [COOKIE_BUF] sizing diagnostics
// Bits 28-31: spare (Memory)

// Bits 32-47: ESP-NOW (double-width bank — the mesh roadmap will grow it)
#define DEBUG_ESPNOW_CORE       DEBUG_BIT(32)
#define DEBUG_ESPNOW_ROUTER     DEBUG_BIT(33)
#define DEBUG_ESPNOW_MESH       DEBUG_BIT(34)
#define DEBUG_ESPNOW_TOPO       DEBUG_BIT(35)
#define DEBUG_ESPNOW_STREAM     DEBUG_BIT(36)
// bit 37 free (was DEBUG_ESPNOW_ENCRYPTION — removed pre-1.0: no producer ever gated on it)
#define DEBUG_ESPNOW_METADATA   DEBUG_BIT(38)  // Metadata exchange (REQ/RESP/PUSH/store)
// Bits 39-47: spare (ESP-NOW)

// Bits 48-55: MQTT
#define DEBUG_MQTT            DEBUG_BIT(48)  // Parent
#define DEBUG_MQTT_CONNECTION DEBUG_BIT(49)  // connect/disconnect, TLS config, broker errors, client init
#define DEBUG_MQTT_PUBSUB     DEBUG_BIT(50)  // subscribe events, publish results, JSON buffer alloc, received messages
#define DEBUG_MQTT_DISCOVERY  DEBUG_BIT(51)  // Home Assistant auto-discovery configs, base topic generation
#define DEBUG_MQTT_COMMANDS   DEBUG_BIT(52)  // inbound MQTT command parsing, auth, response
// Bits 53-55: spare (MQTT)

// Bits 56-63: Automations
#define DEBUG_AUTOMATIONS     DEBUG_BIT(56)  // Parent
#define DEBUG_AUTO_EXEC       DEBUG_BIT(57)
#define DEBUG_AUTO_CONDITION  DEBUG_BIT(58)
#define DEBUG_AUTO_TIMING     DEBUG_BIT(59)
#define DEBUG_AUTO_SCHEDULER  DEBUG_BIT(60)
// Bits 61-63: spare (Automations)

// ---- Word 1 (bits 64-127): connectivity & features -------------------------

// Bits 64-71: Bluetooth
#define DEBUG_BLUETOOTH       DEBUG_BIT(64)  // Parent
#define DEBUG_BLUETOOTH_CORE  DEBUG_BIT(65)  // BLE core lifecycle (init/connect/disconnect)
#define DEBUG_BLUETOOTH_GATT  DEBUG_BIT(66)  // BLE GATT operations (read/write/notify)
#define DEBUG_BLUETOOTH_DATA  DEBUG_BIT(67)  // BLE command/data transfer
// Bits 68-71: spare (Bluetooth)

// Bits 72-87: G2 smart glasses (double-width bank — fastest-growing family).
// All gated through DEBUG_G2_*F macros so the parent toggle still works
// as a master switch — sub-flags refine *which* G2 noise gets through.
#define DEBUG_G2              DEBUG_BIT(72)  // Parent (BLE link to glasses)
#define DEBUG_G2_LIFECYCLE    DEBUG_BIT(73)  // Scan, BLE connect/disconnect, MTU, service enumeration
#define DEBUG_G2_PROTOCOL     DEBUG_BIT(74)  // Envelope TX/RX, CRC, fragmentation, parse errors
#define DEBUG_G2_EVENTS       DEBUG_BIT(75)  // DevEvents, ListEvents, SysEvents, gestures, taps
#define DEBUG_G2_PAGES        DEBUG_BIT(76)  // Page-swap worker, hijack, CREATE-list/text, lens state
#define DEBUG_G2_HEARTBEAT    DEBUG_BIT(77)  // Heartbeat TX + HeartbeatAck (every ~5s; loud)
#define DEBUG_G2_DUMP         DEBUG_BIT(78)  // [G2-DUMP] diagnostic ring buffer dumps on errors
// Bits 79-87: spare (G2)

// Bits 88-95: ESP-SR speech recognition. DEBUG_SR alone matches the legacy
// gSrDebugLevel "level 1" (lifecycle + wake + commands); the sub-flags add
// selective verbosity that previously required raising the global level
// (which dragged in unrelated noise).
#define DEBUG_SR              DEBUG_BIT(88)  // Parent: any SR debug
#define DEBUG_SR_WAKE         DEBUG_BIT(89)  // Wake word detection events
#define DEBUG_SR_COMMAND      DEBUG_BIT(90)  // Command recognition + matching
#define DEBUG_SR_AFE          DEBUG_BIT(91)  // AFE/audio chain (VAD, noise, gain)
#define DEBUG_SR_LIFECYCLE    DEBUG_BIT(92)  // init / start / stop verbose
#define DEBUG_SR_TUNING       DEBUG_BIT(93)  // Auto-tune + confidence threshold
// Bits 94-95: spare (SR)

// Bits 96-103: On-device LLM (llama2.c / System_LLM)
#define DEBUG_LLM             DEBUG_BIT(96)   // Parent (all LLM debug)
#define DEBUG_LLM_LOAD        DEBUG_BIT(97)   // checkpoint load, header validation, weight mapping
#define DEBUG_LLM_TOKENIZER   DEBUG_BIT(98)   // tokenizer file, BPE encode/decode
#define DEBUG_LLM_FORWARD     DEBUG_BIT(99)   // transformer forward (per-step; use sparingly)
#define DEBUG_LLM_GENERATE    DEBUG_BIT(100)  // generation loop, sampling, throughput
#define DEBUG_LLM_MEMORY      DEBUG_BIT(101)  // PSRAM estimates, context cap, allocations
// Bits 102-103: spare (LLM)

// Bits 104-111: Maps
#define DEBUG_MAPS            DEBUG_BIT(104)  // Parent
#define DEBUG_MAPS_LOADING    DEBUG_BIT(105)  // Map file loading, tile directory parsing
#define DEBUG_MAPS_RENDERING  DEBUG_BIT(106)  // Map render pipeline, feature drawing, viewport
#define DEBUG_MAPS_PERF       DEBUG_BIT(107)  // Performance timing (render ms, tile I/O, cache, FPS)
// Bits 108-111: spare (Maps)

// Bits 112-119: Camera. All gated through DEBUG_CAMERA_*F macros with
// `DEBUG_CAMERA | DEBUG_CAMERA_<sub>`, so the parent toggle still works as
// a master switch and sub-flags refine *which* camera noise gets through.
#define DEBUG_CAMERA           DEBUG_BIT(112)  // Parent
#define DEBUG_CAMERA_LIFECYCLE DEBUG_BIT(113)  // initCamera(), stopCamera(), PWDN/RESET sequencing, GPIO state
#define DEBUG_CAMERA_CAPTURE   DEBUG_BIT(114)  // captureFrame(), JPEG validation, frame buffer, recovery path
#define DEBUG_CAMERA_SETTINGS  DEBUG_BIT(115)  // Runtime resolution / quality / sensor register changes
#define DEBUG_CAMERA_VIDEO     DEBUG_BIT(116)  // Video recording start/finalize, frame writing, encoder state
// Bits 117-119: spare (Camera)

// Bits 120-127: I2C bus. Bus enable/disable + sensor auto-start happen at
// runtime, not just boot, so each is independently silenceable.
#define DEBUG_I2C             DEBUG_BIT(120)  // Parent: bus operations, transactions, clock changes, mutex
#define DEBUG_I2C_BUS         DEBUG_BIT(121)  // [I2C] bus lifecycle, polling pause/resume, status bumps, raw transactions
#define DEBUG_I2C_DISCOVERY   DEBUG_BIT(122)  // [Discovery] / [I2C_REGISTRY] / [I2C_SENSORS] — probing, registration, scans
#define DEBUG_I2C_AUTOSTART   DEBUG_BIT(123)  // [AutoStart] sensor auto-start orchestration + per-sensor init results
// Bits 124-127: spare (I2C)

// ---- Words 2-3 (bits 128-255): sensors — one byte each ---------------------
// Uniform per-sensor layout: parent, then LIFECYCLE / POLLING / VALUES,
// then 4 spare bits. Macros gate on parent-OR-sub like every other family.
//   LIFECYCLE — init, connect/disconnect, recovery, error retries
//   POLLING   — poll/sample cadence, capture timing, FPS, frame events
//   VALUES    — parsed readings, value-change events, data processing

// Bits 128-135: GPS (PA1010D)
#define DEBUG_GPS             DEBUG_BIT(128)
#define DEBUG_GPS_LIFECYCLE   DEBUG_BIT(129)
#define DEBUG_GPS_POLLING     DEBUG_BIT(130)
#define DEBUG_GPS_VALUES      DEBUG_BIT(131)

// Bits 136-143: RTC (DS3231)
#define DEBUG_RTC             DEBUG_BIT(136)
#define DEBUG_RTC_LIFECYCLE   DEBUG_BIT(137)
#define DEBUG_RTC_POLLING     DEBUG_BIT(138)
#define DEBUG_RTC_VALUES      DEBUG_BIT(139)

// Bits 144-151: IMU (BNO055)
#define DEBUG_IMU             DEBUG_BIT(144)
#define DEBUG_IMU_LIFECYCLE   DEBUG_BIT(145)
#define DEBUG_IMU_POLLING     DEBUG_BIT(146)  // frame timing, cache operations
#define DEBUG_IMU_VALUES      DEBUG_BIT(147)  // data updates

// Bits 152-159: Thermal (MLX90640)
#define DEBUG_THERMAL           DEBUG_BIT(152)
#define DEBUG_THERMAL_LIFECYCLE DEBUG_BIT(153)
#define DEBUG_THERMAL_POLLING   DEBUG_BIT(154)  // frame timing, capture, FPS
#define DEBUG_THERMAL_VALUES    DEBUG_BIT(155)  // interpolation, processing

// Bits 160-167: ToF (VL53L4CX)
#define DEBUG_TOF             DEBUG_BIT(160)
#define DEBUG_TOF_LIFECYCLE   DEBUG_BIT(161)
#define DEBUG_TOF_POLLING     DEBUG_BIT(162)  // frame capture, object detection
#define DEBUG_TOF_VALUES      DEBUG_BIT(163)

// Bits 168-175: Gamepad (Seesaw) — the shared input-abstraction layer
#define DEBUG_INPUT           DEBUG_BIT(168)
#define DEBUG_INPUT_LIFECYCLE DEBUG_BIT(169)
#define DEBUG_INPUT_POLLING   DEBUG_BIT(170)  // frame timing, connection
#define DEBUG_INPUT_VALUES    DEBUG_BIT(171)  // button press/release events

// Bits 176-183: APDS (APDS9960)
#define DEBUG_APDS            DEBUG_BIT(176)
#define DEBUG_APDS_LIFECYCLE  DEBUG_BIT(177)
#define DEBUG_APDS_POLLING    DEBUG_BIT(178)  // frame timing, connection
#define DEBUG_APDS_VALUES     DEBUG_BIT(179)

// Bits 184-191: Presence (STHS34PF80)
#define DEBUG_PRESENCE           DEBUG_BIT(184)
#define DEBUG_PRESENCE_LIFECYCLE DEBUG_BIT(185)
#define DEBUG_PRESENCE_POLLING   DEBUG_BIT(186)
#define DEBUG_PRESENCE_VALUES    DEBUG_BIT(187)

// Bits 192-199: FM Radio (RDA5807)
#define DEBUG_FMRADIO           DEBUG_BIT(192)  // Parent: operations + I2C debugging
#define DEBUG_FMRADIO_LIFECYCLE DEBUG_BIT(193)
#define DEBUG_FMRADIO_POLLING   DEBUG_BIT(194)
#define DEBUG_FMRADIO_VALUES    DEBUG_BIT(195)

// Bits 200-207: Microphone
#define DEBUG_MICROPHONE      DEBUG_BIT(200)  // Parent
#define DEBUG_MIC_LIFECYCLE   DEBUG_BIT(201)
#define DEBUG_MIC_POLLING     DEBUG_BIT(202)
#define DEBUG_MIC_VALUES      DEBUG_BIT(203)

// Bits 208-215: ANO rotary encoder. Separate from DEBUG_INPUT* (which gates
// the shared input-abstraction layer) and from the seesaw gamepad's flags —
// these only affect the ANO driver's internal logs (init, polling, chord
// state machine, axis toggle).
#define DEBUG_ANO_ENCODER           DEBUG_BIT(208)
#define DEBUG_ANO_ENCODER_LIFECYCLE DEBUG_BIT(209)
#define DEBUG_ANO_ENCODER_POLLING   DEBUG_BIT(210)
#define DEBUG_ANO_ENCODER_VALUES    DEBUG_BIT(211)

// Bits 216-255: spare (five whole banks for future sensors/subsystems)

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

// Global output lane gates, using the MSG_ROUTE_* sink bits declared at the
// top of this header. (The former OUTPUT_* constants were a second, value-
// misaligned copy of the same sinks; unified 2026-07.) Persisted lanes come
// from named settings booleans in applySettings; FILE/BLE are runtime lanes.
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

// Yield inside a bursty debug-output loop until the free-list pool drains back
// above `minFree` slots (bounded by `maxWaitMs`). Prevents a producer loop from
// overflowing the pool and dropping the tail of large boot dumps. No-op when
// the pool is already healthy, so it's free when nothing is being emitted.
void debugQueueBackpressure(int minFree = 96, uint32_t maxWaitMs = 200);
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
#define DEBUG_NOTIFICATIONSF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_NOTIFICATIONS, fmt, ##__VA_ARGS__)

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
