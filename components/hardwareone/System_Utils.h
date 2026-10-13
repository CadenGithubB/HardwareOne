/**
 * System Utilities Header
 * 
 * Shared utility functions used across multiple subsystems
 * Including the centralized command registry system
 */

#ifndef SYSTEM_UTILS_H
#define SYSTEM_UTILS_H

#include <Arduino.h>
#include "System_CommandTypes.h"

// Kept as a compatibility re-export for existing utility consumers. New
// command-table consumers should include System_CommandRegistry.h directly.
#include "System_CommandRegistry.h"

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

// Shared cmd_exec saturation diagnostic. The queue and BLE ingress pool have
// different producer tasks, so all accesses go through this atomic API rather
// than exposing a volatile counter that can lose concurrent increments.
extern "C" void cmdExecDropCountIncrement(void);
extern "C" uint32_t cmdExecDropCountLoad(void);

// ============================================================================
// Security Utilities
// ============================================================================

// Securely clear a String's internal buffer before releasing memory
// Prevents plaintext passwords from lingering in RAM after use
void secureClearString(String& s);

// Legacy subsystem imports retained for existing utility consumers.
#include "System_BuildConfig.h"
#if ENABLE_HTTP_SERVER
  #include "WebServer_Server.h"
#endif
#include "System_SensorStubs.h"
// Defined by System_Command.h; only needed by the audit-redaction overload.
struct CommandResolution;

// HTTP server control (lowercase 'h' matches implementation in .ino)
void startHttpServer();

// Reboot helpers. Route all intentional restarts through these so every reboot is
// attributable (durable REBOOT audit line) AND emits a typed SYSEVT_REBOOT on the
// next boot. recordRebootIntent() records only (for deferred/esp_timer sites);
// rebootDevice() records then flushes + restarts inline; rebootStashReason() is the
// RTC accessor (defined in HardwareOne.cpp with the other reset-state vars).
void rebootStashReason(const char* reason, const char* who, uint8_t source);
void recordRebootIntent(const char* reason, const char* auditDetail);
void rebootDevice(const char* reason, const char* auditDetail, uint32_t flushDelayMs);

// Note: BROADCAST_PRINTF is defined as a macro in debug_system.h, not a function

// File I/O functions
bool readText(const char* path, String& out);
// Trusted internal persistence helpers; callers own authorization. True means
// the expected bytes were written, no write error was reported, and the file
// size matched after flush. The Arduino API cannot report flush/close failure.
// Direct writes may leave a truncated/partial destination when they fail.
bool writeText(const char* path, const String& content);
// Writes a sibling .tmp and renames it only after checked completion. One FS
// lock covers writing, promotion and best-effort temp cleanup. Failure never
// falls back to deleting or rewriting the destination; backend rename and
// power-loss durability guarantees still apply.
bool writeTextAtomic(const char* path, const String& content);

// ============================================================================
// Diagnostics primitives — shared scaffolding for queue/saturation reporters
// ============================================================================
// The house instrumentation pattern (System_ESPNow_Tx stats, `debugbuffer`,
// [MEMSAMPLE]) kept re-rolling three tiny mechanisms; new reporters compose
// these instead. Existing HW-validated sites migrate opportunistically only.

// Rate gate: true at most once per intervalMs. The caller owns the stamp
// (a function-static at the call site). A zero stamp fires immediately —
// right for debug reporters, which should print as soon as their flag is
// switched on rather than waiting out a full interval.
inline bool everyMs(uint32_t* lastMs, uint32_t intervalMs) {
  const uint32_t now = millis();
  if (*lastMs != 0 && (now - *lastMs) < intervalMs) return false;
  *lastMs = now ? now : 1;
  return true;
}

// High-water mark: raise *hwm to value when higher. Best-effort under
// concurrency (volatile read/write, no lock) — the same tolerance the
// codebase already accepts for diagnostic counters (gDebugDropped et al.);
// a lost race costs one stale sample, never corruption.
inline void observeHwm(volatile uint32_t* hwm, uint32_t value) {
  if (value > *hwm) *hwm = value;
}

// Saturation label for a used/capacity pair — the `debugbuffer` ladder.
inline const char* saturationLabel(uint32_t used, uint32_t cap) {
  if (cap == 0) return "OK";
  const uint32_t pct = (used * 100U) / cap;
  if (pct > 90) return "CRITICAL";
  if (pct > 75) return "WARNING";
  if (pct > 50) return "Busy";
  return "OK";
}

// Settings persistence
// NOTE: Use writeSettingsJson() from .ino instead of saveUnifiedSettings()

// Command audit logging (always-on, lightweight)
void logCommandExecution(const AuthContext& ctx, const char* cmd, bool success, const char* result);

// ============================================================================
// Shared command execution (implemented in System_Utils.cpp)
// ============================================================================

// Low-level authorized dispatch. Only the executor task (cmd_exec, including
// work it runs through submitDeferredToCmdExec) and the early-boot path of
// submitAndExecuteSync call it: handlers share the debug buffer and static
// result buffers, so every other caller goes through the submission
// functions below.
bool executeCommand(AuthContext& ctx, const char* cmd, char* out, size_t outSize);

// Validate-only (dry-run) state of the command running on the calling task,
// read from that task's CommandContext. Another task's validate pass never
// makes this true, so output suppression and audit decisions stay per task.
bool currentCommandValidateOnly();

// Run a read-only status command as the internal system identity on the
// executor and copy its result into out (always NUL-terminated). For guest-
// readable status endpoints; mutations must never go through this helper.
bool executeInternalStatusCommand(const char* cmd, char* out, size_t outSize);

// Wait for a command result. The return value reports command success;
// completedOut distinguishes an executed failure from admission failure or
// timeout. A timed-out command may still finish on the executor task.
bool submitAndExecuteSync(const Command& cmd, String& out);
bool submitAndExecuteSync(const Command& cmd, String& out, bool* completedOut);

// True means queued, not executed. The optional callback runs on cmd_exec_task
// and must not block; its result pointer is valid only during that callback.
bool submitCommandAsync(const Command& cmd, ExecAsyncCallback callback,
                        void* userData);

// Firmware-internal work on cmd_exec_task, bypassing command dispatch. On
// success the callback owns arg's cleanup; on failure the caller retains it.
bool submitDeferredToCmdExec(ExecReq::DeferredFn fn, void* arg);

// Trusted firmware/boot actions only: submits with the system identity.
void runUnifiedSystemCommand(const String& argsInput);

// Common web command fields. Callers bind the admitted session epoch before
// submission and retain their interaction, validation, capture and batch choices.
CommandContext makeWebCommandContext(httpd_req_t* req, const AuthContext& auth);

// Execute a request/response command as this authenticated web session. The
// executor owns authorization and session checks; out receives the result.
bool executeUnifiedWebCommand(httpd_req_t* req, AuthContext& ctx,
                              const String& cmd, String& out);

// Audit / Redaction utilities
// Centralized redaction for audit logs and debug output
String redactCmdForAudit(const String& argsInput);
// Reuse a resolution already computed for this exact line. The public wrapper
// above remains the compatibility entrypoint for transports and UI surfaces.
String redactCmdForAudit(const String& argsInput,
                         const CommandResolution& resolution);
String redactOutputForLog(const String& output);

// CLI validation macro - returns "VALID" early when gCLIValidateOnly is set.
// gCLIValidateOnly mirrors the validateOnly flag of the command the executor
// is running (written only by HardwareOne.cpp commandExecTask and the
// early-boot path of submitAndExecuteSync); handlers run there, so it is
// their own task's state. Code that can run on any task reads
// currentCommandValidateOnly() instead.
// Keep the declaration at global namespace and qualify the reference: a
// block-scope `extern` expanded inside an anonymous-namespace command handler
// otherwise declares a different internal-linkage symbol and fails at link.
extern bool gCLIValidateOnly;
#define RETURN_VALID_IF_VALIDATE_CSTR() \
  do { \
    if (::gCLIValidateOnly) return "VALID"; \
  } while(0)

// Parse common boolean argument patterns used across CLI command handlers.
// Returns 1 for true (on/true/1/enable), 0 for false (off/false/0/disable), -1 if unrecognized.
inline int parseBoolArg(const String& arg) {
  String a = arg;
  a.trim();
  a.toLowerCase();
  if (a == "on" || a == "true" || a == "1" || a == "enable") return 1;
  if (a == "off" || a == "false" || a == "0" || a == "disable") return 0;
  return -1;
}

// Generic on/off CLI handler for a persisted bool setting. Collapses the
// 12-line on/off/true/false/1/0/setSetting/return-string template that
// every BOOL toggle command was repeating. `label` is a short prefix used
// in the response strings: "[BLE] Auto-start", "[G2] Auto-reconnect", etc.
//
// Behavior:
//   * Empty arg          → "<label>: enabled" or "<label>: disabled" (current state)
//   * "on"/"true"/"1"    → setSetting(field, true);  return "<label> enabled"
//   * "off"/"false"/"0"  → setSetting(field, false); return "<label> disabled"
//   * Anything else      → "<label>: invalid value (use on|off)"
//
// Result strings use a static buffer (single-threaded CLI assumption); a
// returned const char* is valid until the next call to this helper. If
// you need a stable copy, snprintf into your own buffer.
const char* settingBoolToggle(bool& field, const String& arg, const char* label);

// One-line CLI handler for a persisted bool. Use like:
//   BOOL_CMD(bleautostart, gSettings.bleAutoStart, "[BLE] Auto-start")
// expands to:
//   static const char* cmd_bleautostart(const String& a) {
//     RETURN_VALID_IF_VALIDATE_CSTR();
//     return settingBoolToggle(gSettings.bleAutoStart, a, "[BLE] Auto-start");
//   }
#define BOOL_CMD(cmdname, fieldExpr, label) \
  static const char* cmd_##cmdname(const String& a) { \
    RETURN_VALID_IF_VALIDATE_CSTR(); \
    return settingBoolToggle(fieldExpr, a, label); \
  }

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


// ============================================================================
// Serial Input Helpers
// ============================================================================

String waitForSerialInput(unsigned long timeoutMs);
String waitForSerialInputBlocking();

// ============================================================================
// Time Sync Functions
// ============================================================================

// Returns a ms-precision prefix like "[YYYY-MM-DD HH:MM:SS.mmm] | "
// Reads the wall clock per call (self-healing across any clock step).
// Writes empty string if epoch time invalid
void getTimestampPrefixMsCached(char* out, size_t outSize);

// ============================================================================
// Memory Reporting & System Diagnostics
// ============================================================================

// Shared system-info JSON for CLI, HTTP, SSE and migration callers; available
// without the HTTP server. Omit the I2C device list for compact CLI/MQTT replies.
void buildSystemInfoJson(JsonDocument& doc, bool includeDeviceList = true);

// Print breakdown of connected device libraries (returns total via outTotal)
void printConnectedDevicesLibraries(size_t& outTotal);

// Calculate total estimated memory usage for all sensor systems
size_t calculateSensorSystemMemory();

// System diagnostic commands - migrated from main .ino
void printMemoryReport();
const char* cmd_memreport(const String& argsInput);
const char* cmd_taskstats(const String& argsInput);
const char* cmd_perftop(const String& argsInput);

// ============================================================================
// Input Abstraction Layer
// ============================================================================
// Input handling is now defined in HAL_Input.h for hardware abstraction.
// This allows switching between different controller types at compile time
// by changing INPUT_TYPE in System_BuildConfig.h.
#include "HAL_Input.h"

// ============================================================================
// String / CLI Utilities  (inline — no .cpp dependency)
// ============================================================================

// Normalize a CLI argument in-place: trim whitespace then lowercase.
// Replaces the common two-line pattern `s.trim(); s.toLowerCase();`
// used throughout command handlers.
inline void normalizeCliArg(String& s) {
  s.trim();
  s.toLowerCase();
}

// ============================================================================
// Unified MAC Address API  (always available, independent of ESP-NOW)
// ============================================================================
//
// A 6-byte MAC renders to a string in exactly two forms across this codebase.
// Both are UPPER-case; which one you want is driven by the separator the
// destination needs — pick the function that names the destination:
//
//   DISPLAY     "AA:BB:CC:DD:EE:FF"  colon-separated, UPPER
//               → logs, OLED/web UI, devices.json, settings, CLI output.
//   PATH TOKEN  "AABBCCDDEEFF"       no separator,    UPPER
//               → filesystem paths (/system/espnow/peers/<TOKEN>/) AND
//                 MQTT client-id / topics / HomeAssistant entity ids.
//                 (HA matches MACs case-insensitively; topics, client-id and
//                  unique_id are arbitrary strings, so UPPER is fine there.)
//
// `macParse` is lenient (accepts ':' '-' ' ' separators or none, any case)
// and is the canonical inbound parser.
// ----------------------------------------------------------------------------

// DISPLAY form (colon-separated UPPER). buf must be >= 18 bytes.
inline void macToDisplay(const uint8_t* mac, char* buf, size_t bufSize) {
  snprintf(buf, bufSize, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// DISPLAY form as String (heap allocation; avoid in ISR / hot paths).
inline String macToDisplayStr(const uint8_t* mac) {
  char buf[18];
  macToDisplay(mac, buf, sizeof(buf));
  return String(buf);
}

// PATH TOKEN form (no separator UPPER). out must be >= 13 bytes.
inline void macToPathToken(const uint8_t* mac, char* out13) {
  snprintf(out13, 13, "%02X%02X%02X%02X%02X%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// PATH TOKEN form as String.
inline String macToPathTokenStr(const uint8_t* mac) {
  char buf[13];
  macToPathToken(mac, buf);
  return String(buf);
}

// Compare two 6-byte MAC addresses for equality.
inline bool macEquals(const uint8_t* a, const uint8_t* b) {
  return memcmp(a, b, 6) == 0;
}

// Per-peer cache namespace under /system/espnow/peers/<MAC>/. Two helpers:
//   peerCacheDir(mac, buf, len)        -> "/system/espnow/peers/<MAC>"
//   peerCachePath(mac, name, buf, len) -> "/system/espnow/peers/<MAC>/<name>"
// Single source of truth for the cache layout — used by both the bond peer
// cache (settings.json, schema.json) and any future per-peer cached file. MAC
// uses PATH TOKEN form (no separators, uppercase) for filesystem-friendliness.
// Returns the snprintf byte count (excluding NUL).
inline int peerCacheDir(const uint8_t* mac, char* out, size_t outSize) {
  char macStr[13];
  macToPathToken(mac, macStr);
  return snprintf(out, outSize, "/system/espnow/peers/%s", macStr);
}

inline int peerCachePath(const uint8_t* mac, const char* filename, char* out, size_t outSize) {
  char macStr[13];
  macToPathToken(mac, macStr);
  return snprintf(out, outSize, "/system/espnow/peers/%s/%s", macStr, filename ? filename : "");
}

// Lenient inbound parser. Accepts ':' '-' or ' ' separators or none; any
// case. Requires exactly 12 hex nibbles (two per byte). Returns true and
// fills mac[6] on success, false on any malformed input.
inline bool macParse(const char* s, uint8_t mac[6]) {
  if (!s) return false;
  auto nyb = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  int byteIdx = 0;
  int hi = -1;
  for (const char* p = s; *p && byteIdx < 6; ++p) {
    char c = *p;
    if (c == ':' || c == '-' || c == ' ') continue;  // skip separators
    int v = nyb(c);
    if (v < 0) return false;
    if (hi < 0) { hi = v; }
    else { mac[byteIdx++] = (uint8_t)((hi << 4) | v); hi = -1; }
  }
  return (byteIdx == 6 && hi < 0);
}

// ----------------------------------------------------------------------------
// Backward-compatible aliases for the historical names. Prefer the names
// above in new code; these delegate so existing call sites keep working.
inline void formatMacAddr(const uint8_t* mac, char* buf, size_t bufSize) {
  macToDisplay(mac, buf, bufSize);
}
inline String formatMacAddrStr(const uint8_t* mac) {
  return macToDisplayStr(mac);
}

#endif // SYSTEM_UTILS_H
