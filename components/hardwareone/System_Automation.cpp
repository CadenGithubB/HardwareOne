/**
 * Automation System - Separated from main .ino for compilation
 * 
 * This file contains all automation/scheduler related functionality
 * to reduce the main .ino file size below 1MB threshold.
 * 
 * REFACTORING NOTES (Dec 2025 - FULL MODERNIZATION):
 * - Replaced ALL manual JSON string parsing with ArduinoJson library
 * - Eliminated String concatenation entirely - all hot paths use stack buffers
 * - All conditional evaluation functions use const char* inputs/outputs
 * - Stack-based parsing with fixed-size buffers (256-512 bytes max)
 * - Direct C-string manipulation for case-insensitive comparisons
 * - Zero heap allocations in condition evaluation path
 * - ArduinoJson used only for JSON serialization/deserialization
 * 
 * MODERNIZATION COMPLETE:
 * ✅ computeNextRunTime() - const char* input, ArduinoJson parsing (thin wrapper around Trigger model)
 * ✅ evaluateCondition() - const char* input, stack-based parsing
 * ✅ validateConditionSyntax() - const char* input/output
 * ✅ executeConditionalCommand() - const char* input, minimal String usage
 * ✅ sanitizeAutomationsJson() - ArduinoJson for duplicate ID detection
 * ✅ updateAutomationNextAt() - ArduinoJson for JSON modification
 * 
 * REMAINING DEPENDENCIES (by design):
 * - Extern globals (gSettings, etc.) - shared system state
 * - Some String usage for ArduinoJson serialization output
 * - Command handlers still accept String& for compatibility with command registry
 */

#include "System_Automation.h"
#include "System_AutomationId.h"   // the id range; new ids are made there
#include "System_BuildConfig.h"

#if ENABLE_AUTOMATION

#include <ArduinoJson.h>
#include <atomic>
#include <memory>
#include <utility>
#include "freertos/semphr.h"
#include <FS.h>
#include <ctype.h>
#include <string.h>

#include "System_Command.h"
#include "System_Debug.h"
#include "System_Filesystem.h"
#include "System_VFS.h"
#include "System_MemUtil.h"
#include "System_Settings.h"
#include "System_User.h"
#include "System_AuthIdentity.h"  // currentAuthContext / currentExecUser / currentExecIsAdmin
#include "System_Utils.h"
#include "System_Mutex.h"          // SensorCacheGuard (was pulled in transitively before 71bcd2c)

#include "i2csensor_apds9960.h"
#include "i2csensor_mlx90640.h"
#include "i2csensor_vl53l4cx.h"

// --- Additional condition-variable sources (see evaluateCondition) ---
#include "System_Battery.h"        // BATTERY: valid battery snapshot
#include "System_Events.h"         // event triggers: ring drain + kind names
#include "System_Clock.h"          // NTP: Clock::isSynced()
#include "System_SelfDevice.h"     // SelfDevice::uptimeSeconds: the UPTIME condition sensor
#include "BLE_Server.h"             // BLE: isBLEConnected() (stub returns false when BT disabled)
#include "System_ESPNow.h"         // PEERS/bond/pairing: gMeshPeers, isBondModeOnline, espnowPairModeActive, gEspNow
#include "System_BondedPeer.h"     // BOND_PAIRED: BondedPeer::isPaired()
#include "System_LLMBackend.h"            // LLM: llmBackendStatus() (header self-guards on ENABLE_LLM_SOURCE_ONBOARD)
#include "System_CommandRegistry.h"
#if ENABLE_WIFI
#include <WiFi.h>                  // WIFI / RSSI: WiFi.isConnected() / WiFi.RSSI()
#endif
#include "i2csensor_pa1010d.h"     // GPS / SPEED / SATS / WP_DIST: gpsCacheSnapshot()
#if ENABLE_MAPS
#include "System_Maps.h"           // WP_DIST:<name>: WaypointManager::getDistanceBearingToName()
#endif

// Command types from shared header
#include "System_CommandTypes.h"

static String redactAutomationCommandForAudit(const String& command);

// What automationStepDone needs about a queued step once the run that queued
// it has moved on: the automation's name and the redacted command.
struct AutomationStepTrace {
  char automation[64];
  char command[160];
};

// How `automation run` answers a run that its own condition held back. The
// direct caller gets it as a failure (docs/APP_JSON_CONTRACT.md).
static const char kAutomationConditionSkipLead[] = "Error: Automation not run, its condition is not met";

static bool automationReplyIsConditionSkip(const char* reply) {
  return reply && strncmp(reply, kAutomationConditionSkipLead, sizeof(kAutomationConditionSkipLead) - 1) == 0;
}

// Completion of a queued automation step (runs on cmd_exec). A step that
// failed leaves a FAILED line in the automation log and posts
// SYSEVT_AUTOMATION_ACTION_DROPPED with the reason, at most one event per 5 s.
// A step `automation run id=N` whose target is held back by its condition is
// not a dropped action: a closed gate is the normal state of such a chain, and
// the target's run writes its own AUTO_SKIP line when the automation log is on.
static void automationStepDone(bool ok, const char* result, void* userData) {
  AutomationStepTrace* trace = static_cast<AutomationStepTrace*>(userData);
  if (!trace) return;
  if (!ok && !automationReplyIsConditionSkip(result)) {
    const String safeReply = redactOutputForLog(String(result ? result : ""));
    const char* reply = safeReply.c_str();
    const int replyLine = (int)strcspn(reply, "\r\n");
    char line[360];
    snprintf(line, sizeof(line), "[%s] %s: %.*s", trace->automation, trace->command,
             replyLine > 120 ? 120 : replyLine, reply);
    appendAutoLogEntry("FAILED", line);
    static uint32_t sLastFailEventMs = 0;
    const uint32_t nowMs = millis();
    if (sLastFailEventMs == 0 || (nowMs - sLastFailEventMs) >= 5000) {
      const char* reason = commandReplyReason(reply);
      const int reasonLine = (int)strcspn(reason, "\r\n");
      char detail[SYSEVT_DETAIL_LEN];
      snprintf(detail, sizeof(detail), "failed: %.*s", reasonLine, reason);
      systemEventPost(SYSEVT_AUTOMATION_ACTION_DROPPED, trace->automation, detail);
      sLastFailEventMs = nowMs;
    }
  }
  ps_free(trace);
}

// Queue an automation sub-command through the FreeRTOS command queue (async, non-blocking).
// This avoids deadlock when already on cmd_exec task and avoids blocking the main loop.
// `owner` is the automation's createdBy user — stamped into cmd.ctx.auth so VFS permission
// checks and audit logs attribute the work to the correct principal on cmd_exec.
// `autoName` is the automation's display name — stamped into ctx.automationName so
// executeCommand can write COMMAND/OUTPUT autolog entries attributed to this automation,
// with no race against the scheduler advancing to the next automation before the command runs.
// Returns false when the step was not submitted (executor queue unavailable, or
// the command is over the input limit).
static bool queueAutomationSubCommand(const char* cmd, const char* owner, const char* autoName = nullptr) {
  const String safeCmd = redactAutomationCommandForAudit(
      String(cmd ? cmd : ""));
  Command uc;
  uc.line = cmd;
  uc.ctx.origin = ORIGIN_SYSTEM;
  uc.ctx.auth.transport = SOURCE_INTERNAL;
  uc.ctx.auth.user = owner ? owner : "";
  DEBUG_AUTOMATIONSF("[autos queue] Queueing cmd='%s' user='%s' automation='%s'",
         safeCmd.c_str(), owner ? owner : "", autoName ? autoName : "");
  uc.ctx.auth.ip = "";
  uc.ctx.auth.path = "/automation";
  uc.ctx.auth.sid = "";
  uc.ctx.auth.opaque = nullptr;
  uc.ctx.id = (uint32_t)millis();
  uc.ctx.timestampMs = (uint32_t)millis();
  uc.ctx.outputMask = MSG_ROUTE_SERIAL | MSG_ROUTE_WEB | MSG_ROUTE_FILE;
  uc.ctx.validateOnly = false;
  uc.ctx.replyHandle = nullptr;
  uc.ctx.httpReq = nullptr;
  if (autoName && autoName[0]) {
    strncpy(uc.ctx.automationName, autoName, sizeof(uc.ctx.automationName) - 1);
    uc.ctx.automationName[sizeof(uc.ctx.automationName) - 1] = '\0';
  }
  const bool inputLengthAccepted =
      commandInputLengthAccepted(uc.line.length());
  // The step's own outcome comes back through automationStepDone. Without the
  // trace copy the step still runs; only its failure line is lost.
  AutomationStepTrace* trace = static_cast<AutomationStepTrace*>(
      ps_alloc(sizeof(AutomationStepTrace), AllocPref::PreferPsram, "auto.step"));
  if (trace) {
    snprintf(trace->automation, sizeof(trace->automation), "%s",
             (autoName && autoName[0]) ? autoName : "?");
    snprintf(trace->command, sizeof(trace->command), "%s", safeCmd.c_str());
  }
  if (!submitCommandAsync(uc, trace ? automationStepDone : nullptr, trace)) {
    if (trace) ps_free(trace);
    const char* dropReason = inputLengthAccepted
                                 ? "exec queue unavailable"
                                 : "command exceeds input limit";
    DEBUG_AUTOMATIONSF("[autos] DROPPED sub-command (%s): %s",
           dropReason, safeCmd.c_str());
    // Durable: a scheduled automation silently half-ran — one of its commands
    // never reached cmd_exec (queue unavailable or over the shared input
    // ceiling). Attribute to the automation so it's
    // clear which scheduled run was truncated. Rate-limited: one automation
    // dispatches its whole command list at once and a wedged cmd_exec (or a
    // sub-second interval trigger) can drop many per pass — cap to one durable
    // line per 5 s and fold the suppressed count into it so a flood can't churn
    // system-events.log.
    static uint32_t sLastDropLogMs = 0;
    static uint32_t sDropsSuppressed = 0;
    uint32_t nowMs = millis();
    if (sLastDropLogMs == 0 || (nowMs - sLastDropLogMs) >= 5000) {
      if (sDropsSuppressed > 0) {
        logSystemEvent("AUTO", "sub-command DROPPED (%s) in automation '%s': %s (+%lu more since last)",
                       dropReason,
                       (autoName && autoName[0]) ? autoName : "?", safeCmd.c_str(),
                       (unsigned long)sDropsSuppressed);
      } else {
        logSystemEvent("AUTO", "sub-command DROPPED (%s) in automation '%s': %s",
                       dropReason,
                       (autoName && autoName[0]) ? autoName : "?", safeCmd.c_str());
      }
      // SYSTEM-EVENT: a scheduled automation half-ran — this action never
      // reached cmd_exec. Share the durable log's 5 s throttle so repeated
      // failures cannot churn system-events; fold suppressed drops into detail.
      char dropDetail[80];
      if (sDropsSuppressed > 0) {
        snprintf(dropDetail, sizeof(dropDetail), "%s (+%lu more)", dropReason,
                 (unsigned long)sDropsSuppressed);
      } else {
        snprintf(dropDetail, sizeof(dropDetail), "%s", dropReason);
      }
      systemEventPost(SYSEVT_AUTOMATION_ACTION_DROPPED,
                      (autoName && autoName[0]) ? autoName : "?", dropDetail);
      sLastDropLogMs = nowMs;
      sDropsSuppressed = 0;
    } else {
      sDropsSuppressed++;
    }
    return false;
  }
  DEBUG_AUTOMATIONSF("[autos] Queued sub-command: %s", safeCmd.c_str());
  return true;
}

// Locate a whole-word conditional keyword outside quoted text. Automation
// conditions and commands are user-controlled, so a quoted value containing
// " THEN " / " ELSE " must not shift the branch boundary used for redaction.
static int findAutomationKeywordOutsideQuotes(const String& line,
                                              const char* keyword,
                                              int from,
                                              int* outEnd = nullptr) {
  if (!keyword || !keyword[0]) return -1;
  const int keyLen = (int)strlen(keyword);
  bool quoted = false;
  for (int i = from < 0 ? 0 : from; i + keyLen <= (int)line.length(); ++i) {
    const char ch = line[i];
    if (ch == '"') {
      quoted = !quoted;
      continue;
    }
    if (quoted) continue;
    if (i > 0 && !isspace(static_cast<unsigned char>(line[i - 1]))) continue;
    int k = 0;
    for (; k < keyLen; ++k) {
      char a = line[i + k];
      char b = keyword[k];
      if (a >= 'a' && a <= 'z') a = (char)(a - ('a' - 'A'));
      if (b >= 'a' && b <= 'z') b = (char)(b - ('a' - 'A'));
      if (a != b) break;
    }
    if (k != keyLen) continue;
    const int end = i + keyLen;
    if (end < (int)line.length() &&
        !isspace(static_cast<unsigned char>(line[end]))) continue;
    if (outEnd) *outEnd = end;
    return i;
  }
  return -1;
}

static String redactAutomationCommandForAuditImpl(const String& command,
                                                   unsigned depth) {
  CommandArgs outer(command);
  // automationadd and validate-conditions are removed names; a stale nested
  // line that still uses them is masked all the same.
  if (outer.count() > 0 &&
      (outer.arg(0).equalsIgnoreCase("automationadd") ||
       outer.arg(0).equalsIgnoreCase("validateconditions") ||
       outer.arg(0).equalsIgnoreCase("validate-conditions"))) {
    return outer.arg(0) + " ***";
  }
  if (outer.count() > 1 && outer.arg(0).equalsIgnoreCase("automation") &&
      outer.arg(1).equalsIgnoreCase("add")) {
    return outer.arg(0) + " " + outer.arg(1) + " ***";
  }

  int verbStart = 0;
  while (verbStart < (int)command.length() &&
         isspace(static_cast<unsigned char>(command[verbStart]))) verbStart++;
  const bool conditional =
      verbStart + 2 <= (int)command.length() &&
      (command[verbStart] == 'I' || command[verbStart] == 'i') &&
      (command[verbStart + 1] == 'F' || command[verbStart + 1] == 'f') &&
      verbStart + 2 < (int)command.length() &&
      isspace(static_cast<unsigned char>(command[verbStart + 2]));
  if (!conditional) return redactCmdForAudit(command);

  int thenEnd = 0;
  const int thenStart =
      findAutomationKeywordOutsideQuotes(command, "THEN", verbStart + 2,
                                         &thenEnd);
  if (thenStart < 0) return redactCmdForAudit(command);
  // A deliberately bounded recursion prevents a malicious stored automation
  // from turning a diagnostic/JSON request into unbounded stack consumption.
  if (depth >= 4) return command.substring(0, thenEnd) + " ***";

  int elseEnd = 0;
  const int elseStart =
      findAutomationKeywordOutsideQuotes(command, "ELSE", thenEnd, &elseEnd);
  String thenCommand = command.substring(
      thenEnd, elseStart >= 0 ? elseStart : (int)command.length());
  thenCommand.trim();

  String safe = command.substring(0, thenEnd);
  safe += " ";
  safe += redactAutomationCommandForAuditImpl(thenCommand, depth + 1);
  if (elseStart >= 0) {
    String elseCommand = command.substring(elseEnd);
    elseCommand.trim();
    safe += " ELSE ";
    safe += redactAutomationCommandForAuditImpl(elseCommand, depth + 1);
  }
  return safe;
}

static String redactAutomationCommandForAudit(const String& command) {
  return redactAutomationCommandForAuditImpl(command, 0);
}

static void automationApplyRuntimeSchedule(JsonObject automation);

String redactAutomationsJsonForResponse(const String& json) {
  PSRAM_JSON_DOC(doc);
  if (deserializeJson(doc, json)) return String();

  auto redactObject = [](JsonObject obj) {
    JsonArray commands = obj["commands"].as<JsonArray>();
    if (!commands.isNull()) {
      for (JsonVariant command : commands) {
        if (!command.is<const char*>()) continue;
        command.set(redactAutomationCommandForAudit(
            String(command.as<const char*>())));
      }
    }
    if (obj["command"].is<const char*>()) {
      obj["command"] = redactAutomationCommandForAudit(
          String(obj["command"].as<const char*>()));
    }
    // A client sees the next run the scheduler will use.
    automationApplyRuntimeSchedule(obj);
  };

  if (doc["automations"].is<JsonArray>()) {
    for (JsonObject automation : doc["automations"].as<JsonArray>()) {
      redactObject(automation);
    }
  } else if (doc.is<JsonArray>()) {
    for (JsonObject automation : doc.as<JsonArray>()) redactObject(automation);
  } else if (doc.is<JsonObject>()) {
    redactObject(doc.as<JsonObject>());
  } else {
    return String();
  }

  String safeJson;
  serializeJson(doc, safeJson);
  return safeJson;
}

// Helper: check if executeConditionalCommand result is an internal status (not user-facing output)
static bool isAutoInternalResult(const char* r) {
  if (!r || r[0] == '\0') return true;
  if (strcmp(r, "VALID") == 0) return true;
  if (strcmp(r, "Conditional command completed") == 0) return true;
  if (strstr(r, "queued") != nullptr) return true;  // "Command queued", "Conditional THEN queued", etc.
  if (strcmp(r, "Command executed") == 0) return true;
  return false;
}

// Automation log ownership
// Filesystem operations serialize start/stop/append, including callers already
// holding the FS lock. The private state lock only publishes/copies status; it
// is never held across VFS or diagnostic calls. Lock order is FS -> state.
namespace {
struct AutomationLogSession {
  String filename;
  AuthContext owner{};
};
struct AutomationLogStatus {
  bool active;
  String filename;
};
enum class AutomationLogStopResult { Inactive, Stopped, WriteFailed, Unavailable };
// What a start did to a session that was already active.
struct AutomationLogReplacement {
  bool replaced = false;  // an active session was displaced
  bool marked = false;    // its file received the LOG_STOP marker
  String filename;        // the displaced file
};

AutomationLogSession sAutomationLogSession;
std::atomic_bool sAutomationLogActive{false};
StaticSemaphore_t sAutomationLogStateStorage;
SemaphoreHandle_t sAutomationLogStateMutex =
    xSemaphoreCreateMutexStatic(&sAutomationLogStateStorage);

class AutomationLogStateGuard {
public:
  AutomationLogStateGuard() { xSemaphoreTake(sAutomationLogStateMutex, portMAX_DELAY); }
  ~AutomationLogStateGuard() { xSemaphoreGive(sAutomationLogStateMutex); }
  AutomationLogStateGuard(const AutomationLogStateGuard&) = delete;
  AutomationLogStateGuard& operator=(const AutomationLogStateGuard&) = delete;
};

AutomationLogStatus automationLogStatus() {
  AutomationLogStateGuard guard;
  return {sAutomationLogActive.load(), sAutomationLogSession.filename};
}

// Caller holds the FS lock for the whole write and any session transition.
// Keep the starter's owning identity fields: permission checks consult the
// current user roster on every write, even when a different task appends.
// Do not substitute system identity or the appending task's current identity.
bool writeAutomationLogEntry(const AutomationLogSession& session,
                             const char* type, const String& message) {
  if (!filesystemReady || !type || session.filename.length() == 0) return false;
  char tsPrefix[32];
  getTimestampPrefixMsCached(tsPrefix, sizeof(tsPrefix));
  const size_t expected = strlen(tsPrefix) + strlen(type) + 3 + message.length() + 1;
  String line;
  if (!line.reserve(expected) || !line.concat(tsPrefix) || !line.concat(type) ||
      !line.concat(" | ") || !line.concat(message.c_str()) || !line.concat("\n")) return false;

  // Overflow may add "/sd/". Keep the complete path rather than silently
  // truncating it into a different destination.
  const size_t pathSize = session.filename.length() + 5;
  std::unique_ptr<char, decltype(&free)> dest(
      static_cast<char*>(ps_alloc(pathSize, AllocPref::PreferPsram, "automation.log.path")), &free);
  if (!dest) return false;
  if (VFS::resolveLogPath(session.filename.c_str(), line.length() + 512, dest.get(), pathSize) ==
      VFS::LogTier::Refused) {
    // G-39: LittleFS is below the 100 KB floor and no card takes the
    // overflow; the entry would spend the reserve automations.json and
    // settings saves depend on. Dropped and counted like a capped log line.
    VFS::noteLogReserveDrop();
    return false;
  }
  String destStr(dest.get());
  if (destStr.length() != strlen(dest.get())) return false;
  const int lastSlash = destStr.lastIndexOf('/');
  if (lastSlash > 0) {
    const String dir = destStr.substring(0, lastSlash);
    if (dir.length() != static_cast<size_t>(lastSlash)) return false;
    if (!VFS::existsGuarded(dir, session.owner) && !VFS::mkdirGuarded(dir, session.owner)) return false;
  }
  File file = VFS::openGuarded(destStr, "a", session.owner);
  if (!file) return false;
  if (file.isDirectory()) {
    file.close();
    return false;
  }
  const size_t previousBytes = file.size();
  const size_t written = file.print(line);
  file.flush();
  const bool ok = written == expected && !file.getWriteError() &&
                 file.size() == previousBytes + expected;
  file.close();
  return ok;
}

// Starts a session or replaces the active one. The new LOG_START is written
// first, so a failed start leaves the active session untouched; a displaced
// session's file then gets a LOG_STOP naming the new file.
bool startAutomationLog(const String& filename, const AuthContext& owner,
                        AutomationLogReplacement* replacement = nullptr) {
  if (!filesystemReady || filename.length() == 0) return false;
  AutomationLogSession candidate{filename, owner};
  // Request pointers/session tokens do not belong in a long-lived writer.
  candidate.owner.opaque = nullptr;
  candidate.owner.sid = "";
  // Fail closed if an owning String copy failed, especially a path scope.
  if (candidate.filename != filename || candidate.owner.user != owner.user ||
      candidate.owner.scope != owner.scope || candidate.owner.path != owner.path ||
      candidate.owner.ip != owner.ip) return false;

  FsLockGuard fsGuard("automation.log.start");
  if (!isFsLockedByCurrentTask()) return false;
  if (!writeAutomationLogEntry(candidate, "LOG_START", "Automation logging started")) return false;
  // Sessions are published only under the FS lock held here, so the active
  // one cannot change before the swap below.
  if (sAutomationLogActive.load()) {
    String note("Automation logging replaced by ");
    if (!note.concat(candidate.filename)) note = "Automation logging replaced";
    const bool marked = writeAutomationLogEntry(sAutomationLogSession, "LOG_STOP", note);
    if (replacement) {
      replacement->replaced = true;
      replacement->marked = marked;
      replacement->filename = sAutomationLogSession.filename;
    }
  }
  {
    AutomationLogStateGuard guard;
    sAutomationLogSession = std::move(candidate);
    sAutomationLogActive.store(true);
  }
  return true;
}

AutomationLogStopResult stopAutomationLog(String& filename) {
  FsLockGuard fsGuard("automation.log.stop");
  if (!isFsLockedByCurrentTask()) return AutomationLogStopResult::Unavailable;
  if (!sAutomationLogActive.load()) return AutomationLogStopResult::Inactive;
  filename = sAutomationLogSession.filename;
  const bool written = writeAutomationLogEntry(
      sAutomationLogSession, "LOG_STOP", "Automation logging stopped");
  {
    AutomationLogStateGuard guard;
    sAutomationLogActive.store(false);
    sAutomationLogSession = AutomationLogSession{};
  }
  // Stop always deactivates; a missing final marker is reported separately.
  return written ? AutomationLogStopResult::Stopped : AutomationLogStopResult::WriteFailed;
}
}  // namespace

bool automationLogActive() {
  return sAutomationLogActive.load();
}

bool appendAutoLogEntry(const char* type, const String& message) {
  if (!automationLogActive() || !filesystemReady) return false;
  FsLockGuard fsGuard("automation.log.append");
  if (!isFsLockedByCurrentTask() || !sAutomationLogActive.load()) return false;
  return writeAutomationLogEntry(sAutomationLogSession, type, message);
}

// Forward declarations for functions implemented in this file
bool updateAutomationNextAt(long automationId, time_t newNextAt);
time_t computeNextRunTime(const char* automationJson, time_t fromTime);
// Unified post-fire helper (Trigger model defined later in this file). A
// scheduler pass passes notify=false: its trailing cache rebuild reads the file.
static void rescheduleAfterFire(long id, const char* automationJson, time_t firedAt, bool notify);
static bool automationSeedFirstRuns(long id, const char* automationJson, time_t now);
// Force-recompute every CLOCK trigger of every enabled automation in one
// read/modify/write. Defined below the Trigger type; declared here for
// cmd_automation's "recompute" subcommand. Returns triggers rewritten.
static int automationRecomputeAllClockTriggers(time_t now, int* outSkipped);
const char* executeConditionalCommand(const char* command, const char* owner, const char* autoName = nullptr);
const char* evaluateConditionalChain(const char* chainStr, char* outBuf, size_t outBufSize);
bool evaluateCondition(const char* condition);
const char* validateConditionalHierarchy(const char* conditions);
const char* validateConditionSyntax(const char* condition);
const char* validateConditionalChain(const char* chainStr);
const char* validateConditionalCommand(const char* command);
bool automationIdExistsInJson(const String& json, unsigned long id);
static int automationIdPosInJson(const String& json, unsigned long id);
// jsonEscape now provided by system_utils.h
void findAutomationsArrayBounds(const String& json, int& arrStart, int& arrEnd);

// DEBUG flags and RETURN_VALID_IF_VALIDATE_CSTR are defined centrally in debug_system.h
// and system_utils.h to keep logging behavior consistent across modules.
// BROADCAST_PRINTF is also provided by debug_system.h.

// Filesystem lock helpers (defined in .ino) - now in automation_system.h

// RAII filesystem lock guard (FsLockGuard) + SensorCacheGuard now live in
// System_Mutex.h (consolidated by 71bcd2c); the stale local copy was removed.

// Forward declarations for internal functions
static bool extractJsonString(const char* json, const char* key, char* out, size_t outSize);

// Helper: extract JSON string value by key from C-string (simple parser)
static bool __attribute__((unused)) extractJsonString(const char* json, const char* key, char* out, size_t outSize) {
  out[0] = '\0';
  const char* keyPos = strstr(json, key);
  if (!keyPos) return false;

  const char* colon = strchr(keyPos, ':');
  if (!colon) return false;

  const char* q1 = strchr(colon, '"');
  if (!q1) return false;
  q1++;  // Skip opening quote

  const char* q2 = strchr(q1, '"');
  if (!q2) return false;

  size_t len = q2 - q1;
  if (len >= outSize) len = outSize - 1;
  strncpy(out, q1, len);
  out[len] = '\0';
  return true;
}

// Find the closing brace of a JSON object starting at objStart, handling nested objects/arrays
static int findJsonObjectEnd(const String& json, int objStart) {
  int depth = 0;
  bool inStr = false;
  int len = (int)json.length();
  for (int i = objStart; i < len; i++) {
    char c = json[i];
    if (c == '"' && (i == 0 || json[i - 1] != '\\')) inStr = !inStr;
    if (!inStr) {
      if (c == '{') depth++;
      else if (c == '}') {
        depth--;
        if (depth == 0) return i;
      }
    }
  }
  return -1;
}

// Helper: Find automation array bounds in JSON
void findAutomationsArrayBounds(const String& json, int& arrStart, int& arrEnd) {
  arrStart = -1;
  arrEnd = -1;
  int pos = json.indexOf("\"automations\"");
  if (pos < 0) return;
  int bracket = json.indexOf('[', pos);
  if (bracket < 0) return;
  arrStart = bracket;
  int depth = 0;
  for (int i = bracket; i < (int)json.length(); ++i) {
    if (json[i] == '[') depth++;
    else if (json[i] == ']') {
      depth--;
      if (depth == 0) {
        arrEnd = i;
        return;
      }
    }
  }
}

// The offset of `"id": <id>` in the stored automations text, or -1. The number
// has to end there: id 12 is not found inside 123456.
static int automationIdPosInJson(const String& json, unsigned long id) {
  char needle[32];
  const int needleLen = snprintf(needle, sizeof(needle), "\"id\": %lu", id);
  const char* text = json.c_str();
  for (const char* hit = strstr(text, needle); hit; hit = strstr(hit + 1, needle)) {
    const char next = hit[needleLen];
    if (next < '0' || next > '9') return (int)(hit - text);
  }
  return -1;
}

// Helper: Check if automation ID exists in JSON (stack-based)
bool automationIdExistsInJson(const String& json, unsigned long id) {
  return automationIdPosInJson(json, id) >= 0;
}

// Sanitize the IDs in the automations array using ArduinoJson: a duplicate,
// and an id above kAutomationIdMax (which no reader can take, see
// System_AutomationId.h), is replaced by a new unique id. `counts`, when
// given, says how many of each it replaced (zero when nothing changed).
bool sanitizeAutomationsJson(String& jsonRef, AutomationSanitizeCounts* counts) {
  if (counts) *counts = AutomationSanitizeCounts{};
  // Use ArduinoJson for proper parsing
  PSRAM_JSON_DOC(doc);
  DeserializationError error = deserializeJson(doc, jsonRef);
  if (error) {
    DEBUG_AUTOMATIONSF("[sanitize] JSON parse error: %s", error.c_str());
    return false;
  }
  
  JsonArray automations = doc["automations"].as<JsonArray>();
  if (automations.isNull()) {
    return false;
  }
  
  // Track seen IDs off-stack. This sanitizer can run from schedulerTickMinute
  // on the custom main task after a duplicate is detected; reserving 2 KB in
  // that already-deep call chain can exceed the measured main-stack margin on
  // lean headless builds. The scan is not an ISR/cache-off path, so the normal
  // PSRAM-preferred allocator is appropriate and still falls back on boards
  // without external RAM.
  const int kMax = 512;
  unsigned long* seen = static_cast<unsigned long*>(
      ps_alloc(kMax * sizeof(unsigned long), AllocPref::PreferPsram,
               "auto.sanitize.seen"));
  if (!seen) {
    ERROR_SYSTEMF("[sanitize] Failed to allocate duplicate-ID scratch (%u bytes)",
                  (unsigned)(kMax * sizeof(unsigned long)));
    return false;
  }
  int seenCount = 0;
  bool changed = false;
  
  for (JsonObject automation : automations) {
    if (!automation["id"].is<unsigned long>()) continue;
    unsigned long idVal = automation["id"].as<unsigned long>();
    
    // Check for duplicate
    bool dup = false;
    for (int k = 0; k < seenCount; ++k) {
      if (seen[k] == idVal) {
        dup = true;
        break;
      }
    }
    
    if (!dup && idVal <= kAutomationIdMax) {
      if (seenCount < kMax) seen[seenCount++] = idVal;
      continue;
    }
    
    // Duplicate or out-of-range id found: generate a new unique ID
    unsigned long newId = automationIdSeed((uint32_t)millis());
    int guard = 0;
    
    // Check against already seen IDs in this pass
    auto idExists = [&](unsigned long testId) {
      for (int k = 0; k < seenCount; ++k) {
        if (seen[k] == testId) return true;
      }
      return false;
    };
    
    while (idExists(newId) && guard < 100) {
      newId = automationIdAdvance((uint32_t)newId, 1 + (uint32_t)random(1, 100000));
      guard++;
    }
    
    automation["id"] = newId;
    if (seenCount < kMax) seen[seenCount++] = newId;
    changed = true;
    if (counts) {
      if (dup) counts->duplicates++;
      else counts->outOfRange++;
    }
    
    DEBUG_AUTOMATIONSF("[sanitize] Replaced %s id %lu with %lu", dup ? "duplicate" : "out-of-range", idVal, newId);
  }
  
  if (changed) {
    // Serialize back to string
    jsonRef = "";
    serializeJsonPretty(doc, jsonRef);
  }

  ps_free(seen);
  return changed;
}

// ============================================================================
// automations.json format version
// ============================================================================
// Every read checks the top-level "version" against kAutomationsFileVersion.
// No "version" (older files) or a version up to this one loads as before: the
// reader has never branched on the number, and version 1 files share the
// "automations" array (an entry without a "triggers" array has never fired).
// A newer version is refused: nothing in it is loaded or run, and nothing is
// changed and written back from it, so the newer firmware finds its file as
// it left it. A malformed file or a non-numeric version is left to the
// callers' own parse checks, as before.

// The file's version when it is newer than this firmware reads, else 0.
static uint32_t automationsNewerFileVersion(const String& json) {
  PSRAM_JSON_DOC(filter);
  filter["version"] = true;
  PSRAM_JSON_DOC(doc);
  if (deserializeJson(doc, json, DeserializationOption::Filter(filter))) return 0;
  JsonVariantConst version = doc["version"];
  if (!version.is<double>()) return 0;
  const double n = version.as<double>();
  if (!(n > (double)kAutomationsFileVersion)) return 0;
  return n < 4.0e9 && (uint32_t)n > kAutomationsFileVersion ? (uint32_t)n
                                                            : kAutomationsFileVersion + 1;
}

// The newer version the last read refused (0: the last read was not refused).
// Commands name it in their error. The console line is printed once, again
// only after a read finds another version.
static std::atomic<uint32_t> sAutomationsRefusedVersion{0};

// Missing: a probe found no file. Unreadable: the file is there and could not
// be read (open failure, or no memory for its text); it is never taken for an
// absent one, so nothing is seeded over it.
enum class AutomationsRead : uint8_t { Ok, Missing, Unreadable, Refused };

static AutomationsRead readAutomationsFile(String& json, uint32_t* newerVersion = nullptr) {
  if (newerVersion) *newerVersion = 0;
  const auto auth = VFS::systemAuth(VFS::Scopes::SYSTEM_STATE, "automation.state");
  if (!readText(AUTOMATIONS_JSON_FILE, json, auth)) {
    sAutomationsRefusedVersion = 0;
    return VFS::existsGuarded(AUTOMATIONS_JSON_FILE, auth) ? AutomationsRead::Unreadable
                                                           : AutomationsRead::Missing;
  }
  const uint32_t newer = automationsNewerFileVersion(json);
  const uint32_t before = sAutomationsRefusedVersion.exchange(newer);
  if (newerVersion) *newerVersion = newer;
  if (!newer) return AutomationsRead::Ok;
  if (before != newer) {
    ERROR_SYSTEMF("[automations] %s is format %lu, newer than this firmware reads (%lu): "
                  "automations not loaded, file left unchanged",
                  AUTOMATIONS_JSON_FILE, (unsigned long)newer,
                  (unsigned long)kAutomationsFileVersion);
  }
  json = "";
  return AutomationsRead::Refused;
}

bool readAutomationsJson(String& json) {
  return readAutomationsFile(json) == AutomationsRead::Ok;
}

bool readAutomationsJson(String& json, uint32_t* newerVersion) {
  return readAutomationsFile(json, newerVersion) == AutomationsRead::Ok;
}

String automationsEmptyStoreJson() {
  char text[64];
  snprintf(text, sizeof(text), "{\n  \"version\": %lu,\n  \"automations\": []\n}\n",
           (unsigned long)kAutomationsFileVersion);
  return String(text);
}

// Loads the store for an edit. No file: the empty store, written unless
// persistSeed is false. False when the file was refused or could not be read:
// the caller must stop, and nothing was seeded or written (the empty store
// never replaces a newer file, or one that is there but did not read).
static bool loadAutomationsStoreForEdit(String& json, bool persistSeed) {
  const AutomationsRead read = readAutomationsFile(json);
  if (read == AutomationsRead::Refused) return false;
  if (read == AutomationsRead::Unreadable) {
    json = "";
    return false;
  }
  if (read == AutomationsRead::Missing || json.length() == 0) {
    json = automationsEmptyStoreJson();
    if (persistSeed) {
      writeAutomationsJsonAtomic(json);
      DEBUG_AUTOMATIONSF("[autos add] created default automations.json");
    }
  }
  return true;
}

// ============================================================================
// automations.json mutation owner
// ============================================================================
// Every read-modify-write of automations.json goes through
// mutateAutomationsJson (the shape of mutateUsersJson in System_User.cpp). It
// holds the filesystem lock from the read through parse, mutation,
// serialization and the atomic write, so a writer on another task is neither
// overwritten by a stale copy nor interleaved with the temp-file promotion.
// The filesystem lock is the one lock: it is reentrant per task, every VFS
// call below reuses it, and no second mutex (or lock order) is introduced. No
// caller may hold AutomationStateGuard here: file work never runs under it.
//
// The mutator edits the parsed document (ArduinoJson, PSRAM) and addresses an
// automation by its numeric id, never by a text offset. A document that
// overflowed, serialized short, does not parse back or lost or gained entries
// the mutator did not account for is never published over the live file. The
// file-version refusal stays: a newer file is not loaded (readAutomationsFile)
// and a newer document is not written (automationsStoreWrite).
//
// Stored layout: ArduinoJson pretty output, which the post-fire writebacks
// have always produced ("id": <n> with one space, as automationIdPosInJson
// and the text scanners in runAutomationsOnBoot, schedulerTickMinute and
// cmd_automation_run read it). Object members keep their order, so "id" stays
// the first member of every automation.
enum class AutomationsMutation : uint8_t { Failed, Unchanged, Changed };
typedef AutomationsMutation (*AutomationsMutator)(JsonDocument& doc, JsonArray automations,
                                                  void* context, String& errorOut);
enum class AutomationsMutateResult : uint8_t {
  Changed,      // the mutator changed the document and it was written
  Unchanged,    // the mutator changed nothing; the file was not touched
  Refused,      // the mutator failed and set errorOut; the file was not touched
  ReadFailed,   // missing (without Seed), unreadable, or a newer format
  Malformed,    // the stored file does not parse or has no "automations" array
  WriteFailed,  // out of memory for the document, or the atomic write failed
};
constexpr uint8_t kAutomationsMutateNotify = 0x01;  // wake the scheduler after the write
constexpr uint8_t kAutomationsMutateSeed = 0x02;    // a missing file starts as the empty store
constexpr uint8_t kAutomationsMutateDryRun = 0x04;  // run the mutator, write nothing

static bool automationsStoreWrite(const String& json, bool notify, bool verified);

static AutomationsMutateResult mutateAutomationsJson(const char* tag, AutomationsMutator mutator,
                                                     void* context, uint8_t flags,
                                                     String& errorOut) {
  // One lock for the whole read-modify-write. Blocking: this is the command
  // path and the scheduler's post-fire writeback (a missed writeback would
  // fire the automation again). The scheduler claims a pass only after a
  // try-lock (automationTick).
  FsLockGuard guard(tag);
  String json;
  if (flags & kAutomationsMutateSeed) {
    // The empty store is not written on its own: it reaches the file with the
    // mutator's change, in one write.
    if (!loadAutomationsStoreForEdit(json, false)) {
      errorOut = "failed to read automations.json";
      return AutomationsMutateResult::ReadFailed;
    }
  } else if (readAutomationsFile(json) != AutomationsRead::Ok) {
    errorOut = "failed to read automations.json";
    return AutomationsMutateResult::ReadFailed;
  }
  PSRAM_JSON_DOC(doc);
  if (deserializeJson(doc, json)) {
    errorOut = "malformed automations.json";
    return AutomationsMutateResult::Malformed;
  }
  JsonArray automations = doc["automations"].as<JsonArray>();
  if (automations.isNull()) {
    errorOut = "malformed automations.json";
    return AutomationsMutateResult::Malformed;
  }
  json = "";  // the text is not needed again; the document holds its own copy
  const AutomationsMutation outcome = mutator(doc, automations, context, errorOut);
  if (outcome == AutomationsMutation::Failed) return AutomationsMutateResult::Refused;
  if (outcome == AutomationsMutation::Unchanged) return AutomationsMutateResult::Unchanged;
  if (doc.overflowed()) {
    errorOut = "out of memory for automations.json";
    return AutomationsMutateResult::WriteFailed;
  }
  if (flags & kAutomationsMutateDryRun) return AutomationsMutateResult::Changed;
  const size_t expectedLength = measureJsonPretty(doc);
  const size_t expectedEntries = automations.size();
  serializeJsonPretty(doc, json);
  bool sound = expectedLength > 0 && json.length() == expectedLength;
  if (sound) {
    // What is about to replace the live file must read back as the document
    // that was just built.
    PSRAM_JSON_DOC(verify);
    sound = !deserializeJson(verify, json) && !verify.overflowed();
    if (sound) {
      JsonArrayConst written = verify["automations"].as<JsonArrayConst>();
      sound = !written.isNull() && written.size() == expectedEntries;
    }
  }
  if (!sound) {
    ERROR_SYSTEMF("[automations] %s: the rebuilt automations.json did not verify; file left unchanged", tag);
    errorOut = "out of memory for automations.json";
    return AutomationsMutateResult::WriteFailed;
  }
  if (!automationsStoreWrite(json, (flags & kAutomationsMutateNotify) != 0, true)) {
    errorOut = "failed to write automations.json";
    return AutomationsMutateResult::WriteFailed;
  }
  return AutomationsMutateResult::Changed;
}

// The automation with this id in a parsed store, and its place in the array.
// The id is compared as a number: 12 never matches 123456, and text inside a
// name or command is never taken for an id.
static JsonObject findAutomationById(JsonArray automations, long id, size_t* indexOut = nullptr) {
  size_t index = 0;
  for (JsonObject automation : automations) {
    JsonVariantConst stored = automation["id"];
    if (stored.is<long>() && stored.as<long>() == id) {
      if (indexOut) *indexOut = index;
      return automation;
    }
    ++index;
  }
  return JsonObject();
}

// ============================================================================
// In-RAM cache of per-automation scheduling metadata
// ============================================================================
// The scheduler runs every main-loop iteration. Without a cache it would have
// to read automations.json from LittleFS and parse it every time — ~15-30ms of
// work per tick for no benefit when nothing is due.
//
// This cache holds just enough to answer "is anything due right now?" without
// any I/O: id, nextAt, enabled. Invalidated on any file write (via
// writeAutomationsJsonAtomic) or explicit notifyAutomationScheduler(). Rebuilt
// at the end of each schedulerTickMinute, which already reads+parses the file.

struct AutomationsCacheEntry {
  time_t nextAt;
  long id;
  bool enabled;
};
static_assert(sizeof(AutomationsCacheEntry) == 16,
              "AutomationsCacheEntry must remain within its 16-byte entry budget");

static constexpr int AUTOMATIONS_CACHE_MAX = 64;
// Cache lifetime and publication share one short lock. File reads, JSON
// parsing and command dispatch always run outside it. Only automationTick's
// main task uses the condition back-off and full scheduler tick.
static StaticSemaphore_t sAutomationStateStorage;
static SemaphoreHandle_t sAutomationStateMutex =
    xSemaphoreCreateMutexStatic(&sAutomationStateStorage);
class AutomationStateGuard {
public:
  AutomationStateGuard() { xSemaphoreTake(sAutomationStateMutex, portMAX_DELAY); }
  ~AutomationStateGuard() { xSemaphoreGive(sAutomationStateMutex); }
  AutomationStateGuard(const AutomationStateGuard&) = delete;
  AutomationStateGuard& operator=(const AutomationStateGuard&) = delete;
};
static std::atomic_bool sAutomationRunning{false};
// Consumed only before a full tick: a change arriving during its file read or
// cache publication remains pending for the next pass.
static std::atomic_bool sAutomationRefreshPending{false};
static AutomationsCacheEntry* gAutomationsCache = nullptr;
static int gAutomationsCacheCount = 0;
static std::atomic_bool gAutomationsCacheValid{false};
// The cached next runs are uptime seconds: the clock was not set at the
// rebuild (AutomationUptimeSchedule). automationsAnyDue() asks for a pass when
// that no longer matches the clock.
static std::atomic_bool gAutomationsCacheOnUptime{false};

// Which SystemEventKind bits any ENABLED automation subscribes to via an
// event trigger. 256-bit mask stored as 8x32-bit words (word = kind >> 5,
// bit = kind & 31). Rebuilt with the cache; consulted by systemEventPost
// (via automationOnSystemEvent) so unsubscribed events never force a tick.
static constexpr size_t AUTOMATION_EVENT_MASK_WORDS = 8;
static constexpr size_t AUTOMATION_EVENT_MASK_BITS =
    AUTOMATION_EVENT_MASK_WORDS * 32u;
static_assert(static_cast<size_t>(SYSEVT_COUNT) <= AUTOMATION_EVENT_MASK_BITS,
              "System Event catalog outgrew the automation subscription mask");
static std::atomic<uint32_t>
    gAutomationEventKindMask[AUTOMATION_EVENT_MASK_WORDS]{};
// Set when a subscribed event was posted; cleared when the tick drains.
static std::atomic_bool gAutomationEventsPending{false};
// Newest bus event seq when the scheduler last started (UINT32_MAX: never).
// The first event drain starts here, not at its own "now" (G-73): setup starts
// the scheduler, but its first drain runs in loop(), so events posted in
// between (battery_low from a low cell at boot) were skipped unmatched.
static std::atomic<uint32_t> gAutomationEventStartSeq{UINT32_MAX};

// Publish a coherent, valid empty view when the backing file is absent or
// malformed.  A merely-invalid cache makes automationsAnyDue() request another
// full scheduler tick on every main-loop pass, so a device that has never had
// Automations compiled in would otherwise hot-loop LittleFS after the feature
// is enabled.  Clear the subscription mask too: it may still describe a file
// that was removed or replaced with malformed content.
static void markAutomationsCacheEmptyValid() {
  AutomationStateGuard guard;
  if (!sAutomationRunning) return;
  gAutomationsCacheValid = false;
  gAutomationsCacheCount = 0;
  for (size_t w = 0; w < AUTOMATION_EVENT_MASK_WORDS; ++w) {
    gAutomationEventKindMask[w] = 0;
  }
  gAutomationsCacheOnUptime = !Clock::isValidEpoch(time(nullptr));
  gAutomationsCacheValid = true;
}

void automationOnSystemEvent(uint8_t kind) {
  if (kind >= SYSEVT_COUNT) return;
  // Cache invalid = subscriptions unknown; be conservative and wake.
  if (sAutomationRefreshPending || !gAutomationsCacheValid ||
      (gAutomationEventKindMask[kind >> 5].load() & (1UL << (kind & 31)))) {
    gAutomationEventsPending = true;
  }
}

// Caller holds AutomationStateGuard for allocation, release and cache reads.
static bool ensureAutomationsCache() {
  if (gAutomationsCache) return true;
  gAutomationsCache = (AutomationsCacheEntry*)ps_calloc(
      AUTOMATIONS_CACHE_MAX, sizeof(AutomationsCacheEntry),
      AllocPref::PreferPsram, "auto.cache");  // main task only, under a FreeRTOS mutex: PSRAM is fine
  if (!gAutomationsCache) {
    ERROR_SYSTEMF("[automations] Failed to allocate scheduler cache (%u bytes)",
                  (unsigned)(AUTOMATIONS_CACHE_MAX * sizeof(AutomationsCacheEntry)));
    gAutomationsCacheCount = 0;
    gAutomationsCacheValid = false;
    return false;
  }
  return true;
}

static void releaseAutomationsCache() {
  gAutomationsCacheValid = false;
  if (gAutomationsCache) {
    ps_free(gAutomationsCache);
    gAutomationsCache = nullptr;
  }
  gAutomationsCacheCount = 0;
  gAutomationsCacheValid = false;
  for (size_t w = 0; w < AUTOMATION_EVENT_MASK_WORDS; ++w) {
    gAutomationEventKindMask[w] = 0;
  }
}

// How late a clock trigger may be and still fire. Beyond this the occurrence
// is treated as missed: the schedule advances but the commands do not run.
// 15 min comfortably covers a slow boot or a busy main loop while still
// suppressing the "device was off for hours" / "clock just jumped from 1970"
// burst. Interval triggers shorter than this simply lose the stale occurrence,
// which is what an interval means anyway.
static constexpr time_t kMissedFireGraceSec = 15 * 60;

// ---------------------------------------------------------------------------
// Next run of short interval triggers, kept in RAM
// ---------------------------------------------------------------------------
// Every fire of an interval trigger used to save its next run time, which
// rewrote automations.json once per fire: 17,000 times a day at 5 s. An
// interval shorter than the missed-fire grace gains nothing from that record.
// After a restart its stale occurrence is dropped and the interval starts
// again, so its next run lives here and the file keeps the value it had.
// Longer intervals and every other trigger type are still saved.
//
// An entry is found by automation id and the trigger's position in the
// automation's "triggers" array. The table holds no lock of its own; the
// functions below it take sVolatileScheduleMux around every call.
struct AutomationVolatileSchedule {
  static constexpr int kEntries = 16;
  struct Entry {
    uint32_t nextAt;      // epoch seconds
    long     id;          // 0 = free
    uint32_t intervalMs;  // the interval this entry was made for
    uint8_t  slot;        // position in the triggers array
    bool     seen;        // touched since the last sweep began
  };
  Entry entries[kEntries] = {};

  static bool covers(uint32_t intervalMs) {
    return intervalMs > 0 && intervalMs < (uint32_t)kMissedFireGraceSec * 1000UL;
  }
  static time_t stepOf(uint32_t intervalMs) {
    const time_t step = (time_t)(intervalMs / 1000);
    return step < 1 ? 1 : step;
  }
  Entry* find(long id, uint8_t slot) {
    if (id == 0) return nullptr;
    for (Entry& e : entries) {
      if (e.id == id && e.slot == slot) return &e;
    }
    return nullptr;
  }
  // False when the trigger is not a short interval or the table is full; the
  // caller then saves the value to the file as before.
  bool set(long id, uint8_t slot, uint32_t intervalMs, time_t nextAt) {
    if (id == 0 || nextAt <= 0 || !covers(intervalMs)) return false;
    Entry* e = find(id, slot);
    for (int i = 0; !e && i < kEntries; i++) {
      if (entries[i].id == 0) e = &entries[i];
    }
    if (!e) return false;
    e->nextAt = (uint32_t)nextAt;
    e->id = id;
    e->intervalMs = intervalMs;
    e->slot = slot;
    e->seen = true;
    return true;
  }
  // The next run the scheduler should use for an interval trigger whose file
  // value is `fileNextAt`.
  time_t effective(long id, uint8_t slot, uint32_t intervalMs, time_t fileNextAt,
                   time_t now, bool timeValid) {
    if (id == 0 || !covers(intervalMs)) return fileNextAt;
    Entry* e = find(id, slot);
    if (e && e->intervalMs != intervalMs) {  // the interval was edited
      *e = Entry{};
      e = nullptr;
    }
    if (e) {
      e->seen = true;
      // A file value later than the entry was written on purpose (an edit, a
      // recompute); the entry follows it.
      if (fileNextAt > (time_t)e->nextAt) e->nextAt = (uint32_t)fileNextAt;
      return (time_t)e->nextAt;
    }
    if (fileNextAt <= 0 || !timeValid) return fileNextAt;
    // No entry: first sight since a restart. The file value is as old as the
    // last save, so a clearly stale one restarts the interval from now
    // instead of being reported as a missed run.
    const time_t step = stepOf(intervalMs);
    const time_t stale = (2 * step > 60) ? 2 * step : 60;
    if (now - fileNextAt <= stale) return fileNextAt;
    return set(id, slot, intervalMs, now + step) ? now + step : fileNextAt;
  }
  // The value to show a reader: the entry when there is one. Changes nothing.
  time_t peek(long id, uint8_t slot, uint32_t intervalMs, time_t fileNextAt) {
    Entry* e = find(id, slot);
    if (!e || e->intervalMs != intervalMs) return fileNextAt;
    return fileNextAt > (time_t)e->nextAt ? fileNextAt : (time_t)e->nextAt;
  }
  // Sweep: drop entries whose trigger is disabled or no longer in the file.
  void beginSweep() {
    for (Entry& e : entries) e.seen = false;
  }
  void endSweep() {
    for (Entry& e : entries) {
      if (e.id != 0 && !e.seen) e = Entry{};
    }
  }
};

static AutomationVolatileSchedule sVolatileSchedule;
static portMUX_TYPE sVolatileScheduleMux = portMUX_INITIALIZER_UNLOCKED;

static time_t automationVolatileNextAt(long id, uint8_t slot, uint32_t intervalMs,
                                       time_t fileNextAt, time_t now, bool timeValid) {
  taskENTER_CRITICAL(&sVolatileScheduleMux);
  const time_t at = sVolatileSchedule.effective(id, slot, intervalMs, fileNextAt, now, timeValid);
  taskEXIT_CRITICAL(&sVolatileScheduleMux);
  return at;
}

static bool automationVolatileSet(long id, uint8_t slot, uint32_t intervalMs, time_t nextAt) {
  taskENTER_CRITICAL(&sVolatileScheduleMux);
  const bool kept = sVolatileSchedule.set(id, slot, intervalMs, nextAt);
  taskEXIT_CRITICAL(&sVolatileScheduleMux);
  return kept;
}

// ---------------------------------------------------------------------------
// Interval and armed-delay triggers while the clock is not set
// ---------------------------------------------------------------------------
// Without a set clock (Clock::isValidEpoch false: no NTP, no RTC) time() counts
// from 1970 at boot, and a next run in automations.json means nothing: one
// saved while the clock was set lies in 2026 and never came due. So while the
// clock is not set, interval and armed-delay triggers count uptime
// (SelfDevice::uptimeSeconds: monotonic, it does not start again when millis()
// wraps after 49.7 days) and their next runs live here, in RAM:
// - an interval fires one interval after the scheduler first sees it (after a
//   boot: one interval after automations start), then every interval;
// - `automation trigger` arms a delay here, and its fire disarms it;
// - neither reads nor writes its nextAt in the file.
// Every other trigger has no next run meanwhile: a time of day waits for a set
// clock (automationNextRunTime), boot and event triggers never had one.
//
// The table holds kEntries triggers. Without a clock an interval beyond them
// waits for a set clock like a time of day, and `automation trigger` refuses
// to arm a delay; a removed or disabled automation gives its interval entry
// back on the next rebuild (an armed delay stays while the trigger exists).
// A restart starts with an empty table, as an armed delay in RAM is lost.
//
// When the clock becomes set, the next scheduler pass carries an entry over to
// clock time with the time it had left (automationCarryUptimeSchedule) when it
// replaced the file: an armed delay, or an interval that already ran on uptime
// (the file's next run is stale then). No such fire is repeated or made up,
// and none is reported as missed by the decades between 1970 and now. An
// interval still in its first countdown since the scheduler saw it changes
// nothing in the file: it is dropped, and the trigger goes on from the file's
// next run as before (the clock usually arrives seconds after boot, and a
// carry would move a daily run's phase to the boot time on every power cycle).
// Adding, editing or deleting an automation drops its entries
// (automationUptimeForget), as it rebuilds or removes the stored triggers.
//
// A schedule time names its own base: below 2020 (not a valid epoch) it is an
// uptime. The scheduler pass, the due cache and the post-fire update take the
// uptime as their `now` while the clock is not set.
struct AutomationUptimeSchedule {
  static constexpr int kEntries = 32;
  struct Entry {
    uint32_t nextAt;      // uptime seconds
    long     id;          // 0 = free
    uint32_t intervalMs;  // an interval trigger's interval; 0 = an armed delay
    uint8_t  slot;        // position in the triggers array
    bool     seen;        // touched since the last sweep began
    bool     ran;         // an interval advanced on uptime (fired, or advanced
                          // past a missed run): the file's nextAt is stale
  };
  // No initializer: the one instance is static (zeroed .bss in PSRAM); a
  // local one is made with `{}`.
  Entry entries[kEntries];

  Entry* find(long id, uint8_t slot) {
    if (id == 0) return nullptr;
    for (Entry& e : entries) {
      if (e.id == id && e.slot == slot) return &e;
    }
    return nullptr;
  }
  Entry* findOrFree(long id, uint8_t slot) {
    Entry* e = find(id, slot);
    for (int i = 0; !e && i < kEntries; i++) {
      if (entries[i].id == 0) e = &entries[i];
    }
    return e;
  }
  // An interval trigger's next run. First sight starts it: one interval from
  // `now`. 0 when there is no interval or no room.
  time_t interval(long id, uint8_t slot, uint32_t intervalMs, time_t now) {
    if (id == 0 || intervalMs == 0) return 0;
    Entry* e = find(id, slot);
    if (e && e->intervalMs != intervalMs) {  // edited, or an armed delay before
      *e = Entry{};
      e = nullptr;
    }
    if (!e) {
      e = findOrFree(id, slot);
      if (!e) return 0;
      e->nextAt = (uint32_t)(now + AutomationVolatileSchedule::stepOf(intervalMs));
      e->id = id;
      e->intervalMs = intervalMs;
      e->slot = slot;
    }
    e->seen = true;
    return (time_t)e->nextAt;
  }
  // An armed delay's run, 0 when the trigger is not armed.
  time_t armed(long id, uint8_t slot) {
    Entry* e = find(id, slot);
    if (!e || e->intervalMs != 0) return 0;
    e->seen = true;
    return (time_t)e->nextAt;
  }
  // Arm a delay to run at `at`; a second arm moves it. False when full.
  bool arm(long id, uint8_t slot, time_t at) {
    if (id == 0) return false;
    Entry* e = findOrFree(id, slot);
    if (!e) return false;
    *e = Entry{};
    e->nextAt = (uint32_t)(at > 0 ? at : 1);
    e->id = id;
    e->slot = slot;
    e->seen = true;
    return true;
  }
  // After a fire: the trigger's next run, or 0 to free the entry (a delay
  // that fired).
  void advance(long id, uint8_t slot, time_t nextAt) {
    Entry* e = find(id, slot);
    if (!e) return;
    if (nextAt <= 0) {
      *e = Entry{};
      return;
    }
    e->nextAt = (uint32_t)nextAt;
    e->seen = true;
    e->ran = true;
  }
  // Whether the clock-set carry-over keeps this entry: an armed delay, or an
  // interval that ran on uptime. One still in its first countdown is dropped.
  static bool carries(const Entry& e) { return e.id != 0 && (e.intervalMs == 0 || e.ran); }
  // Drop every entry of automation `id` (it was added, edited or deleted).
  void forget(long id) {
    if (id == 0) return;
    for (Entry& e : entries) {
      if (e.id == id) e = Entry{};
    }
  }
  bool empty() const {
    for (const Entry& e : entries) {
      if (e.id != 0) return false;
    }
    return true;
  }
  bool anyCarries() const {
    for (const Entry& e : entries) {
      if (carries(e)) return true;
    }
    return false;
  }
  void clear() {
    for (Entry& e : entries) e = Entry{};
  }
  void beginSweep() {
    for (Entry& e : entries) e.seen = false;
  }
  void endSweep() {
    for (Entry& e : entries) {
      if (e.id != 0 && !e.seen) e = Entry{};
    }
  }
};

// PSRAM: the table is only touched under its lock, never from an ISR.
EXT_RAM_BSS_ATTR static AutomationUptimeSchedule sUptimeSchedule;
static portMUX_TYPE sUptimeScheduleMux = portMUX_INITIALIZER_UNLOCKED;

static time_t automationUptimeNow() { return (time_t)SelfDevice::uptimeSeconds(); }

// True when `t` is not a clock time: an uptime (see above).
static bool automationOnUptime(time_t t) { return !Clock::isValidEpoch(t); }

// The scheduler's now: the clock when it is set, else the uptime.
static time_t automationScheduleNow() {
  const time_t now = time(nullptr);
  return Clock::isValidEpoch(now) ? now : automationUptimeNow();
}

static time_t automationUptimeIntervalAt(long id, uint8_t slot, uint32_t intervalMs, time_t now) {
  taskENTER_CRITICAL(&sUptimeScheduleMux);
  const time_t at = sUptimeSchedule.interval(id, slot, intervalMs, now);
  taskEXIT_CRITICAL(&sUptimeScheduleMux);
  return at;
}

static time_t automationUptimeArmedAt(long id, uint8_t slot) {
  taskENTER_CRITICAL(&sUptimeScheduleMux);
  const time_t at = sUptimeSchedule.armed(id, slot);
  taskEXIT_CRITICAL(&sUptimeScheduleMux);
  return at;
}

static bool automationUptimeArm(long id, uint8_t slot, time_t at) {
  taskENTER_CRITICAL(&sUptimeScheduleMux);
  const bool armed = sUptimeSchedule.arm(id, slot, at);
  taskEXIT_CRITICAL(&sUptimeScheduleMux);
  return armed;
}

static void automationUptimeAdvance(long id, uint8_t slot, time_t nextAt) {
  taskENTER_CRITICAL(&sUptimeScheduleMux);
  sUptimeSchedule.advance(id, slot, nextAt);
  taskEXIT_CRITICAL(&sUptimeScheduleMux);
}

// `automation add` (also an edit through id=) and `automation delete` drop the
// automation's uptime entries, as they rebuild its stored triggers with a
// fresh next run or remove them: without a clock an armed delay does not
// survive the edit (with one, the rebuilt trigger is written unarmed), and an
// interval starts again one interval after the scheduler next sees it.
static void automationUptimeForget(long id) {
  taskENTER_CRITICAL(&sUptimeScheduleMux);
  sUptimeSchedule.forget(id);
  taskEXIT_CRITICAL(&sUptimeScheduleMux);
}

static void automationUptimeSweep(bool begin) {
  taskENTER_CRITICAL(&sUptimeScheduleMux);
  if (begin) sUptimeSchedule.beginSweep();
  else sUptimeSchedule.endSweep();
  taskEXIT_CRITICAL(&sUptimeScheduleMux);
}

// The clock is set (`now` is clock time) and the table may still hold the
// uptime schedule: give every entry that replaced the file
// (AutomationUptimeSchedule::carries: an armed delay, an interval that ran on
// uptime) the time it had left, from now. A short interval goes to the RAM
// schedule above (AutomationVolatileSchedule); a long one, an interval that
// finds that table full and an armed delay are written to their trigger's
// nextAt, in one write. An entry that was due runs now, not as a missed run.
// An interval still in its first countdown is dropped: its trigger keeps the
// file's next run, a future one as it is and a past one under the pass's
// missed-run rule, as without the uptime schedule. Then the table is empty.
// Called by the scheduler pass before it reads the file, so it writes without
// notifying: the pass's trailing rebuild reads the result.
struct AutomationCarryContext {
  time_t now;
  time_t upNow;
};

static AutomationsMutation automationCarryMutation(JsonDocument& doc, JsonArray automations,
                                                   void* opaque, String& errorOut) {
  (void)doc;
  (void)errorOut;
  const AutomationCarryContext* ctx = static_cast<const AutomationCarryContext*>(opaque);
  bool changed = false;
  for (JsonObject automation : automations) {
    const long id = automation["id"] | 0L;
    int slot = -1;
    // A named array, not the member proxy's temporary: GCC's
    // -Werror=dangling-reference rejects a range-for over the latter.
    JsonArray triggers = automation["triggers"].as<JsonArray>();
    for (JsonObject t : triggers) {
      if (++slot >= 255) break;
      AutomationUptimeSchedule::Entry e{};
      taskENTER_CRITICAL(&sUptimeScheduleMux);
      AutomationUptimeSchedule::Entry* held = sUptimeSchedule.find(id, (uint8_t)slot);
      if (held) {
        e = *held;
        *held = AutomationUptimeSchedule::Entry{};
      }
      taskEXIT_CRITICAL(&sUptimeScheduleMux);
      if (!AutomationUptimeSchedule::carries(e)) continue;
      const time_t left = (time_t)e.nextAt > ctx->upNow ? (time_t)e.nextAt - ctx->upNow : 0;
      const time_t at = ctx->now + left;
      if (e.intervalMs != 0 && automationVolatileSet(id, (uint8_t)slot, e.intervalMs, at)) continue;
      t["nextAt"] = (unsigned long)at;
      changed = true;
    }
  }
  return changed ? AutomationsMutation::Changed : AutomationsMutation::Unchanged;
}

static void automationCarryUptimeSchedule(time_t now) {
  // Nothing to carry (the table is empty, or holds only first countdowns):
  // no file read; the table is emptied.
  taskENTER_CRITICAL(&sUptimeScheduleMux);
  const bool any = sUptimeSchedule.anyCarries();
  if (!any) sUptimeSchedule.clear();
  taskEXIT_CRITICAL(&sUptimeScheduleMux);
  if (!any) return;
  AutomationCarryContext ctx{now, automationUptimeNow()};
  String unused;
  // One locked read-modify-write; no notification (see above).
  (void)mutateAutomationsJson("automation.carry", automationCarryMutation, &ctx, 0, unused);
  // Entries no trigger in the file holds any more, or a file that did not
  // read: nothing is left to carry.
  taskENTER_CRITICAL(&sUptimeScheduleMux);
  sUptimeSchedule.clear();
  taskEXIT_CRITICAL(&sUptimeScheduleMux);
}

// An automation as a client should see it: a short interval shows the next
// run the scheduler holds in RAM instead of the stale value in the file. The
// web page reloads its list while a shown next run is in the past. Without a
// set clock interval and delay triggers count uptime, which no clock shows:
// they show no next run (null) instead of the file's value, which the
// scheduler does not use meanwhile.
static void automationApplyRuntimeSchedule(JsonObject automation) {
  const long id = automation["id"] | 0L;
  const bool onUptime = automationOnUptime(time(nullptr));
  int slot = -1;
  JsonArray triggers = automation["triggers"].as<JsonArray>();
  for (JsonObject t : triggers) {
    if (++slot >= 255) break;
    const char* type = t["type"] | "";
    if (onUptime) {
      if (strcmp(type, "interval") == 0 || strcmp(type, "manual") == 0) t["nextAt"] = nullptr;
      continue;
    }
    if (strcmp(type, "interval") != 0) continue;
    const uint32_t intervalMs = t["intervalMs"] | uint32_t(0);
    const unsigned long fileNextAt = t["nextAt"] | 0UL;
    taskENTER_CRITICAL(&sVolatileScheduleMux);
    const time_t at = sVolatileSchedule.peek(id, (uint8_t)slot, intervalMs, (time_t)fileNextAt);
    taskEXIT_CRITICAL(&sVolatileScheduleMux);
    if (at != (time_t)fileNextAt) t["nextAt"] = (unsigned long)at;
  }
}

static void automationVolatileSweep(bool begin) {
  taskENTER_CRITICAL(&sVolatileScheduleMux);
  if (begin) sVolatileSchedule.beginSweep();
  else sVolatileSchedule.endSweep();
  taskEXIT_CRITICAL(&sVolatileScheduleMux);
}

// Re-read automations.json and refill the cache. Called at the end of the
// full tick so post-fire nextAt updates are captured on the same pass.
static void rebuildAutomationsCache() {
  String json;
  if (!readAutomationsJson(json)) {
    markAutomationsCacheEmptyValid();
    return;
  }
  PSRAM_JSON_DOC(doc);
  if (deserializeJson(doc, json)) {
    markAutomationsCacheEmptyValid();
    return;
  }
  AutomationStateGuard guard;
  // A stop may complete while the file is being read/parsed. Do not recreate
  // its released cache or publish into freed storage after that point.
  if (!sAutomationRunning || !ensureAutomationsCache()) return;
  gAutomationsCacheValid = false;
  JsonArrayConst autos = doc["automations"].as<JsonArrayConst>();
  int n = 0;
  uint32_t evtMask[AUTOMATION_EVENT_MASK_WORDS]{};
  const time_t now = time(nullptr);
  // A trigger of type "time" names a time of day and waits for a set clock
  // (the scheduler pass leaves it out, see automationNextRunTime). It is left
  // out here as well, or its stored next run would make automationsAnyDue()
  // ask for a full pass on every loop while the pass refuses to fire it.
  const bool clockSet = Clock::isValidEpoch(now);
  // Without a set clock the cached next runs are uptime seconds: an interval
  // or an armed delay from AutomationUptimeSchedule, nothing else.
  const time_t upNow = clockSet ? 0 : automationUptimeNow();
  bool sawAll = true;
  automationVolatileSweep(true);
  if (!clockSet) automationUptimeSweep(true);
  for (JsonObjectConst a : autos) {
    if (n >= AUTOMATIONS_CACHE_MAX) { sawAll = false; break; }
    gAutomationsCache[n].id = a["id"].as<long>();
    gAutomationsCache[n].enabled = a["enabled"] | false;
    // Cache the min nextAt across the automation's triggers. 0 means nothing
    // is scheduled (all manual/boot/event, or unset). Also collect the event
    // kinds enabled automations subscribe to (for the fast wake mask).
    time_t minAt = 0;
    JsonArrayConst triggers = a["triggers"].as<JsonArrayConst>();
    if (!triggers.isNull()) {
      int slot = -1;
      for (JsonVariantConst t : triggers) {
        slot++;
        bool isEventTrig = (strcmp(t["type"] | "", "event") == 0);
        const bool waitsForClock = !clockSet && strcmp(t["type"] | "", "time") == 0;
        // Event triggers never clock-fire; a stale hand-edited nextAt on one
        // would otherwise make automationsAnyDue() spin the full tick every
        // loop pass forever (the scheduler computes 0 for it and never
        // clears the persisted value). Mirror parseOneTrigger's defense.
        if (!isEventTrig && !waitsForClock) {
          unsigned long raw = t["nextAt"] | 0UL;
          time_t na = (time_t)raw;
          // A short interval's next run is in RAM; the file value is stale.
          // A disabled automation holds no entry, so enabling it restarts
          // its interval. Without a set clock every interval counts uptime.
          if (gAutomationsCache[n].enabled && slot < 255 &&
              strcmp(t["type"] | "", "interval") == 0) {
            const uint32_t intervalMs = t["intervalMs"] | uint32_t(0);
            na = clockSet ? automationVolatileNextAt(gAutomationsCache[n].id, (uint8_t)slot, intervalMs,
                                                     na, now, clockSet)
                          : automationUptimeIntervalAt(gAutomationsCache[n].id, (uint8_t)slot,
                                                       intervalMs, upNow);
          } else if (!clockSet) {
            // The file's next run is not used without a set clock; an armed
            // delay (also on a disabled automation) is kept in RAM.
            na = (slot < 255 && strcmp(t["type"] | "", "manual") == 0)
                     ? automationUptimeArmedAt(gAutomationsCache[n].id, (uint8_t)slot)
                     : 0;
          }
          if (na > 0 && (minAt == 0 || na < minAt)) minAt = na;
        }
        if (gAutomationsCache[n].enabled && isEventTrig) {
          int kind = systemEventKindFromName(t["on"] | "");
          if (kind > 0 && kind < (int)SYSEVT_COUNT) evtMask[kind >> 5] |= (1UL << (kind & 31));
        }
      }
    }
    gAutomationsCache[n].nextAt = minAt;
    n++;
  }
  // Every trigger in the file was visited: entries left unseen belong to
  // automations or triggers that were removed or disabled.
  if (sawAll) {
    automationVolatileSweep(false);
    if (!clockSet) automationUptimeSweep(false);
  }
  gAutomationsCacheCount = n;
  gAutomationsCacheOnUptime = !clockSet;
  for (size_t w = 0; w < AUTOMATION_EVENT_MASK_WORDS; ++w) {
    gAutomationEventKindMask[w] = evtMask[w];
  }
  gAutomationsCacheValid = true;
}

// Fast in-RAM check: is any enabled automation's nextAt at or before `now`?
// ---------------------------------------------------------------------------
// Condition-gate back-off
// ---------------------------------------------------------------------------
// A clock-due automation whose "Fire when" condition is FALSE deliberately
// keeps its nextAt in the past: the UI promises it fires when the condition
// becomes true, so the occurrence is deferred, not skipped. But a past nextAt
// makes automationsAnyDue() return true on EVERY main-loop iteration, and the
// full tick re-reads and re-parses automations.json each time — the same
// forever-spin the event-trigger defense in rebuildAutomationsCache() guards
// against. Re-checking a sensor condition a few times a second buys nothing,
// so a blocked automation is muted for kCondBlockMs and re-evaluated after.
// Ring buffer, same shape as the EVENT fire cooldown below.
static constexpr int      kCondBlockSlots = 16;
static constexpr uint32_t kCondBlockMs    = 5000;
static long     sCondBlockId[kCondBlockSlots] = {};
static uint32_t sCondBlockAtMs[kCondBlockSlots] = {};
static int      sCondBlockNext = 0;

// True while `id` is inside its condition back-off window.
static bool automationCondBlocked(long id) {
  if (id == 0) return false;  // 0 marks an empty slot — never a real id
  const uint32_t nowMs = millis();
  for (int i = 0; i < kCondBlockSlots; i++) {
    if (sCondBlockId[i] == id) {
      return (nowMs - sCondBlockAtMs[i]) < kCondBlockMs;
    }
  }
  return false;
}

// Note that `id` just failed its condition gate while clock-due.
static void automationNoteCondBlocked(long id) {
  if (id == 0) return;
  for (int i = 0; i < kCondBlockSlots; i++) {
    if (sCondBlockId[i] == id) { sCondBlockAtMs[i] = millis(); return; }
  }
  sCondBlockId[sCondBlockNext]   = id;
  sCondBlockAtMs[sCondBlockNext] = millis();
  sCondBlockNext = (sCondBlockNext + 1) % kCondBlockSlots;
}

// Clear the back-off for `id` (it fired, or its record changed).
static void automationClearCondBlock(long id) {
  for (int i = 0; i < kCondBlockSlots; i++) {
    if (sCondBlockId[i] == id) { sCondBlockId[i] = 0; sCondBlockAtMs[i] = 0; return; }
  }
}

// Called from the main loop every iteration. Returns true if the full tick
// must run (something is due, or the cache is stale).
static bool automationsAnyDue(time_t now) {
  if (!gAutomationsCache) return true;       // force a full tick to allocate/rebuild
  if (!gAutomationsCacheValid) return true;  // force a full tick to rebuild
  // Without a set clock the cache holds uptime seconds. When the clock has
  // been set (or lost) since the rebuild, one pass carries the schedule over
  // and rebuilds.
  const bool onUptime = automationOnUptime(now);
  if (onUptime != gAutomationsCacheOnUptime.load()) return true;
  if (onUptime) now = automationUptimeNow();
  for (int i = 0; i < gAutomationsCacheCount; i++) {
    if (gAutomationsCache[i].enabled && gAutomationsCache[i].nextAt > 0 && now >= gAutomationsCache[i].nextAt) {
      // Deferred-by-condition automations stay due by design; don't let one
      // spin the full tick (and its file read) on every loop pass.
      if (automationCondBlocked(gAutomationsCache[i].id)) continue;
      return true;
    }
  }
  return false;
}

// The one place automations.json is written. Refuses a document in a newer
// format, and (unless the caller has just verified it) one that does not parse
// or has no "automations" array, so no caller can publish a broken store over
// the live one. The write runs under the filesystem lock (reentrant: the
// mutation owner already holds it). Notify only after a successful save: the
// next owner tick refreshes the cache; a failed replacement leaves both the
// previous file and schedule intact.
static bool automationsStoreWrite(const String& json, bool notify, bool verified) {
  if (automationsNewerFileVersion(json)) {
    ERROR_SYSTEMF("[automations] Refused to write %s with a format newer than %lu",
                  AUTOMATIONS_JSON_FILE, (unsigned long)kAutomationsFileVersion);
    return false;
  }
  if (!verified) {
    PSRAM_JSON_DOC(check);
    if (deserializeJson(check, json) || check["automations"].as<JsonArrayConst>().isNull()) {
      ERROR_SYSTEMF("[automations] Refused to write %s: the document is not a valid automations store",
                    AUTOMATIONS_JSON_FILE);
      return false;
    }
  }
  FsLockGuard guard("automation.write");
  if (!writeTextAtomic(AUTOMATIONS_JSON_FILE, json, VFS::systemAuth(VFS::Scopes::SYSTEM_STATE, "automation.state"))) return false;
  if (notify) notifyAutomationScheduler();
  return true;
}

// The public writer, for a whole document a caller built (the empty store, an
// import). It cannot make a caller's earlier read current: code that reads,
// changes and writes back goes through mutateAutomationsJson or
// automationsSanitizeStore, which hold the lock across all three.
bool writeAutomationsJsonAtomic(const String& json) {
  return automationsStoreWrite(json, true, false);
}


// Deployment-profile migration: a provisioned device may have completed its
// first-time setup while Automations was compiled out, so the normal setup
// seed never created this file.  Create only when it is genuinely absent;
// never replace an existing malformed/unreadable file and risk losing rules.
static bool ensureAutomationsFile() {
  if (VFS::existsGuarded(AUTOMATIONS_JSON_FILE,
                         VFS::systemAuth(VFS::Scopes::SYSTEM_STATE, "automation.init"))) {
    return true;
  }

  const String empty = automationsEmptyStoreJson();
  if (!writeAutomationsJsonAtomic(empty)) {
    ERROR_SYSTEMF("[automations] Failed to create %s", AUTOMATIONS_JSON_FILE);
    return false;
  }
  DEBUG_AUTOMATIONSF(
         "[automations] Created empty automation store for profile migration");
  return true;
}

// Update the nextAt field of a specific trigger within an automation.
// Phase 1: `triggerIdx=-1` is an alias for "first schedulable trigger" (the
// behavior legacy callers that used to pass only an id+nextAt expect).
// Phase 1B will remove the alias and force an explicit trigger index.
struct AutomationNextAtContext {
  long automationId;
  int triggerIdx;
  time_t newNextAt;
};

static AutomationsMutation automationNextAtMutation(JsonDocument& doc, JsonArray automations,
                                                    void* opaque, String& errorOut) {
  (void)doc;
  const AutomationNextAtContext* ctx = static_cast<const AutomationNextAtContext*>(opaque);
  for (JsonObject automation : automations) {
    if (automation["id"].as<long>() != ctx->automationId) continue;
    JsonArray triggers = automation["triggers"].as<JsonArray>();
    if (triggers.isNull()) break;
    // Pick the trigger to update.
    int chosen = ctx->triggerIdx;
    if (chosen < 0) {
      // Legacy: first schedulable (non-boot/manual) trigger; else first.
      int idx = 0;
      chosen = 0;
      for (JsonVariantConst t : triggers) {
        const char* tt = t["type"] | "";
        if (strcmp(tt, "time") == 0 || strcmp(tt, "interval") == 0) { chosen = idx; break; }
        idx++;
      }
    }
    int idx = 0;
    for (JsonVariant t : triggers) {
      if (idx == chosen) {
        t["nextAt"] = (unsigned long)ctx->newNextAt;
        return AutomationsMutation::Changed;
      }
      idx++;
    }
    break;
  }
  errorOut = "automation trigger not found";
  return AutomationsMutation::Failed;
}

bool updateAutomationTriggerNextAt(long automationId, int triggerIdx, time_t newNextAt) {
  AutomationNextAtContext ctx{automationId, triggerIdx, newNextAt};
  String error;
  return mutateAutomationsJson("automation.nextAt", automationNextAtMutation, &ctx,
                               kAutomationsMutateNotify, error) == AutomationsMutateResult::Changed;
}

// Legacy shim: writes nextAt into the first schedulable trigger. Callers that
// pre-dated the trigger-array refactor still work unchanged.
bool updateAutomationNextAt(long automationId, time_t newNextAt) {
  return updateAutomationTriggerNextAt(automationId, -1, newNextAt);
}

// Read, sanitize the ids and write back under one lock (the one-call form of
// readAutomationsJson + sanitizeAutomationsJson + writeAutomationsJsonAtomic).
// False when the file could not be read (missing, unreadable, newer format) or
// the repaired document could not be written; *changedOut says whether
// anything was repaired, `counts` what.
bool automationsSanitizeStore(AutomationSanitizeCounts* counts, bool* changedOut) {
  if (changedOut) *changedOut = false;
  if (counts) *counts = AutomationSanitizeCounts{};
  FsLockGuard guard("automation.sanitize");
  String json;
  if (!readAutomationsJson(json)) return false;
  if (!sanitizeAutomationsJson(json, counts)) return true;
  if (!writeAutomationsJsonAtomic(json)) return false;
  if (changedOut) *changedOut = true;
  return true;
}

// A command's reply to a read error: names the newer format when the file was
// refused.
static const char* automationsReadErrorReply() {
  const uint32_t newer = sAutomationsRefusedVersion.load();
  if (!newer) return "Error: failed to read automations.json";
  if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
  snprintf(getDebugBuffer(), GLOBAL_DEBUG_BUFFER_SIZE,
           "Error: automations.json is format %lu, newer than this firmware reads (%lu). "
           "Not loaded and left unchanged; update the firmware to use it.",
           (unsigned long)newer, (unsigned long)kAutomationsFileVersion);
  return getDebugBuffer();
}

// Run automations on boot
void runAutomationsOnBoot() {
  static bool s_ran = false;
  if (s_ran) return;
  s_ran = true;

  if (!filesystemReady) return;

  DEBUG_AUTOMATIONSF("[automations] Checking for boot automations");

  // Read automations.json
  String json;
  if (!readAutomationsJson(json)) {
    DEBUG_AUTOMATIONSF("[automations] No automations file found");
    return;
  }

  time_t now = time(nullptr);

  int pos = 0;
  while (true) {
    int idPos = json.indexOf("\"id\"", pos);
    if (idPos < 0) break;
    int colon = json.indexOf(':', idPos);
    if (colon < 0) break;

    int objStart = json.lastIndexOf('{', idPos);
    if (objStart < 0) {
      pos = colon + 1;
      continue;
    }

    int objEnd = findJsonObjectEnd(json, objStart);
    if (objEnd < 0) break;

    // Extract id value
    int comma = json.indexOf(',', colon + 1);
    int idValEnd = (comma > 0 && comma < objEnd) ? comma : objEnd;
    String idStr = json.substring(colon + 1, idValEnd);
    idStr.trim();
    long id = idStr.toInt();

    String obj = json.substring(objStart, objEnd + 1);

    bool enabled = (obj.indexOf("\"enabled\": true") >= 0) || (obj.indexOf("\"enabled\":true") >= 0);
    if (!enabled) {
      pos = objEnd + 1;
      continue;
    }

    // An automation runs at boot if it has any trigger of type "boot" in its
    // triggers array. We detect via string scan since the triggers array
    // contains the substring `"type":"boot"` for any boot trigger.
    bool bootTrigger = (obj.indexOf("\"type\": \"boot\"") >= 0) || (obj.indexOf("\"type\":\"boot\"") >= 0);
    if (!bootTrigger) {
      pos = objEnd + 1;
      continue;
    }

    int bootDelayMs = 0;
    {
      int keyPos = obj.indexOf("\"bootDelayMs\"");
      if (keyPos >= 0) {
        int c = obj.indexOf(':', keyPos);
        if (c > 0) {
          int end1 = obj.indexOf(',', c + 1);
          int end2 = obj.indexOf('}', c + 1);
          int end = (end1 > 0 && (end2 < 0 || end1 < end2)) ? end1 : end2;
          if (end > c) {
            String v = obj.substring(c + 1, end);
            v.trim();
            bootDelayMs = v.toInt();
          }
        }
      }
    }

    String autoName = "Unknown";
    {
      int namePos = obj.indexOf("\"name\"");
      if (namePos >= 0) {
        int c = obj.indexOf(':', namePos);
        int q1 = obj.indexOf('"', c + 1);
        int q2 = obj.indexOf('"', q1 + 1);
        if (q1 >= 0 && q2 > q1) autoName = obj.substring(q1 + 1, q2);
      }
    }

    // Read global condition expression (new schema: just the expression, e.g. "ROOM=bedroom")
    String condition = "";
    {
      int condPos = obj.indexOf("\"condition\"");
      // Make sure it's not "conditions" (the old plural key)
      if (condPos >= 0 && obj[condPos + 11] == '"') {
        condPos = -1; // false match on "conditions"
      }
      if (condPos >= 0) {
        int c = obj.indexOf(':', condPos);
        int q1 = obj.indexOf('"', c + 1);
        int q2 = obj.indexOf('"', q1 + 1);
        if (q1 >= 0 && q2 > q1) {
          condition = obj.substring(q1 + 1, q2);
          condition.trim();
        }
      }
    }

    char* cmdsList[64];  // Array of pointers instead of String objects
    int cmdsCount = 0;
    {
      int cmdsPos = obj.indexOf("\"commands\"");
      bool haveArray = false;
      int arrStart = -1, arrEnd = -1;
      if (cmdsPos >= 0) {
        int c = obj.indexOf(':', cmdsPos);
        arrStart = obj.indexOf('[', c);
        if (arrStart > 0) {
          int depth = 0;
          for (int i = arrStart; i < (int)obj.length(); ++i) {
            char ch = obj[i];
            if (ch == '[') depth++;
            else if (ch == ']') {
              depth--;
              if (depth == 0) {
                arrEnd = i;
                break;
              }
            }
          }
          haveArray = (arrStart > 0 && arrEnd > arrStart);
        }
      }
      if (haveArray) {
        String body = obj.substring(arrStart + 1, arrEnd);
        int i = 0;
        while (i < (int)body.length() && cmdsCount < 64) {
          while (i < (int)body.length() && (body[i] == ' ' || body[i] == ',' || body[i] == '\n' || body[i] == '\r' || body[i] == '\t')) i++;
          if (i >= (int)body.length()) break;
          if (body[i] == '"') {
            int q1 = i;
            int q2 = body.indexOf('"', q1 + 1);
            if (q2 < 0) break;
            String one = body.substring(q1 + 1, q2);
            one.trim();
            if (one.length() && cmdsCount < 64) {
              cmdsList[cmdsCount] = strdup(one.c_str());  // Allocate and copy
              if (cmdsList[cmdsCount]) cmdsCount++;
            }
            i = q2 + 1;
          } else {
            int next = body.indexOf(',', i);
            if (next < 0) break;
            i = next + 1;
          }
        }
      } else {
        int cpos = obj.indexOf("\"command\"");
        if (cpos >= 0) {
          int c = obj.indexOf(':', cpos);
          int q1 = obj.indexOf('"', c + 1);
          int q2 = obj.indexOf('"', q1 + 1);
          if (q1 > 0 && q2 > q1) {
            String cmd = obj.substring(q1 + 1, q2);
            cmd.trim();
            if (cmd.length() && cmdsCount < 64) {
              cmdsList[cmdsCount] = strdup(cmd.c_str());  // Allocate and copy
              if (cmdsList[cmdsCount]) cmdsCount++;
            }
          }
        }
      }
    }

    if (cmdsCount == 0) {
      pos = objEnd + 1;
      continue;
    }

    // Evaluate global condition (expression-only, e.g. "ROOM=bedroom")
    if (condition.length() > 0) {
      char wrapped[384];
      snprintf(wrapped, sizeof(wrapped), "IF %s THEN _", condition.c_str());
      bool conditionMet = evaluateCondition(wrapped);
      DEBUG_AUTOMATIONSF("[automations] id=%ld boot condition='%s' result=%s",
             id, condition.c_str(), conditionMet ? "TRUE" : "FALSE");
      if (!conditionMet) {
        if (automationLogActive()) {
          char skipMsg[256];
          snprintf(skipMsg, sizeof(skipMsg), "Boot automation skipped: ID=%ld Name=%s Condition not met: %s",
                   id, autoName.c_str(), condition.c_str());
          appendAutoLogEntry("AUTO_SKIP", skipMsg);
        }
        // The command copies were made before the condition was known; a
        // skipped automation releases them here (they used to leak each boot).
        for (int ci = 0; ci < cmdsCount; ++ci) {
          free(cmdsList[ci]);
        }
        pos = objEnd + 1;
        continue;
      }
    }

    // Log automation start
    if (bootDelayMs > 0) {
      DEBUG_AUTOMATIONSF("[automations] Running boot automation: %s (delay: %dms)", autoName.c_str(), bootDelayMs);
      delay(bootDelayMs);
    } else {
      DEBUG_AUTOMATIONSF("[automations] Running boot automation: %s", autoName.c_str());
    }

    char _createdByBuf[64];
    extractJsonString(obj.c_str(), "\"createdBy\"", _createdByBuf, sizeof(_createdByBuf));

    if (automationLogActive()) {
      char startMsg[256];
      snprintf(startMsg, sizeof(startMsg), "Boot automation started: ID=%ld Name=%s User=%s",
               id, autoName.c_str(), _createdByBuf);
      appendAutoLogEntry("AUTO_START", startMsg);
    }

    for (int ci = 0; ci < cmdsCount; ++ci) {
      const char* result = executeConditionalCommand(cmdsList[ci], _createdByBuf, autoName.c_str());

      // Output the result (skip internal status messages - actual output comes from queue)
      if (!isAutoInternalResult(result)) {
        char outMsg[384];
        snprintf(outMsg, sizeof(outMsg), "[Boot Automation %ld] %s", id, result);
        broadcastOutput(outMsg);
      }
    }

    // SYSTEM-EVENT: boot automation dispatched its command list.
    systemEventPost(SYSEVT_AUTOMATION_FIRED, autoName.c_str(), "boot");

    // Free allocated command strings
    for (int ci = 0; ci < cmdsCount; ++ci) {
      free(cmdsList[ci]);
    }

    if (automationLogActive()) {
      char endMsg[256];
      snprintf(endMsg, sizeof(endMsg), "Boot automation completed: ID=%ld Name=%s Commands=%d",
               id, autoName.c_str(), cmdsCount);
      appendAutoLogEntry("AUTO_END", endMsg);
    }

    DEBUG_AUTOMATIONSF("[automations] Boot automation completed: %s", autoName.c_str());

    // Without a set clock nothing is written: interval and delay triggers
    // then count uptime in RAM, and a next run from 1970 means nothing.
    if (Clock::isValidEpoch(now)) {
      time_t newNextAt = computeNextRunTime(obj.c_str(), now);
      if (newNextAt > 0) {
        updateAutomationNextAt(id, newNextAt);
      }
    }

    pos = objEnd + 1;
  }
}

// Initialize automation system
bool initAutomationSystem() {
  DEBUG_AUTOMATIONSF("[automations] System initialized");
  
  // Start the automation scheduler
  if (!startAutomationScheduler()) {
    DEBUG_AUTOMATIONSF("[automations] WARNING: Failed to start scheduler");
    return false;
  }
  
  return true;
}

// Suspend automation system
void suspendAutomationSystem() {
  stopAutomationScheduler();
  DEBUG_AUTOMATIONSF("[automations] System suspended");
}

// Resume automation system
void resumeAutomationSystem() {
  startAutomationScheduler();
  DEBUG_AUTOMATIONSF("[automations] System resumed");
}

// Execute automation command (queues through FreeRTOS command queue, non-blocking)
void runAutomationCommandUnified(const String& argsInput) {
  (void)queueAutomationSubCommand(argsInput.c_str(), "");
}

// Automation command handlers
const char* cmd_automation_list(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String json;
  if (!readAutomationsJson(json)) {
    return automationsReadErrorReply();
  }
  const String safeJson = redactAutomationsJsonForResponse(json);
  if (!safeJson.length()) {
    return "Error: malformed automations.json";
  }
  // The document is the reply and nothing is printed (OUTPUT CONTRACT in
  // System_Utils.cpp): a printed line is cut at 255 B.
  PSRAM_STATIC_BUF(listJson, CMD_RESULT_MAX);
  if (safeJson.length() >= listJson_SIZE) {
    snprintf(listJson, listJson_SIZE,
             "Error: The automation list is %u B, over the %u B reply limit. Read it on the web Automations page.",
             (unsigned)safeJson.length(), (unsigned)(listJson_SIZE - 1));
    return listJson;
  }
  memcpy(listJson, safeJson.c_str(), safeJson.length() + 1);
  return listJson;
}

// The keys `automation add` takes.
static const char* const kAutomationAddKeys[] = {
  "name", "type", "time", "recurrence", "days", "weekinterval", "dayofmonth", "month",
  "secondarytriggers", "delayms", "intervalms", "runatboot", "bootdelayms", "command",
  "commands", "condition", "enabled", "on", "match", "id",
};

// Appends ` name, type, ..., id.` (the keys above) to a reply.
static void automationAddAppendKeys(char* reply, size_t replySize) {
  const char* lead = " ";
  for (const char* key : kAutomationAddKeys) {
    const size_t used = strlen(reply);
    if (used + 1 >= replySize) return;
    snprintf(reply + used, replySize - used, "%s%s", lead, key);
    lead = ", ";
  }
  const size_t used = strlen(reply);
  if (used + 1 < replySize) snprintf(reply + used, replySize - used, ".");
}

// Refuses an `automation add` line that holds a key the command does not take
// or a word that is no key=value (the rest of an unquoted value with spaces).
// nullptr when every token is one of its keys. The reply repeats nothing of
// the line but a key the command takes: a value may be a command with a
// secret in it, and a bare word of it that holds '=' reads as a key. The
// reply lists the keys itself, `help automation` does not name them all.
static const char* automationAddRefuseToken(const CommandArgs& a) {
  EXT_RAM_BSS_ATTR static char reply[384];
  const char* line = a.raw().c_str();
  int cursor = 0, start = 0, keyLen = 0;
  int lastKeyStart = -1, lastKeyLen = 0;
  while (a.nextKeyToken(cursor, start, keyLen)) {
    const int shown = lastKeyLen > 24 ? 24 : lastKeyLen;
    if (keyLen == 0) {
      if (lastKeyStart < 0) {
        snprintf(reply, sizeof(reply),
                 "Error: Unexpected text before the first argument. Give key=value arguments. The keys are");
        automationAddAppendKeys(reply, sizeof(reply));
        return reply;
      }
      snprintf(reply, sizeof(reply),
               "Error: Unexpected text after %.*s=. Put a value with spaces in quotes, for example %.*s=\"two words\".",
               shown, line + lastKeyStart, shown, line + lastKeyStart);
      return reply;
    }
    bool known = false;
    for (const char* key : kAutomationAddKeys) {
      if (strlen(key) == (size_t)keyLen && strncmp(line + start, key, (size_t)keyLen) == 0) {
        known = true;
        break;
      }
    }
    if (!known) {
      if (lastKeyStart < 0) {
        snprintf(reply, sizeof(reply), "Error: The first argument is not a key of automation add. The keys are");
      } else {
        snprintf(reply, sizeof(reply),
                 "Error: Unknown argument after %.*s=. Put a value with spaces in quotes. The keys are",
                 shown, line + lastKeyStart);
      }
      automationAddAppendKeys(reply, sizeof(reply));
      return reply;
    }
    lastKeyStart = start;
    lastKeyLen = keyLen;
  }
  return nullptr;
}

// A whole-number argument of `automation add` inside [low, high]: nullptr when
// `text` is one (then `value` holds it), else the reply that names the key and
// its range.
static const char* automationAddNumber(const char* key, const String& text, long long low,
                                       long long high, long long& value) {
  if (parseIntArg(text, low, high, value)) return nullptr;
  EXT_RAM_BSS_ATTR static char reply[96];
  snprintf(reply, sizeof(reply), "Error: %s must be a whole number from %lld to %lld.", key, low, high);
  return reply;
}

// automationMutateErrorReply and automationCallerMayChange are defined with
// the enable/disable command below.
static const char* automationMutateErrorReply(AutomationsMutateResult result, const char* refusal);
static bool automationCallerMayChange(JsonObjectConst automation, const String& caller, bool callerIsAdmin);

struct AutomationAddContext {
  JsonObjectConst entry;  // the built automation; its "id" is set below
  bool hasOverrideId;     // id=<n> was given: replace that entry if it is stored
  long id;                // in: the id= value; out: the id the entry was stored under
  String caller;
  bool callerIsAdmin;
  const char* reply;      // the refusal, when the mutator fails
  const String* steps;    // the commands= text, checked against the owner's role
};

// Writes the refusal for one step an account may not run (commandRoleRefusalFor):
// the owner, and on someone else's edit the editor too
// and returns it; nullptr when the owner may run it. The text lives in a static
// buffer: replies are read before the next command runs on cmd_exec.
static const char* automationOwnerRefusalText(CommandRoleRefusal refusal,
                                              const String& owner,
                                              const String& row) {
  EXT_RAM_BSS_ATTR static char text[224];
  const char* who = owner.length() ? owner.c_str() : "(no owner)";
  const char* name = row.length() ? row.c_str() : "this step";
  switch (refusal) {
    case CommandRoleRefusal::None:
      return nullptr;
    case CommandRoleRefusal::Guest:
      snprintf(text, sizeof(text),
               "Error: '%.40s' is a guest account, and guests run no commands. Not saved.",
               who);
      break;
    case CommandRoleRefusal::Admin:
      snprintf(text, sizeof(text),
               "Error: '%.40s' needs an administrator, which '%.40s' is not. Remove the step. Not saved.",
               name, who);
      break;
    case CommandRoleRefusal::SuperAdmin:
      snprintf(text, sizeof(text),
               "Error: '%.40s' needs a super admin, which '%.40s' is not. Remove the step. Not saved.",
               name, who);
      break;
    case CommandRoleRefusal::RemoteAdmin:
      snprintf(text, sizeof(text),
               "Error: sending '%.40s' to the bonded device needs an administrator, which '%.40s' is not. Not saved.",
               name, who);
      break;
    case CommandRoleRefusal::NestedRemote:
      snprintf(text, sizeof(text), "Error: nested remote commands are not allowed. Not saved.");
      break;
    case CommandRoleRefusal::RoleUnavailable:
      snprintf(text, sizeof(text),
               "Error: the role of the owner '%.40s' could not be read. Not saved; try again.", who);
      break;
  }
  return text;
}

// The keyword's position the way executeConditionalCommand finds it when the
// step runs: the first " THEN " / " ELSE " (space or tab around it, any case)
// at or after `from`, quotes not considered. The save check must split a step
// exactly as the run does, or a branch it never checked would run. -1 if none;
// *end is the index after the keyword.
static int automationRunKeyword(const String& step, const char* keyword, int from, int* end) {
  const int length = (int)step.length();
  for (int i = from < 0 ? 0 : from; i + 5 < length; ++i) {
    if (step[i] != ' ' && step[i] != '\t') continue;
    int k = 0;
    for (; k < 4; ++k) {
      char c = step[i + 1 + k];
      if (c >= 'a' && c <= 'z') c = (char)(c - ('a' - 'A'));
      if (c != keyword[k]) break;
    }
    if (k == 4 && (step[i + 5] == ' ' || step[i + 5] == '\t')) {
      if (end) *end = i + 5;
      return i + 1;
    }
  }
  return -1;
}

// One step, and the THEN/ELSE branches of a conditional step, split the way
// the run splits them (automationRunKeyword), with bounded depth. PRINT is
// not a command.
static const char* automationOwnerStepRefusal(const String& owner, const String& stepIn,
                                              unsigned depth) {
  String step = stepIn;
  step.trim();
  if (step.length() == 0) return nullptr;
  const bool conditional =
      step.length() > 3 && (step[0] == 'I' || step[0] == 'i') &&
      (step[1] == 'F' || step[1] == 'f') && isspace(static_cast<unsigned char>(step[2]));
  if (conditional) {
    if (depth >= 4) {
      return "Error: conditional steps nest more than 4 deep. Not saved.";
    }
    int thenEnd = 0;
    const int thenStart = automationRunKeyword(step, "THEN", 2, &thenEnd);
    if (thenStart < 0) return nullptr;  // the chain syntax check reports it
    int elseEnd = 0;
    const int elseStart = automationRunKeyword(step, "ELSE", thenEnd, &elseEnd);
    const String thenStep =
        step.substring(thenEnd, elseStart >= 0 ? elseStart : (int)step.length());
    if (const char* refusal = automationOwnerStepRefusal(owner, thenStep, depth + 1)) {
      return refusal;
    }
    if (elseStart < 0) return nullptr;
    return automationOwnerStepRefusal(owner, step.substring(elseEnd), depth + 1);
  }
  if (strncasecmp(step.c_str(), "PRINT ", 6) == 0) return nullptr;
  String row;
  const CommandRoleRefusal refusal = commandRoleRefusalFor(owner, step, &row);
  return automationOwnerRefusalText(refusal, owner, row);
}

// Every `;`-separated step of an automation, checked against its owner's role
// when it is saved (added or edited). Run time checks each step again as the
// owner (authorizeCommand), so a role lowered later still stops the step.
static const char* automationOwnerStepsRefusal(const String& owner, const String& steps) {
  int start = 0;
  const int len = steps.length();
  for (int i = 0; i <= len; ++i) {
    if (i == len || steps[i] == ';') {
      if (const char* refusal =
              automationOwnerStepRefusal(owner, steps.substring(start, i), 0)) {
        return refusal;
      }
      start = i + 1;
    }
  }
  return nullptr;
}

// Adds the built automation to the parsed store. With id=<n>: an entry that
// holds that id is removed first, after the creator-or-admin check (an edit),
// and the new entry takes its id. Without: the entry gets a new id that no
// stored automation has. "id" stays the first member of the stored object.
static AutomationsMutation automationAddMutation(JsonDocument& doc, JsonArray automations,
                                                 void* opaque, String& errorOut) {
  (void)doc;
  (void)errorOut;
  AutomationAddContext* ctx = static_cast<AutomationAddContext*>(opaque);
  // A new automation belongs to whoever adds it. An edit never changes the
  // owner, also when an admin edits another account's automation.
  String owner = ctx->caller;
  bool editsStored = false;
  if (ctx->hasOverrideId) {
    size_t index = 0;
    JsonObject existing = findAutomationById(automations, ctx->id, &index);
    if (!existing.isNull()) {
      DEBUG_AUTOMATIONSF("[autos add/edit] Editing existing id=%ld createdBy='%s' requestedBy='%s' isAdmin=%d",
             ctx->id, existing["createdBy"] | "", ctx->caller.c_str(), ctx->callerIsAdmin);
      if (!automationCallerMayChange(existing, ctx->caller, ctx->callerIsAdmin)) {
        ctx->reply = "Error: You do not have permission to edit this automation. Only the creator or an admin can edit it.";
        return AutomationsMutation::Failed;
      }
      owner = existing["createdBy"] | "";
      editsStored = true;
    }
  }
  // Every step against the owner's role, before anything is removed. An edit
  // by someone else is checked against the editor's role too: an
  // administrator editing a super admin's automation must not store a step
  // only a super admin may run, which would then run as that super admin.
  if (const char* refusal = automationOwnerStepsRefusal(owner, *ctx->steps)) {
    ctx->reply = refusal;
    return AutomationsMutation::Failed;
  }
  if (editsStored && owner != ctx->caller) {
    if (const char* refusal = automationOwnerStepsRefusal(ctx->caller, *ctx->steps)) {
      ctx->reply = refusal;
      return AutomationsMutation::Failed;
    }
  }
  if (editsStored) {
    size_t index = 0;
    findAutomationById(automations, ctx->id, &index);
    automations.remove(index);
  } else if (!ctx->hasOverrideId) {
    // From the uptime, kept inside 1..kAutomationIdMax: after 24.9 days of
    // uptime millis() itself is a number no reader of the id can take.
    unsigned long id = automationIdSeed((uint32_t)millis());
    int guard = 0;
    while (!findAutomationById(automations, (long)id).isNull() && guard < 100) {
      id = automationIdAdvance((uint32_t)id, 1 + (uint32_t)random(1, 100000));
      guard++;
    }
    ctx->id = (long)id;
  }
  JsonObject added = automations.add<JsonObject>();
  if (added.isNull() || !added.set(ctx->entry)) {
    ctx->reply = "Error: out of memory for automations.json";
    return AutomationsMutation::Failed;
  }
  added["id"] = ctx->id;  // the member exists (first in the entry); only its value changes
  added["createdBy"] = owner;
  return AutomationsMutation::Changed;
}

const char* cmd_automation_add(const String& argsInput) {
  // Do not early-return on validate; we want to perform full argument checks
  bool validateOnly = gCliValidateOnly;
  
  CommandArgs a(argsInput);
  if (const char* refusal = automationAddRefuseToken(a)) return refusal;

  String name = a.value("name");
  String type = a.value("type");
  String timeS = a.value("time");
  String recurrence = a.value("recurrence");
  String days = a.value("days");
  String weekInterval = a.value("weekinterval");
  String dayOfMonth = a.value("dayofmonth");
  String monthOfYear = a.value("month");
  String secondaryTriggersJson = a.value("secondarytriggers");
  String delayMs = a.value("delayms");
  String intervalMs = a.value("intervalms");
  String runAtBootStr = a.value("runatboot");
  String bootDelayMsStr = a.value("bootdelayms");
  String cmdStr = a.value("command");
  String cmdsList = a.value("commands");
  String condition = a.value("condition");
  String enabledStr = a.value("enabled");
  String eventOn = a.value("on");
  String eventMatch = a.value("match");
  
  // No enabled= stores the automation disabled.
  const int enabledArg = enabledStr.length() ? parseBoolArg(enabledStr) : 0;
  if (enabledArg < 0) return kCommandReplyNotOnOff;
  bool enabled = enabledArg == 1;
  
  String typeNorm = type;
  typeNorm.trim();
  typeNorm.toLowerCase();
  
  DEBUG_AUTOMATIONSF("[autos add] name='%s' type='%s' time='%s' days='%s' delayms='%s' intervalms='%s' enabled=%d",
         name.c_str(), typeNorm.c_str(), timeS.c_str(), days.c_str(), delayMs.c_str(), intervalMs.c_str(), enabled ? 1 : 0);
  
  if (name.length() == 0) {
    return "Error: missing name";
  }
  if (typeNorm.length() == 0) {
    return "Error: missing type (atTime|afterDelay|interval|event)";
  }
  if ((cmdStr.length() == 0 && cmdsList.length() == 0)) {
    return "Error: missing commands (provide commands=<cmd1;cmd2;...> or command=<cmd>)";
  }
  
  // Validate global condition expression if provided (bare expression, e.g. "ROOM=bedroom")
  // Wrap in IF...THEN to reuse validateConditionSyntax operator/structure checks
  if (condition.length() > 0) {
    condition.trim();
    String wrapped = "IF " + condition + " THEN _";
    const char* conditionError = validateConditionSyntax(wrapped.c_str());
    if (conditionError && conditionError[0] != '\0') {
      EXT_RAM_BSS_ATTR static char errorBuf[192];
      snprintf(errorBuf, sizeof(errorBuf), "Error: Invalid condition expression - %s", conditionError);
      broadcastOutput(errorBuf);
      return errorBuf;
    }
  }
  
  // Validate individual commands
  String combined = cmdsList.length() ? cmdsList : cmdStr;
  int start = 0;
  String s = combined;
  int len = s.length();
  for (int i = 0; i <= len; ++i) {
    if (i == len || s[i] == ';') {
      String part = s.substring(start, i);
      part.trim();
      if (part.length()) {
        // Check if this is a conditional command (IF...THEN..., with or without ELSE/ELSE IF)
        String upperPart = part;
        upperPart.toUpperCase();
        bool isConditional = (upperPart.startsWith("IF ") && upperPart.indexOf(" THEN ") >= 0);
        
        if (isConditional) {
          // Validate as a conditional chain
          const char* validationError = validateConditionalChain(part.c_str());
          if (validationError && validationError[0] != '\0') {
            return validationError;  // an "Error: ..." literal
          }
        } else {
          // Validate this individual command exists in the registry.
          // Using findCommand() instead of recursive executeCommand() — a full
          // executeCommand() call from inside cmd_automation_add (which is
          // itself running inside executeCommand on cmd_exec) doubles the
          // already-deep stack and causes an overflow on 24 KB budgets.
          // findCommand() is a plain registry walk with no stack-heavy setup.
          if (!findCommand(part)) {
            const String safePart = redactAutomationCommandForAudit(part);
            if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
            snprintf(getDebugBuffer(), GLOBAL_DEBUG_BUFFER_SIZE, "Error: Unknown command '%s'",
                     safePart.c_str());
            return getDebugBuffer();
          }
        }
      }
      start = i + 1;
    }
  }
  
  // The numbers the stored trigger takes. The form sends the time, delay and
  // interval fields with every type, so each is checked for the type that
  // uses it; the others are checked whenever they are given.
  long long delayMsValue = 0, intervalMsValue = 0, bootDelayMsValue = 0;
  long long weekIntervalValue = 1, dayOfMonthValue = 0, monthValue = 0;
  
  if (typeNorm == "attime") {
    timeS.trim();
    if (timeS.length() == 0) {
      return "Error: atTime requires time=HH:MM";
    }
    const bool timeForm = timeS.length() == 5 && timeS[2] == ':' &&
                          isdigit(static_cast<unsigned char>(timeS[0])) &&
                          isdigit(static_cast<unsigned char>(timeS[1])) &&
                          isdigit(static_cast<unsigned char>(timeS[3])) &&
                          isdigit(static_cast<unsigned char>(timeS[4]));
    // The scheduler takes 00:00 to 23:59; another time would be stored and never fire.
    if (!timeForm || (timeS[0] - '0') * 10 + (timeS[1] - '0') > 23 ||
        (timeS[3] - '0') * 10 + (timeS[4] - '0') > 59) {
      return "Error: time must be HH:MM from 00:00 to 23:59. Example: time=07:30.";
    }
  } else if (typeNorm == "afterdelay") {
    if (delayMs.length() == 0) {
      return "Error: afterDelay requires numeric delayms (milliseconds)";
    }
    // 0 is what the form sends for its On Boot type.
    if (const char* error = automationAddNumber("delayms", delayMs, 0, 2147483647LL, delayMsValue)) return error;
  } else if (typeNorm == "interval") {
    if (!parseIntArg(intervalMs, intervalMsValue)) {
      return "Error: interval requires numeric intervalms (milliseconds)";
    }
    // Reject sub-second (and 0) rather than clamping: the scheduler stores
    // nextAt in whole seconds, so anything under 1000 ms would reschedule to
    // the same second and rewrite automations.json on every main-loop pass —
    // real flash wear. 0 is a permanently-inert trigger. The web UI's delay
    // unit defaults to milliseconds, so this is easy to hit by accident.
    if (intervalMsValue < 1000) {
      return "Error: intervalms must be at least 1000 (1 second)";
    }
    // The scheduler reads the interval as 32 bits.
    if (intervalMsValue > 4294967295LL) {
      return "Error: intervalms must be a whole number from 1000 to 4294967295.";
    }
  } else if (typeNorm == "event") {
    eventOn.trim();
    if (systemEventKindFromName(eventOn.c_str()) <= 0) {
      String err = "Error: event requires on=<kind>. Valid kinds:";
      for (int k = SYSEVT_NONE + 1; k < SYSEVT_COUNT; k++) {
        err += " ";
        err += systemEventKindName((uint8_t)k);
      }
      if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
      snprintf(getDebugBuffer(), GLOBAL_DEBUG_BUFFER_SIZE, "%s", err.c_str());
      return getDebugBuffer();
    }
  } else {
    return "Error: invalid type (expected atTime|afterDelay|interval|event)";
  }
  
  // Validate boot delay if provided
  if (bootDelayMsStr.length() > 0) {
    if (const char* error = automationAddNumber("bootdelayms", bootDelayMsStr, 0, 2147483647LL, bootDelayMsValue)) return error;
  }

  // The schedule of a time trigger. A value outside its range is refused:
  // left out, it would store a monthly or yearly trigger that never fires.
  if (weekInterval.length() > 0) {
    if (const char* error = automationAddNumber("weekinterval", weekInterval, 1, 52, weekIntervalValue)) return error;
  }
  if (dayOfMonth.length() > 0) {
    if (const char* error = automationAddNumber("dayofmonth", dayOfMonth, 1, 31, dayOfMonthValue)) return error;
  }
  if (monthOfYear.length() > 0) {
    if (const char* error = automationAddNumber("month", monthOfYear, 1, 12, monthValue)) return error;
  }
  if (recurrence.length() > 0) {
    // Stored in lower case, the spelling the scheduler compares with.
    recurrence.toLowerCase();
    if (recurrence != "daily" && recurrence != "weekly" && recurrence != "monthly" && recurrence != "yearly") {
      return "Error: Unknown recurrence. Use daily, weekly, monthly or yearly.";
    }
    if (typeNorm == "attime" && recurrence != "daily" && recurrence != "weekly" && dayOfMonth.length() == 0) {
      return "Error: recurrence=monthly and recurrence=yearly need dayofmonth=1..31.";
    }
    if (typeNorm == "attime" && recurrence == "yearly" && monthOfYear.length() == 0) {
      return "Error: recurrence=yearly needs month=1..12.";
    }
  }

  // id= adds or replaces the automation with that id. Only an id the scheduler
  // addresses is taken: one outside 1..kAutomationIdMax used to be stored and
  // then renumbered by the next scheduler pass (System_AutomationId.h).
  String idOverrideStr = a.value("id");
  static_assert(kAutomationIdMax == 2147483647u, "the reply below names the id range");
  if (idOverrideStr.length() > 0 && !automationIdTextInRange(idOverrideStr.c_str())) {
    return "Error: id must be a whole number from 1 to 2147483647";
  }
  
  const int runAtBootArg = runAtBootStr.length() ? parseBoolArg(runAtBootStr) : 0;
  if (runAtBootArg < 0) return kCommandReplyNotOnOff;
  bool runAtBoot = runAtBootArg == 1;

  // Secondary triggers are checked whole before the store is loaded: one that
  // cannot be stored refuses the add instead of being left out.
  PSRAM_JSON_DOC(secDoc);
  JsonArrayConst secArr;
  const int primaryTriggerCount = runAtBoot ? 2 : 1;
  if (secondaryTriggersJson.length() > 0) {
    if (deserializeJson(secDoc, secondaryTriggersJson) || !secDoc.is<JsonArrayConst>()) {
      return "Error: secondarytriggers is not a JSON array. Send a list of trigger objects, or leave it out.";
    }
    secArr = secDoc.as<JsonArrayConst>();
    if (primaryTriggerCount + (int)secArr.size() > 4) {
      return "Error: Too many triggers. An automation takes at most 4.";
    }
    for (JsonVariantConst sv : secArr) {
      const char* stype = sv["type"] | "";
      const bool isEvent = strcmp(stype, "event") == 0;
      if (!isEvent && strcmp(stype, "time") != 0 && strcmp(stype, "interval") != 0 &&
          strcmp(stype, "manual") != 0 && strcmp(stype, "boot") != 0) {
        if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
        snprintf(getDebugBuffer(), GLOBAL_DEBUG_BUFFER_SIZE,
                 "Error: Unknown trigger type '%.40s'. Use time, interval, manual, boot or event.", stype);
        return getDebugBuffer();
      }
      const char* onKind = sv["on"] | "";
      if (isEvent && systemEventKindFromName(onKind) <= 0) {
        if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
        snprintf(getDebugBuffer(), GLOBAL_DEBUG_BUFFER_SIZE,
                 "Error: Unknown event kind '%.40s'. List the kinds with 'events kinds'.", onKind);
        return getDebugBuffer();
      }
    }
  }
  
  // The store is read, changed and written in one locked step at the end
  // (automationAddMutation): an id= that names a stored automation replaces
  // that entry (after the creator-or-admin check), and a new automation gets
  // an id no stored automation has. The entry is built here first, with the
  // id it was asked for, or 0 until the mutation settles it.
  const bool hasOverrideId = idOverrideStr.length() > 0;
  unsigned long id = hasOverrideId ? strtoul(idOverrideStr.c_str(), nullptr, 10) : 0;

  // Build commands array
  auto buildCommandsArray = [&](const String& csv) {
    String arr = "[";
    int start = 0;
    bool first = true;
    String s = csv;
    int len = s.length();
    for (int i = 0; i <= len; ++i) {
      if (i == len || s[i] == ';') {
        String part = s.substring(start, i);
        part.trim();
        if (part.length()) {
          if (!first) arr += ", ";
          arr += "\"" + jsonEscape(part) + "\"";
          first = false;
        }
        start = i + 1;
      }
    }
    arr += "]";
    return arr;
  };
  
  String commandsJson = buildCommandsArray(combined);
  
  // Normalize legacy type names to v1 trigger type names.
  String triggerType = typeNorm;
  if (triggerType == "attime") triggerType = "time";
  else if (triggerType == "afterdelay") triggerType = "manual";
  else if (triggerType == "onboot") triggerType = "boot";
  // "interval" and "boot" pass through unchanged.

  // Helper: build a single trigger object body (without nextAt) for a given
  // type. Used to construct each element of the triggers array below.
  auto buildPrimaryBody = [&](const String& tt) -> String {
    String b;
    b += "      \"type\": \"" + tt + "\"";
    if (tt == "time") {
      if (timeS.length() > 0) b += ",\n      \"time\": \"" + jsonEscape(timeS) + "\"";
      if (recurrence.length() > 0) b += ",\n      \"recurrence\": \"" + jsonEscape(recurrence) + "\"";
      if (days.length() > 0) b += ",\n      \"days\": \"" + jsonEscape(days) + "\"";
      if (weekIntervalValue > 1) b += ",\n      \"weekInterval\": " + String((long)weekIntervalValue);
      if (dayOfMonth.length() > 0) b += ",\n      \"dayOfMonth\": " + String((long)dayOfMonthValue);
      if (monthOfYear.length() > 0) b += ",\n      \"month\": " + String((long)monthValue);
    } else if (tt == "manual") {
      if (delayMs.length() > 0) b += ",\n      \"delayMs\": " + String((long)delayMsValue);
    } else if (tt == "interval") {
      if (intervalMs.length() > 0) b += ",\n      \"intervalMs\": " + String((unsigned long)intervalMsValue);
    } else if (tt == "boot") {
      if (bootDelayMsStr.length() > 0) b += ",\n      \"bootDelayMs\": " + String((long)bootDelayMsValue);
    } else if (tt == "event") {
      b += ",\n      \"on\": \"" + jsonEscape(eventOn) + "\"";
      if (eventMatch.length() > 0) b += ",\n      \"match\": \"" + jsonEscape(eventMatch) + "\"";
    }
    return b;
  };

  // Compute initial nextAt for a given trigger body (wraps into a temporary
  // automation object so computeNextRunTime can parse it).
  auto computeInitialNextAt = [&](const String& body) -> time_t {
    // No next run without a set clock: time() then counts up from 1970, and
    // a next run computed from it would be stored as a date. The scheduler
    // pass gives the trigger its first run once the clock is set
    // (automationSeedFirstRuns).
    time_t now = time(nullptr);
    if (!Clock::isValidEpoch(now)) return 0;
    String tmp = "{\"triggers\":[{\n" + body + "\n    }]}";
    return computeNextRunTime(tmp.c_str(), now);
  };

  // Build the triggers array. Phase 1: one primary trigger, plus an optional
  // boot trigger if the user checked "Run at Boot" on a non-boot primary.
  // Phase 2 will add UI support for arbitrary multi-trigger.
  String triggersJson = "  \"triggers\": [\n";
  {
    String body = buildPrimaryBody(triggerType);
    time_t nextAt = computeInitialNextAt(body);
    if (nextAt > 0) body += ",\n      \"nextAt\": " + String((unsigned long)nextAt);
    else              body += ",\n      \"nextAt\": null";
    triggersJson += "    {\n" + body + "\n    }";
    DEBUG_AUTOMATIONSF("[autos add] primary trigger type=%s nextAt=%lu",
           triggerType.c_str(), (unsigned long)nextAt);
  }
  if (runAtBoot && triggerType != "boot") {
    String body = buildPrimaryBody("boot");
    triggersJson += ",\n    {\n" + body + ",\n      \"nextAt\": null\n    }";
    DEBUG_AUTOMATIONSF("[autos add] added synthesized boot trigger from runAtBoot");
  }

  // Append the secondary triggers (from the UI's + Add Trigger rows), checked
  // above. Each is a JSON object in the same shape as the primary trigger
  // would produce — we just serialize it back in and compute nextAt per entry.
  if (!secArr.isNull()) {
    int addedSec = 0;
    for (JsonVariantConst sv : secArr) {
      const char* stype = sv["type"] | "";
      String body;
      body += "      \"type\": \"" + String(stype) + "\"";
      if (strcmp(stype, "time") == 0) {
        const char* tval = sv["time"] | "";
        if (tval[0]) body += ",\n      \"time\": \"" + String(tval) + "\"";
        const char* rec = sv["recurrence"] | "";
        if (rec[0]) body += ",\n      \"recurrence\": \"" + String(rec) + "\"";
        const char* daysV = sv["days"] | "";
        if (daysV[0]) body += ",\n      \"days\": \"" + String(daysV) + "\"";
      } else if (strcmp(stype, "interval") == 0) {
        const uint32_t ims = sv["intervalMs"] | uint32_t(0);
        if (ims > 0) body += ",\n      \"intervalMs\": " + String(ims);
      } else if (strcmp(stype, "manual") == 0) {
        long dms = sv["delayMs"] | 0;
        body += ",\n      \"delayMs\": " + String(dms);
      } else if (strcmp(stype, "boot") == 0) {
        long bms = sv["bootDelayMs"] | 0;
        body += ",\n      \"bootDelayMs\": " + String(bms);
      } else {  // event
        const char* onV = sv["on"] | "";
        body += ",\n      \"on\": \"" + jsonEscape(String(onV)) + "\"";
        const char* matchV = sv["match"] | "";
        if (matchV[0]) body += ",\n      \"match\": \"" + jsonEscape(String(matchV)) + "\"";
      }
      // The initial nextAt of this secondary trigger, by the primary's rule:
      // none while the clock is not set (the scheduler pass gives it its
      // first run once it is).
      const time_t snext = computeInitialNextAt(body);
      if (snext > 0) body += ",\n      \"nextAt\": " + String((unsigned long)snext);
      else              body += ",\n      \"nextAt\": null";
      triggersJson += ",\n    {\n" + body + "\n    }";
      addedSec++;
    }
    if (addedSec > 0) {
      DEBUG_AUTOMATIONSF("[autos add] appended %d secondary trigger(s)", addedSec);
    }
  }

  triggersJson += "\n  ]";

  // Build final automation object
  String obj = "{\n";
  String createdBy = currentExecUser();
  DEBUG_AUTOMATIONSF("[autos add] Storing automation id=%ld name='%s' createdBy='%s'", id, name.c_str(), createdBy.c_str());
  obj += "  \"id\": " + String(id) + ",\n";
  obj += "  \"name\": \"" + jsonEscape(name) + "\",\n";
  obj += "  \"createdBy\": \"" + jsonEscape(createdBy) + "\",\n";
  obj += "  \"enabled\": " + String(enabled ? "true" : "false") + ",\n";
  if (condition.length() > 0) obj += "  \"condition\": \"" + jsonEscape(condition) + "\",\n";
  obj += triggersJson + ",\n";
  obj += "  \"commands\": " + commandsJson + "\n";
  obj += "}";
  // The built entry has to be one JSON object before it may go near the
  // store: text that does not parse is refused here, never spliced in.
  PSRAM_JSON_DOC(entryDoc);
  if (deserializeJson(entryDoc, obj) || entryDoc.overflowed() || !entryDoc.is<JsonObject>()) {
    return "Error: the automation could not be built from these arguments";
  }
  obj = "";

  // One locked read-modify-write. In validate mode the mutation runs (so a
  // refused file or a missing permission is still reported) and nothing is
  // written, not even the empty store of a first add.
  AutomationAddContext ctx{entryDoc.as<JsonObjectConst>(), hasOverrideId, (long)id,
                           currentExecUser(), currentExecIsAdmin(), nullptr, &combined};
  String error;
  const AutomationsMutateResult result = mutateAutomationsJson(
      "automation.add", automationAddMutation, &ctx,
      (uint8_t)(kAutomationsMutateNotify | kAutomationsMutateSeed |
                (validateOnly ? kAutomationsMutateDryRun : 0)),
      error);
  if (result == AutomationsMutateResult::Malformed) return "Error: malformed automations.json";
  if (const char* reply = automationMutateErrorReply(result, ctx.reply)) return reply;
  id = (unsigned long)ctx.id;

  RETURN_VALID_IF_VALIDATE_CSTR();

  DEBUG_AUTOMATIONSF("[autos add] wrote automations.json id=%lu", id);
  // The stored triggers were rebuilt with fresh next runs: an uptime arm or
  // interval phase of the old ones goes too (AutomationUptimeSchedule).
  automationUptimeForget((long)id);
  
  DEBUG_AUTOMATIONSF("[autos add] scheduler refresh queued (type=%s)", typeNorm.c_str());
  
  // SYSTEM-EVENT: a new automation was created and persisted. Edits (id= reuse)
  // share this path but are not creations, so post only on a genuine add.
  if (idOverrideStr.length() == 0) {
    systemEventPost(SYSEVT_AUTOMATION_ADDED, name.c_str());
  }

  BROADCAST_PRINTF("%s automation id=%ld name=%s", idOverrideStr.length() > 0 ? "Updated" : "Added", id, name.c_str());
  return "OK";
}

// The reply of enable, disable, delete and run to an id= that is not an id.
static_assert(kAutomationIdMax == 2147483647u, "the reply below names the id range");
static const char kAutomationIdArgReply[] =
    "Error: id must be a whole number from 1 to 2147483647. List the ids with 'automation list'.";

// What a command replies when its mutation did not go through: the mutator's
// own refusal (a fixed reply text), else the reason the owner stopped.
// nullptr when the mutation was applied or changed nothing.
static const char* automationMutateErrorReply(AutomationsMutateResult result, const char* refusal) {
  switch (result) {
    case AutomationsMutateResult::Changed:
    case AutomationsMutateResult::Unchanged:
      return nullptr;
    case AutomationsMutateResult::Refused:
      return refusal ? refusal : "Error: automation not changed";
    case AutomationsMutateResult::ReadFailed:
      return automationsReadErrorReply();
    case AutomationsMutateResult::Malformed:
      return "Error: malformed JSON";
    case AutomationsMutateResult::WriteFailed:
      break;
  }
  return "Error: failed to write automations.json";
}

// Only the creator or an admin changes an automation. The creator comes from
// the parsed "createdBy", so a quote or brace in a name or command cannot
// move it.
static bool automationCallerMayChange(JsonObjectConst automation, const String& caller, bool callerIsAdmin) {
  if (callerIsAdmin) return true;
  const char* createdBy = automation["createdBy"] | "";
  return caller == createdBy;
}

struct AutomationEnableContext {
  long id;
  bool enable;
  String caller;
  bool callerIsAdmin;
  const char* reply;  // the refusal, when the mutator fails
};

static AutomationsMutation automationEnableMutation(JsonDocument& doc, JsonArray automations,
                                                    void* opaque, String& errorOut) {
  (void)doc;
  (void)errorOut;
  AutomationEnableContext* ctx = static_cast<AutomationEnableContext*>(opaque);
  JsonObject automation = findAutomationById(automations, ctx->id);
  if (automation.isNull()) {
    ctx->reply = "Error: automation id not found";
    return AutomationsMutation::Failed;
  }
  DEBUG_AUTOMATIONSF("[autos %s] id=%ld createdBy='%s' requestedBy='%s' isAdmin=%d",
         ctx->enable ? "enable" : "disable", ctx->id, automation["createdBy"] | "",
         ctx->caller.c_str(), ctx->callerIsAdmin);
  if (!automationCallerMayChange(automation, ctx->caller, ctx->callerIsAdmin)) {
    ctx->reply = "Error: You do not have permission to modify this automation. Only the creator or an admin can modify it.";
    return AutomationsMutation::Failed;
  }
  // The flag of this automation only: an entry stored without one is refused
  // as before, and never answered from the next automation's flag.
  JsonVariant enabled = automation["enabled"];
  if (!enabled.is<bool>()) {
    ctx->reply = "Error: malformed automation";
    return AutomationsMutation::Failed;
  }
  if (enabled.as<bool>() == ctx->enable) return AutomationsMutation::Unchanged;
  automation["enabled"] = ctx->enable;
  return AutomationsMutation::Changed;
}

struct AutomationDisableOwnedContext {
  const char* owner;
  int disabled;
};

static AutomationsMutation automationDisableOwnedMutation(JsonDocument& doc, JsonArray automations,
                                                          void* opaque, String& errorOut) {
  (void)doc;
  (void)errorOut;
  AutomationDisableOwnedContext* ctx = static_cast<AutomationDisableOwnedContext*>(opaque);
  for (JsonObject automation : automations) {
    const char* createdBy = automation["createdBy"] | "";
    // Exact, case-sensitive, as steps are run (the stored name is the
    // identity the step executes under). An entry with no creator is nobody's.
    if (!createdBy[0] || strcmp(createdBy, ctx->owner) != 0) continue;
    if (!(automation["enabled"] | false)) continue;
    automation["enabled"] = false;
    ctx->disabled++;
  }
  return ctx->disabled > 0 ? AutomationsMutation::Changed : AutomationsMutation::Unchanged;
}

// Disables every enabled automation whose createdBy is `username` (exact
// match), in one locked write. For `userdelete`: automation steps run as their
// creator, so a later account with the same name would otherwise inherit the
// deleted user's automations. They stay in the file, disabled, and nothing
// re-enables them. Returns how many were disabled, or -1 when automations.json
// could not be read, parsed or written (nothing was changed then).
int automationDisableOwnedBy(const String& username) {
  if (username.length() == 0) return 0;
  AutomationDisableOwnedContext ctx{username.c_str(), 0};
  String error;
  const AutomationsMutateResult result = mutateAutomationsJson(
      "automation.ownerRemoved", automationDisableOwnedMutation, &ctx, kAutomationsMutateNotify, error);
  if (result == AutomationsMutateResult::Changed) {
    logSystemEvent("AUTO", "%d automation(s) of removed user '%s' disabled", ctx.disabled, username.c_str());
    return ctx.disabled;
  }
  if (result == AutomationsMutateResult::Unchanged) return 0;
  // No file at all is "no automations", not a failure.
  if (result == AutomationsMutateResult::ReadFailed && sAutomationsRefusedVersion.load() == 0 &&
      !VFS::existsGuarded(AUTOMATIONS_JSON_FILE, VFS::systemAuth(VFS::Scopes::SYSTEM_STATE, "automation.state"))) {
    return 0;
  }
  return -1;
}

const char* cmd_automation_enable_disable(const String& argsInput, bool enable) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  CommandArgs a(argsInput);
  String idStr = a.value("id");
  if (idStr.length() == 0) {
    return enable ? "Usage: automation enable id=<id>" : "Usage: automation disable id=<id>";
  }
  long id = 0;
  if (!parseIntArg(idStr, 1, kAutomationIdMax, id)) return kAutomationIdArgReply;

  // One locked read-modify-write, the automation found by its numeric id.
  AutomationEnableContext ctx{id, enable, currentExecUser(), currentExecIsAdmin(), nullptr};
  String error;
  const AutomationsMutateResult result = mutateAutomationsJson(
      enable ? "automation.enable" : "automation.disable", automationEnableMutation, &ctx,
      kAutomationsMutateNotify, error);
  if (const char* reply = automationMutateErrorReply(result, ctx.reply)) return reply;

  BROADCAST_PRINTF("%s automation id=%ld", enable ? "Enabled" : "Disabled", id);
  return "OK";
}

struct AutomationDeleteContext {
  long id;
  String caller;
  bool callerIsAdmin;
  const char* reply;  // the refusal, when the mutator fails
  char name[64];      // the removed automation's display name, for its event
};

static AutomationsMutation automationDeleteMutation(JsonDocument& doc, JsonArray automations,
                                                    void* opaque, String& errorOut) {
  (void)doc;
  (void)errorOut;
  AutomationDeleteContext* ctx = static_cast<AutomationDeleteContext*>(opaque);
  size_t index = 0;
  JsonObject automation = findAutomationById(automations, ctx->id, &index);
  if (automation.isNull()) {
    ctx->reply = "Error: automation id not found";
    return AutomationsMutation::Failed;
  }
  DEBUG_AUTOMATIONSF("[autos delete] id=%ld createdBy='%s' requestedBy='%s' isAdmin=%d",
         ctx->id, automation["createdBy"] | "", ctx->caller.c_str(), ctx->callerIsAdmin);
  if (!automationCallerMayChange(automation, ctx->caller, ctx->callerIsAdmin)) {
    ctx->reply = "Error: You do not have permission to delete this automation. Only the creator or an admin can delete it.";
    return AutomationsMutation::Failed;
  }
  // The display name, copied before the entry goes, is the event's subject.
  snprintf(ctx->name, sizeof(ctx->name), "%s", automation["name"] | "");
  automations.remove(index);
  return AutomationsMutation::Changed;
}

const char* cmd_automation_delete(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  CommandArgs a(argsInput);
  String idStr = a.value("id");
  if (idStr.length() == 0) {
    return "Usage: automation delete id=<id>";
  }
  long id = 0;
  if (!parseIntArg(idStr, 1, kAutomationIdMax, id)) return kAutomationIdArgReply;

  // One locked read-modify-write, the automation found by its numeric id and
  // removed from the parsed array.
  AutomationDeleteContext ctx{id, currentExecUser(), currentExecIsAdmin(), nullptr, {0}};
  String error;
  const AutomationsMutateResult result = mutateAutomationsJson(
      "automation.delete", automationDeleteMutation, &ctx, kAutomationsMutateNotify, error);
  if (const char* reply = automationMutateErrorReply(result, ctx.reply)) return reply;

  // Its uptime arm or interval phase goes with it (AutomationUptimeSchedule).
  automationUptimeForget(id);

  // SYSTEM-EVENT: automation removed and the change persisted.
  systemEventPost(SYSEVT_AUTOMATION_DELETED, ctx.name);

  BROADCAST_PRINTF("Deleted automation id=%ld", id);
  return "OK";
}

const char* cmd_automation_run(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  CommandArgs a(argsInput);
  String idStr = a.value("id");
  if (idStr.length() == 0) {
    return "Usage: automation run id=<id>";
  }
  long id = 0;
  if (!parseIntArg(idStr, 1, kAutomationIdMax, id)) return kAutomationIdArgReply;
  // The id in the form the lines below print.
  idStr = String(id);
  
  String json;
  if (!readAutomationsJson(json)) {
    return automationsReadErrorReply();
  }
  
  // The automation with exactly this id, not the first whose id starts with it.
  int idPos = automationIdPosInJson(json, (unsigned long)id);
  if (idPos < 0) {
    return "Error: automation id not found";
  }
  
  int objStart = json.lastIndexOf('{', idPos);
  if (objStart < 0) {
    return "Error: malformed automations.json (objStart)";
  }
  
  // The same string-aware matcher the boot pass and the scheduler use: a
  // brace inside a name or command does not end the object.
  const int objEnd = findJsonObjectEnd(json, objStart);
  if (objEnd < 0) {
    return "Error: malformed automations.json (objEnd)";
  }
  
  String obj = json.substring(objStart, objEnd + 1);
  
  // Extract automation name for logging
  String autoName = "Unknown";
  int namePos = obj.indexOf("\"name\"");
  if (namePos >= 0) {
    int colonPos = obj.indexOf(':', namePos);
    if (colonPos >= 0) {
      int q1 = obj.indexOf('"', colonPos + 1);
      int q2 = obj.indexOf('"', q1 + 1);
      if (q1 >= 0 && q2 >= 0) {
        autoName = obj.substring(q1 + 1, q2);
      }
    }
  }
  
  // Log automation start if logging is active
  if (automationLogActive()) {
    char startBuf[256];
    snprintf(startBuf, sizeof(startBuf), "Automation started: ID=%s Name=%s User=%s", idStr.c_str(), autoName.c_str(), currentExecUser().c_str());
    appendAutoLogEntry("AUTO_START", startBuf);
  }
  
  // Extract commands array (preferred) or single command (fallback)
  int cmdsPos = obj.indexOf("\"commands\"");
  bool haveArray = false;
  int arrStart = -1, arrEnd = -1;
  
  if (cmdsPos >= 0) {
    int colon = obj.indexOf(':', cmdsPos);
    if (colon > 0) {
      arrStart = obj.indexOf('[', colon);
      if (arrStart > 0) {
        int depth = 0;
        for (int i = arrStart; i < (int)obj.length(); ++i) {
          char c = obj[i];
          if (c == '[') depth++;
          else if (c == ']') {
            depth--;
            if (depth == 0) {
              arrEnd = i;
              break;
            }
          }
        }
        haveArray = (arrStart > 0 && arrEnd > arrStart);
      }
    }
  }
  
  static constexpr int MAX_AUTO_CMDS = 16;  // 16 commands per automation (saves ~576B stack vs 64)
  String cmdsList[MAX_AUTO_CMDS];
  int cmdsCount = 0;
  
  if (haveArray) {
    String body = obj.substring(arrStart + 1, arrEnd);
    int i = 0;
    while (i < (int)body.length() && cmdsCount < MAX_AUTO_CMDS) {
      while (i < (int)body.length() && (body[i] == ' ' || body[i] == ',' || body[i] == '\n' || body[i] == '\r' || body[i] == '\t')) i++;
      if (i >= (int)body.length()) break;
      if (body[i] == '"') {
        int q1 = i;
        int q2 = body.indexOf('"', q1 + 1);
        if (q2 < 0) break;
        String one = body.substring(q1 + 1, q2);
        one.trim();
        if (one.length() && cmdsCount < MAX_AUTO_CMDS) { cmdsList[cmdsCount++] = one; }
        i = q2 + 1;
      } else {
        int next = body.indexOf(',', i);
        if (next < 0) break;
        i = next + 1;
      }
    }
  } else {
    int cpos = obj.indexOf("\"command\"");
    if (cpos < 0) {
      return "Error: no command(s) found";
    }
    int ccolon = obj.indexOf(':', cpos);
    int cq1 = obj.indexOf('"', ccolon + 1);
    int cq2 = obj.indexOf('"', cq1 + 1);
    if (cq1 < 0 || cq2 < 0) {
      return "Error: bad command field";
    }
    String cmd = obj.substring(cq1 + 1, cq2);
    cmd.trim();
    if (cmd.length() && cmdsCount < MAX_AUTO_CMDS) { cmdsList[cmdsCount++] = cmd; }
  }
  
  if (cmdsCount == 0) {
    return "Error: no commands to run";
  }
  
  // Check global condition expression (new schema: just the expression, e.g. "ROOM=bedroom")
  String condition = "";
  {
    int condPos = obj.indexOf("\"condition\"");
    // Ensure it's not "conditions" (old plural key)
    if (condPos >= 0 && obj[condPos + 11] == '"') condPos = -1;
    if (condPos >= 0) {
      int condColon = obj.indexOf(':', condPos);
      if (condColon >= 0) {
        int condQ1 = obj.indexOf('"', condColon + 1);
        int condQ2 = obj.indexOf('"', condQ1 + 1);
        if (condQ1 >= 0 && condQ2 >= 0) {
          condition = obj.substring(condQ1 + 1, condQ2);
          condition.trim();
        }
      }
    }
  }

  // Evaluate global condition if present
  if (condition.length() > 0) {
    String wrapped = "IF " + condition + " THEN _";
    bool conditionMet = evaluateCondition(wrapped.c_str());
    DEBUG_AUTOMATIONSF("[autos run] id=%s condition='%s' result=%s",
           idStr.c_str(), condition.c_str(), conditionMet ? "TRUE" : "FALSE");
    if (!conditionMet) {
      if (automationLogActive()) {
        char skipBuf[256];
        snprintf(skipBuf, sizeof(skipBuf), "Automation skipped: ID=%s Name=%s Condition not met: %s", idStr.c_str(), autoName.c_str(), condition.c_str());
        appendAutoLogEntry("AUTO_SKIP", skipBuf);
      }
      // Nothing ran: the caller is told so, with the condition that held it back.
      EXT_RAM_BSS_ATTR static char skipReply[224];
      snprintf(skipReply, sizeof(skipReply),
               "%s (%.120s). Run it again when the condition holds.",
               kAutomationConditionSkipLead, condition.c_str());
      return skipReply;
    }
  }
  
  char _createdByBuf[64];
  extractJsonString(obj.c_str(), "\"createdBy\"", _createdByBuf, sizeof(_createdByBuf));
  DEBUG_AUTOMATIONSF("[autos run] Extracted createdBy='%s' for automation id=%s", _createdByBuf, idStr.c_str());

  // Authorization: non-admins may only trigger automations they created themselves
  if (!currentExecIsAdmin() && String(_createdByBuf) != currentExecUser()) {
    DEBUG_AUTOMATIONSF("[autos run] AUTHORIZATION DENIED: user='%s' isAdmin=%d tried to run automation created by '%s'",
           currentExecUser().c_str(), currentExecIsAdmin(), _createdByBuf);
    // Leads with the executor's admin refusal, so callers treat it as one.
    EXT_RAM_BSS_ATTR static char authReply[192];
    snprintf(authReply, sizeof(authReply),
             "%s to run an automation created by %s. Ask an admin or its creator to run it.",
             kCommandReplyAdminRequired, _createdByBuf);
    return authReply;
  }
  DEBUG_AUTOMATIONSF("[autos run] AUTHORIZATION OK: user='%s' isAdmin=%d running automation created by '%s'",
         currentExecUser().c_str(), currentExecIsAdmin(), _createdByBuf);

  // Execute all commands (with conditional logic support)
  int notRun = 0;
  // The reason of the first step that did not run. The step's reply lives in
  // a static buffer that the next step overwrites, so it is copied here.
  char firstReason[128] = "";
  for (int ci = 0; ci < cmdsCount; ++ci) {
    const String safeRunCmd = redactAutomationCommandForAudit(cmdsList[ci]);
    DEBUG_AUTOMATIONSF("[autos run] id=%s cmd[%d]='%s'", idStr.c_str(), ci, safeRunCmd.c_str());
    
    // Protect against malformed commands
    if (cmdsList[ci].length() == 0 || cmdsList[ci] == "\\") {
      DEBUG_AUTOMATIONSF("[autos run] skipping malformed command: '%s'", safeRunCmd.c_str());
      continue;
    }
    
    // Queue command for execution (async, non-blocking)
    const char* result = executeConditionalCommand(cmdsList[ci].c_str(), _createdByBuf, autoName.c_str());
    // A step that did not run: it never reached the executor, or the stored
    // step itself is not a usable command (a chain without THEN, a branch over
    // the length limit). Its reason is printed below and the first one goes
    // back to the caller, who does not get the printed lines.
    if (commandReplyIsFailure(result)) {
      if (notRun == 0) snprintf(firstReason, sizeof(firstReason), "%s", commandReplyReason(result));
      notRun++;
    }
    
    // Output the result (skip internal status messages - actual output comes from queue)
    if (!isAutoInternalResult(result)) {
      {
        char autoBuf[256];
        snprintf(autoBuf, sizeof(autoBuf), "[Automation %s] %s", idStr.c_str(), result);
        broadcastOutput(autoBuf);
      }
    }
  }

  // SYSTEM-EVENT: manual automation run dispatched its command list.
  systemEventPost(SYSEVT_AUTOMATION_FIRED, autoName.c_str(), "manual");
  
  // Advance nextAt after manual execution via the unified post-fire helper.
  // Without a set clock `now` is the uptime and the uptime schedule advances.
  const time_t now = automationScheduleNow();
  rescheduleAfterFire(id, obj.c_str(), now, true);
  
  // Log automation end if logging is active
  if (automationLogActive()) {
    char endBuf[256];
    snprintf(endBuf, sizeof(endBuf), "Automation completed: ID=%s Name=%s Commands=%d", idStr.c_str(), autoName.c_str(), cmdsCount);
    appendAutoLogEntry("AUTO_END", endBuf);
  }
  
  if (notRun > 0) {
    EXT_RAM_BSS_ATTR static char partialReply[256];
    snprintf(partialReply, sizeof(partialReply),
             "Error: %d of %d commands did not run. First reason: %s",
             notRun, cmdsCount, firstReason);
    return partialReply;
  }

  char resultBuf[128];
  snprintf(resultBuf, sizeof(resultBuf), "Ran automation id=%s (%d command%s)", idStr.c_str(), cmdsCount, cmdsCount == 1 ? "" : "s");
  broadcastOutput(resultBuf);
  return "OK";
}

// Arm an afterDelay automation's timer: nextAt = now + delayMs/1000, saved in
// the file; without a set clock it counts uptime in RAM instead
// (AutomationUptimeSchedule). No commands run here; the scheduler fires them
// when the delay elapses.
const char* cmd_automation_trigger(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  CommandArgs a(argsInput);
  String idStr = a.value("id");
  if (idStr.length() == 0) {
    return "Usage: automation trigger id=<id>";
  }
  // An armed timer fires only from a running scheduler.
  if (!automationSchedulerRunning()) {
    return "Error: The automation scheduler is not running. Run 'automation system enable' first.";
  }

  String json;
  if (!readAutomationsJson(json)) {
    return automationsReadErrorReply();
  }

  PSRAM_JSON_DOC(doc);
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    return "Error: automations.json parse error";
  }

  JsonArray automations = doc["automations"].as<JsonArray>();
  if (automations.isNull()) {
    return "Error: automations.json missing automations array";
  }

  long id = 0;
  if (!parseIntArg(idStr, id)) return kCommandReplyNotANumber;
  JsonObject target;
  for (JsonObject obj : automations) {
    if (obj["id"].as<long>() == id) { target = obj; break; }
  }
  if (target.isNull()) {
    return "Error: automation id not found";
  }
  EXT_RAM_BSS_ATTR static char reply[160];
  // The scheduler skips a disabled automation, armed or not.
  if (!(target["enabled"] | false)) {
    snprintf(reply, sizeof(reply),
             "Error: Automation %ld is disabled. Run 'automation enable id=%ld' first.", id, id);
    return reply;
  }

  // Find the first manual trigger in the automation's triggers array.
  JsonArrayConst triggers = target["triggers"].as<JsonArrayConst>();
  if (triggers.isNull()) {
    return "Error: automation has no triggers array";
  }
  int manualIdx = -1;
  int manualDelayMs = 0;
  int idx = 0;
  for (JsonVariantConst t : triggers) {
    const char* tt = t["type"] | "";
    if (strcmp(tt, "manual") == 0) {
      manualIdx = idx;
      manualDelayMs = t["delayMs"] | 0;
      break;
    }
    idx++;
  }
  if (manualIdx < 0) {
    return "Error: automation has no manual trigger to arm";
  }
  if (manualDelayMs <= 0) {
    return "Error: manual trigger has no valid delayMs";
  }

  time_t now = time(nullptr);
  int delayMs = manualDelayMs;  // alias for the log message below
  if (!Clock::isValidEpoch(now)) {
    // No set clock: the delay counts uptime and is armed in RAM, not in the
    // file (AutomationUptimeSchedule). It is lost on a restart.
    const time_t at = automationUptimeNow() + (manualDelayMs / 1000);
    if (manualIdx >= 255 || !automationUptimeArm(id, (uint8_t)manualIdx, at)) {
      static_assert(AutomationUptimeSchedule::kEntries == 32, "the reply below names the table size");
      return "Error: cannot arm while the clock is not set: all 32 uptime timers are in use";
    }
  } else {
    time_t nextAt = now + (manualDelayMs / 1000);
    if (!updateAutomationTriggerNextAt(id, manualIdx, nextAt)) {
      return "Error: failed to update nextAt";
    }
    // An arm from before the clock was set is replaced by this one.
    if (manualIdx < 255) automationUptimeAdvance(id, (uint8_t)manualIdx, 0);
  }

  // Wake the scheduler so it re-reads the file and picks up the armed nextAt.
  // Without this, the armed automation waits up to one tick interval before
  // the scheduler notices.
  notifyAutomationScheduler();

  snprintf(reply, sizeof(reply), "Armed automation id=%ld (fires in %d ms)", id, delayMs);
  cliHint("the commands run later when the timer elapses, not now - to capture what they output, start 'autolog start <file>' first and read it back with 'fileview \"<file>\"'");
  return reply;
}

// The reply of `automation sanitize` when it changed the file: what it fixed,
// with counts. "Sanitized automations.json: fixed 2 duplicate IDs",
// "... fixed 1 out-of-range ID", "... fixed 1 duplicate ID and 3 out-of-range
// IDs". An out-of-range id is one above kAutomationIdMax (System_AutomationId.h).
static const char* automationSanitizeReply(const AutomationSanitizeCounts& counts) {
  // 34 + 39 + 5 + 39 + 1 bytes at most (two 10-digit counts).
  EXT_RAM_BSS_ATTR static char reply[128];
  char duplicates[40] = "";
  char outOfRange[40] = "";
  if (counts.duplicates > 0) {
    snprintf(duplicates, sizeof(duplicates), "%d duplicate ID%s", counts.duplicates,
             counts.duplicates == 1 ? "" : "s");
  }
  if (counts.outOfRange > 0) {
    snprintf(outOfRange, sizeof(outOfRange), "%d out-of-range ID%s", counts.outOfRange,
             counts.outOfRange == 1 ? "" : "s");
  }
  if (!duplicates[0] && !outOfRange[0]) return "Sanitized automations.json: fixed IDs";
  snprintf(reply, sizeof(reply), "Sanitized automations.json: fixed %s%s%s", duplicates,
           duplicates[0] && outOfRange[0] ? " and " : "", outOfRange);
  return reply;
}

// Main automation command dispatcher
const char* cmd_automation(const String& argsInput) {
  CommandArgs a(argsInput);
  String subCmd = a.arg(0);
  subCmd.toLowerCase();
  String subArgs = a.remaining(0);

  // `add` checks its arguments in a validate pass and answers VALID itself;
  // every other sub-command validates as VALID here.
  if (subCmd == "add") return cmd_automation_add(subArgs);
  RETURN_VALID_IF_VALIDATE_CSTR();

  // Handle "system" subcommand: enable | disable | status, with an optional trailing `json` token.
  // Parse the action as the FIRST token (not the whole tail): the companion app sends
  // `automation system <action> json` and parses the reply as {"schema":1,"enabled":bool}, while the
  // web sends the bare command and reads the plain-text line. Matching subArgs whole against "enable"
  // turned the app's `enable json` into "invalid arguments" — the toggle (and the status read on
  // load) silently failed over BLE while working on web.
  if (subCmd == "system") {
    CommandArgs sysArgs(subArgs);
    String action = sysArgs.arg(0);
    action.toLowerCase();
    bool wantJson = false;
    for (int i = 1; i < sysArgs.count(); i++) if (sysArgs.arg(i).equalsIgnoreCase("json")) wantJson = true;

    bool en;
    if (action == "enable" || action == "disable") {
      en = action == "enable";
      if (const char* error = automationSetSystemEnabled(en)) return error;
    } else if (action == "status") {
      en = automationSchedulerRunning();
    } else {
      return "Error: invalid arguments — Usage: automation system <enable|disable|status> [json]";
    }

    if (wantJson) {
      static String sysJson;
      sysJson  = "{\"schema\":1,\"enabled\":";
      sysJson += (en ? "true" : "false");
      sysJson += "}";
      return sysJson.c_str();
    }
    return en ? "Automation system: enabled" : "Automation system: disabled";
  }

  // Handle regular automation commands
  if (subCmd == "list") {
    return cmd_automation_list(subArgs);
  } else if (subCmd == "enable") {
    return cmd_automation_enable_disable(subArgs, true);
  } else if (subCmd == "disable") {
    return cmd_automation_enable_disable(subArgs, false);
  } else if (subCmd == "delete") {
    return cmd_automation_delete(subArgs);
  } else if (subCmd == "sanitize") {
    // Read, repair and write back under one lock.
    AutomationSanitizeCounts fixed;
    bool repaired = false;
    {
      FsLockGuard storeLock("automation.sanitize");
      String probe;
      if (!readAutomationsJson(probe)) {
        return automationsReadErrorReply();
      }
      probe = "";
      if (!automationsSanitizeStore(&fixed, &repaired)) return "Error: failed to write automations.json";
    }
    if (repaired) {
      DEBUG_AUTOMATIONSF("[autos] CLI sanitize: fixed duplicate IDs; scheduler refresh queued");
      return automationSanitizeReply(fixed);
    } else {
      DEBUG_AUTOMATIONSF("[autos] CLI sanitize: no duplicate IDs found");
      return "Sanitize: no changes needed";
    }
  } else if (subCmd == "recompute") {
    time_t now = time(nullptr);
    if (!Clock::isValidEpoch(now)) {
      return "Error: the clock is not set, so next run times cannot be recomputed. Set it with NTP or 'timeset' and retry.";
    }

    int skipped = 0;
    const int rewritten = automationRecomputeAllClockTriggers(now, &skipped);
    if (rewritten < 0) return "Error: failed to rewrite automations.json";

    notifyAutomationScheduler();
    DEBUG_AUTOMATIONSF("[autos recompute] rewrote %d clock trigger(s), skipped %d",
           rewritten, skipped);
    BROADCAST_PRINTF("Recomputed %d clock trigger(s); %d automation(s) skipped "
                     "(disabled or no schedulable trigger). Armed afterDelay timers left intact.",
                     rewritten, skipped);
    return "OK";
  } else if (subCmd == "run") {
    return cmd_automation_run(subArgs);
  } else if (subCmd == "trigger") {
    return cmd_automation_trigger(subArgs);
  }

  return "Error: Unknown automation command. Use: list, add, enable, disable, delete, run, trigger, sanitize, recompute";
}

// ============================================================================
// Internal trigger model (v2: per-automation array of triggers)
// ============================================================================
// An automation has an array of Triggers (up to MAX_TRIGGERS) that each answer:
// what causes this automation to fire? Multiple triggers are OR-combined — if
// any one is due, the automation fires. After firing, each trigger that was
// due is rescheduled independently; not-yet-due triggers are untouched.
//
// Two functions — `nextFire` and `onFired` — encapsulate the scheduling
// algorithm for a single trigger. Callers iterate arrays via `triggersFromJson`
// and `rescheduleAfterFire`.
//
// Per-trigger `nextAt` is stored inside each element of the `triggers` array
// in the JSON. The cache holds just the min across triggers per automation,
// which is all the fast-due check needs.
//
// Adding a new trigger variant (e.g. condition-based triggers) means adding
// one case to `parseOneTrigger`, one case to `nextFire`, and optionally one
// case to `onFired`. No other code paths need to change.

static constexpr int MAX_TRIGGERS = 4;

struct Trigger {
  enum Type { NONE, TIME, MONTHLY, YEARLY, INTERVAL, MANUAL, BOOT, EVENT };
  Type type = NONE;

  // TIME / MONTHLY / YEARLY share hour+minute
  int hour = -1;            // 0..23
  int minute = -1;          // 0..59

  // TIME (daily/weekly/biweekly)
  uint8_t daysMask = 0;     // bit i => day i (0=Sun..6=Sat); 0 means "any day"
  uint16_t weekInterval = 1; // 1=weekly, 2=biweekly, ...
  time_t anchor = 0;         // first-fire timestamp (set for weekInterval > 1)

  // MONTHLY (dayOfMonth) / YEARLY (monthOfYear + dayOfMonth)
  int dayOfMonth = 0;        // 1..31
  int monthOfYear = 0;       // 1..12

  // MANUAL
  uint32_t delayMs = 0;

  // INTERVAL
  uint32_t intervalMs = 0;

  // BOOT
  uint32_t bootDelayMs = 0;

  // EVENT — fired by the system event register (System_Events.h), never by
  // the clock. evtKind 0 = unknown kind (parses, never matches). Match buffer
  // spans the full detail field so long text_rx patterns aren't silently
  // truncated into broader prefix matches.
  uint8_t evtKind = 0;
  char evtMatch[SYSEVT_DETAIL_LEN + 1] = {};

  // Scheduling state. Saved per trigger inside the JSON, except for a short
  // interval, whose next run is kept in RAM (AutomationVolatileSchedule).
  time_t nextAt = 0;
  // Position in the automation's "triggers" array.
  uint8_t slot = 0;
};

// Convert "mon,wed,fri" (case-insensitive, any whitespace) to a 7-bit mask.
static uint8_t daysCsvToMask(const char* csv) {
  if (!csv || !csv[0]) return 0;
  static const char* names[7] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };
  char buf[128];
  size_t len = strlen(csv);
  if (len >= sizeof(buf) - 1) len = sizeof(buf) - 2;
  size_t j = 0;
  for (size_t i = 0; i < len; i++) {
    char c = csv[i];
    if (c == ' ' || c == '\t') continue;
    if (c >= 'A' && c <= 'Z') c += 32;
    buf[j++] = c;
  }
  buf[j] = '\0';

  uint8_t mask = 0;
  const char* p = buf;
  while (*p) {
    while (*p == ',') p++;
    if (!*p) break;
    for (int d = 0; d < 7; d++) {
      if (strncmp(p, names[d], 3) == 0) {
        mask |= (uint8_t)(1u << d);
        break;
      }
    }
    while (*p && *p != ',') p++;
  }
  return mask;
}

// Parse a single trigger object. Returns false if the object is malformed.
// Used by `triggersFromJson` to iterate the `triggers` array.
static bool parseOneTrigger(JsonVariantConst trig, Trigger& out) {
  out = Trigger();
  if (trig.isNull()) return false;
  const char* type = trig["type"] | "";
  if (!type || !type[0]) return false;

  // Persisted nextAt carries from previous fires.
  unsigned long rawNextAt = trig["nextAt"] | 0UL;
  out.nextAt = (time_t)rawNextAt;

  if (strcmp(type, "time") == 0) {
    const char* timeStr = trig["time"] | "";
    if (strlen(timeStr) != 5 || timeStr[2] != ':') return false;
    int h = (timeStr[0] - '0') * 10 + (timeStr[1] - '0');
    int m = (timeStr[3] - '0') * 10 + (timeStr[4] - '0');
    if (h < 0 || h > 23 || m < 0 || m > 59) return false;
    out.hour = h;
    out.minute = m;

    const char* recur = trig["recurrence"] | "";

    if (strcmp(recur, "monthly") == 0 || strcmp(recur, "Monthly") == 0) {
      int dom = trig["dayOfMonth"] | 0;
      if (dom < 1 || dom > 31) return false;
      out.type = Trigger::MONTHLY;
      out.dayOfMonth = dom;
      return true;
    }
    if (strcmp(recur, "yearly") == 0 || strcmp(recur, "Yearly") == 0) {
      int dom = trig["dayOfMonth"] | 0;
      int moy = trig["month"] | 0;
      if (dom < 1 || dom > 31 || moy < 1 || moy > 12) return false;
      out.type = Trigger::YEARLY;
      out.dayOfMonth = dom;
      out.monthOfYear = moy;
      return true;
    }

    const char* daysStr = trig["days"] | "";
    out.type = Trigger::TIME;
    out.daysMask = daysCsvToMask(daysStr);
    int wi = trig["weekInterval"] | 1;
    out.weekInterval = (wi < 1) ? 1 : (uint16_t)wi;
    unsigned long a = trig["anchor"] | 0UL;
    out.anchor = (time_t)a;
    return true;
  }
  if (strcmp(type, "manual") == 0) {
    out.type = Trigger::MANUAL;
    out.delayMs = trig["delayMs"] | 0;
    return true;
  }
  if (strcmp(type, "interval") == 0) {
    out.type = Trigger::INTERVAL;
    // The default decides the type that is read: through `| 0` (int) an
    // interval above 2^31 ms (24.8 days) came back as 0, "no interval".
    out.intervalMs = trig["intervalMs"] | uint32_t(0);
    return true;
  }
  if (strcmp(type, "boot") == 0) {
    out.type = Trigger::BOOT;
    out.bootDelayMs = trig["bootDelayMs"] | 0;
    return true;
  }
  if (strcmp(type, "event") == 0) {
    out.type = Trigger::EVENT;
    // Unknown "on" names parse as evtKind 0 (never matches) instead of
    // failing: a dropped trigger would shift rescheduleAfterFire's
    // positional nextAt write-back and corrupt sibling triggers.
    int kind = systemEventKindFromName(trig["on"] | "");
    out.evtKind = (kind > 0) ? (uint8_t)kind : 0;
    const char* m = trig["match"] | "";
    strncpy(out.evtMatch, m, sizeof(out.evtMatch) - 1);
    out.evtMatch[sizeof(out.evtMatch) - 1] = '\0';
    out.nextAt = 0;  // events never clock-fire; ignore any stale persisted nextAt
    return true;
  }
  return false;
}

// Parse an automation object's `triggers` array. Fills `out[]` up to
// `maxCount` entries. Returns the number of valid triggers (0 if none).
// `now` is the scheduler's now (automationScheduleNow): an uptime while the
// clock is not set, and then each nextAt is the uptime schedule's
// (AutomationUptimeSchedule) instead of the file's.
static int triggersFromJson(const char* automationJson, Trigger* out, int maxCount, time_t now) {
  if (maxCount <= 0) return 0;
  PSRAM_JSON_DOC(doc);
  if (deserializeJson(doc, automationJson)) return 0;
  JsonArrayConst arr = doc["triggers"].as<JsonArrayConst>();
  if (arr.isNull()) return 0;
  const long id = doc["id"] | 0L;
  const bool onUptime = automationOnUptime(now);
  int n = 0;
  int slot = -1;
  for (JsonVariantConst t : arr) {
    slot++;
    if (n >= maxCount) break;
    Trigger tmp;
    if (parseOneTrigger(t, tmp)) {
      if (slot < 255) {
        tmp.slot = (uint8_t)slot;
        if (onUptime) {
          tmp.nextAt = tmp.type == Trigger::INTERVAL
                           ? automationUptimeIntervalAt(id, tmp.slot, tmp.intervalMs, now)
                       : tmp.type == Trigger::MANUAL ? automationUptimeArmedAt(id, tmp.slot)
                                                     : 0;
        } else if (tmp.type == Trigger::INTERVAL) {
          // Not on uptime here, so `now` is the wall clock and it is set.
          tmp.nextAt = automationVolatileNextAt(id, tmp.slot, tmp.intervalMs, tmp.nextAt,
                                                now, !onUptime);
        }
      } else {
        tmp.slot = 255;  // never matches a RAM entry
        if (onUptime) tmp.nextAt = 0;
      }
      out[n++] = tmp;
    }
  }
  return n;
}

static int triggersFromJson(const char* automationJson, Trigger* out, int maxCount) {
  return triggersFromJson(automationJson, out, maxCount, automationScheduleNow());
}

// ---------------------------------------------------------------------------
// List summaries for the OLED and G2 pages
// ---------------------------------------------------------------------------
// Pages receive copies built here from parseOneTrigger, so a page describes a
// trigger the way the scheduler runs it and never reads automations.json.

// Largest whole unit: 7200000 -> "2h", 5400000 -> "90m", 45000 -> "45s".
static void formatSummaryDuration(char* out, size_t cap, uint32_t ms) {
  if (ms >= 3600000UL && ms % 3600000UL == 0) {
    snprintf(out, cap, "%luh", (unsigned long)(ms / 3600000UL));
  } else if (ms >= 60000UL && ms % 60000UL == 0) {
    snprintf(out, cap, "%lum", (unsigned long)(ms / 60000UL));
  } else {
    snprintf(out, cap, "%lus", (unsigned long)(ms / 1000UL));
  }
}

static AutomationTriggerKind summaryKind(Trigger::Type type) {
  switch (type) {
    case Trigger::TIME:     return AutomationTriggerKind::Time;
    case Trigger::MONTHLY:  return AutomationTriggerKind::Monthly;
    case Trigger::YEARLY:   return AutomationTriggerKind::Yearly;
    case Trigger::INTERVAL: return AutomationTriggerKind::Interval;
    case Trigger::MANUAL:   return AutomationTriggerKind::Manual;
    case Trigger::BOOT:     return AutomationTriggerKind::Boot;
    case Trigger::EVENT:    return AutomationTriggerKind::Event;
    default:                return AutomationTriggerKind::Invalid;
  }
}

// Text for one parsed trigger: "daily 07:30", "weekly 08:00 MoWe",
// "every 2w 07:30 Fr", "monthly 15 07:30", "yearly Mar 9 06:05", "every 5m",
// "delay 10s", "boot", "boot +5s", "on <event>". The time comes before the day
// list so narrow displays that cut the text keep it.
static void formatTriggerSummary(const Trigger& t, char* out, size_t cap) {
  static const char* const kDayCodes[7] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
  static const char* const kMonths[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
  char part[16];
  switch (t.type) {
    case Trigger::TIME: {
      // Monday-first day codes; no days or all seven is every day.
      char days[16] = "";
      const uint8_t mask = t.daysMask & 0x7F;
      if (mask != 0x7F) {
        size_t n = 0;
        for (int i = 1; i <= 7; i++) {
          const int d = i % 7;
          if (mask & (1u << d)) {
            days[n++] = kDayCodes[d][0];
            days[n++] = kDayCodes[d][1];
          }
        }
        days[n] = '\0';
      }
      if (t.weekInterval > 1) {
        snprintf(out, cap, "every %uw %02d:%02d%s%s", (unsigned)t.weekInterval,
                 t.hour, t.minute, days[0] ? " " : "", days);
      } else if (days[0]) {
        snprintf(out, cap, "weekly %02d:%02d %s", t.hour, t.minute, days);
      } else {
        snprintf(out, cap, "daily %02d:%02d", t.hour, t.minute);
      }
      return;
    }
    case Trigger::MONTHLY:
      snprintf(out, cap, "monthly %d %02d:%02d", t.dayOfMonth, t.hour, t.minute);
      return;
    case Trigger::YEARLY:
      snprintf(out, cap, "yearly %s %d %02d:%02d", kMonths[t.monthOfYear - 1],
               t.dayOfMonth, t.hour, t.minute);
      return;
    case Trigger::INTERVAL:
      formatSummaryDuration(part, sizeof(part), t.intervalMs);
      snprintf(out, cap, "every %s", part);
      return;
    case Trigger::MANUAL:
      formatSummaryDuration(part, sizeof(part), t.delayMs);
      snprintf(out, cap, "delay %s", part);
      return;
    case Trigger::BOOT:
      if (t.bootDelayMs == 0) {
        snprintf(out, cap, "boot");
        return;
      }
      formatSummaryDuration(part, sizeof(part), t.bootDelayMs);
      snprintf(out, cap, "boot +%s", part);
      return;
    case Trigger::EVENT:
      snprintf(out, cap, "on %s", t.evtKind ? systemEventKindName(t.evtKind) : "unknown event");
      return;
    default:
      snprintf(out, cap, "invalid trigger");
      return;
  }
}

static void fillAutomationSummary(JsonObjectConst a, AutomationSummary& out) {
  memset(&out, 0, sizeof(out));
  out.id = a["id"].as<long>();
  out.enabled = a["enabled"] | false;
  const char* name = a["name"] | "";
  if (name[0]) snprintf(out.name, sizeof(out.name), "%s", name);
  else snprintf(out.name, sizeof(out.name), "Auto #%ld", out.id);
  const size_t commands = a["commands"].as<JsonArrayConst>().size();
  out.commandCount = (uint16_t)(commands > 0xFFFF ? 0xFFFF : commands);
  JsonArrayConst triggers = a["triggers"].as<JsonArrayConst>();
  const size_t triggerCount = triggers.size();
  out.triggerCount = (uint8_t)(triggerCount > 0xFF ? 0xFF : triggerCount);
  if (triggerCount == 0) {
    out.kind = AutomationTriggerKind::None;
    return;
  }
  // The first trigger the scheduler accepts, in triggersFromJson's order.
  Trigger primary;
  bool parsed = false;
  for (JsonVariantConst t : triggers) {
    if (parseOneTrigger(t, primary)) {
      parsed = true;
      break;
    }
  }
  if (parsed) {
    out.kind = summaryKind(primary.type);
    formatTriggerSummary(primary, out.trigger, sizeof(out.trigger));
  } else {
    out.kind = AutomationTriggerKind::Invalid;
    snprintf(out.trigger, sizeof(out.trigger), "invalid trigger");
  }
  if (triggerCount > 1) {
    const size_t len = strlen(out.trigger);
    snprintf(out.trigger + len, sizeof(out.trigger) - len, " +%u", (unsigned)(triggerCount - 1));
  }
}

bool automationCopySummaries(AutomationSummary* out, size_t maxCount, size_t* count) {
  if (count) *count = 0;
  String json;
  if (!readAutomationsJson(json)) return false;
  PSRAM_JSON_DOC(doc);
  if (deserializeJson(doc, json)) return false;
  JsonArrayConst autos = doc["automations"].as<JsonArrayConst>();
  size_t n = 0;
  for (JsonObjectConst a : autos) {
    if (!out || n >= maxCount) break;
    fillAutomationSummary(a, out[n++]);
  }
  if (count) *count = n;
  return true;
}

bool automationFindSummary(long id, AutomationSummary* out) {
  if (!out) return false;
  String json;
  if (!readAutomationsJson(json)) return false;
  PSRAM_JSON_DOC(doc);
  if (deserializeJson(doc, json)) return false;
  JsonArrayConst autos = doc["automations"].as<JsonArrayConst>();
  for (JsonObjectConst a : autos) {
    if (a["id"].as<long>() != id) continue;
    fillAutomationSummary(a, *out);
    return true;
  }
  return false;
}

// Compute the next firing time at or after `from`, or 0 if there is none.
// afterDelay always returns 0 (manually armed); atTime returns the next
// matching candidate respecting day-of-week and weekInterval filters;
// interval returns `from + intervalMs/1000`.
static time_t nextFire(const Trigger& s, time_t from) {
  switch (s.type) {
    case Trigger::MANUAL:
    case Trigger::BOOT:
    case Trigger::EVENT:
      return 0;  // manually armed / dispatched by runAtBoot or the event matcher, not by the clock
    case Trigger::INTERVAL: {
      if (s.intervalMs == 0) return 0;  // unset/invalid — not schedulable
      // Floor at one second: intervalMs/1000 integer-divides to 0 for any
      // sub-second interval, which reschedules to `from` (still due), and
      // every fire runs rescheduleAfterFire -> writeAutomationsJsonAtomic,
      // i.e. a LittleFS write per main-loop pass. The add paths reject
      // sub-second values; this is the belt for hand-edited JSON.
      time_t step = (time_t)(s.intervalMs / 1000);
      if (step < 1) step = 1;
      return from + step;
    }
    case Trigger::TIME: {
      struct tm tmNow;
      if (!localtime_r(&from, &tmNow)) return 0;
      // Scan today + enough days ahead to cover any weekInterval cycle.
      int maxDays = (s.weekInterval > 1) ? 7 * (s.weekInterval + 1) : 8;
      for (int d = 0; d < maxDays; d++) {
        struct tm t = tmNow;
        t.tm_mday += d;
        t.tm_hour = s.hour;
        t.tm_min = s.minute;
        t.tm_sec = 0;
        t.tm_isdst = -1;
        time_t cand = mktime(&t);
        if (cand <= from) continue;
        struct tm tcheck;
        if (!localtime_r(&cand, &tcheck)) continue;
        if (s.daysMask != 0 && !(s.daysMask & (uint8_t)(1u << tcheck.tm_wday))) continue;
        // weekInterval filter: once an anchor is set, only fire on "on" weeks.
        if (s.weekInterval > 1 && s.anchor > 0) {
          long deltaSec = (long)(cand - s.anchor);
          if (deltaSec < 0) deltaSec = 0;
          long deltaWeeks = deltaSec / (7L * 86400L);
          if ((deltaWeeks % s.weekInterval) != 0) continue;
        }
        return cand;
      }
      return 0;
    }
    case Trigger::MONTHLY: {
      // Fire on dayOfMonth at hour:minute every month. If the month has fewer
      // days than dayOfMonth (Feb 30 etc.), clamp to the last day of that month.
      if (s.dayOfMonth < 1 || s.dayOfMonth > 31) return 0;
      struct tm tmNow;
      if (!localtime_r(&from, &tmNow)) return 0;
      for (int monthOff = 0; monthOff < 2; monthOff++) {
        struct tm t = tmNow;
        t.tm_mon += monthOff;
        t.tm_mday = s.dayOfMonth;
        t.tm_hour = s.hour;
        t.tm_min = s.minute;
        t.tm_sec = 0;
        t.tm_isdst = -1;
        time_t cand = mktime(&t);
        // mktime normalizes out-of-range dayOfMonth by carrying into the next
        // month (e.g. Feb 30 → Mar 2). Detect that and clamp to last-of-month.
        struct tm tcheck;
        if (!localtime_r(&cand, &tcheck)) continue;
        if (tcheck.tm_mday != s.dayOfMonth) {
          // Use day 0 of the month AFTER the target = last day of target month.
          struct tm clamp = tmNow;
          clamp.tm_mon += monthOff + 1;
          clamp.tm_mday = 0;
          clamp.tm_hour = s.hour;
          clamp.tm_min = s.minute;
          clamp.tm_sec = 0;
          clamp.tm_isdst = -1;
          cand = mktime(&clamp);
        }
        if (cand > from) return cand;
      }
      return 0;
    }
    case Trigger::YEARLY: {
      // Fire on month/dayOfMonth at hour:minute every year. Clamp Feb 29 to
      // Feb 28 in non-leap years.
      if (s.dayOfMonth < 1 || s.dayOfMonth > 31) return 0;
      if (s.monthOfYear < 1 || s.monthOfYear > 12) return 0;
      struct tm tmNow;
      if (!localtime_r(&from, &tmNow)) return 0;
      for (int yearOff = 0; yearOff < 2; yearOff++) {
        struct tm t = tmNow;
        t.tm_year += yearOff;
        t.tm_mon = s.monthOfYear - 1;
        t.tm_mday = s.dayOfMonth;
        t.tm_hour = s.hour;
        t.tm_min = s.minute;
        t.tm_sec = 0;
        t.tm_isdst = -1;
        time_t cand = mktime(&t);
        struct tm tcheck;
        if (!localtime_r(&cand, &tcheck)) continue;
        if (tcheck.tm_mday != s.dayOfMonth || tcheck.tm_mon != s.monthOfYear - 1) {
          // Feb 29 in non-leap year etc. — clamp to last day of target month.
          struct tm clamp = tmNow;
          clamp.tm_year += yearOff;
          clamp.tm_mon = s.monthOfYear;  // month AFTER target (0-indexed +1 = 1-indexed target)
          clamp.tm_mday = 0;
          clamp.tm_hour = s.hour;
          clamp.tm_min = s.minute;
          clamp.tm_sec = 0;
          clamp.tm_isdst = -1;
          cand = mktime(&clamp);
        }
        if (cand > from) return cand;
      }
      return 0;
    }
    default:
      return 0;
  }
}

// Mutates `s` after a successful firing. Returns true if `s.anchor` changed
// (caller must persist it). Currently only sets the anchor on the first fire
// of a weekInterval > 1 time trigger.
static bool onFired(Trigger& s, time_t firedAt) {
  if (s.type == Trigger::TIME && s.weekInterval > 1 && s.anchor == 0) {
    s.anchor = firedAt;
    return true;
  }
  return false;
}

struct AutomationRescheduleContext {
  long id;
  const Trigger* triggers;
  int count;
};

// The triggers array of one automation gets the next runs the caller worked
// out. An automation that is gone (deleted since the pass read the file) is
// left alone: nothing is written.
static AutomationsMutation automationRescheduleMutation(JsonDocument& doc, JsonArray automations,
                                                        void* opaque, String& errorOut) {
  (void)doc;
  (void)errorOut;
  const AutomationRescheduleContext* ctx = static_cast<const AutomationRescheduleContext*>(opaque);
  for (JsonObject automation : automations) {
    if (automation["id"].as<long>() != ctx->id) continue;
    JsonArray arr = automation["triggers"].as<JsonArray>();
    if (arr.isNull()) break;
    int idx = 0;
    for (JsonVariant t : arr) {
      if (idx >= ctx->count) break;
      JsonObject tobj = t.as<JsonObject>();
      tobj["nextAt"] = (unsigned long)ctx->triggers[idx].nextAt;
      if (ctx->triggers[idx].type == Trigger::TIME && ctx->triggers[idx].weekInterval > 1 &&
          ctx->triggers[idx].anchor > 0) {
        tobj["anchor"] = (unsigned long)ctx->triggers[idx].anchor;
      }
      idx++;
    }
    return AutomationsMutation::Changed;
  }
  return AutomationsMutation::Unchanged;
}

// Unified post-fire helper. For each trigger whose nextAt was at or before
// `firedAt`, compute a new nextAt (and maybe update anchor). A short interval
// keeps its new value in RAM; anything else is saved by writing the whole
// triggers array back to the JSON file.
static void rescheduleAfterFire(long id, const char* automationJson, time_t firedAt, bool notify) {
  Trigger triggers[MAX_TRIGGERS];
  int n = triggersFromJson(automationJson, triggers, MAX_TRIGGERS, firedAt);
  if (n == 0) return;  // Malformed — scheduler will skip on next rebuild.
  // `firedAt` is an uptime while the clock is not set: every next run then
  // stays in AutomationUptimeSchedule and the file is not written.
  const bool onUptime = automationOnUptime(firedAt);

  // For each trigger that was due, advance it. Leave others untouched.
  bool save = false;
  for (int i = 0; i < n; i++) {
    if (triggers[i].nextAt > 0 && triggers[i].nextAt <= firedAt) {
      onFired(triggers[i], firedAt);  // mutates anchor if needed
      triggers[i].nextAt = nextFire(triggers[i], firedAt);  // 0 = disarm
      if (onUptime) {
        if (triggers[i].slot < 255) automationUptimeAdvance(id, triggers[i].slot, triggers[i].nextAt);
        continue;
      }
      const bool inRam = triggers[i].type == Trigger::INTERVAL && triggers[i].slot < 255 &&
                         automationVolatileSet(id, triggers[i].slot, triggers[i].intervalMs,
                                               triggers[i].nextAt);
      if (!inRam) save = true;
    }
  }
  if (!save) {
    // Nothing in the file changed. Outside a pass the scheduler still has to
    // refresh its due cache from the new RAM values.
    if (notify) notifyAutomationScheduler();
    return;
  }

  // Rewrite the triggers array in the file, in one locked read-modify-write
  // (a short interval's entry carries its current RAM value along). The lock
  // is taken blocking: a missed write here would leave the old next run in
  // the file and fire the automation again on the next pass.
  // Inside a pass the trailing rebuild picks this write up; a notification
  // would only schedule a second full pass. Other callers notify as usual.
  AutomationRescheduleContext ctx{id, triggers, n};
  String unused;
  (void)mutateAutomationsJson("automation.reschedule", automationRescheduleMutation, &ctx,
                              notify ? kAutomationsMutateNotify : 0, unused);
}

// A clock trigger without a next run gets its first one. `now` is clock time
// (the caller asks only while the clock is set).
//
// A trigger of type time, monthly, yearly or interval whose nextAt is 0 can
// never come due by itself: the pass works its next run out afresh from `now`
// on every lap (always ahead), the due cache reads the stored 0 as "nothing
// scheduled", and rescheduleAfterFire only advances a next run that exists.
// That is the state of an automation added while the clock was not set
// (`automation add` stores no next run then: a time counted from 1970 is not
// one) and of an interval still in its first uptime countdown when the clock
// was set (automationCarryUptimeSchedule drops it). So the pass seeds it:
// nextFire from now, nothing fires. A short interval's first run goes to the
// RAM schedule (AutomationVolatileSchedule), anything else is written to the
// trigger's nextAt, in one locked read-modify-write with no notification:
// the pass's trailing rebuild reads the result. Manual, boot and event
// triggers have no clock run and are left alone. True when a trigger was
// seeded.
static bool automationSeedFirstRuns(long id, const char* automationJson, time_t now) {
  Trigger triggers[MAX_TRIGGERS];
  const int n = triggersFromJson(automationJson, triggers, MAX_TRIGGERS, now);
  bool seeded = false, save = false;
  for (int i = 0; i < n; i++) {
    const bool clockTrigger =
        triggers[i].type == Trigger::TIME || triggers[i].type == Trigger::MONTHLY ||
        triggers[i].type == Trigger::YEARLY || triggers[i].type == Trigger::INTERVAL;
    if (!clockTrigger || triggers[i].nextAt > 0) continue;
    const time_t first = nextFire(triggers[i], now);
    if (first <= 0) continue;  // nothing to schedule (an interval of 0, a date that never comes)
    triggers[i].nextAt = first;
    seeded = true;
    const bool inRam = triggers[i].type == Trigger::INTERVAL && triggers[i].slot < 255 &&
                       automationVolatileSet(id, triggers[i].slot, triggers[i].intervalMs, first);
    if (!inRam) save = true;
  }
  if (!save) return seeded;
  AutomationRescheduleContext ctx{id, triggers, n};
  String unused;
  (void)mutateAutomationsJson("automation.seed", automationRescheduleMutation, &ctx, 0, unused);
  return true;
}

// Force-recompute every clock trigger of every enabled automation.
//
// Why this exists rather than reusing computeNextRunTime + updateAutomationNextAt:
// computeNextRunTime returns the MINIMUM nextAt across an automation's triggers
// (and returns the PERSISTED value when it is already > 0), while
// updateAutomationNextAt(id, v) resolves its -1 trigger index to the FIRST
// schedulable trigger. Together that made `automation recompute` (a) a no-op
// for a single-trigger automation — it wrote the same stale value back — and
// (b) actively corrupting for a multi-trigger one, stamping one trigger's
// schedule onto a different trigger. This walks triggers explicitly instead.
//
// MANUAL / BOOT / EVENT triggers are left strictly alone: nextFire() returns 0
// for them, and an ARMED afterDelay (stored as type "manual" with a real
// nextAt, see cmd_automation_trigger) would be silently disarmed.
struct AutomationRecomputeContext {
  time_t now;
  int rewritten;
  int skipped;
};

static AutomationsMutation automationRecomputeMutation(JsonDocument& doc, JsonArray autos,
                                                       void* opaque, String& errorOut) {
  (void)doc;
  (void)errorOut;
  AutomationRecomputeContext* ctx = static_cast<AutomationRecomputeContext*>(opaque);
  const time_t now = ctx->now;
  int rewritten = 0, skipped = 0;
  for (JsonObject automation : autos) {
    if (!(automation["enabled"] | false)) { skipped++; continue; }
    JsonArray arr = automation["triggers"].as<JsonArray>();
    if (arr.isNull()) { skipped++; continue; }

    // Parse through the scheduler's own path so trigger-field semantics can
    // never drift between recompute and the tick.
    String objStr;
    serializeJson(automation, objStr);
    Trigger triggers[MAX_TRIGGERS];
    const int n = triggersFromJson(objStr.c_str(), triggers, MAX_TRIGGERS);
    if (n == 0) { skipped++; continue; }

    int idx = 0;
    for (JsonVariant t : arr) {
      if (idx >= n) break;
      JsonObject tobj = t.as<JsonObject>();
      switch (triggers[idx].type) {
        case Trigger::TIME:
        case Trigger::MONTHLY:
        case Trigger::YEARLY:
        case Trigger::INTERVAL: {
          const time_t na = nextFire(triggers[idx], now);
          tobj["nextAt"] = (unsigned long)na;
          if (triggers[idx].type == Trigger::TIME && triggers[idx].weekInterval > 1 &&
              triggers[idx].anchor > 0) {
            tobj["anchor"] = (unsigned long)triggers[idx].anchor;
          }
          rewritten++;
          break;
        }
        default:
          break;  // manually armed / event-dispatched — not ours to touch
      }
      idx++;
    }
  }
  ctx->rewritten = rewritten;
  ctx->skipped = skipped;
  return rewritten > 0 ? AutomationsMutation::Changed : AutomationsMutation::Unchanged;
}

static int automationRecomputeAllClockTriggers(time_t now, int* outSkipped) {
  if (outSkipped) *outSkipped = 0;
  AutomationRecomputeContext ctx{now, 0, 0};
  String error;
  // ONE locked read-modify-write for the lot; nothing to rewrite, no write.
  const AutomationsMutateResult result = mutateAutomationsJson(
      "automation.recompute", automationRecomputeMutation, &ctx, kAutomationsMutateNotify, error);
  if (result != AutomationsMutateResult::Changed && result != AutomationsMutateResult::Unchanged) return -1;
  if (outSkipped) *outSkipped = ctx.skipped;
  return ctx.rewritten;
}

// The min nextAt across the triggers of the automation, or 0 if none is
// scheduled. `clockSet` is System_Clock's answer to "is the clock set". While
// it is false a trigger that names a time of day (daily, weekly, monthly,
// yearly) is left out: against a clock that still counts from 1970 its
// "07:00" is seven hours after boot. Interval and armed-delay triggers then
// count uptime: `fromTime` is the uptime and their next runs come from
// AutomationUptimeSchedule (triggersFromJson).
static time_t automationNextRunTime(const char* automationJson, time_t fromTime, bool clockSet) {
  Trigger triggers[MAX_TRIGGERS];
  int n = triggersFromJson(automationJson, triggers, MAX_TRIGGERS, fromTime);
  if (n == 0) return 0;
  // For triggers without persisted nextAt, compute fresh ones.
  time_t minAt = 0;
  for (int i = 0; i < n; i++) {
    const bool namesTimeOfDay = triggers[i].type == Trigger::TIME || triggers[i].type == Trigger::MONTHLY ||
                                triggers[i].type == Trigger::YEARLY;
    if (namesTimeOfDay && !clockSet) continue;
    time_t t = (triggers[i].nextAt > 0) ? triggers[i].nextAt : nextFire(triggers[i], fromTime);
    if (t > 0 && (minAt == 0 || t < minAt)) minAt = t;
  }
  return minAt;
}

// Thin wrapper preserving the existing public API. Returns the min nextAt
// across all triggers of the automation, or 0 if none are scheduled.
time_t computeNextRunTime(const char* automationJson, time_t fromTime) {
  return automationNextRunTime(automationJson, fromTime, true);
}

// Validate condition syntax (const char* input)
const char* validateConditionSyntax(const char* condition) {
  const char* cond = condition;
  size_t len = strlen(condition);
  
  const String safeConditionForTrace =
      redactAutomationCommandForAudit(String(condition ? condition : ""));
  DEBUG_AUTOMATIONSF("[validate] Input condition: '%s'", safeConditionForTrace.c_str());
  
  // Skip leading whitespace
  while (*cond == ' ' || *cond == '\t') { cond++; len--; }
  
  // Must start with IF (case-insensitive)
  if (len < 3 || 
      (cond[0] != 'I' && cond[0] != 'i') ||
      (cond[1] != 'F' && cond[1] != 'f') ||
      cond[2] != ' ') {
    DEBUG_AUTOMATIONSF("[validate] FAIL: Condition must start with 'IF'");
    return "Condition must start with 'IF'";
  }

  // Find THEN (case-insensitive). The bound is written as a sum: `len - 5`
  // wraps for a text shorter than five characters, and the scan then reads
  // past the terminator.
  const char* thenPos = nullptr;
  for (size_t i = 3; i + 5 < len; i++) {
    if ((cond[i] == ' ' || cond[i] == '\t') &&
        (cond[i+1] == 'T' || cond[i+1] == 't') &&
        (cond[i+2] == 'H' || cond[i+2] == 'h') &&
        (cond[i+3] == 'E' || cond[i+3] == 'e') &&
        (cond[i+4] == 'N' || cond[i+4] == 'n') &&
        (cond[i+5] == ' ' || cond[i+5] == '\t')) {
      thenPos = cond + i;
      break;
    }
  }
  
  if (!thenPos) {
    DEBUG_AUTOMATIONSF("[validate] FAIL: Condition must contain 'THEN'");
    return "Condition must contain 'THEN'";
  }

  // Check condition part has content
  size_t condLen = thenPos - (cond + 3);
  if (condLen == 0) {
    DEBUG_AUTOMATIONSF("[validate] FAIL: Missing condition after 'IF'");
    return "Missing condition after 'IF'";
  }

  // Check command part has content
  const char* cmdStart = thenPos + 5;
  while (*cmdStart == ' ' || *cmdStart == '\t') cmdStart++;
  if (*cmdStart == '\0') {
    DEBUG_AUTOMATIONSF("[validate] FAIL: Missing command after 'THEN'");
    return "Missing command after 'THEN'";
  }

  // Validate condition has an operator
  const char* operators[] = { "CONTAINS", ">=", "<=", "!=", ">", "<", "=" };
  bool hasOperator = false;
  const char* foundOp = nullptr;
  for (int i = 0; i < 7; i++) {
    if (strstr(cond + 3, operators[i])) {
      hasOperator = true;
      foundOp = operators[i];
      break;
    }
  }

  if (!hasOperator) {
    DEBUG_AUTOMATIONSF("[validate] FAIL: No operator found in condition");
    return "Condition must contain an operator (>, <, =, >=, <=, !=, CONTAINS)";
  }

  DEBUG_AUTOMATIONSF("[validate] PASS: Found operator '%s'", foundOp);
  return "";
}

// Evaluate condition (const char* input)
bool evaluateCondition(const char* condition) {
  // Skip leading whitespace
  while (*condition == ' ' || *condition == '\t') condition++;
  
  // Must start with IF (case-insensitive)
  if (strlen(condition) < 3 ||
      (condition[0] != 'I' && condition[0] != 'i') ||
      (condition[1] != 'F' && condition[1] != 'f') ||
      condition[2] != ' ') {
    return false;
  }
  
  // Find THEN
  const char* thenPos = nullptr;
  for (const char* p = condition + 3; *p; p++) {
    if ((*p == ' ' || *p == '\t') &&
        (p[1] == 'T' || p[1] == 't') &&
        (p[2] == 'H' || p[2] == 'h') &&
        (p[3] == 'E' || p[3] == 'e') &&
        (p[4] == 'N' || p[4] == 'n') &&
        (p[5] == ' ' || p[5] == '\t')) {
      thenPos = p + 1;
      break;
    }
  }
  
  if (!thenPos) return false;
  
  // Extract condition part (between IF and THEN) to stack buffer
  char condBuf[256];
  size_t condLen = thenPos - (condition + 3);
  if (condLen >= sizeof(condBuf)) condLen = sizeof(condBuf) - 1;
  strncpy(condBuf, condition + 3, condLen);
  condBuf[condLen] = '\0';
  
  // Trim and uppercase condition part
  char* condStart = condBuf;
  while (*condStart == ' ' || *condStart == '\t') condStart++;
  char* condEnd = condStart + strlen(condStart) - 1;
  while (condEnd > condStart && (*condEnd == ' ' || *condEnd == '\t')) *condEnd-- = '\0';
  for (char* p = condStart; *p; p++) {
    if (*p >= 'a' && *p <= 'z') *p -= 32;
  }
  
  // Parse: sensor operator value
  char sensor[64] = "";
  char op[16] = "";  // Increased size for "CONTAINS"
  char value[64] = "";
  const char* operators[] = { "CONTAINS", ">=", "<=", "!=", ">", "<", "=" };  // CONTAINS first for longest match
  const char* opFound = nullptr;
  
  DEBUG_AUTOMATIONSF("[eval] Parsing condition: '%s'", condStart);
  
  for (int i = 0; i < 7; i++) {
    const char* pos = strstr(condStart, operators[i]);
    if (pos && pos > condStart) {
      // Extract sensor
      size_t sensorLen = pos - condStart;
      if (sensorLen >= sizeof(sensor)) sensorLen = sizeof(sensor) - 1;
      strncpy(sensor, condStart, sensorLen);
      sensor[sensorLen] = '\0';
      // Trim sensor
      char* sEnd = sensor + strlen(sensor) - 1;
      while (sEnd > sensor && (*sEnd == ' ' || *sEnd == '\t')) *sEnd-- = '\0';
      
      // Copy operator
      strlcpy(op, operators[i], sizeof(op));
      
      // Extract value
      const char* valStart = pos + strlen(operators[i]);
      while (*valStart == ' ' || *valStart == '\t') valStart++;
      strlcpy(value, valStart, sizeof(value));
      // Trim value
      char* vEnd = value + strlen(value) - 1;
      while (vEnd > value && (*vEnd == ' ' || *vEnd == '\t')) *vEnd-- = '\0';
      
      opFound = operators[i];
      break;
    }
  }
  
  if (!opFound) {
    DEBUG_AUTOMATIONSF("[eval] FAIL: No operator found in parsed condition");
    return false;
  }
  
  DEBUG_AUTOMATIONSF("[eval] Parsed: sensor='%s' op='%s' value='%s'", sensor, op, value);

  // Get current sensor value
  float currentValue = 0;
  bool isNumeric = true;
  char currentStringValue[32] = "";

  if (strcmp(sensor, "TEMP") == 0) {
 #if ENABLE_THERMAL_SENSOR
    ThermalCache thermal{};
    if (!thermalCacheSnapshot(thermal, 50) || !thermal.thermalDataValid) return false;
    currentValue = thermal.thermalAvgTemp;
 #else
    return false;
 #endif
  } else if (strcmp(sensor, "HUMIDITY") == 0) {
    DEBUG_AUTOMATIONSF("[condition] Humidity sensor not available");
    return false;
  } else if (strcmp(sensor, "DISTANCE") == 0) {
    // Special handling for distance - check if ANY valid object meets the condition
    float targetValue = atof(value);
#if ENABLE_TOF_SENSOR
    bool anyObjectMeetsCondition = false;
    TofCache tof{};
    if (!tofCacheSnapshot(tof, 50)) return false;
    const int tofTotal = tof.tofTotalObjects;
    const auto& objs = tof.tofObjects;

    DEBUG_AUTOMATIONSF("[condition] distance: checking %d objects against %s%.1f",
           tofTotal, op, targetValue);

    for (int j = 0; j < tofTotal && j < 4; j++) {
      if (objs[j].valid) {
        float objDistance = objs[j].distance_cm;
        bool objMeetsCondition = false;

        if (strcmp(op, ">") == 0) objMeetsCondition = objDistance > targetValue;
        else if (strcmp(op, "<") == 0) objMeetsCondition = objDistance < targetValue;
        else if (strcmp(op, "=") == 0) objMeetsCondition = fabs(objDistance - targetValue) < 0.1;
        else if (strcmp(op, ">=") == 0) objMeetsCondition = objDistance >= targetValue;
        else if (strcmp(op, "<=") == 0) objMeetsCondition = objDistance <= targetValue;
        else if (strcmp(op, "!=") == 0) objMeetsCondition = fabs(objDistance - targetValue) >= 0.1;

        DEBUG_AUTOMATIONSF("[condition] obj[%d]: %.1fcm %s %.1f = %s",
               j, objDistance, op, targetValue, objMeetsCondition ? "TRUE" : "FALSE");

        if (objMeetsCondition) {
          anyObjectMeetsCondition = true;
        }
      }
    }

    DEBUG_AUTOMATIONSF("[condition] distance result: %s",
           anyObjectMeetsCondition ? "TRUE" : "FALSE");
    return anyObjectMeetsCondition;
 #else
    (void)targetValue;
    return false;
 #endif
  } else if (strcmp(sensor, "LIGHT") == 0) {
 #if ENABLE_APDS_SENSOR
    ApdsCache apds{};
    if (!apdsCacheSnapshot(apds, 50) || !apds.apdsDataValid) return false;
    currentValue = (float)apds.apdsClear;
 #else
    return false;
 #endif
  } else if (strcmp(sensor, "MOTION") == 0) {
    isNumeric = false;
 #if ENABLE_APDS_SENSOR
    ApdsCache apds{};
    if (!apdsCacheSnapshot(apds, 50) || !apds.apdsDataValid) return false;
    strlcpy(currentStringValue, (apds.apdsProximity > 50) ? "DETECTED" : "NONE", sizeof(currentStringValue));
 #else
    return false;
 #endif
  } else if (strcmp(sensor, "TIME") == 0) {
    isNumeric = false;
    time_t now = time(nullptr);
    struct tm* timeinfo = localtime(&now);
    int hour = timeinfo->tm_hour;
    if (hour >= 6 && hour < 12) strlcpy(currentStringValue, "MORNING", sizeof(currentStringValue));
    else if (hour >= 12 && hour < 18) strlcpy(currentStringValue, "AFTERNOON", sizeof(currentStringValue));
    else if (hour >= 18 && hour < 24) strlcpy(currentStringValue, "EVENING", sizeof(currentStringValue));
    else strlcpy(currentStringValue, "NIGHT", sizeof(currentStringValue));
  } else if (strcmp(sensor, "ROOM") == 0) {
    // ESP-NOW metadata: room assignment
    isNumeric = false;
    if (gSettings.espnowRoom.length() > 0) {
      strlcpy(currentStringValue, gSettings.espnowRoom.c_str(), sizeof(currentStringValue));
      // Uppercase for comparison
      for (char* p = currentStringValue; *p; p++) {
        if (*p >= 'a' && *p <= 'z') *p -= 32;
      }
    } else {
      strlcpy(currentStringValue, "NONE", sizeof(currentStringValue));
    }
    DEBUG_AUTOMATIONSF("[eval] ROOM: current='%s' (from setting='%s')", 
           currentStringValue, gSettings.espnowRoom.c_str());
  } else if (strcmp(sensor, "ZONE") == 0) {
    // ESP-NOW metadata: zone assignment
    isNumeric = false;
    if (gSettings.espnowZone.length() > 0) {
      strlcpy(currentStringValue, gSettings.espnowZone.c_str(), sizeof(currentStringValue));
      // Uppercase for comparison
      for (char* p = currentStringValue; *p; p++) {
        if (*p >= 'a' && *p <= 'z') *p -= 32;
      }
    } else {
      strlcpy(currentStringValue, "NONE", sizeof(currentStringValue));
    }
    DEBUG_AUTOMATIONSF("[eval] ZONE: current='%s' (from setting='%s')", 
           currentStringValue, gSettings.espnowZone.c_str());
  } else if (strcmp(sensor, "TAGS") == 0) {
    // ESP-NOW metadata: tags (supports CONTAINS operator)
    isNumeric = false;
    if (gSettings.espnowTags.length() > 0) {
      strlcpy(currentStringValue, gSettings.espnowTags.c_str(), sizeof(currentStringValue));
      // Uppercase for comparison
      for (char* p = currentStringValue; *p; p++) {
        if (*p >= 'a' && *p <= 'z') *p -= 32;
      }
    } else {
      strlcpy(currentStringValue, "NONE", sizeof(currentStringValue));
    }
    DEBUG_AUTOMATIONSF("[eval] TAGS: current='%s' (from setting='%s')",
           currentStringValue, gSettings.espnowTags.c_str());
  // ---- System-state variables (numeric) ----
  } else if (strcmp(sensor, "BATTERY") == 0) {
    // Gauge state of charge or approximate ADC voltage-derived percentage.
    // Unavailable telemetry must not trigger a low/empty battery automation.
    const BatteryState battery = getBatterySnapshot();
    if (!battery.percentageValid) return false;
    currentValue = battery.percentage;
  } else if (strcmp(sensor, "HEAP") == 0) {
    // Free INTERNAL DRAM in KB (the small, contested pool -- not the combined heap)
    currentValue = (float)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024);
  } else if (strcmp(sensor, "PSRAM") == 0) {
    // Free PSRAM/SPIRAM in KB (0 on boards without PSRAM)
    currentValue = (float)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
  } else if (strcmp(sensor, "FSFREE") == 0) {
    // Free LittleFS space in KB. The reading is the log-overflow check's
    // stored figure while it is fresh (2-15 s); when it is stale, after a
    // write burst or after a remove, this call takes the filesystem lock and
    // walks the volume (esp_littlefs_info), so it can wait on other flash work.
    currentValue = (float)(VFS::getCachedLittleFsFree() / 1024);
  } else if (strcmp(sensor, "UPTIME") == 0) {
    // Minutes since boot, from the uptime owner (it does not restart at 49.7 days)
    currentValue = (float)(SelfDevice::uptimeSeconds() / 60u);
  } else if (strcmp(sensor, "CHIPTEMP") == 0) {
    // On-die SoC temperature in Celsius (self-heated, not ambient). No
    // believable reading (a failed read, the classic ESP32's fake 53.3 C)
    // must not trigger an automation: the condition is false.
    float tc = 0.0f;
    if (!SelfDevice::chipTemperatureC(tc)) return false;
    currentValue = tc;
  } else if (strcmp(sensor, "HOUR") == 0) {
    // Local hour of day 0-23 (reflects boot epoch until the clock is synced)
    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);
    currentValue = (float)ti->tm_hour;
  } else if (strcmp(sensor, "RSSI") == 0) {
    // WiFi signal strength in dBm (negative); only meaningful while associated
 #if ENABLE_WIFI
    if (!WiFi.isConnected()) return false;
    currentValue = (float)WiFi.RSSI();
 #else
    return false;
 #endif
  } else if (strcmp(sensor, "SPEED") == 0) {
    // GPS ground speed in knots (requires a fix)
 #if ENABLE_GPS_SENSOR
    GpsCache gps;
    if (!gpsCacheSnapshot(gps) || !gps.hasFix) return false;
    currentValue = gps.speed;
 #else
    return false;
 #endif
  } else if (strcmp(sensor, "SATS") == 0) {
    // GPS satellites in view. Deliberately does NOT require hasFix — the count is
    // meaningful while still acquiring — but it DOES require live data, so a
    // closed/never-started GPS is unevaluable rather than reading 0. Without the
    // dataValid gate this fails OPEN: gpsTask zeroes satellites on `closegps`, so
    // "IF SATS < n" would fire continuously while the sensor is deliberately off.
    // Fail-closed matches SPEED / GPS / WP_DIST above.
 #if ENABLE_GPS_SENSOR
    GpsCache gps;
    if (!gpsCacheSnapshot(gps) || !gps.dataValid) return false;
    currentValue = (float)gps.satellites;
 #else
    return false;
 #endif
  } else if (strncmp(sensor, "WP_DIST:", 8) == 0 || strcmp(sensor, "WP_DIST") == 0) {
    // Meters from current GPS fix to a named waypoint (wp_dist:<name>).
    // Fail closed when maps/GPS are off, no fix, empty name, or waypoint missing.
 #if ENABLE_MAPS && ENABLE_GPS_SENSOR
    const char* wpName = (strncmp(sensor, "WP_DIST:", 8) == 0) ? (sensor + 8) : "";
    if (!wpName[0]) return false;
    GpsCache gps;
    if (!gpsCacheSnapshot(gps) || !gps.hasFix) return false;
    float distM = 0, bearingDeg = 0;
    if (!WaypointManager::getDistanceBearingToName(wpName, gps.latitude, gps.longitude,
                                                   distM, bearingDeg)) {
      return false;
    }
    currentValue = distM;
 #else
    return false;
 #endif
  } else if (strcmp(sensor, "PEERS") == 0) {
    // Paired ESP-NOW peers that are online: a mesh heartbeat within 30 s and
    // an encrypted session up (espnowPeerCounts, the state every surface uses).
 #if ENABLE_ESPNOW
    currentValue = (float)espnowPeerCounts().online;
 #else
    return false;
 #endif
  // ---- Connectivity / device-state variables (string/enum) ----
  } else if (strcmp(sensor, "WIFI") == 0) {
    // STA association + IP -> CONNECTED / NONE
    isNumeric = false;
 #if ENABLE_WIFI
    strlcpy(currentStringValue, WiFi.isConnected() ? "CONNECTED" : "NONE", sizeof(currentStringValue));
 #else
    return false;
 #endif
  } else if (strcmp(sensor, "BLE") == 0) {
    // BLE GATT client connected -> CONNECTED / NONE (NONE when Bluetooth disabled)
    isNumeric = false;
    strlcpy(currentStringValue, isBleConnected() ? "CONNECTED" : "NONE", sizeof(currentStringValue));
  } else if (strcmp(sensor, "NTP") == 0) {
    // Wall clock holds a real date (RTC or NTP source) -> SYNCED / NONE
    isNumeric = false;
    strlcpy(currentStringValue, Clock::isSynced() ? "SYNCED" : "NONE", sizeof(currentStringValue));
  } else if (strcmp(sensor, "DAY") == 0) {
    // Day of week -> SUN..SAT (tm_wday is 0=Sunday)
    isNumeric = false;
    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);
    static const char* const kDays[] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
    int wd = ti->tm_wday;
    if (wd < 0 || wd > 6) wd = 0;
    strlcpy(currentStringValue, kDays[wd], sizeof(currentStringValue));
  } else if (strcmp(sensor, "GPS") == 0) {
    // GPS fix status -> FIX / NOFIX
    isNumeric = false;
 #if ENABLE_GPS_SENSOR
    GpsCache gps;
    if (!gpsCacheSnapshot(gps)) return false;
    strlcpy(currentStringValue, gps.hasFix ? "FIX" : "NOFIX", sizeof(currentStringValue));
 #else
    return false;
 #endif
  } else if (strcmp(sensor, "LLM") == 0) {
    // On-device model state -> READY / BUSY / NONE
    isNumeric = false;
 #if ENABLE_LLM_BACKEND
    LlmState st = llmBackendStatus().state;
    const char* v = (st == LlmState::READY) ? "READY"
                  : (st == LlmState::GENERATING || st == LlmState::LOADING) ? "BUSY" : "NONE";
    strlcpy(currentStringValue, v, sizeof(currentStringValue));
 #else
    return false;
 #endif
  // ---- ESP-NOW / bond / pairing state ----
  } else if (strcmp(sensor, "ESPNOW") == 0) {
    // ESP-NOW radio stack initialized -> ACTIVE / NONE
    isNumeric = false;
 #if ENABLE_ESPNOW
    strlcpy(currentStringValue, (gEspNow && gEspNow->initialized) ? "ACTIVE" : "NONE", sizeof(currentStringValue));
 #else
    return false;
 #endif
  } else if (strcmp(sensor, "BOND_MODE") == 0) {
    // Bond mode enabled in config -> ACTIVE / NONE
    isNumeric = false;
 #if ENABLE_BONDED_MODE
    strlcpy(currentStringValue, gSettings.bondModeEnabled ? "ACTIVE" : "NONE", sizeof(currentStringValue));
 #else
    return false;
 #endif
  } else if (strcmp(sensor, "BOND_ROLE") == 0) {
    // This device's bond role -> MASTER / WORKER
    isNumeric = false;
 #if ENABLE_BONDED_MODE
    strlcpy(currentStringValue, isBondMaster() ? "MASTER" : "WORKER", sizeof(currentStringValue));
 #else
    return false;
 #endif
  } else if (strcmp(sensor, "BOND_PAIRED") == 0) {
    // A bonded partner is configured -> PAIRED / NONE
    isNumeric = false;
 #if ENABLE_BONDED_MODE
    strlcpy(currentStringValue, BondedPeer::isPaired() ? "PAIRED" : "NONE", sizeof(currentStringValue));
 #else
    return false;
 #endif
  } else if (strcmp(sensor, "BOND_ONLINE") == 0) {
    // Bonded peer currently reachable -> ONLINE / OFFLINE
    isNumeric = false;
 #if ENABLE_BONDED_MODE
    strlcpy(currentStringValue, isBondModeOnline() ? "ONLINE" : "OFFLINE", sizeof(currentStringValue));
 #else
    return false;
 #endif
  } else if (strcmp(sensor, "BOND_SYNCED") == 0) {
    // Bonded peer online AND config-synced -> SYNCED / NONE
    isNumeric = false;
 #if ENABLE_BONDED_MODE
    strlcpy(currentStringValue, isBondSynced() ? "SYNCED" : "NONE", sizeof(currentStringValue));
 #else
    return false;
 #endif
  } else if (strcmp(sensor, "PAIRMODE") == 0) {
    // WPS-style pairing window open -> ACTIVE / NONE
    isNumeric = false;
 #if ENABLE_ESPNOW
    strlcpy(currentStringValue, espnowPairModeActive() ? "ACTIVE" : "NONE", sizeof(currentStringValue));
 #else
    return false;
 #endif
  } else if (strcmp(sensor, "PAIRMODE_SECS") == 0) {
    // Seconds left in the pairing window (0 when closed)
 #if ENABLE_ESPNOW
    currentValue = (float)(espnowPairModeRemainingMs() / 1000UL);
 #else
    return false;
 #endif
  } else if (strcmp(sensor, "BOND_RSSI") == 0) {
    // Link RSSI (dBm) of the last bond heartbeat; only meaningful while online
 #if ENABLE_ESPNOW && ENABLE_BONDED_MODE
    if (!gEspNow || !isBondModeOnline()) return false;
    currentValue = (float)gEspNow->bondRssiLast;
 #else
    return false;
 #endif
  } else if (strcmp(sensor, "BOND_PEER_HEAP") == 0) {
    // Bonded peer's reported free heap in KB (from the ~30s status snapshot)
 #if ENABLE_ESPNOW && ENABLE_BONDED_MODE
    if (!gEspNow || !gEspNow->bondPeerStatusValid) return false;
    currentValue = (float)(gEspNow->bondPeerStatus.freeHeap / 1024);
 #else
    return false;
 #endif
  } else if (strcmp(sensor, "BOND_PEER_UPTIME") == 0) {
    // Bonded peer's reported uptime in minutes (peer-reboot detection)
 #if ENABLE_ESPNOW && ENABLE_BONDED_MODE
    if (!gEspNow || !gEspNow->bondPeerStatusValid) return false;
    currentValue = (float)(gEspNow->bondPeerStatus.uptimeSec / 60);
 #else
    return false;
 #endif
  } else if (strcmp(sensor, "PEERSKNOWN") == 0) {
    // Count of known mesh-peer slots (recently-seen, NOT alive-filtered like PEERS)
 #if ENABLE_ESPNOW
    int known = 0;
    if (gMeshPeers) {
      for (int i = 0; i < gMeshPeerSlots; i++) {
        if (gMeshPeers[i].isActive && !isSelfMac(gMeshPeers[i].mac)) known++;
      }
    }
    currentValue = (float)known;
 #else
    return false;
 #endif
  } else if (strcmp(sensor, "STALESTPEERAGE") == 0) {
    // Seconds since the oldest mesh-peer heartbeat (link-degrading early warning)
 #if ENABLE_ESPNOW
    uint32_t nowMs = millis();
    uint32_t worst = 0;
    bool anyPeer = false;
    if (gMeshPeers) {
      for (int i = 0; i < gMeshPeerSlots; i++) {
        if (gMeshPeers[i].isActive && !isSelfMac(gMeshPeers[i].mac) && gMeshPeers[i].lastMeshHeartbeatMs != 0) {
          uint32_t age = (nowMs - gMeshPeers[i].lastMeshHeartbeatMs) / 1000UL;
          if (age > worst) worst = age;
          anyPeer = true;
        }
      }
    }
    if (!anyPeer) return false;
    currentValue = (float)worst;
 #else
    return false;
 #endif
  } else {
    DEBUG_AUTOMATIONSF("[condition] Unknown sensor: %s", sensor);
    return false;
  }

  // Evaluate condition
  if (isNumeric) {
    float targetValue = atof(value);
    bool result = false;
    if (strcmp(op, ">") == 0) result = currentValue > targetValue;
    else if (strcmp(op, "<") == 0) result = currentValue < targetValue;
    else if (strcmp(op, "=") == 0) result = fabs(currentValue - targetValue) < 0.1;
    else if (strcmp(op, ">=") == 0) result = currentValue >= targetValue;
    else if (strcmp(op, "<=") == 0) result = currentValue <= targetValue;
    else if (strcmp(op, "!=") == 0) result = fabs(currentValue - targetValue) >= 0.1;
    DEBUG_AUTOMATIONSF("[eval] Numeric: %.2f %s %.2f = %s", 
           currentValue, op, targetValue, result ? "TRUE" : "FALSE");
    return result;
  } else {
    // Uppercase value for comparison
    for (char* p = value; *p; p++) {
      if (*p >= 'a' && *p <= 'z') *p -= 32;
    }
    bool result = false;
    if (strcmp(op, "=") == 0) {
      result = strcmp(currentStringValue, value) == 0;
      DEBUG_AUTOMATIONSF("[eval] String: '%s' = '%s' = %s", 
             currentStringValue, value, result ? "TRUE" : "FALSE");
    } else if (strcmp(op, "!=") == 0) {
      result = strcmp(currentStringValue, value) != 0;
      DEBUG_AUTOMATIONSF("[eval] String: '%s' != '%s' = %s", 
             currentStringValue, value, result ? "TRUE" : "FALSE");
    } else if (strcmp(op, "CONTAINS") == 0) {
      // Check if currentStringValue contains value (case-insensitive substring match)
      result = strstr(currentStringValue, value) != nullptr;
      DEBUG_AUTOMATIONSF("[eval] String: '%s' CONTAINS '%s' = %s", 
             currentStringValue, value, result ? "TRUE" : "FALSE");
    }
    return result;
  }

  return false;
}

// Validate conditional chain (const char* version matching header)
const char* validateConditionalChain(const char* chainStr) {
  if (!chainStr || chainStr[0] == '\0') {
    return "Error: Empty conditional chain";
  }

  // Copy and uppercase to stack buffer
  char input[512];
  size_t len = strlen(chainStr);
  if (len >= sizeof(input)) len = sizeof(input) - 1;
  
  for (size_t i = 0; i < len; i++) {
    char c = chainStr[i];
    input[i] = (c >= 'a' && c <= 'z') ? (c - 32) : c;
  }
  input[len] = '\0';

  // Rule 1: Must start with IF
  if (strncmp(input, "IF ", 3) != 0) {
    return "Error: Conditional chain must start with 'IF'";
  }

  // State machine to track chain structure
  bool sawIf = false;
  bool sawElse = false;
  size_t position = 0;

  while (position < len) {
    // Skip whitespace
    while (position < len && input[position] == ' ') position++;
    if (position >= len) break;

    // Check for keywords
    bool isIf = (strncmp(input + position, "IF ", 3) == 0);
    bool isElseIf = (position + 8 <= len && strncmp(input + position, "ELSE IF ", 8) == 0);
    bool isElse = (strncmp(input + position, "ELSE ", 5) == 0);

    if (isIf) {
      if (position > 0) {
        return "Error: 'IF' can only appear at the beginning of a conditional chain";
      }
      sawIf = true;

      // Check for THEN
      const char* thenPos = strstr(input + position + 3, " THEN ");
      if (!thenPos) {
        return "Error: 'IF' statement missing 'THEN' keyword";
      }

      position = (thenPos - input) + 6;

    } else if (isElseIf) {
      if (!sawIf) {
        return "Error: 'ELSE IF' must follow 'IF' statement";
      }
      if (sawElse) {
        return "Error: Cannot use 'ELSE IF' after 'ELSE' (ELSE must be terminal)";
      }

      // Check for THEN
      const char* thenPos = strstr(input + position + 8, " THEN ");
      if (!thenPos) {
        return "Error: 'ELSE IF' statement missing 'THEN' keyword";
      }

      position = (thenPos - input) + 6;

    } else if (isElse) {
      if (!sawIf) {
        return "Error: 'ELSE' must follow 'IF' statement";
      }
      if (sawElse) {
        return "Error: Multiple 'ELSE' clauses not allowed";
      }
      sawElse = true;

      position += 5;
      position = len;  // ELSE is terminal

    } else {
      position++;
    }
  }

  if (!sawIf) {
    return "Error: No valid 'IF' statement found";
  }

  return "";  // Empty string means valid
}

// Evaluate conditional chain (const char* with output buffer)
const char* evaluateConditionalChain(const char* chainStr, char* outBuf, size_t outBufSize) {
  if (!chainStr || chainStr[0] == '\0' || !outBuf || outBufSize == 0) {
    if (outBuf && outBufSize > 0) outBuf[0] = '\0';
    return outBuf;
  }

  // Copy and uppercase to stack buffer
  char input[512];
  size_t len = strlen(chainStr);
  if (len >= sizeof(input)) len = sizeof(input) - 1;
  
  for (size_t i = 0; i < len; i++) {
    char c = chainStr[i];
    input[i] = (c >= 'a' && c <= 'z') ? (c - 32) : c;
  }
  input[len] = '\0';

  size_t position = 0;

  while (position < len) {
    // Skip whitespace
    while (position < len && input[position] == ' ') position++;
    if (position >= len) break;

    // Check for keywords
    bool isIf = (strncmp(input + position, "IF ", 3) == 0);
    bool isElseIf = (position + 8 <= len && strncmp(input + position, "ELSE IF ", 8) == 0);
    bool isElse = (strncmp(input + position, "ELSE ", 5) == 0);

    if (isIf || isElseIf) {
      // Extract condition and action
      size_t condStart = position + (isElseIf ? 8 : 3);
      const char* thenPos = strstr(input + condStart, " THEN ");
      if (!thenPos) {
        outBuf[0] = '\0';
        return outBuf;
      }

      // Build full condition for evaluation
      char fullCond[256];
      size_t condLen = thenPos - (input + condStart);
      if (condLen >= sizeof(fullCond) - 12) condLen = sizeof(fullCond) - 13;
      snprintf(fullCond, sizeof(fullCond), "IF %.*s THEN dummy", (int)condLen, input + condStart);

      // Find end of action
      size_t actionStart = (thenPos - input) + 6;
      size_t actionEnd = len;

      // Look for next conditional keyword
      for (size_t i = actionStart; i + 7 < len; i++) {
        if (strncmp(input + i, " ELSE IF ", 9) == 0 || strncmp(input + i, " ELSE ", 6) == 0) {
          actionEnd = i;
          break;
        }
      }

      // Evaluate this condition
      bool conditionMet = evaluateCondition(fullCond);

      if (conditionMet) {
        // Extract action from original string (preserve case)
        size_t actionLen = actionEnd - actionStart;
        if (actionLen >= outBufSize) actionLen = outBufSize - 1;
        strncpy(outBuf, chainStr + actionStart, actionLen);
        outBuf[actionLen] = '\0';
        // Trim
        char* start = outBuf;
        while (*start == ' ' || *start == '\t') start++;
        if (start != outBuf) memmove(outBuf, start, strlen(start) + 1);
        char* end = outBuf + strlen(outBuf) - 1;
        while (end > outBuf && (*end == ' ' || *end == '\t')) *end-- = '\0';
        return outBuf;
      }

      position = actionEnd;
    } else if (isElse) {
      // ELSE - always execute
      size_t actionStart = position + 5;
      size_t actionLen = len - actionStart;
      if (actionLen >= outBufSize) actionLen = outBufSize - 1;
      strncpy(outBuf, chainStr + actionStart, actionLen);
      outBuf[actionLen] = '\0';
      // Trim
      char* start = outBuf;
      while (*start == ' ' || *start == '\t') start++;
      if (start != outBuf) memmove(outBuf, start, strlen(start) + 1);
      char* end = outBuf + strlen(outBuf) - 1;
      while (end > outBuf && (*end == ' ' || *end == '\t')) *end-- = '\0';
      return outBuf;
    } else {
      position++;
    }
  }

  outBuf[0] = '\0';
  return outBuf;
}

// Execute conditional command (const char* input, static return buffer)
// `owner` is the automation's createdBy user, forwarded into every queued sub-command.
// `autoName` is the automation's display name for autolog COMMAND/OUTPUT attribution.
const char* executeConditionalCommand(const char* command, const char* owner, const char* autoName) {
  EXT_RAM_BSS_ATTR static char errorBuf[128];  // Static buffer for error messages
  // The step never reached the executor. The text must not hold the word the
  // callers use to recognise an accepted step (isAutoInternalResult).
  static const char kNotSubmitted[] =
      "Error: Command not submitted (executor busy or command too long). Run the automation again.";
  const char* cmdStr = command;
  size_t cmdLen = strlen(command);
  
  // Check for PRINT command (case-insensitive)
  if (cmdLen >= 6) {
    char prefix[7];
    for (int i = 0; i < 6 && i < (int)cmdLen; i++) {
      char c = cmdStr[i];
      prefix[i] = (c >= 'a' && c <= 'z') ? (c - 32) : c;
    }
    prefix[6] = '\0';
    
    if (strncmp(prefix, "PRINT ", 6) == 0) {
      // Extract message after "PRINT "
      const char* msg = cmdStr + 6;
      while (*msg == ' ' || *msg == '\t') msg++;
      if (*msg != '\0') {
        broadcastOutput(msg);
        return "Message printed";
      } else {
        strlcpy(errorBuf, "Error: PRINT requires a message", sizeof(errorBuf));
        return errorBuf;
      }
    }
  }
  
  // Check for standalone ELSE/ELSE IF (case-insensitive)
  if (cmdLen >= 7) {
    char prefix[8];
    for (int i = 0; i < 7 && i < (int)cmdLen; i++) {
      char c = cmdStr[i];
      prefix[i] = (c >= 'a' && c <= 'z') ? (c - 32) : c;
    }
    prefix[7] = '\0';
    
    if (strncmp(prefix, "ELSE IF", 7) == 0) {
      strlcpy(errorBuf, "Error: 'ELSE IF' cannot be used as a standalone command", sizeof(errorBuf));
      return errorBuf;
    }
    if (cmdLen >= 5 && strncmp(prefix, "ELSE ", 5) == 0) {
      strlcpy(errorBuf, "Error: 'ELSE' cannot be used as a standalone command", sizeof(errorBuf));
      return errorBuf;
    }
  }

  // Check if starts with IF (case-insensitive)
  if (cmdLen >= 3) {
    char prefix[4];
    for (int i = 0; i < 3; i++) {
      char c = cmdStr[i];
      prefix[i] = (c >= 'a' && c <= 'z') ? (c - 32) : c;
    }
    prefix[3] = '\0';
    
    if (strcmp(prefix, "IF ") == 0) {
      // Find THEN position (case-insensitive). The bound is written as a sum:
      // `cmdLen - 5` wraps for "IF " plus 0 or 1 characters, and the scan then
      // reads past the terminator.
      int thenPos = -1;
      for (size_t i = 3; i + 5 < cmdLen; i++) {
        if ((cmdStr[i] == ' ' || cmdStr[i] == '\t') &&
            (cmdStr[i+1] == 'T' || cmdStr[i+1] == 't') &&
            (cmdStr[i+2] == 'H' || cmdStr[i+2] == 'h') &&
            (cmdStr[i+3] == 'E' || cmdStr[i+3] == 'e') &&
            (cmdStr[i+4] == 'N' || cmdStr[i+4] == 'n') &&
            (cmdStr[i+5] == ' ' || cmdStr[i+5] == '\t')) {
          thenPos = i + 1;
          break;
        }
      }
      
      if (thenPos < 0) {
        strlcpy(errorBuf, "Error: Conditional command missing THEN", sizeof(errorBuf));
        return errorBuf;
      }

      // Find ELSE position (case-insensitive)
      int elsePos = -1;
      for (size_t i = thenPos + 5; i + 5 < cmdLen; i++) {
        if ((cmdStr[i] == ' ' || cmdStr[i] == '\t') &&
            (cmdStr[i+1] == 'E' || cmdStr[i+1] == 'e') &&
            (cmdStr[i+2] == 'L' || cmdStr[i+2] == 'l') &&
            (cmdStr[i+3] == 'S' || cmdStr[i+3] == 's') &&
            (cmdStr[i+4] == 'E' || cmdStr[i+4] == 'e') &&
            (cmdStr[i+5] == ' ' || cmdStr[i+5] == '\t')) {
          elsePos = i + 1;
          break;
        }
      }

      // Extract condition part (between IF and THEN) to stack buffer
      char conditionBuf[128];
      size_t condLen = thenPos - 3;
      if (condLen >= sizeof(conditionBuf)) {
        strlcpy(errorBuf, "Error: IF condition too long (max 127 characters)", sizeof(errorBuf));
        return errorBuf;
      }
      strncpy(conditionBuf, cmdStr + 3, condLen);
      conditionBuf[condLen] = '\0';
      // Trim
      char* condStart = conditionBuf;
      while (*condStart == ' ' || *condStart == '\t') condStart++;
      char* condEnd = conditionBuf + strlen(conditionBuf) - 1;
      while (condEnd > conditionBuf && (*condEnd == ' ' || *condEnd == '\t')) *condEnd-- = '\0';

      // Extract THEN command to stack buffer
      char thenBuf[128];
      size_t thenStart = thenPos + 5;
      size_t thenEnd = (elsePos > thenPos) ? elsePos : cmdLen;
      size_t thenLen = thenEnd - thenStart;
      if (thenLen >= sizeof(thenBuf)) {
        strlcpy(errorBuf, "Error: THEN command too long (max 127 characters)", sizeof(errorBuf));
        return errorBuf;
      }
      strncpy(thenBuf, cmdStr + thenStart, thenLen);
      thenBuf[thenLen] = '\0';
      // Trim
      char* thenCmdStart = thenBuf;
      while (*thenCmdStart == ' ' || *thenCmdStart == '\t') thenCmdStart++;
      char* thenCmdEnd = thenBuf + strlen(thenBuf) - 1;
      while (thenCmdEnd > thenBuf && (*thenCmdEnd == ' ' || *thenCmdEnd == '\t')) *thenCmdEnd-- = '\0';

      // Extract ELSE command if present
      char elseBuf[128];
      elseBuf[0] = '\0';
      if (elsePos > thenPos) {
        size_t elseStart = elsePos + 5;
        size_t elseLen = cmdLen - elseStart;
        if (elseLen >= sizeof(elseBuf)) {
          strlcpy(errorBuf, "Error: ELSE command too long (max 127 characters)", sizeof(errorBuf));
          return errorBuf;
        }
        strncpy(elseBuf, cmdStr + elseStart, elseLen);
        elseBuf[elseLen] = '\0';
        // Trim
        char* elseCmdStart = elseBuf;
        while (*elseCmdStart == ' ' || *elseCmdStart == '\t') elseCmdStart++;
        if (elseCmdStart != elseBuf) memmove(elseBuf, elseCmdStart, strlen(elseCmdStart) + 1);
        char* elseCmdEnd = elseBuf + strlen(elseBuf) - 1;
        while (elseCmdEnd > elseBuf && (*elseCmdEnd == ' ' || *elseCmdEnd == '\t')) *elseCmdEnd-- = '\0';
      }

      // Evaluate condition (build full condition string once)
      char fullCondBuf[192];
      snprintf(fullCondBuf, sizeof(fullCondBuf), "IF %s THEN dummy", condStart);
      bool conditionMet = evaluateCondition(fullCondBuf);

      DEBUG_AUTOMATIONSF("[conditional] condition='%s' result=%s",
             condStart, conditionMet ? "TRUE" : "FALSE");

      // Execute appropriate command via async queue (avoids deadlock)
      if (conditionMet) {
        if (thenCmdStart[0] != '\0') {
          const String safeThenCmd = redactAutomationCommandForAudit(String(thenCmdStart));
          DEBUG_AUTOMATIONSF("[conditional] queuing THEN: %s", safeThenCmd.c_str());
          if (!queueAutomationSubCommand(thenCmdStart, owner, autoName)) return kNotSubmitted;
          return "Conditional THEN queued";
        }
      } else {
        if (elseBuf[0] != '\0') {
          const String safeElseCmd = redactAutomationCommandForAudit(String(elseBuf));
          DEBUG_AUTOMATIONSF("[conditional] queuing ELSE: %s", safeElseCmd.c_str());
          if (!queueAutomationSubCommand(elseBuf, owner, autoName)) return kNotSubmitted;
          return "Conditional ELSE queued";
        }
      }

      return "Conditional command completed";
    }
  }
  
  // Regular command - queue through FreeRTOS command queue (async, non-blocking)
  if (!queueAutomationSubCommand(command, owner, autoName)) return kNotSubmitted;
  return "Command queued";
}

// Validate conditional hierarchy (const char* version)
const char* validateConditionalHierarchy(const char* conditions) {
  if (!conditions || conditions[0] == '\0') return "VALID";

  // Copy and uppercase to stack buffer
  char input[512];
  size_t len = strlen(conditions);
  if (len >= sizeof(input)) len = sizeof(input) - 1;
  
  for (size_t i = 0; i < len; i++) {
    char c = conditions[i];
    input[i] = (c >= 'a' && c <= 'z') ? (c - 32) : c;
  }
  input[len] = '\0';

  enum ConditionalState {
    EXPECTING_IF,
    EXPECTING_ELSE_OR_END,
    EXPECTING_END
  };

  ConditionalState state = EXPECTING_IF;
  size_t position = 0;

  while (position < len) {
    while (position < len && input[position] == ' ') position++;
    if (position >= len) break;

    bool foundIf = (strncmp(input + position, "IF ", 3) == 0);
    bool foundElseIf = (position + 8 <= len && strncmp(input + position, "ELSE IF ", 8) == 0);
    bool foundElse = (strncmp(input + position, "ELSE ", 5) == 0);

    switch (state) {
      case EXPECTING_IF:
        if (!foundIf) {
          return "Error: The chain must start with IF. Write IF <condition> THEN <command>.";
        }
        state = EXPECTING_ELSE_OR_END;
        position += 3;
        break;

      case EXPECTING_ELSE_OR_END:
        if (foundElseIf) {
          state = EXPECTING_ELSE_OR_END;
          position += 8;
        } else if (foundElse) {
          state = EXPECTING_END;
          position += 5;
        } else {
          const char* thenPos = strstr(input + position, "THEN");
          if (!thenPos) {
            return "Error: Missing THEN keyword. Write IF <condition> THEN <command>.";
          }
          position = (thenPos - input) + 4;
          while (position < len && 
                 !(position + 8 <= len && strncmp(input + position, "ELSE IF ", 8) == 0) && 
                 !(strncmp(input + position, "ELSE ", 5) == 0)) {
            position++;
          }
          continue;
        }
        break;

      case EXPECTING_END:
        if (foundIf || foundElseIf || foundElse) {
          return "Error: Nothing may follow the ELSE branch. Remove the conditions after ELSE.";
        }
        position++;
        break;
    }

    while (position < len && input[position] != 'E' && input[position] != 'I') {
      position++;
    }
  }

  return "VALID";
}

// ============================================================================
// Automation Logging Command
// ============================================================================

const char* cmd_autolog(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  CommandArgs args(argsInput);
  String action = args.arg(0);
  action.toLowerCase();
  if (action == "start") {
    const String filename = args.arg(1);
    if (filename.length() == 0) return "Error: invalid arguments — Usage: autolog start <filename>";
    AutomationLogReplacement replaced;
    if (!startAutomationLog(filename, currentAuthContext(), &replaced)) {
      snprintf(getDebugBuffer(), 1024, "Error: Failed to create log file: %s", filename.c_str());
    } else if (!replaced.replaced) {
      snprintf(getDebugBuffer(), 1024, "Automation logging started: %s", filename.c_str());
    } else {
      snprintf(getDebugBuffer(), 1024, replaced.marked
          ? "Automation logging started: %s (replaced %s)"
          : "Automation logging started: %s (replaced %s; its LOG_STOP marker could not be written)",
          filename.c_str(), replaced.filename.c_str());
    }
    return getDebugBuffer();
  }
  if (action == "stop") {
    String filename;
    const auto result = stopAutomationLog(filename);
    if (result == AutomationLogStopResult::Inactive) {
      return "Error: Automation logging is not running. Start it with 'autolog start <filename>'.";
    }
    if (result == AutomationLogStopResult::Unavailable) {
      return "Error: Filesystem lock unavailable; automation logging unchanged";
    }
    snprintf(getDebugBuffer(), 1024, result == AutomationLogStopResult::Stopped
        ? "Automation logging stopped: %s"
        : "Automation logging stopped: %s (its LOG_STOP marker could not be written)", filename.c_str());
    return getDebugBuffer();
  }
  if (action == "status") {
    const auto status = automationLogStatus();
    if (!status.active) return "Automation logging INACTIVE";
    snprintf(getDebugBuffer(), 1024, "Automation logging ACTIVE: %s", status.filename.c_str());
    return getDebugBuffer();
  }
  return "Error: invalid arguments — Usage: autolog start <filename> | autolog stop | autolog status";
}

// Validate conditions command
const char* cmd_validate_conditions(const String& argsInput) {
  String conditions = argsInput;
  conditions.trim();
  if (!conditions.length()) {
    return "Usage: validateconditions IF <condition> THEN <command> [ELSE <command>]";
  }
  // The verdict is the reply: an invalid chain answers its reason.
  const char* validationResult = validateConditionalHierarchy(conditions.c_str());
  if (commandReplyIsFailure(validationResult)) return validationResult;
  return gCliValidateOnly ? "VALID" : "Conditions are valid";
}

// Automation scheduler (runs from main loop)
// ============================================================================

// NOTE: cmd_downloadautomation and cmd_conditional are implemented
// in the main .ino file to avoid duplication and linker conflicts.

// Notify the automation scheduler to run on next main loop iteration and
// invalidate the in-RAM cache so it gets rebuilt from the file.
void notifyAutomationScheduler() {
  sAutomationRefreshPending = true;
}

// True when any of the automation's event triggers matches any drained bus
// event (kind equal + match pattern hits subject/detail). Sets
// *matchedKindName to the firing kind's name for logging.
static bool automationEventTriggersMatch(const char* automationJson, const SystemEvent* evs,
                                         int evCount, const char** matchedKindName) {
  if (evCount <= 0) return false;
  Trigger triggers[MAX_TRIGGERS];
  int n = triggersFromJson(automationJson, triggers, MAX_TRIGGERS);
  for (int i = 0; i < n; i++) {
    if (triggers[i].type != Trigger::EVENT || triggers[i].evtKind == 0) continue;
    for (int e = 0; e < evCount; e++) {
      if (evs[e].kind != triggers[i].evtKind) continue;
      if (systemEventMatches(evs[e], triggers[i].evtMatch)) {
        if (matchedKindName) *matchedKindName = systemEventKindName(evs[e].kind);
        return true;
      }
    }
  }
  return false;
}

// Scratch space for the event drain in schedulerTickMinute(). Sized off the
// ring (48 x ~164 B), so it is too big to reserve at link time on behalf of the
// many devices that never turn automations on — the only caller runs from a
// running scheduler (automationTick), so a device whose scheduler never starts
// never pays for this.
//
// PSRAM-preferred: on PSRAM builds the ring itself already lives there, and
// systemEventFetchSince bounds each spinlock hold to four 164-byte records, so
// making the destination external introduces no new cache-access class or
// unbounded critical section. The rest of the minute tick is CPU-only work on
// the custom main task; no ISR, DMA, driver, panic, or cache-off path receives
// this pointer. Event payloads also gain no new exposure: the authoritative ring
// already stores the same records externally, and credential-bearing remote
// commands are redacted before posting.
//
// ps_calloc falls back to the ordinary heap when PSRAM is absent/exhausted or
// globally bypassed, preserving event triggers on boards without usable PSRAM.
//
// Deliberately never freed. `automation system disable` can stop the scheduler
// from a command task while the custom main task is mid-scan, so releasing it
// on stop would hand a live reader a dangling pointer. Allocate-on-first-use-
// and-keep buys back the memory for anyone who leaves automations off, with no
// teardown race to get wrong.
static SystemEvent* sEventBuf = nullptr;

// Core scheduler logic - extracted for reuse
static void schedulerTickMinute() {
  time_t now = time(nullptr);
  // System_Clock owns "is the clock set". A trigger that names a time of day
  // waits for it. Without it interval and armed-delay triggers count uptime
  // (AutomationUptimeSchedule): `now` is then the uptime, the base of every
  // next run this pass compares, logs and reschedules.
  const bool clockSet = Clock::isValidEpoch(now);
  if (!clockSet) now = automationUptimeNow();

  // Drain pending bus events first; event triggers work on devices without
  // NTP/RTC. Events older than 60 s are discarded
  // unmatched — they piled up while automations were disabled, and firing
  // on minute-old edges after a re-enable would surprise the user.
  static uint32_t sEventCursor = UINT32_MAX;  // UINT32_MAX = start "from now"
  static uint32_t sEventRingSkipped = 0;  // ring overwrote events before we read them
  gAutomationEventsPending.exchange(false);  // later posts remain pending for the next tick
  if (!sEventBuf) {
    sEventBuf = static_cast<SystemEvent*>(
        ps_calloc(SYSEVT_RING_SIZE, sizeof(SystemEvent),
                  AllocPref::PreferPsram, "auto.eventDrain"));
    if (!sEventBuf) {
      static uint32_t sAllocWarnMs = 0;
      if (everyMs(&sAllocWarnMs, 10000)) {
        DEBUG_AUTOMATIONSF("[AUTO] event drain buffer alloc failed (%u B) - event triggers skipped until it succeeds",
                           (unsigned)(SYSEVT_RING_SIZE * sizeof(SystemEvent)));
      }
    }
  }
  int evCount = 0;  // no buffer => nothing drained; the clock scan below still runs
  if (sEventBuf) {
    // First drain: resume from the scheduler start (G-73). If no start was
    // recorded the sentinel stays and fetch begins "from now".
    if (sEventCursor == UINT32_MAX) sEventCursor = gAutomationEventStartSeq.load();
    uint32_t skippedBefore = sEventRingSkipped;
    evCount = systemEventFetchSince(&sEventCursor, sEventBuf, SYSEVT_RING_SIZE,
                                    &sEventRingSkipped);
    if (sEventRingSkipped != skippedBefore) {
      // SILENT LOSS with correctness impact: any event trigger in the skipped
      // gap NEVER fired. Warn (throttled) so it's visible in debug output.
      static uint32_t sSkipWarnMs = 0;
      if (everyMs(&sSkipWarnMs, 10000)) {
        DEBUG_AUTOMATIONSF("[AUTO] event ring overflowed this consumer: %lu event(s) skipped total - triggers in the gap did not fire",
                           (unsigned long)sEventRingSkipped);
      }
    }
    if (evCount > 0) {
      uint32_t nowMs = millis();
      int w = 0;
      for (int i = 0; i < evCount; i++) {
        if (nowMs - sEventBuf[i].tsMs <= 60000UL) sEventBuf[w++] = sEventBuf[i];
      }
      evCount = w;
      DEBUG_AUTOMATIONSF("[automations] tick drained %d fresh event(s)", evCount);
    }
  } else {
    // Heap too tight to drain: event triggers sit this tick out rather than
    // deref a null buffer. The cursor is still UINT32_MAX (this branch can only
    // be reached before the first successful alloc), so a later tick that does
    // get memory starts from the scheduler start; the 60 s discard drops what
    // went stale meanwhile, and a skip gap means the ring really overwrote
    // events this consumer never read. Clearing
    // gAutomationEventsPending above means we wait for the next posted event or the
    // 60 s safety tick to retry, instead of re-reading and re-parsing
    // automations.json every 250 ms while the heap is already exhausted.
    // The alloc site above already warned; this branch is only ever reached on
    // the same tick as that failure, so warning again would just double it.
  }

  // The clock was set while interval or armed-delay triggers counted uptime:
  // carry their schedule over to it before the file is read (no-op otherwise).
  if (clockSet) automationCarryUptimeSchedule(now);

  DEBUG_AUTOMATIONSF("[automations] tick now=%lu", (unsigned long)now);

  // Load automations.json
  String json;
  if (!readAutomationsJson(json)) {
    // Missing is a valid empty deployment state (notably when upgrading a
    // profile that previously compiled Automations out).  Publish that state
    // so the fast due check remains I/O-free until the 60-second safety tick
    // or a real file mutation invalidates the cache.
    markAutomationsCacheEmptyValid();
    return;
  }
  DEBUG_AUTOMATIONSF("[automations] json size=%d", json.length());

  int evaluated = 0, executed = 0;
  bool queueSanitize = false;
  long seenIds[128];
  int seenCount = 0;

  int pos = 0;
  while (true) {
    int idPos = json.indexOf("\"id\"", pos);
    if (idPos < 0) break;
    int colon = json.indexOf(':', idPos);
    if (colon < 0) break;

    // Extract the object substring using depth-tracked brace matching
    int objStart = json.lastIndexOf('{', idPos);
    if (objStart < 0) {
      pos = colon + 1;
      continue;
    }
    int objEnd = findJsonObjectEnd(json, objStart);
    if (objEnd < 0) break;

    // Extract id value
    int comma = json.indexOf(',', colon + 1);
    int idValEnd = (comma > 0 && comma < objEnd) ? comma : objEnd;
    String idStr = json.substring(colon + 1, idValEnd);
    idStr.trim();
    long id = idStr.toInt();

    String obj = json.substring(objStart, objEnd + 1);

    // An id above kAutomationIdMax does not fit the `long` it was just read
    // into (toInt cut it to 2147483647), so it could be neither rescheduled
    // after a fire nor addressed. Leave it out of this pass and let the
    // sanitizer below give it a new id, as it does for a duplicate.
    if (automationIdTextAboveRange(idStr.c_str())) {
      DEBUG_AUTOMATIONSF("[autos] id %s is above the id range; skipping and queuing sanitize", idStr.c_str());
      queueSanitize = true;
      pos = objEnd + 1;
      continue;
    }

    // Duplicate-id guard
    bool dupSeen = false;
    for (int i = 0; i < seenCount; ++i) {
      if (seenIds[i] == id) {
        dupSeen = true;
        break;
      }
    }
    if (dupSeen) {
      DEBUG_AUTOMATIONSF("[autos] duplicate id detected at runtime id=%ld; skipping and queuing sanitize", id);
      queueSanitize = true;
      pos = objEnd + 1;
      continue;
    }
    if (seenCount < (int)(sizeof(seenIds) / sizeof(seenIds[0]))) { seenIds[seenCount++] = id; }

    evaluated++;

    // Check if enabled
    bool enabled = (obj.indexOf("\"enabled\": true") >= 0) || (obj.indexOf("\"enabled\":true") >= 0);
    if (!enabled) {
      DEBUG_AUTOMATIONSF("[autos] id=%ld skip: disabled", id);
      pos = objEnd + 1;
      continue;
    }

    // Fire when ANY trigger is due. computeNextRunTime returns the MINIMUM nextAt
    // across all of the automation's triggers (each trigger's persisted nextAt, or
    // a freshly computed nextFire for one whose nextAt is missing). We must NOT read
    // the raw first "nextAt" string here: nextAt is stored per-trigger with no
    // top-level field and triggers are never sorted, so the first occurrence is
    // always triggers[0] (the primary). Gating on triggers[0] alone meant an
    // additional trigger scheduled sooner than the primary never fired on its own
    // cadence, and disagreed with the min-based due cache (rebuildAutomationsCache) --
    // busy-spinning the full tick every main-loop pass until the primary came due.
    // rescheduleAfterFire (below) then advances each trigger that was due, so the
    // min moves forward and the cache/fire-check agree again.
    // With the clock not set a time-of-day trigger is not part of the minimum
    // (automationNextRunTime), so it cannot be due.
    // A clock trigger that has no next run yet (added while the clock was not
    // set) gets its first one now that the clock is; it comes due on a later
    // pass like any other.
    if (clockSet) (void)automationSeedFirstRuns(id, obj.c_str(), now);
    time_t nextAt = automationNextRunTime(obj.c_str(), now, clockSet);
    bool clockDue = (nextAt > 0 && now >= nextAt);

    // Event triggers: fire when a drained bus event matches one of this
    // automation's event triggers. At most one fire per automation per tick
    // even if several events matched (commands carry no per-event data, so
    // repeat fires within one tick would be pure duplicates).
    const char* evKindName = nullptr;
    bool eventDue = automationEventTriggersMatch(obj.c_str(), sEventBuf, evCount, &evKindName);

    // Feedback-loop damping: an automation whose commands produce the very
    // event it subscribes to (on=setting_changed running a set, on=llm_gen_done
    // running an llm command) would otherwise re-fire on every tick, each
    // cycle costing command execution and possibly a flash write. Cap each
    // automation to one EVENT fire per 2s window; clock fires are unaffected.
    if (eventDue) {
      static constexpr int EVT_COOLDOWN_SLOTS = 16;
      static constexpr uint32_t EVT_COOLDOWN_MS = 2000;
      static long sEvtFireId[EVT_COOLDOWN_SLOTS] = {};
      static uint32_t sEvtFireMs[EVT_COOLDOWN_SLOTS] = {};
      static int sEvtFireNext = 0;
      uint32_t nowMs = millis();
      int slot = -1;
      for (int i = 0; i < EVT_COOLDOWN_SLOTS; i++) {
        if (sEvtFireId[i] == id) { slot = i; break; }
      }
      if (slot >= 0 && (nowMs - sEvtFireMs[slot]) < EVT_COOLDOWN_MS) {
        eventDue = false;
        DEBUG_AUTOMATIONSF("[autos] id=%ld event fire suppressed (cooldown)", id);
        if (automationLogActive()) {
          char cdBuf[128];
          snprintf(cdBuf, sizeof(cdBuf), "Event automation skipped: ID=%ld event cooldown (2s)", id);
          appendAutoLogEntry("AUTO_SKIP", cdBuf);
        }
      } else if (eventDue) {
        if (slot < 0) {
          slot = sEvtFireNext;
          sEvtFireNext = (sEvtFireNext + 1) % EVT_COOLDOWN_SLOTS;
          sEvtFireId[slot] = id;
        }
        sEvtFireMs[slot] = nowMs;
      }
    }

    if (nextAt <= 0 && !eventDue) {
      DEBUG_AUTOMATIONSF("[autos] id=%ld skip: no schedulable trigger due", id);
      pos = objEnd + 1;
      continue;
    }

    // Missed-fire suppression. clockDue is a bare `now >= nextAt`, so anything
    // that leaves nextAt far in the past — the device was powered off, or the
    // clock just jumped forward (a dark boot adopting the R1 ring's time moves
    // it from 1970 to now) — would otherwise fire EVERY stale automation at
    // once, at a semantically wrong wall-clock moment, each with its own
    // automations.json rewrite. Past the grace window we advance the schedule
    // WITHOUT running the commands: a 07:00 morning routine should not run at
    // 14:00 because that is when the clock became known.
    // A next run that is no clock time while the clock is set was computed
    // before it was set (`automation add`, `automation recompute` or older
    // firmware counted from 1970): it is advanced the same way, but not
    // reported as a run decades late.
    if (clockDue && (now - nextAt) > kMissedFireGraceSec) {
      if (clockSet && !Clock::isValidEpoch(nextAt)) {
        DEBUG_AUTOMATIONSF("[autos] id=%ld next run %ld predates the clock; advancing without firing",
                           id, (long)nextAt);
      } else {
        logSystemEvent("AUTO", "automation %ld missed its %ld s-late schedule — skipped, rescheduled",
                       id, (long)(now - nextAt));
        DEBUG_AUTOMATIONSF("[autos] id=%ld MISSED by %lds (grace %ds) — advancing without firing",
               id, (long)(now - nextAt), (int)kMissedFireGraceSec);
      }
      rescheduleAfterFire(id, obj.c_str(), now, false);
      automationClearCondBlock(id);
      if (!eventDue) {  // an event match still deserves its fire
        pos = objEnd + 1;
        continue;
      }
      clockDue = false;  // fire for the event only; the clock leg is settled
    }

    // Check if it's time to run
    if (clockDue || eventDue) {
      const char* fireLabel = (eventDue && !clockDue) ? "Event" : "Scheduled";
      if (eventDue) {
        DEBUG_AUTOMATIONSF("[autos] id=%ld event trigger matched: %s", id,
               evKindName ? evKindName : "?");
      }
      // Extract commands
      static constexpr int MAX_AUTO_CMDS = 16;  // 16 commands per automation (saves ~576B stack vs 64)
      String cmdsList[MAX_AUTO_CMDS];
      int cmdsCount = 0;
      int cmdsPos = obj.indexOf("\"commands\"");
      bool haveArray = false;
      int arrStart = -1, arrEnd = -1;

      if (cmdsPos >= 0) {
        int cmdsColon = obj.indexOf(':', cmdsPos);
        if (cmdsColon > 0) {
          arrStart = obj.indexOf('[', cmdsColon);
          if (arrStart > 0) {
            int depth = 0;
            for (int i = arrStart; i < (int)obj.length(); ++i) {
              char c = obj[i];
              if (c == '[') depth++;
              else if (c == ']') {
                depth--;
                if (depth == 0) {
                  arrEnd = i;
                  break;
                }
              }
            }
            haveArray = (arrStart > 0 && arrEnd > arrStart);
          }
        }
      }

      if (haveArray) {
        String body = obj.substring(arrStart + 1, arrEnd);
        int i = 0;
        while (i < (int)body.length() && cmdsCount < MAX_AUTO_CMDS) {
          while (i < (int)body.length() && (body[i] == ' ' || body[i] == ',' || body[i] == '\n' || body[i] == '\r' || body[i] == '\t')) i++;
          if (i >= (int)body.length()) break;
          if (body[i] == '"') {
            int q1 = i;
            int q2 = body.indexOf('"', q1 + 1);
            if (q2 < 0) break;
            String one = body.substring(q1 + 1, q2);
            one.trim();
            if (one.length() && cmdsCount < MAX_AUTO_CMDS) { cmdsList[cmdsCount++] = one; }
            i = q2 + 1;
          } else {
            int next = body.indexOf(',', i);
            if (next < 0) break;
            i = next + 1;
          }
        }
      } else {
        // Fallback to single command
        int cpos = obj.indexOf("\"command\"");
        if (cpos >= 0) {
          int ccolon = obj.indexOf(':', cpos);
          int cq1 = obj.indexOf('"', ccolon + 1);
          int cq2 = obj.indexOf('"', cq1 + 1);
          if (cq1 > 0 && cq2 > cq1) {
            String cmd = obj.substring(cq1 + 1, cq2);
            cmd.trim();
            if (cmd.length() && cmdsCount < MAX_AUTO_CMDS) { cmdsList[cmdsCount++] = cmd; }
          }
        }
      }

      if (cmdsCount > 0) {
        // Extract automation name for logging
        String autoName = "Unknown";
        int namePos = obj.indexOf("\"name\"");
        if (namePos >= 0) {
          int colonPos = obj.indexOf(':', namePos);
          if (colonPos >= 0) {
            int q1 = obj.indexOf('"', colonPos + 1);
            int q2 = obj.indexOf('"', q1 + 1);
            if (q1 >= 0 && q2 >= 0) {
              autoName = obj.substring(q1 + 1, q2);
            }
          }
        }

        // Check global condition expression (new schema: expression only, e.g. "ROOM=bedroom")
        String condition = "";
        {
          int condPos = obj.indexOf("\"condition\"");
          if (condPos >= 0 && obj[condPos + 11] == '"') condPos = -1; // reject "conditions"
          if (condPos >= 0) {
            int condColon = obj.indexOf(':', condPos);
            if (condColon >= 0) {
              int condQ1 = obj.indexOf('"', condColon + 1);
              int condQ2 = obj.indexOf('"', condQ1 + 1);
              if (condQ1 >= 0 && condQ2 >= 0) {
                condition = obj.substring(condQ1 + 1, condQ2);
                condition.trim();
              }
            }
          }
        }

        // Evaluate global condition gate if present
        if (condition.length() > 0) {
          String wrapped = "IF " + condition + " THEN _";
          bool conditionMet = evaluateCondition(wrapped.c_str());
          DEBUG_AUTOMATIONSF("[autos] id=%ld condition='%s' result=%s",
                 id, condition.c_str(), conditionMet ? "TRUE" : "FALSE");
          if (!conditionMet) {
            if (automationLogActive()) {
              char skipBuf[256];
              snprintf(skipBuf, sizeof(skipBuf), "%s automation skipped: ID=%ld Name=%s Condition not met: %s", fireLabel, id, autoName.c_str(), condition.c_str());
              appendAutoLogEntry("AUTO_SKIP", skipBuf);
            }
            DEBUG_AUTOMATIONSF("[autos] id=%ld skipped - condition not met: %s", id, condition.c_str());
            // Deferred, not skipped: nextAt stays in the past so the fire
            // happens once the condition holds. Mute the re-check for a few
            // seconds so automationsAnyDue() stops forcing a full
            // automations.json read+parse on every main-loop pass.
            if (clockDue) automationNoteCondBlocked(id);
            pos = objEnd + 1;
            continue;
          }
        }

        char _createdByBuf[64];
        extractJsonString(obj.c_str(), "\"createdBy\"", _createdByBuf, sizeof(_createdByBuf));

        // Log scheduled automation start if logging is active
        if (automationLogActive()) {
          char startBuf[256];
          if (eventDue && !clockDue) {
            snprintf(startBuf, sizeof(startBuf), "Event automation started: ID=%ld Name=%s User=%s Trigger=%s", id, autoName.c_str(), _createdByBuf, evKindName ? evKindName : "?");
          } else {
            snprintf(startBuf, sizeof(startBuf), "Scheduled automation started: ID=%ld Name=%s User=%s", id, autoName.c_str(), _createdByBuf);
          }
          appendAutoLogEntry("AUTO_START", startBuf);
        }

        // Execute commands (with conditional logic support)
        for (int ci = 0; ci < cmdsCount; ++ci) {
          const String safeRunCmd = redactAutomationCommandForAudit(cmdsList[ci]);
          DEBUG_AUTOMATIONSF("[autos] id=%ld run cmd[%d]='%s'", id, ci, safeRunCmd.c_str());

          // Queue command for execution (async, non-blocking)
          const char* result = executeConditionalCommand(cmdsList[ci].c_str(), _createdByBuf, autoName.c_str());

          // Output the result (skip internal status messages - actual output comes from queue)
          if (!isAutoInternalResult(result)) {
            {
              char schedBuf[256];
              snprintf(schedBuf, sizeof(schedBuf), "[%s Automation %ld] %s", fireLabel, id, result);
              broadcastOutput(schedBuf);
            }
          }
        }
        executed++;

        // SYSTEM-EVENT: automation matched and dispatched its command list.
        {
          const char* trigType = (eventDue && !clockDue) ? "event"
                                 : ((obj.indexOf("\"type\": \"interval\"") >= 0) ||
                                    (obj.indexOf("\"type\":\"interval\"") >= 0)) ? "interval"
                                 : "time";
          systemEventPost(SYSEVT_AUTOMATION_FIRED, autoName.c_str(), trigType);
        }

        // Log scheduled automation end if logging is active
        if (automationLogActive()) {
          char endBuf[256];
          snprintf(endBuf, sizeof(endBuf), "%s automation completed: ID=%ld Name=%s Commands=%d", fireLabel, id, autoName.c_str(), cmdsCount);
          appendAutoLogEntry("AUTO_END", endBuf);
        }

        // Update next run time via the unified post-fire helper. Pure event
        // fires touch no clock trigger, so skip the (flash-writing) reschedule.
        if (clockDue) {
          automationClearCondBlock(id);  // it ran; drop any condition back-off
          rescheduleAfterFire(id, obj.c_str(), now, false);
        }
      } else {
        DEBUG_AUTOMATIONSF("[autos] id=%ld skip: no commands found", id);
        // A command-less automation can never do anything, so deferring it is
        // pointless — advance it like a fire, or its past nextAt spins the
        // full tick (and its file read) on every main-loop pass forever.
        if (clockDue) rescheduleAfterFire(id, obj.c_str(), now, false);
      }
    } else {
      DEBUG_AUTOMATIONSF("[autos] id=%ld wait: nextAt=%lu now=%lu", id, (unsigned long)nextAt, (unsigned long)now);
    }

    pos = objEnd + 1;
  }

  DEBUG_AUTOMATIONSF("[autos] evaluated=%d executed=%d", evaluated, executed);

  // Handle duplicate sanitization
  static unsigned long s_lastAutoSanitizeMs = 0;
  if (queueSanitize) {
    unsigned long nowMs = millis();
    if (nowMs - s_lastAutoSanitizeMs > 5000UL) {
      bool repaired = false;
      if (automationsSanitizeStore(nullptr, &repaired) && repaired) {
        DEBUG_AUTOMATIONSF("[autos] Runtime sanitize applied after duplicate detection; scheduler refresh queued");
      } else {
        DEBUG_AUTOMATIONSF("[autos] Runtime sanitize: no changes needed");
      }
      s_lastAutoSanitizeMs = nowMs;
    } else {
      DEBUG_AUTOMATIONSF("[autos] Runtime sanitize skipped (debounced)");
    }
  }

  // Rebuild the in-RAM cache from the (possibly post-fire updated) file so the
  // fast due-check in the main loop can skip a rescan until something changes.
  rebuildAutomationsCache();
}

bool automationSchedulerRunning() { return sAutomationRunning.load(); }

// True when the filesystem lock is free for this task right now (or this task
// already holds it, or there is no lock yet). A probe: the lock is released
// again at once, so it is never held across condition evaluation, sensor
// cache locks or command dispatch; each file step of the pass takes it for
// itself. Called by automationTick before it claims a pass.
static bool automationPassFilesystemFree() {
  FsLockGuard probe("automation.pass", 0);
  return probe.held || isFsLockedByCurrentTask() || gFsMutex == nullptr;
}

// Called only by the main task. Keep the cadence and pending-work consumption
// with the cache owner; other tasks merely notify, start or stop the scheduler.
void automationTick() {
  static uint32_t lastAutoCheck = 0;
  const uint32_t nowMs = millis();
  const time_t now = time(nullptr);
  {
    // Is a pass wanted? Nothing is consumed yet: the refresh flag, the pending
    // events and the due entries all stay as they are until the pass is
    // claimed below.
    AutomationStateGuard guard;
    if (!sAutomationRunning) return;
    if (!sAutomationRefreshPending.load() && !automationsAnyDue(now) &&
        !(gAutomationEventsPending && (nowMs - lastAutoCheck >= 250)) &&
        nowMs - lastAutoCheck < 60000) return;
  }
  // A pass starts with file work (the uptime carry and the read of
  // automations.json). The main loop never queues behind another task's
  // filesystem work for it: try the filesystem lock, and when it is busy leave
  // everything pending and come back on the next lap. A miss is never taken
  // for a missing or unreadable file (the cache, the refresh flag and the
  // event cursor are untouched). No state lock is held here: file work and the
  // filesystem lock stay outside AutomationStateGuard.
  if (!automationPassFilesystemFree()) {
    static uint32_t sPassLockMisses = 0;
    static uint32_t sPassLockNoteMs = 0;
    ++sPassLockMisses;
    if (everyMs(&sPassLockNoteMs, 10000)) {
      DEBUG_AUTOMATIONSF("[automations] pass deferred, filesystem busy (%lu deferral(s) so far)",
                         (unsigned long)sPassLockMisses);
    }
    (void)sPassLockMisses;  // read only by the debug line above
    return;
  }
  {
    AutomationStateGuard guard;
    if (!sAutomationRunning) return;
    sAutomationRefreshPending.exchange(false);
    // Subscription reads stay conservative until publication completes. Do
    // not clear refresh again after the tick: a concurrent save may have
    // replaced the file after we read it, even if this rebuild succeeds.
    gAutomationsCacheValid = false;
  }
  // A stop prevents later passes from being claimed. An already claimed pass
  // may finish; it holds no borrowed cache pointer or state lock during I/O
  // or command dispatch, and its rebuild cannot revive a stopped cache.
  schedulerTickMinute();
  lastAutoCheck = nowMs;
}

// Start the automation scheduler (now runs from main loop, no dedicated task)
bool startAutomationScheduler() {
  // This is the shared boot, resume, and runtime-enable chokepoint.  Keeping
  // the migration here means every way of starting Automations gets a usable
  // CLI/web store, including devices first provisioned by a profile that had
  // the feature compiled out.
  if (!ensureAutomationsFile()) return false;
  // Events posted from here on are matched by the first drain (G-73). Later
  // starts store it too, but only a drain that has not run yet reads it.
  gAutomationEventStartSeq = systemEventLatestSeq();
  {
    AutomationStateGuard guard;
    if (!ensureAutomationsCache()) return false;
    gAutomationsCacheValid = false;
    sAutomationRefreshPending = true;
    sAutomationRunning = true;
  }
  DEBUG_AUTOMATIONSF("[automations] Scheduler enabled (runs from main loop)");
  return true;
}

// Stop the automation scheduler and release its due-check cache.
void stopAutomationScheduler() {
  {
    AutomationStateGuard guard;
    sAutomationRunning = false;
    releaseAutomationsCache();
  }
  DEBUG_AUTOMATIONSF("[automations] Scheduler disabled");
}

// `automation system enable|disable` lands here (`features` only lists since
// 2026-11-20): it starts or stops the scheduler now and saves nothing; whether
// it runs from boot is automationautostart. Returns nullptr, or an error reply.
const char* automationSetSystemEnabled(bool enable) {
  if (!enable) {
    stopAutomationScheduler();
    return nullptr;
  }
  if (!startAutomationScheduler()) {
    return "Error: failed to allocate automation scheduler cache";
  }
  return nullptr;
}

// ============================================================================
// Print / Broadcast Command
// ============================================================================

static const char* cmd_print(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (argsInput.length() == 0) return "Error: invalid arguments — Usage: print <message>";
  broadcastOutput(argsInput.c_str());
  return "Message printed";
}

// ============================================================================
// Automation Command Registry
// ============================================================================

// CommandEntry struct is defined in system_utils.h (included via automation_system.h)

// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
const CommandEntry automationCommands[] = {
  // Primary dispatcher: "automation <subcommand> [args]"
  // Subcommands: system enable|disable|status, list, add, enable, disable, delete, run, sanitize, recompute
  { "automation", "Automation system: automation <subcommand> [args].", true, cmd_automation,
    "Usage: automation <system enable|disable|status | list | add | enable | disable | delete | run | trigger | sanitize | recompute>" },

  // Open to every signed-in user (the phone app reads `automationlist json`);
  // 'automation list' is the same list behind the admin row above.
  { "automationlist", "List all automations.", false, cmd_automation_list },

  // Utility commands
  { "autolog", "Automation logging: autolog start <file> | stop | status.", true, cmd_autolog, "Usage: autolog start <filename> | autolog stop | autolog status" },
  { "validateconditions", "Validate conditional automation syntax: validateconditions IF temp>75 THEN ledcolor red.", true, cmd_validate_conditions },
  { "print", "Broadcast a message to all outputs: print <message>.", false, cmd_print },
  
  // NOTE: downloadautomation and if/conditional commands are registered
  // in the main .ino file's command registry to avoid duplication
};

const size_t automationCommandsCount = sizeof(automationCommands) / sizeof(automationCommands[0]);

// Registration handled by gCommandModules[] in System_Utils.cpp

// ============================================================================
// Automation Settings Module
// ============================================================================

// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry automationSettingEntries[] = {
  { "automationAutoStart", SETTING_BOOL, &gSettings.automationAutoStart, 1, 0, nullptr, 0, 1, "Auto-start at boot", nullptr, false, nullptr, "automationautostart" },

};

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule automationSettingsModule = {
  "automation", "apps.automation", automationSettingEntries,
  sizeof(automationSettingEntries) / sizeof(automationSettingEntries[0]),
  nullptr,
  "Automation rules and scheduling"
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp

#endif // ENABLE_AUTOMATION
