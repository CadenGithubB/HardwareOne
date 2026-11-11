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
// cmd_exec Saturation Diagnostic
// ============================================================================

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

// File I/O helpers. Every one runs through the guarded VFS under the identity
// the caller passes; there is no identity-free overload. Internal callers pass
// VFS::systemAuth(<scope>, "<owner>.<reason>") with the narrowest scope that
// holds the file and, for the atomic writer, its ".tmp" sibling.
struct AuthContext;
// False when the file does not open or could not be read whole (out is then
// empty). False never means "no such file": probe with VFS::existsGuarded.
bool readText(const char* path, String& out, const AuthContext& ctx);
// Persistence helpers; the guarded VFS authorizes `ctx` on every open, rename
// and remove. True means the expected bytes were written, no write error was
// reported, and the file size matched after flush. The Arduino API cannot
// report flush/close failure. Direct writes may leave a truncated/partial
// destination when they fail.
bool writeText(const char* path, const String& content, const AuthContext& ctx);
// Writes a sibling .tmp and renames it only after checked completion. One FS
// lock covers writing, promotion and best-effort temp cleanup. Failure never
// falls back to deleting or rewriting the destination; backend rename and
// power-loss durability guarantees still apply.
bool writeTextAtomic(const char* path, const String& content, const AuthContext& ctx);

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
// functions below. Any other task gets false and an Error result.
bool executeCommand(AuthContext& ctx, const char* cmd, char* out, size_t outSize);

// `remote:<command>` reply. A bonded send that left answers
//   "Remote command sent: <redacted command> (reqId <msgId>)"
// msgId is the CMD frame's id; the peer's CMD_RESP echoes it as
// EspNowCmdResult::reqId, so a sender waiting for its answer can tell it from a
// reply to anyone else's remote: command. The echo is shortened first, so the
// id survives any result buffer that holds the prefix and the tag. The parser
// accepts only that shape and a non-zero id. (ENABLE_ESPNOW && ENABLE_BONDED_MODE)
void formatRemoteCommandSent(char* out, size_t outSize, const char* safeCommand,
                             uint32_t msgId);
bool parseRemoteCommandSentReqId(const char* reply, uint32_t* msgIdOut);

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

// True means queued, not executed. The optional callback runs on cmd_exec
// and must not block; its result pointer is valid only during that callback.
bool submitCommandAsync(const Command& cmd, ExecAsyncCallback callback,
                        void* userData);

// Firmware-internal work on cmd_exec, bypassing command dispatch. On
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
// Replaces every MAC address in the text (six hex pairs joined by ':' or '-')
// with "<mac>", so a Bluetooth, Wi-Fi or ESP-NOW address never reaches a log,
// the command audit or a lens trace. redactCmdForAudit applies it to every line.
String maskMacTokens(const String& text);

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

// Command arguments are parsed in one place each:
//   numbers  parseIntArg()  (System_ArgParse.h), failing with kCommandReplyNotANumber
//   on/off   parseBoolArg() below, or argIsOn()/argIsOff() for a single test,
//            failing with kCommandReplyNotOnOff
// A handler does not compare against "on"/"true"/"1" itself and does not call
// String::toInt() or atoi() on an argument (CONTRIBUTING.md, "Small text jobs have one owner each").
#include "System_ArgParse.h"
#include "System_CommandReply.h"

// Returns 1 for true (on/true/1/enable), 0 for false (off/false/0/disable), -1 if unrecognized.
// Case and surrounding spaces do not matter.
inline int parseBoolArg(const String& arg) {
  String a = arg;
  a.trim();
  a.toLowerCase();
  if (a == "on" || a == "true" || a == "1" || a == "enable") return 1;
  if (a == "off" || a == "false" || a == "0" || a == "disable") return 0;
  return -1;
}
inline bool argIsOn(const String& arg) { return parseBoolArg(arg) == 1; }
inline bool argIsOff(const String& arg) { return parseBoolArg(arg) == 0; }

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

String base64Decode(const String& input);

// ============================================================================
// URL Encoding/Decoding Utilities
// ============================================================================

String urlDecode(const String& s);
String extractFormField(const String& body, const String& key);

// JSON string text. The escaping rule and the fixed-buffer forms are in
// System_JsonText.h; these are the String forms built on it.
#include "System_JsonText.h"
String jsonEscape(const String& in);                   // escaped text, no quotes
void jsonAppendEscaped(String& out, const char* s);    // appends escaped text
String jsonQuote(const char* text);                    // the text as a quoted JSON string
inline String jsonQuote(const String& text) { return jsonQuote(text.c_str()); }

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

// Text for an esp_reset_reason_t value, both from one table in System_Utils.cpp.
// resetReasonName() is the stable lower-case form ("task_wdt", "other" beyond the
// table); resetReasonLabel() is the display form ("Task WDT", "Unknown" beyond it).
const char* resetReasonName(uint32_t reason);
const char* resetReasonLabel(uint32_t reason);

// Print breakdown of connected device libraries (returns total via outTotal)
void printConnectedDevicesLibraries(size_t& outTotal);

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
// MAC address text
// ============================================================================
// The formats, the buffer formatters, the comparison and the parser live in
// System_MacText.h (no Arduino types, so every module and host test can use
// them). Here are the String forms and the per-peer cache paths built on them.
#include "System_MacText.h"

// DISPLAY form as String (heap allocation; avoid in ISR / hot paths).
inline String macToDisplayStr(const uint8_t* mac) {
  char buf[18];
  macToDisplay(mac, buf, sizeof(buf));
  return String(buf);
}

// PATH TOKEN form as String.
inline String macToPathTokenStr(const uint8_t* mac) {
  char buf[13];
  macToPathToken(mac, buf);
  return String(buf);
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

bool ensureFileViewBuffers();
extern uint32_t gBootCounter;
extern char* gFileOutBuf;
extern char* gFileReadBuf;
extern uint32_t gNTPAnchorId;
extern const size_t kFileOutBufSize;
extern const size_t kFileReadBufSize;
bool serialCommandSecretSinkReady();
bool serialWriteCommandSecret(const char* text, size_t len);

#endif // SYSTEM_UTILS_H
