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
#include "System_BuildConfig.h"

#if ENABLE_AUTOMATION

#include <ArduinoJson.h>
#include <LittleFS.h>
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

#if ENABLE_APDS_SENSOR
#include "i2csensor_apds9960.h"
#endif
#if ENABLE_THERMAL_SENSOR
#include "i2csensor_mlx90640.h"
#endif
#if ENABLE_TOF_SENSOR
#include "i2csensor_vl53l4cx.h"
#endif

// External dependencies from .ino
extern bool gCLIValidateOnly;
// gDebugBuffer, gDebugFlags, ensureDebugBuffer now from debug_system.h
extern void runUnifiedSystemCommand(const String& argsInput);

// Command types from shared header
#include "System_CommandTypes.h"
extern bool submitCommandAsync(const Command& cmd, ExecAsyncCallback callback, void* userData);

// Queue an automation sub-command through the FreeRTOS command queue (async, non-blocking).
// This avoids deadlock when already on cmd_exec task and avoids blocking the main loop.
// `owner` is the PRINCIPAL to execute as — stamped into cmd.ctx.auth.user so VFS permission
// checks, audit logs, and notifications attribute the work to the right user on cmd_exec_task.
// For autonomous triggers (boot/scheduled) this is the automation's createdBy; for a MANUAL
// run it is the TRIGGERING user (an automation run executes as whoever fired it, not the
// creator). isAdmin is recomputed from this username by ExecIdentityGuard, so the username
// alone is the complete principal.
// `autoName` is the automation's display name — stamped into ctx.automationName so
// executeCommand can write COMMAND/OUTPUT autolog entries attributed to this automation,
// with no race against the scheduler advancing to the next automation before the command runs.
static void queueAutomationSubCommand(const char* cmd, const char* owner, const char* autoName = nullptr) {
  Command uc;
  uc.line = cmd;
  uc.ctx.origin = ORIGIN_AUTOMATION;
  uc.ctx.auth.transport = SOURCE_INTERNAL;
  uc.ctx.auth.user = owner ? owner : "";
  DEBUGF(DEBUG_AUTOMATIONS, "[autos queue] Queueing cmd='%s' user='%s' automation='%s'",
         cmd, owner ? owner : "", autoName ? autoName : "");
  uc.ctx.auth.ip = "";
  uc.ctx.auth.path = "/automation";
  uc.ctx.auth.sid = "";
  uc.ctx.auth.opaque = nullptr;
  uc.ctx.id = (uint32_t)millis();
  uc.ctx.timestampMs = (uint32_t)millis();
  uc.ctx.outputMask = CMD_OUT_SERIAL | CMD_OUT_WEB | CMD_OUT_LOG;
  uc.ctx.validateOnly = false;
  uc.ctx.replyHandle = nullptr;
  uc.ctx.httpReq = nullptr;
  if (autoName && autoName[0]) {
    strncpy(uc.ctx.automationName, autoName, sizeof(uc.ctx.automationName) - 1);
    uc.ctx.automationName[sizeof(uc.ctx.automationName) - 1] = '\0';
  }
  if (!submitCommandAsync(uc, nullptr, nullptr)) {
    DEBUGF(DEBUG_AUTOMATIONS, "[autos] FAILED to queue sub-command: %s", cmd);
  } else {
    DEBUGF(DEBUG_AUTOMATIONS, "[autos] Queued sub-command: %s", cmd);
  }
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

// Automation state variables (defined here, used by .ino and this file)
bool gAutoLogActive = false;
String gAutoLogFile = "";

// Captured AuthContext of the user who started automation logging. Each
// automation log line is written via VFS::openGuarded with this context, so
// the caller's permissions gate every write — not just the initial start.
//
// Why captured-and-held instead of read-from-current-task at write time:
// appendAutoLogEntry fires from the automation evaluator (and from command
// dispatch hooks) at moments that have no relation to any user CLI session,
// so currentAuthContext() would resolve to ANON or whatever the firing task
// happens to have installed. Capturing the starter's identity gives every
// subsequent write a stable, named principal.
//
// Lifecycle: zero-init (anon) at boot. Populated when `autolog start` runs.
// Cleared back to anon when `autolog stop` runs. While active, the same ctx
// applies to every line. If the starter's permissions change mid-run (e.g.
// admin demoted to user), individual writes will start failing — that's the
// intended behavior; they no longer have the right to write the log.
//
// This is the first long-lived AuthContext in the codebase. If you find
// yourself adding a second (e.g. for scheduled commands), consider
// generalising the pattern instead of growing per-feature globals.
AuthContext gAutoLogOwnerCtx;

// Forward declarations for functions implemented in this file
bool updateAutomationNextAt(long automationId, time_t newNextAt);
time_t computeNextRunTime(const char* automationJson, time_t fromTime);
// Unified post-fire helper (Trigger model defined later in this file).
static void rescheduleAfterFire(long id, const char* automationJson, time_t firedAt);
const char* executeConditionalCommand(const char* command, const char* owner, const char* autoName = nullptr);
const char* evaluateConditionalChain(const char* chainStr, char* outBuf, size_t outBufSize);
bool evaluateCondition(const char* condition);
const char* validateConditionalHierarchy(const char* conditions);
const char* validateConditionSyntax(const char* condition);
const char* validateConditionalChain(const char* chainStr);
const char* validateConditionalCommand(const char* command);
bool automationIdExistsInJson(const String& json, unsigned long id);
// jsonEscape now provided by system_utils.h
void findAutomationsArrayBounds(const String& json, int& arrStart, int& arrEnd);

// DEBUG flags and RETURN_VALID_IF_VALIDATE_CSTR are defined centrally in debug_system.h
// and system_utils.h to keep logging behavior consistent across modules.
// BROADCAST_PRINTF is also provided by debug_system.h.

// File paths
extern const char* AUTOMATIONS_JSON_FILE;  // Defined in .ino as "/system/automations.json"

// Filesystem lock helpers (defined in .ino) - now in automation_system.h

// RAII filesystem lock guard
class FsLockGuard {
public:
  FsLockGuard(const char* owner) { fsLock(owner); }
  ~FsLockGuard() { fsUnlock(); }
};

// Global automation state
long* gAutoMemoId = nullptr;
time_t* gAutoMemoNextAt = nullptr;
int gAutoMemoCount = 0;
bool gAutosDirty = false;

// Forward declarations for internal functions
static bool extractJsonString(const char* json, const char* key, char* out, size_t outSize);
static long extractJsonLong(const char* json, const char* key);
static bool extractJsonBool(const char* json, const char* key);

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

// Helper: extract JSON number value by key from C-string
static long extractJsonLong(const char* json, const char* key) {
  const char* keyPos = strstr(json, key);
  if (!keyPos) return 0;

  const char* colon = strchr(keyPos, ':');
  if (!colon) return 0;

  return atol(colon + 1);
}

// Helper: check if JSON boolean is true
static bool extractJsonBool(const char* json, const char* key) {
  const char* keyPos = strstr(json, key);
  if (!keyPos) return false;

  const char* colon = strchr(keyPos, ':');
  if (!colon) return false;

  // Skip whitespace after colon
  const char* p = colon + 1;
  while (*p == ' ' || *p == '\t') p++;

  return (strncmp(p, "true", 4) == 0);
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

// Streaming automation parser: reads file in chunks and calls callback for each automation object
bool streamParseAutomations(const char* path, AutomationCallback callback, void* userData) {
  FsLockGuard guard("streamParseAutos");
  File f = VFS::openGuarded(path, "r", VFS::systemAuth("auto.stream_parse"));
  if (!f) return false;

  // Read file in chunks, looking for automation objects
  const size_t kChunkSize = 512;
  static char* readBuf = nullptr;
  if (!readBuf) {
    readBuf = (char*)ps_alloc(kChunkSize, AllocPref::PreferPSRAM, "auto.stream.read");
    if (!readBuf) {
      f.close();
      return false;
    }
  }

  // Buffer to accumulate current automation object
  static char* objBuf = nullptr;
  static const size_t kObjBufSize = 4096;  // Max size for one automation object
  if (!objBuf) {
    objBuf = (char*)ps_alloc(kObjBufSize, AllocPref::PreferPSRAM, "auto.stream.obj");
    if (!objBuf) {
      f.close();
      return false;
    }
  }

  size_t objLen = 0;
  int braceDepth = 0;
  bool inString = false;
  bool inArray = false;  // Track if we're inside "automations" array
  bool foundArray = false;
  char prevChar = 0;

  while (f.available()) {
    size_t n = f.readBytes(readBuf, kChunkSize);
    if (n == 0) break;

    for (size_t i = 0; i < n; ++i) {
      char c = readBuf[i];

      // Track string boundaries (ignore escaped quotes)
      if (c == '"' && prevChar != '\\') {
        inString = !inString;
      }

      // Only process structure outside of strings
      if (!inString) {
        // Look for "automations" array start
        if (!foundArray && c == '[') {
          inArray = true;
          foundArray = true;
          prevChar = c;
          continue;
        }

        if (inArray) {
          if (c == '{') {
            braceDepth++;
            if (braceDepth == 1) {
              // Start of new automation object
              objLen = 0;
            }
          } else if (c == '}') {
            if (objLen < kObjBufSize - 1) objBuf[objLen++] = c;
            braceDepth--;
            if (braceDepth == 0 && objLen > 0) {
              // Complete automation object extracted
              objBuf[objLen] = '\0';

              // Call callback with this automation
              bool continueProcessing = callback(objBuf, objLen, userData);
              if (!continueProcessing) {
                f.close();
                return true;  // Early exit requested by callback
              }

              objLen = 0;
            }
            prevChar = c;
            continue;
          } else if (c == ']' && braceDepth == 0) {
            // End of automations array
            inArray = false;
            break;
          }
        }
      }

      // Accumulate characters for current object
      if (inArray && braceDepth > 0 && objLen < kObjBufSize - 1) {
        objBuf[objLen++] = c;
      }

      prevChar = c;
    }

    if (!inArray && foundArray) break;  // Finished processing automations array
  }

  f.close();
  return true;
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

// Helper: Check if automation ID exists in JSON (stack-based)
bool automationIdExistsInJson(const String& json, unsigned long id) {
  char needle[32];
  snprintf(needle, sizeof(needle), "\"id\": %lu", id);
  return strstr(json.c_str(), needle) != nullptr;
}

// Sanitize duplicate IDs in automations array using ArduinoJson
bool sanitizeAutomationsJson(String& jsonRef) {
  // Use ArduinoJson for proper parsing
  PSRAM_JSON_DOC(doc);
  DeserializationError error = deserializeJson(doc, jsonRef);
  if (error) {
    DEBUGF(DEBUG_AUTOMATIONS, "[sanitize] JSON parse error: %s", error.c_str());
    return false;
  }
  
  JsonArray automations = doc["automations"].as<JsonArray>();
  if (automations.isNull()) {
    return false;
  }
  
  // Track seen IDs
  const int kMax = 512;
  unsigned long seen[kMax];
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
    
    if (!dup) {
      if (seenCount < kMax) seen[seenCount++] = idVal;
      continue;
    }
    
    // Duplicate found: generate a new unique ID
    unsigned long newId = (unsigned long)millis();
    int guard = 0;
    
    // Check against already seen IDs in this pass
    auto idExists = [&](unsigned long testId) {
      for (int k = 0; k < seenCount; ++k) {
        if (seen[k] == testId) return true;
      }
      return false;
    };
    
    while (idExists(newId) && guard < 100) {
      newId += 1 + (unsigned long)random(1, 100000);
      guard++;
    }
    
    automation["id"] = newId;
    if (seenCount < kMax) seen[seenCount++] = newId;
    changed = true;
    
    DEBUGF(DEBUG_AUTOMATIONS, "[sanitize] Replaced duplicate id %lu with %lu", idVal, newId);
  }
  
  if (changed) {
    // Serialize back to string
    jsonRef = "";
    serializeJsonPretty(doc, jsonRef);
  }
  
  return changed;
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

struct AutoCacheEntry {
  long id;
  time_t nextAt;
  bool enabled;
};

static constexpr int AUTO_CACHE_MAX = 64;
static AutoCacheEntry gAutoCache[AUTO_CACHE_MAX];
static volatile int gAutoCacheCount = 0;
static volatile bool gAutoCacheValid = false;

// Re-read automations.json and refill the cache. Called at the end of the
// full tick so post-fire nextAt updates are captured on the same pass.
static void rebuildAutoCache() {
  String json;
  if (!readText(AUTOMATIONS_JSON_FILE, json)) {
    gAutoCacheCount = 0;
    gAutoCacheValid = true;
    return;
  }
  PSRAM_JSON_DOC(doc);
  if (deserializeJson(doc, json)) {
    gAutoCacheCount = 0;
    gAutoCacheValid = true;
    return;
  }
  JsonArrayConst autos = doc["automations"].as<JsonArrayConst>();
  int n = 0;
  for (JsonObjectConst a : autos) {
    if (n >= AUTO_CACHE_MAX) break;
    gAutoCache[n].id = a["id"].as<long>();
    gAutoCache[n].enabled = a["enabled"] | false;
    // Cache the min nextAt across the automation's triggers. 0 means nothing
    // is scheduled (all manual/boot, or unset).
    time_t minAt = 0;
    JsonArrayConst triggers = a["triggers"].as<JsonArrayConst>();
    if (!triggers.isNull()) {
      for (JsonVariantConst t : triggers) {
        unsigned long raw = t["nextAt"] | 0UL;
        time_t na = (time_t)raw;
        if (na > 0 && (minAt == 0 || na < minAt)) minAt = na;
      }
    }
    gAutoCache[n].nextAt = minAt;
    n++;
  }
  gAutoCacheCount = n;
  gAutoCacheValid = true;
}

// Fast in-RAM check: is any enabled automation's nextAt at or before `now`?
// Called from the main loop every iteration. Returns true if the full tick
// must run (something is due, or the cache is stale).
bool automationsAnyDue(time_t now) {
  if (!gAutoCacheValid) return true;  // force a full tick to rebuild
  for (int i = 0; i < gAutoCacheCount; i++) {
    if (gAutoCache[i].enabled && gAutoCache[i].nextAt > 0 && now >= gAutoCache[i].nextAt) {
      return true;
    }
  }
  return false;
}

// Atomic writer for automations.json. Invalidates the cache on every write
// so the next main-loop iteration rebuilds it.
bool writeAutomationsJsonAtomic(const String& json) {
  gAutoCacheValid = false;
  return writeTextAtomic(AUTOMATIONS_JSON_FILE, json);
}

// Update the nextAt field of a specific trigger within an automation.
// Phase 1: `triggerIdx=-1` is an alias for "first schedulable trigger" (the
// behavior legacy callers that used to pass only an id+nextAt expect).
// Phase 1B will remove the alias and force an explicit trigger index.
bool updateAutomationTriggerNextAt(long automationId, int triggerIdx, time_t newNextAt) {
  String json;
  if (!readText(AUTOMATIONS_JSON_FILE, json)) return false;

  PSRAM_JSON_DOC(doc);
  DeserializationError error = deserializeJson(doc, json);
  if (error) {
    DEBUGF(DEBUG_AUTOMATIONS, "[updateNextAt] JSON parse error: %s", error.c_str());
    return false;
  }

  JsonArray automations = doc["automations"].as<JsonArray>();
  if (automations.isNull()) return false;

  bool found = false;
  for (JsonObject automation : automations) {
    if (automation["id"].as<long>() != automationId) continue;
    JsonArray triggers = automation["triggers"].as<JsonArray>();
    if (triggers.isNull()) break;
    // Pick the trigger to update.
    int chosen = triggerIdx;
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
        t["nextAt"] = (unsigned long)newNextAt;
        found = true;
        break;
      }
      idx++;
    }
    break;
  }

  if (!found) return false;

  json = "";
  serializeJsonPretty(doc, json);
  return writeAutomationsJsonAtomic(json);
}

// Legacy shim: writes nextAt into the first schedulable trigger. Callers that
// pre-dated the trigger-array refactor still work unchanged.
bool updateAutomationNextAt(long automationId, time_t newNextAt) {
  return updateAutomationTriggerNextAt(automationId, -1, newNextAt);
}

// Run automations on boot
void runAutomationsOnBoot() {
  static bool s_ran = false;
  if (s_ran) return;
  s_ran = true;

  if (!filesystemReady) return;

  DEBUGF(DEBUG_AUTOMATIONS, "[automations] Checking for boot automations");

  // Read automations.json
  String json;
  if (!readText(AUTOMATIONS_JSON_FILE, json)) {
    DEBUGF(DEBUG_AUTOMATIONS, "[automations] No automations file found");
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
      DEBUGF(DEBUG_AUTOMATIONS, "[automations] id=%ld boot condition='%s' result=%s",
             id, condition.c_str(), conditionMet ? "TRUE" : "FALSE");
      if (!conditionMet) {
        if (gAutoLogActive) {
          char skipMsg[256];
          snprintf(skipMsg, sizeof(skipMsg), "Boot automation skipped: ID=%ld Name=%s Condition not met: %s",
                   id, autoName.c_str(), condition.c_str());
          appendAutoLogEntry("AUTO_SKIP", skipMsg);
        }
        pos = objEnd + 1;
        continue;
      }
    }

    // Log automation start
    if (bootDelayMs > 0) {
      DEBUGF(DEBUG_AUTOMATIONS, "[automations] Running boot automation: %s (delay: %dms)", autoName.c_str(), bootDelayMs);
      delay(bootDelayMs);
    } else {
      DEBUGF(DEBUG_AUTOMATIONS, "[automations] Running boot automation: %s", autoName.c_str());
    }

    char _createdByBuf[64];
    extractJsonString(obj.c_str(), "\"createdBy\"", _createdByBuf, sizeof(_createdByBuf));

    if (gAutoLogActive) {
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

    // Free allocated command strings
    for (int ci = 0; ci < cmdsCount; ++ci) {
      free(cmdsList[ci]);
    }

    if (gAutoLogActive) {
      char endMsg[256];
      snprintf(endMsg, sizeof(endMsg), "Boot automation completed: ID=%ld Name=%s Commands=%d",
               id, autoName.c_str(), cmdsCount);
      appendAutoLogEntry("AUTO_END", endMsg);
    }

    DEBUGF(DEBUG_AUTOMATIONS, "[automations] Boot automation completed: %s", autoName.c_str());

    if (now > 0) {
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
  // Allocate memo buffers if not already allocated
  if (!gAutoMemoId) {
    gAutoMemoId = (long*)ps_alloc(kAutoMemoCap * sizeof(long), AllocPref::PreferPSRAM, "auto.memo.id");
    if (!gAutoMemoId) return false;
  }
  if (!gAutoMemoNextAt) {
    gAutoMemoNextAt = (time_t*)ps_alloc(kAutoMemoCap * sizeof(time_t), AllocPref::PreferPSRAM, "auto.memo.nextat");
    if (!gAutoMemoNextAt) return false;
  }
  
  gAutoMemoCount = 0;
  DEBUGF(DEBUG_AUTOMATIONS, "[automations] System initialized");
  
  // Start the automation scheduler
  if (!startAutomationScheduler()) {
    DEBUGF(DEBUG_AUTOMATIONS, "[automations] WARNING: Failed to start scheduler");
    return false;
  }
  
  return true;
}

// Suspend automation system
void suspendAutomationSystem() {
  stopAutomationScheduler();
  DEBUGF(DEBUG_AUTOMATIONS, "[automations] System suspended");
}

// Resume automation system
void resumeAutomationSystem() {
  startAutomationScheduler();
  DEBUGF(DEBUG_AUTOMATIONS, "[automations] System resumed");
}

// Execute automation command (queues through FreeRTOS command queue, non-blocking)
void runAutomationCommandUnified(const String& argsInput) {
  queueAutomationSubCommand(argsInput.c_str(), "");
}

// Automation command handlers
const char* cmd_automation_list(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const bool wantJson = argWantsJson(argsInput);

  String json;
  if (!readText(AUTOMATIONS_JSON_FILE, json)) {
    if (wantJson) return "{\"error\":\"read failed\"}";
    broadcastOutput("Error: failed to read automations.json");
    return "ERROR";
  }

  // Private channel: the stored automations document goes back verbatim via the
  // RETURN VALUE (the app reads it off its own channel). No broadcastOutput —
  // that unconditional broadcast is what used to dump this whole JSON blob onto
  // the shared human consoles. Held in a function-static String so the returned
  // c_str() stays valid until the next command (handlers run serially on
  // cmd_exec_task). Returned verbatim — it's a document with its own schema, not
  // a synthesized {"v":1} status blob, so wrapping it would break app compat.
  if (wantJson) {
    static String jsonHold;
    jsonHold = json;
    return jsonHold.c_str();
  }

  // Human channel: a readable summary instead of the raw JSON wall.
  PSRAM_JSON_DOC(doc);
  if (deserializeJson(doc, json)) {
    broadcastOutput("Error: automations.json is corrupt (use 'automationlist json' for the raw record)");
    return "ERROR";
  }
  JsonArray arr = doc["automations"].as<JsonArray>();
  const int n = arr.size();
  if (n == 0) {
    broadcastOutput("No automations configured.");
    return "OK";
  }
  BROADCAST_PRINTF("Automations (%d):", n);
  int i = 0;
  for (JsonObject a : arr) {
    const char* nm = a["name"] | "(unnamed)";
    BROADCAST_PRINTF("  [%d] %-24.24s (id=%ld, %s)",
                     i++, nm, a["id"].as<long>(),
                     (a["enabled"] | false) ? "enabled" : "disabled");
  }
  broadcastOutput("(use 'automationlist json' for the full record)");
  return "OK";
}

const char* cmd_automation_add(const String& argsInput) {
  // Do not early-return on validate; we want to perform full argument checks
  bool validateOnly = gCLIValidateOnly;
  
  CommandArgs a(argsInput);

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
  // triggerMode (Option 2): "once" => the top-level condition fires only on the
  // false->true crossing; "repeat"/missing => fire every poll while true (legacy).
  String triggerMode = a.value("triggermode");
  String enabledStr = a.value("enabled");
  
  bool enabled = (enabledStr.equalsIgnoreCase("1") || enabledStr.equalsIgnoreCase("true") || enabledStr.equalsIgnoreCase("yes"));
  
  String typeNorm = type;
  typeNorm.trim();
  typeNorm.toLowerCase();
  
  DEBUGF(DEBUG_AUTOMATIONS, "[autos add] name='%s' type='%s' time='%s' days='%s' delayms='%s' intervalms='%s' enabled=%d",
         name.c_str(), typeNorm.c_str(), timeS.c_str(), days.c_str(), delayMs.c_str(), intervalMs.c_str(), enabled ? 1 : 0);
  
  if (name.length() == 0) {
    broadcastOutput("Error: missing name");
    return "ERROR";
  }
  if (typeNorm.length() == 0) {
    broadcastOutput("Error: missing type (atTime|afterDelay|interval)");
    return "ERROR";
  }
  if ((cmdStr.length() == 0 && cmdsList.length() == 0)) {
    broadcastOutput("Error: missing commands (provide commands=<cmd1;cmd2;...> or command=<cmd>)");
    return "ERROR";
  }
  
  // Validate global condition expression if provided (bare expression, e.g. "ROOM=bedroom")
  // Wrap in IF...THEN to reuse validateConditionSyntax operator/structure checks
  if (condition.length() > 0) {
    condition.trim();
    String wrapped = "IF " + condition + " THEN _";
    const char* conditionError = validateConditionSyntax(wrapped.c_str());
    if (conditionError && conditionError[0] != '\0') {
      static char errorBuf[192];
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
            broadcastOutput(validationError);
            return "ERROR";
          }
        } else {
          // Validate this individual command exists in the registry.
          // Using findCommand() instead of recursive executeCommand() — a full
          // executeCommand() call from inside cmd_automation_add (which is
          // itself running inside executeCommand on cmd_exec_task) doubles the
          // already-deep stack and causes an overflow on 24 KB budgets.
          // findCommand() is a plain registry walk with no stack-heavy setup.
          if (!findCommand(part)) {
            BROADCAST_PRINTF("Error: Unknown command '%s'", part.c_str());
            return "ERROR";
          }
        }
      }
      start = i + 1;
    }
  }
  
  auto isNumeric = [&](const String& s) {
    if (!s.length()) return false;
    for (size_t i = 0; i < s.length(); ++i) {
      char c = s[i];
      if (c < '0' || c > '9') return false;
    }
    return true;
  };
  
  if (typeNorm == "attime") {
    timeS.trim();
    if (timeS.length() == 0) {
      broadcastOutput("Error: atTime requires time=HH:MM");
      return "ERROR";
    }
    if (!(timeS.length() == 5 && timeS[2] == ':' && isdigit(timeS[0]) && isdigit(timeS[1]) && isdigit(timeS[3]) && isdigit(timeS[4]))) {
      broadcastOutput("Error: time must be HH:MM");
      return "ERROR";
    }
  } else if (typeNorm == "afterdelay") {
    if (!isNumeric(delayMs)) {
      broadcastOutput("Error: afterDelay requires numeric delayms (milliseconds)");
      return "ERROR";
    }
  } else if (typeNorm == "interval") {
    if (!isNumeric(intervalMs)) {
      broadcastOutput("Error: interval requires numeric intervalms (milliseconds)");
      return "ERROR";
    }
  } else {
    broadcastOutput("Error: invalid type (expected atTime|afterDelay|interval)");
    return "ERROR";
  }
  
  // Validate boot delay if provided
  if (bootDelayMsStr.length() > 0 && !isNumeric(bootDelayMsStr)) {
    broadcastOutput("Error: bootdelayms must be numeric (milliseconds)");
    return "ERROR";
  }
  
  bool runAtBoot = (runAtBootStr.equalsIgnoreCase("1") || runAtBootStr.equalsIgnoreCase("true") || runAtBootStr.equalsIgnoreCase("yes"));
  
  String json;
  bool hadFile = readText(AUTOMATIONS_JSON_FILE, json);
  if (!hadFile || json.length() == 0) {
    json = String("{\n  \"version\": 2,\n  \"automations\": []\n}\n");
    if (!validateOnly) {
      writeAutomationsJsonAtomic(json);
      DEBUGF(DEBUG_AUTOMATIONS, "[autos add] created default automations.json");
    }
  }
  
  // If a specific id= was provided and that entry already exists, remove it first
  String idOverrideStr = a.value("id");
  if (idOverrideStr.length() > 0) {
    unsigned long overrideId = strtoul(idOverrideStr.c_str(), nullptr, 10);
    if (automationIdExistsInJson(json, overrideId)) {
      char needleBuf[32];
      snprintf(needleBuf, sizeof(needleBuf), "\"id\": %lu", overrideId);
      String needle = needleBuf;
      int idPos = json.indexOf(needle);
      int aS = json.indexOf('[');
      int aE = json.lastIndexOf(']');
      if (idPos >= 0 && aS >= 0 && aE >= 0) {
        int oS = json.lastIndexOf('{', idPos);
        int oE = (oS >= 0) ? findJsonObjectEnd(json, oS) : -1;
        if (oS >= 0 && oE >= 0) {
          // Authorization check for edit: extract createdBy from existing automation
          String existingObj = json.substring(oS, oE + 1);
          char existingCreatedByBuf[64];
          extractJsonString(existingObj.c_str(), "\"createdBy\"", existingCreatedByBuf, sizeof(existingCreatedByBuf));
          String existingCreatedBy = existingCreatedByBuf;
          
          DEBUGF(DEBUG_AUTOMATIONS, "[autos add/edit] Editing existing id=%lu createdBy='%s' requestedBy='%s' isAdmin=%d",
                 overrideId, existingCreatedBy.c_str(), currentExecUser().c_str(), currentExecIsAdmin());

          if (!currentExecIsAdmin() && existingCreatedBy != currentExecUser()) {
            DEBUGF(DEBUG_AUTOMATIONS, "[autos add/edit] AUTHORIZATION DENIED: non-admin '%s' cannot edit automation created by '%s'",
                   currentExecUser().c_str(), existingCreatedBy.c_str());
            broadcastOutput("Error: You do not have permission to edit this automation. Only the creator or an admin can edit it.");
            return "ERROR";
          }
          DEBUGF(DEBUG_AUTOMATIONS, "[autos add/edit] AUTHORIZATION OK for edit");
          String arrTmp = json.substring(aS + 1, aE); arrTmp.trim();
          if (arrTmp.indexOf('{') == arrTmp.lastIndexOf('{')) {
            json = json.substring(0, aS + 1) + json.substring(aE);
          } else {
            int dS = oS, dE = oE + 1;
            int cs = dE;
            while (cs < (int)json.length() &&
                   (json[cs]==' '||json[cs]=='\n'||json[cs]=='\r'||json[cs]=='\t')) cs++;
            if (cs < (int)json.length() && json[cs] == ',') {
              dE = cs + 1;
            } else {
              int cp = json.lastIndexOf(',', oS);
              if (cp > aS) dS = cp;
            }
            json = json.substring(0, dS) + json.substring(dE);
          }
        }
      }
    }
  }

  int arrStart = json.indexOf("\"automations\"");
  int bracket = (arrStart >= 0) ? json.indexOf('[', arrStart) : -1;
  int lastBracket = -1;
  if (bracket >= 0) {
    int depth = 0;
    for (int i = bracket; i < (int)json.length(); ++i) {
      char c = json[i];
      if (c == '[') depth++;
      else if (c == ']') {
        depth--;
        if (depth == 0) {
          lastBracket = i;
          break;
        }
      }
    }
  }
  
  if (lastBracket < 0) {
    broadcastOutput("Error: malformed automations.json");
    return "ERROR";
  }
  
  String between = json.substring(bracket + 1, lastBracket);
  between.trim();
  bool empty = (between.length() == 0);
  
  // Use provided id= if given, otherwise generate a unique one
  unsigned long id;
  if (idOverrideStr.length() > 0) {
    id = strtoul(idOverrideStr.c_str(), nullptr, 10);
  } else {
    id = millis();
    int guard = 0;
    while (automationIdExistsInJson(json, id) && guard < 100) {
      id += 1 + (unsigned long)random(1, 100000);
      guard++;
    }
  }
  
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
      if (weekInterval.length() > 0 && weekInterval.toInt() > 1) b += ",\n      \"weekInterval\": " + String(weekInterval.toInt());
      if (dayOfMonth.length() > 0) {
        int dom = dayOfMonth.toInt();
        if (dom >= 1 && dom <= 31) b += ",\n      \"dayOfMonth\": " + String(dom);
      }
      if (monthOfYear.length() > 0) {
        int moy = monthOfYear.toInt();
        if (moy >= 1 && moy <= 12) b += ",\n      \"month\": " + String(moy);
      }
    } else if (tt == "manual") {
      if (delayMs.length() > 0) b += ",\n      \"delayMs\": " + delayMs;
    } else if (tt == "interval") {
      if (intervalMs.length() > 0) b += ",\n      \"intervalMs\": " + intervalMs;
    } else if (tt == "boot") {
      if (bootDelayMsStr.length() > 0) b += ",\n      \"bootDelayMs\": " + bootDelayMsStr;
    }
    return b;
  };

  // Compute initial nextAt for a given trigger body (wraps into a temporary
  // automation object so computeNextRunTime can parse it).
  auto computeInitialNextAt = [&](const String& body) -> time_t {
    time_t now = time(nullptr);
    if (now <= 0) return 0;
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
    DEBUGF(DEBUG_AUTOMATIONS, "[autos add] primary trigger type=%s nextAt=%lu",
           triggerType.c_str(), (unsigned long)nextAt);
  }
  if (runAtBoot && triggerType != "boot") {
    String body = buildPrimaryBody("boot");
    triggersJson += ",\n    {\n" + body + ",\n      \"nextAt\": null\n    }";
    DEBUGF(DEBUG_AUTOMATIONS, "[autos add] added synthesized boot trigger from runAtBoot");
  }

  // Parse and append secondary triggers (from the UI's + Add Trigger rows).
  // Each secondary is a JSON object in the same shape as the primary trigger
  // would produce — we just serialize it back in and compute nextAt per entry.
  if (secondaryTriggersJson.length() > 0) {
    PSRAM_JSON_DOC(secDoc);
    if (!deserializeJson(secDoc, secondaryTriggersJson)) {
      JsonArrayConst secArr = secDoc.as<JsonArrayConst>();
      if (!secArr.isNull()) {
        time_t nowSec = time(nullptr);
        int primaryCount = 1 + ((runAtBoot && triggerType != "boot") ? 1 : 0);
        int addedSec = 0;
        for (JsonVariantConst sv : secArr) {
          if (primaryCount + addedSec >= 4) break;  // cap at 4 total
          const char* stype = sv["type"] | "";
          if (!stype[0]) continue;
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
            long ims = sv["intervalMs"] | 0;
            if (ims > 0) body += ",\n      \"intervalMs\": " + String(ims);
          } else if (strcmp(stype, "manual") == 0) {
            long dms = sv["delayMs"] | 0;
            body += ",\n      \"delayMs\": " + String(dms);
          } else if (strcmp(stype, "boot") == 0) {
            long bms = sv["bootDelayMs"] | 0;
            body += ",\n      \"bootDelayMs\": " + String(bms);
          } else {
            continue;  // unknown type, skip
          }
          // Compute initial nextAt for this secondary trigger.
          time_t snext = 0;
          if (nowSec > 0) {
            String tmp = "{\"triggers\":[{\n" + body + "\n    }]}";
            snext = computeNextRunTime(tmp.c_str(), nowSec);
          }
          if (snext > 0) body += ",\n      \"nextAt\": " + String((unsigned long)snext);
          else              body += ",\n      \"nextAt\": null";
          triggersJson += ",\n    {\n" + body + "\n    }";
          addedSec++;
        }
        if (addedSec > 0) {
          DEBUGF(DEBUG_AUTOMATIONS, "[autos add] appended %d secondary trigger(s)", addedSec);
        }
      }
    }
  }

  triggersJson += "\n  ]";

  // Build final automation object
  String obj = "{\n";
  String createdBy = currentExecUser();
  DEBUGF(DEBUG_AUTOMATIONS, "[autos add] Storing automation id=%ld name='%s' createdBy='%s'", id, name.c_str(), createdBy.c_str());
  obj += "  \"id\": " + String(id) + ",\n";
  obj += "  \"name\": \"" + jsonEscape(name) + "\",\n";
  obj += "  \"createdBy\": \"" + jsonEscape(createdBy) + "\",\n";
  obj += "  \"enabled\": " + String(enabled ? "true" : "false") + ",\n";
  if (condition.length() > 0) obj += "  \"condition\": \"" + jsonEscape(condition) + "\",\n";
  // Persist triggerMode only when "once" (missing/"repeat" = default, keeps JSON
  // clean and backward-compatible with existing automations).
  if (triggerMode == "once") obj += "  \"triggerMode\": \"once\",\n";
  obj += triggersJson + ",\n";
  obj += "  \"commands\": " + commandsJson + "\n";
  obj += "}";
  String insert = empty ? ("\n" + obj + "\n") : (",\n" + obj + "\n");
  json = json.substring(0, lastBracket) + insert + json.substring(lastBracket);
  
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!writeAutomationsJsonAtomic(json)) {
    broadcastOutput("Error: failed to write automations.json");
    return "ERROR";
  }
  
  DEBUGF(DEBUG_AUTOMATIONS, "[autos add] wrote automations.json (len=%d) id=%lu", json.length(), id);
  
  gAutosDirty = true;
  DEBUGF(DEBUG_AUTOMATIONS, "[autos add] scheduler refresh queued (type=%s)", typeNorm.c_str());
  
  BROADCAST_PRINTF("%s automation id=%ld name=%s", idOverrideStr.length() > 0 ? "Updated" : "Added", id, name.c_str());
  return "OK";
}

const char* cmd_automation_enable_disable(const String& argsInput, bool enable) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  CommandArgs a(argsInput);
  String idStr = a.value("id");
  if (idStr.length() == 0) {
    BROADCAST_PRINTF("Usage: automation %s id=<id>", enable ? "enable" : "disable");
    return "ERROR";
  }
  
  String json;
  if (!readText(AUTOMATIONS_JSON_FILE, json)) {
    broadcastOutput("Error: failed to read automations.json");
    return "ERROR";
  }
  
  char needleBuf[32];
  snprintf(needleBuf, sizeof(needleBuf), "\"id\": %s", idStr.c_str());
  int idPos = json.indexOf(needleBuf);
  if (idPos < 0) {
    broadcastOutput("Error: automation id not found");
    return "ERROR";
  }
  
  // Authorization check: extract createdBy and verify user can modify this automation
  char createdByBuf[64];
  int objStart = json.lastIndexOf('{', idPos);
  if (objStart < 0) {
    broadcastOutput("Error: malformed JSON");
    return "ERROR";
  }
  int objEnd = findJsonObjectEnd(json, objStart);
  if (objEnd < 0) {
    broadcastOutput("Error: malformed JSON");
    return "ERROR";
  }
  String automationObj = json.substring(objStart, objEnd + 1);
  extractJsonString(automationObj.c_str(), "\"createdBy\"", createdByBuf, sizeof(createdByBuf));
  String createdBy = createdByBuf;
  
  DEBUGF(DEBUG_AUTOMATIONS, "[autos %s] id=%s createdBy='%s' requestedBy='%s' isAdmin=%d",
         enable ? "enable" : "disable", idStr.c_str(), createdBy.c_str(), currentExecUser().c_str(), currentExecIsAdmin());

  if (!currentExecIsAdmin() && createdBy != currentExecUser()) {
    DEBUGF(DEBUG_AUTOMATIONS, "[autos %s] AUTHORIZATION DENIED: non-admin '%s' cannot modify automation created by '%s'",
           enable ? "enable" : "disable", currentExecUser().c_str(), createdBy.c_str());
    broadcastOutput("Error: You do not have permission to modify this automation. Only the creator or an admin can modify it.");
    return "ERROR";
  }
  DEBUGF(DEBUG_AUTOMATIONS, "[autos %s] AUTHORIZATION OK", enable ? "enable" : "disable");
  
  int enabledPos = json.indexOf("\"enabled\":", idPos);
  if (enabledPos < 0) {
    broadcastOutput("Error: malformed automation");
    return "ERROR";
  }
  
  int valueStart = json.indexOf(':', enabledPos) + 1;
  while (valueStart < (int)json.length() && json[valueStart] == ' ') valueStart++;
  int valueEnd = json.indexOf(',', valueStart);
  if (valueEnd < 0) valueEnd = json.indexOf('}', valueStart);
  if (valueEnd < 0) {
    broadcastOutput("Error: malformed JSON");
    return "ERROR";
  }
  
  json = json.substring(0, valueStart) + (enable ? "true" : "false") + json.substring(valueEnd);
  
  if (!writeAutomationsJsonAtomic(json)) {
    broadcastOutput("Error: failed to write automations.json");
    return "ERROR";
  }
  
  gAutosDirty = true;
  
  BROADCAST_PRINTF("%s automation id=%s", enable ? "Enabled" : "Disabled", idStr.c_str());
  return "OK";
}

const char* cmd_automation_delete(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  CommandArgs a(argsInput);
  String idStr = a.value("id");
  if (idStr.length() == 0) {
    broadcastOutput("Usage: automation delete id=<id>");
    return "ERROR";
  }
  
  String json;
  if (!readText(AUTOMATIONS_JSON_FILE, json)) {
    BROADCAST_PRINTF("Error: failed to read automations.json");
    return "ERROR";
  }

  char needleBuf[32];
  snprintf(needleBuf, sizeof(needleBuf), "\"id\": %s", idStr.c_str());
  int idPos = json.indexOf(needleBuf);
  if (idPos < 0) {
    BROADCAST_PRINTF("Error: automation id not found");
    return "ERROR";
  }

  // Authorization check: extract createdBy and verify user can delete this automation
  char createdByBuf[64];
  int objStart = json.lastIndexOf('{', idPos);
  if (objStart < 0) {
    BROADCAST_PRINTF("Error: malformed JSON");
    return "ERROR";
  }
  int objEnd = findJsonObjectEnd(json, objStart);
  if (objEnd < 0) {
    BROADCAST_PRINTF("Error: malformed JSON");
    return "ERROR";
  }
  String automationObj = json.substring(objStart, objEnd + 1);
  extractJsonString(automationObj.c_str(), "\"createdBy\"", createdByBuf, sizeof(createdByBuf));
  String createdBy = createdByBuf;
  
  DEBUGF(DEBUG_AUTOMATIONS, "[autos delete] id=%s createdBy='%s' requestedBy='%s' isAdmin=%d",
         idStr.c_str(), createdBy.c_str(), currentExecUser().c_str(), currentExecIsAdmin());

  if (!currentExecIsAdmin() && createdBy != currentExecUser()) {
    DEBUGF(DEBUG_AUTOMATIONS, "[autos delete] AUTHORIZATION DENIED: non-admin '%s' cannot delete automation created by '%s'",
           currentExecUser().c_str(), createdBy.c_str());
    broadcastOutput("Error: You do not have permission to delete this automation. Only the creator or an admin can delete it.");
    return "ERROR";
  }
  DEBUGF(DEBUG_AUTOMATIONS, "[autos delete] AUTHORIZATION OK");
  
  // Find array bounds
  int arrayStart = json.indexOf('[');
  if (arrayStart < 0) {
    broadcastOutput("Error: malformed JSON - no array");
    return "ERROR";
  }
  int arrayEnd = json.lastIndexOf(']');
  if (arrayEnd < 0) {
    broadcastOutput("Error: malformed JSON - no array end");
    return "ERROR";
  }
  
  // Check if this is the only object in the array
  String arrayContent = json.substring(arrayStart + 1, arrayEnd);
  arrayContent.trim();
  bool isOnlyObject = (arrayContent.indexOf('{') == arrayContent.lastIndexOf('{'));
  
  if (isOnlyObject) {
    // If it's the only object, replace with empty array
    json = json.substring(0, arrayStart + 1) + json.substring(arrayEnd);
  } else {
    // Multiple objects - handle comma removal
    int delStart = objStart, delEnd = objEnd + 1;
    
    // Look for comma after the object, skipping whitespace/newlines
    int commaSearch = delEnd;
    while (commaSearch < (int)json.length() &&
           (json[commaSearch] == ' ' || json[commaSearch] == '\n' ||
            json[commaSearch] == '\r' || json[commaSearch] == '\t')) {
      commaSearch++;
    }
    if (commaSearch < (int)json.length() && json[commaSearch] == ',') {
      delEnd = commaSearch + 1;  // Include trailing comma (and whitespace before it)
    } else {
      // No trailing comma, look for leading comma
      int commaPos = json.lastIndexOf(',', objStart);
      if (commaPos > arrayStart) {
        delStart = commaPos;  // Include leading comma
      }
    }
    
    json = json.substring(0, delStart) + json.substring(delEnd);
  }
  
  if (!writeAutomationsJsonAtomic(json)) {
    broadcastOutput("Error: failed to write automations.json");
    return "ERROR";
  }
  
  gAutosDirty = true;
  
  BROADCAST_PRINTF("Deleted automation id=%s", idStr.c_str());
  return "OK";
}

const char* cmd_automation_run(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  CommandArgs a(argsInput);
  String idStr = a.value("id");
  if (idStr.length() == 0) {
    broadcastOutput("Usage: automation run id=<id>");
    return "ERROR";
  }
  
  String json;
  if (!readText(AUTOMATIONS_JSON_FILE, json)) {
    broadcastOutput("Error: failed to read automations.json");
    return "ERROR";
  }
  
  char needleBuf[32];
  snprintf(needleBuf, sizeof(needleBuf), "\"id\": %s", idStr.c_str());
  int idPos = json.indexOf(needleBuf);
  if (idPos < 0) {
    broadcastOutput("Error: automation id not found");
    return "ERROR";
  }
  
  int objStart = json.lastIndexOf('{', idPos);
  if (objStart < 0) {
    broadcastOutput("Error: malformed automations.json (objStart)");
    return "ERROR";
  }
  
  int depth = 0, objEnd = -1;
  for (int i = objStart; i < (int)json.length(); ++i) {
    char c = json[i];
    if (c == '{') depth++;
    else if (c == '}') {
      depth--;
      if (depth == 0) {
        objEnd = i;
        break;
      }
    }
  }
  
  if (objEnd < 0) {
    broadcastOutput("Error: malformed automations.json (objEnd)");
    return "ERROR";
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
  if (gAutoLogActive) {
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
      broadcastOutput("Error: no command(s) found");
      return "ERROR";
    }
    int ccolon = obj.indexOf(':', cpos);
    int cq1 = obj.indexOf('"', ccolon + 1);
    int cq2 = obj.indexOf('"', cq1 + 1);
    if (cq1 < 0 || cq2 < 0) {
      broadcastOutput("Error: bad command field");
      return "ERROR";
    }
    String cmd = obj.substring(cq1 + 1, cq2);
    cmd.trim();
    if (cmd.length() && cmdsCount < MAX_AUTO_CMDS) { cmdsList[cmdsCount++] = cmd; }
  }
  
  if (cmdsCount == 0) {
    broadcastOutput("Error: no commands to run");
    return "ERROR";
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
    DEBUGF(DEBUG_AUTOMATIONS, "[autos run] id=%s condition='%s' result=%s",
           idStr.c_str(), condition.c_str(), conditionMet ? "TRUE" : "FALSE");
    if (!conditionMet) {
      if (gAutoLogActive) {
        char skipBuf[256];
        snprintf(skipBuf, sizeof(skipBuf), "Automation skipped: ID=%s Name=%s Condition not met: %s", idStr.c_str(), autoName.c_str(), condition.c_str());
        appendAutoLogEntry("AUTO_SKIP", skipBuf);
      }
      {
        char skipBroadcast[160];
        snprintf(skipBroadcast, sizeof(skipBroadcast), "Automation skipped - condition not met: %.120s", condition.c_str());
        broadcastOutput(skipBroadcast);
      }
      return "OK";
    }
  }
  
  char _createdByBuf[64];
  extractJsonString(obj.c_str(), "\"createdBy\"", _createdByBuf, sizeof(_createdByBuf));
  DEBUGF(DEBUG_AUTOMATIONS, "[autos run] Extracted createdBy='%s' for automation id=%s", _createdByBuf, idStr.c_str());

  // Authorization: non-admins may only trigger automations they created themselves
  if (!currentExecIsAdmin() && String(_createdByBuf) != currentExecUser()) {
    DEBUGF(DEBUG_AUTOMATIONS, "[autos run] AUTHORIZATION DENIED: user='%s' isAdmin=%d tried to run automation created by '%s'",
           currentExecUser().c_str(), currentExecIsAdmin(), _createdByBuf);
    {
      char authErr[120];
      snprintf(authErr, sizeof(authErr), "Error: Admin required to trigger automation created by %s", _createdByBuf);
      broadcastOutput(authErr);
    }
    return "Error: Admin required";
  }
  DEBUGF(DEBUG_AUTOMATIONS, "[autos run] AUTHORIZATION OK: user='%s' isAdmin=%d running automation created by '%s'",
         currentExecUser().c_str(), currentExecIsAdmin(), _createdByBuf);

  // Run AS the triggering user, not the creator. createdBy stays the owner
  // (storage + the edit/enable/delete authz above), but a manually-fired
  // automation is just a saved command sequence executed by whoever fired it:
  // permission checks, audit, and notifications must attribute to them. This
  // also closes a privilege-escalation path — executing as the creator would
  // let a triggerer inherit the creator's rights. Captured once; currentExecUser
  // is invariant for this command's lifetime on cmd_exec_task. (Scheduled/boot
  // triggers have no triggering user and keep running as createdBy.)
  String triggerUser = currentExecUser();

  // Execute all commands (with conditional logic support)
  for (int ci = 0; ci < cmdsCount; ++ci) {
    DEBUGF(DEBUG_AUTOMATIONS, "[autos run] id=%s cmd[%d]='%s'", idStr.c_str(), ci, cmdsList[ci].c_str());

    // Protect against malformed commands
    if (cmdsList[ci].length() == 0 || cmdsList[ci] == "\\") {
      DEBUGF(DEBUG_AUTOMATIONS, "[autos run] skipping malformed command: '%s'", cmdsList[ci].c_str());
      continue;
    }

    // Queue command for execution (async, non-blocking) under the triggering user.
    const char* result = executeConditionalCommand(cmdsList[ci].c_str(), triggerUser.c_str(), autoName.c_str());
    
    // Output the result (skip internal status messages - actual output comes from queue)
    if (!isAutoInternalResult(result)) {
      {
        char autoBuf[256];
        snprintf(autoBuf, sizeof(autoBuf), "[Automation %s] %s", idStr.c_str(), result);
        broadcastOutput(autoBuf);
      }
    }
  }
  
  // Advance nextAt after manual execution via the unified post-fire helper.
  time_t now = time(nullptr);
  if (now > 0) {
    rescheduleAfterFire(idStr.toInt(), obj.c_str(), now);
  }
  
  // Log automation end if logging is active
  if (gAutoLogActive) {
    char endBuf[256];
    snprintf(endBuf, sizeof(endBuf), "Automation completed: ID=%s Name=%s Commands=%d", idStr.c_str(), autoName.c_str(), cmdsCount);
    appendAutoLogEntry("AUTO_END", endBuf);
  }
  
  char resultBuf[128];
  snprintf(resultBuf, sizeof(resultBuf), "Ran automation id=%s (%d command%s)", idStr.c_str(), cmdsCount, cmdsCount == 1 ? "" : "s");
  broadcastOutput(resultBuf);
  return "OK";
}

// Arm an afterDelay automation's timer: nextAt = now + delayMs/1000.
// No commands run here; the scheduler fires them when the delay elapses.
const char* cmd_automation_trigger(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  CommandArgs a(argsInput);
  String idStr = a.value("id");
  if (idStr.length() == 0) {
    broadcastOutput("Usage: automation trigger id=<id>");
    return "ERROR";
  }

  String json;
  if (!readText(AUTOMATIONS_JSON_FILE, json)) {
    broadcastOutput("Error: failed to read automations.json");
    return "ERROR";
  }

  PSRAM_JSON_DOC(doc);
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    broadcastOutput("Error: automations.json parse error");
    return "ERROR";
  }

  JsonArray automations = doc["automations"].as<JsonArray>();
  if (automations.isNull()) {
    broadcastOutput("Error: automations.json missing automations array");
    return "ERROR";
  }

  long id = idStr.toInt();
  JsonObject target;
  for (JsonObject obj : automations) {
    if (obj["id"].as<long>() == id) { target = obj; break; }
  }
  if (target.isNull()) {
    broadcastOutput("Error: automation id not found");
    return "ERROR";
  }

  // Find the first manual trigger in the automation's triggers array.
  JsonArrayConst triggers = target["triggers"].as<JsonArrayConst>();
  if (triggers.isNull()) {
    broadcastOutput("Error: automation has no triggers array");
    return "ERROR";
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
    broadcastOutput("Error: automation has no manual trigger to arm");
    return "ERROR";
  }
  if (manualDelayMs <= 0) {
    broadcastOutput("Error: manual trigger has no valid delayMs");
    return "ERROR";
  }

  time_t now = time(nullptr);
  if (now <= 0) {
    broadcastOutput("Error: system time not set");
    return "ERROR";
  }
  time_t nextAt = now + (manualDelayMs / 1000);

  int delayMs = manualDelayMs;  // alias for the log message below

  if (!updateAutomationTriggerNextAt(id, manualIdx, nextAt)) {
    broadcastOutput("Error: failed to update nextAt");
    return "ERROR";
  }

  // Wake the scheduler so it re-reads the file and picks up the armed nextAt.
  // Without this, the armed automation waits up to one tick interval before
  // the scheduler notices.
  notifyAutomationScheduler();

  char msg[160];
  snprintf(msg, sizeof(msg), "Armed automation id=%ld (fires in %d ms)", id, delayMs);
  broadcastOutput(msg);
  return "OK";
}

// Main automation command dispatcher
const char* cmd_automation(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  CommandArgs a(argsInput);
  String subCmd = a.arg(0);
  subCmd.toLowerCase();
  String subArgs = a.remaining(0);

  // Handle "system" subcommand
  if (subCmd == "system") {
    if (subArgs.equalsIgnoreCase("enable")) {
      setSetting(gSettings.automationsEnabled, true);
      return "Automation system: enabled";
    } else if (subArgs.equalsIgnoreCase("disable")) {
      setSetting(gSettings.automationsEnabled, false);
      return "Automation system: disabled";
    } else if (subArgs.equalsIgnoreCase("status")) {
      if (gSettings.automationsEnabled) {
        return "Automation system: enabled";
      } else {
        return "Automation system: disabled";
      }
    }
    return "Usage: automation system <enable|disable|status>";
  }

  // Handle regular automation commands
  if (subCmd == "list") {
    return cmd_automation_list(subArgs);
  } else if (subCmd == "add") {
    return cmd_automation_add(subArgs);
  } else if (subCmd == "enable") {
    return cmd_automation_enable_disable(subArgs, true);
  } else if (subCmd == "disable") {
    return cmd_automation_enable_disable(subArgs, false);
  } else if (subCmd == "delete") {
    return cmd_automation_delete(subArgs);
  } else if (subCmd == "sanitize") {
    String json;
    if (!readText(AUTOMATIONS_JSON_FILE, json)) return "Error: failed to read automations.json";
    if (sanitizeAutomationsJson(json)) {
      if (!writeAutomationsJsonAtomic(json)) return "Error: failed to write automations.json";
      gAutosDirty = true;
      DEBUGF(DEBUG_AUTOMATIONS, "[autos] CLI sanitize: fixed duplicate IDs; scheduler refresh queued");
      return "Sanitized automations.json: fixed duplicate IDs";
    } else {
      DEBUGF(DEBUG_AUTOMATIONS, "[autos] CLI sanitize: no duplicate IDs found");
      return "Sanitize: no changes needed";
    }
  } else if (subCmd == "recompute") {
    String json;
    if (!readText(AUTOMATIONS_JSON_FILE, json)) return "Error: failed to read automations.json";
    
    time_t now = time(nullptr);
    if (now <= 0) return "Error: no valid system time for recompute";
    
    int recomputed = 0, failed = 0;
    bool modified = false;
    
    // Parse through all automations and recompute nextAt
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

      int comma = json.indexOf(',', colon + 1);
      int idValEnd = (comma > 0 && comma < objEnd) ? comma : objEnd;
      String idStr = json.substring(colon + 1, idValEnd);
      idStr.trim();
      long id = idStr.toInt();

      String obj = json.substring(objStart, objEnd + 1);
      
      // Check if enabled
      bool enabled = (obj.indexOf("\"enabled\": true") >= 0) || (obj.indexOf("\"enabled\":true") >= 0);
      if (!enabled) {
        DEBUGF(DEBUG_AUTOMATIONS, "[autos recompute] id=%ld skip: disabled", id);
        pos = objEnd + 1;
        continue;
      }
      
      // Compute nextAt
      time_t nextAt = computeNextRunTime(obj.c_str(), now);
      if (nextAt > 0) {
        if (updateAutomationNextAt(id, nextAt)) {
          recomputed++;
          modified = true;
          DEBUGF(DEBUG_AUTOMATIONS, "[autos recompute] id=%ld nextAt=%lu", id, (unsigned long)nextAt);
        } else {
          failed++;
          DEBUGF(DEBUG_AUTOMATIONS, "[autos recompute] id=%ld failed to update", id);
        }
      } else {
        failed++;
        DEBUGF(DEBUG_AUTOMATIONS, "[autos recompute] id=%ld could not compute nextAt", id);
      }
      
      pos = objEnd + 1;
    }
    
    if (modified) {
      gAutosDirty = true;
      DEBUGF(DEBUG_AUTOMATIONS, "[autos recompute] scheduler refresh queued");
    }
    
    BROADCAST_PRINTF("Recomputed nextAt: %d succeeded, %d failed", recomputed, failed);
    return "OK";
  } else if (subCmd == "run") {
    return cmd_automation_run(subArgs);
  } else if (subCmd == "trigger") {
    return cmd_automation_trigger(subArgs);
  }

  broadcastOutput("Unknown automation command. Use: list, add, enable, disable, delete, run, trigger, sanitize, recompute");
  return "ERROR";
}

// Callback function for streaming automation parser
bool processAutomationCallback(const char* autoJson, size_t jsonLen, void* userData) {
  SchedulerContext* ctx = (SchedulerContext*)userData;

  // Extract ID
  long id = extractJsonLong(autoJson, "\"id\"");
  if (id == 0) return true;  // Skip invalid

  // Duplicate-id guard
  bool dupSeen = false;
  for (int i = 0; i < ctx->seenCount; ++i) {
    if (ctx->seenIds[i] == id) {
      dupSeen = true;
      break;
    }
  }
  if (dupSeen) {
    DEBUGF(DEBUG_AUTO_SCHEDULER, "[autos] duplicate id detected at runtime id=%ld; skipping and queuing sanitize", id);
    ctx->queueSanitize = true;
    return true;  // Continue processing
  }
  if (ctx->seenCount < 128) { ctx->seenIds[ctx->seenCount++] = id; }

  ctx->evaluated++;

  // Check if enabled
  bool enabled = extractJsonBool(autoJson, "\"enabled\"");
  if (!enabled) {
    DEBUGF(DEBUG_AUTO_SCHEDULER, "[autos] id=%ld skip: disabled", id);
    return true;  // Continue processing
  }

  // Parse nextAt field
  time_t nextAt = (time_t)extractJsonLong(autoJson, "\"nextAt\"");

  // If nextAt is missing or invalid, compute it now
  if (nextAt <= 0) {
    nextAt = computeNextRunTime(autoJson, ctx->now);
    if (nextAt > 0) {
      updateAutomationNextAt(id, nextAt);
      DEBUGF(DEBUG_AUTO_TIMING, "[autos] id=%ld computed missing nextAt=%lu", id, (unsigned long)nextAt);
    } else {
      DEBUGF(DEBUG_AUTO_TIMING, "[autos] id=%ld skip: could not compute nextAt", id);
      return true;  // Continue processing
    }
  }

  // Check if it's time to run
  if (ctx->now >= nextAt) {
    // For command execution, convert to String (existing functions expect String)
    String obj(autoJson);

    // Extract commands (reuse existing logic)
    String cmdsList[64];
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
      while (i < (int)body.length() && cmdsCount < 64) {
        while (i < (int)body.length() && (body[i] == ' ' || body[i] == ',' || body[i] == '\n' || body[i] == '\r' || body[i] == '\t')) i++;
        if (i >= (int)body.length()) break;
        if (body[i] == '"') {
          int q1 = i;
          int q2 = body.indexOf('"', q1 + 1);
          if (q2 < 0) break;
          String one = body.substring(q1 + 1, q2);
          one.trim();
          if (one.length() && cmdsCount < 64) { cmdsList[cmdsCount++] = one; }
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
          if (cmd.length() && cmdsCount < 64) { cmdsList[cmdsCount++] = cmd; }
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
        DEBUGF(DEBUG_AUTO_CONDITION, "[autos] id=%ld condition='%s' result=%s",
               id, condition.c_str(), conditionMet ? "TRUE" : "FALSE");
        if (!conditionMet) {
          if (gAutoLogActive) {
            char skipBuf[256];
            snprintf(skipBuf, sizeof(skipBuf), "Scheduled automation skipped: ID=%ld Name=%s Condition not met: %s", id, autoName.c_str(), condition.c_str());
            appendAutoLogEntry("AUTO_SKIP", skipBuf);
          }
          DEBUGF(DEBUG_AUTO_CONDITION, "[autos] id=%ld skipped - condition not met: %s", id, condition.c_str());
          return true;
        }
      }

      char _createdByBuf[64];
      extractJsonString(obj.c_str(), "\"createdBy\"", _createdByBuf, sizeof(_createdByBuf));

      // Log scheduled automation start if logging is active
      if (gAutoLogActive) {
        if (ensureDebugBuffer()) {
          snprintf(getDebugBuffer(), 1024, "Scheduled automation started: ID=%lu Name=%s User=%s", id, autoName.c_str(), _createdByBuf);
          appendAutoLogEntry("AUTO_START", String(getDebugBuffer()));
        }
      }

      // Execute commands (with conditional logic support)
      for (int ci = 0; ci < cmdsCount; ++ci) {
        DEBUGF(DEBUG_AUTO_EXEC, "[autos] id=%ld run cmd[%d]='%s'", id, ci, cmdsList[ci].c_str());

        // Queue command for execution (async, non-blocking)
        const char* result = executeConditionalCommand(cmdsList[ci].c_str(), _createdByBuf, autoName.c_str());

        // Output the result (skip internal status messages - actual output comes from queue)
        if (!isAutoInternalResult(result)) {
          BROADCAST_PRINTF("[Scheduled Automation %lu] %s", id, result);
        }
      }
      ctx->executed++;

      // Log scheduled automation end if logging is active
      if (gAutoLogActive) {
        if (ensureDebugBuffer()) {
          snprintf(getDebugBuffer(), 1024, "Scheduled automation completed: ID=%lu Name=%s Commands=%d", id, autoName.c_str(), cmdsCount);
          appendAutoLogEntry("AUTO_END", String(getDebugBuffer()));
        }
      }

      // Update next run time via the unified post-fire helper.
      rescheduleAfterFire(id, obj.c_str(), ctx->now);
    } else {
      DEBUGF(DEBUG_AUTO_SCHEDULER, "[autos] id=%ld skip: no commands found", id);
    }
  } else {
    DEBUGF(DEBUG_AUTO_TIMING, "[autos] id=%ld wait: nextAt=%lu now=%lu", id, (unsigned long)nextAt, (unsigned long)ctx->now);
  }

  return true;  // Continue processing next automation
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
  enum Type { NONE, TIME, MONTHLY, YEARLY, INTERVAL, MANUAL, BOOT };
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

  // Scheduling state (persisted per-trigger inside the JSON)
  time_t nextAt = 0;
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
    out.intervalMs = trig["intervalMs"] | 0;
    return true;
  }
  if (strcmp(type, "boot") == 0) {
    out.type = Trigger::BOOT;
    out.bootDelayMs = trig["bootDelayMs"] | 0;
    return true;
  }
  return false;
}

// Parse an automation object's `triggers` array. Fills `out[]` up to
// `maxCount` entries. Returns the number of valid triggers (0 if none).
static int triggersFromJson(const char* automationJson, Trigger* out, int maxCount) {
  if (maxCount <= 0) return 0;
  PSRAM_JSON_DOC(doc);
  if (deserializeJson(doc, automationJson)) return 0;
  JsonArrayConst arr = doc["triggers"].as<JsonArrayConst>();
  if (arr.isNull()) return 0;
  int n = 0;
  for (JsonVariantConst t : arr) {
    if (n >= maxCount) break;
    Trigger tmp;
    if (parseOneTrigger(t, tmp)) {
      out[n++] = tmp;
    }
  }
  return n;
}

// Compute the next firing time at or after `from`, or 0 if there is none.
// afterDelay always returns 0 (manually armed); atTime returns the next
// matching candidate respecting day-of-week and weekInterval filters;
// interval returns `from + intervalMs/1000`.
static time_t nextFire(const Trigger& s, time_t from) {
  switch (s.type) {
    case Trigger::MANUAL:
    case Trigger::BOOT:
      return 0;  // manually armed / dispatched by runAtBoot, not by scheduler
    case Trigger::INTERVAL:
      return (s.intervalMs > 0) ? (from + (time_t)(s.intervalMs / 1000)) : 0;
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

// Returns the minimum nextAt across all triggers (0 if none are scheduled).
// Used by the RAM cache to answer "is anything due?" with a single comparison.
static time_t minNextAtAcrossTriggers(const Trigger* arr, int count) {
  time_t minAt = 0;
  for (int i = 0; i < count; i++) {
    if (arr[i].nextAt > 0 && (minAt == 0 || arr[i].nextAt < minAt)) {
      minAt = arr[i].nextAt;
    }
  }
  return minAt;
}

// Unified post-fire helper. For each trigger whose nextAt was at or before
// `firedAt`, compute a new nextAt (and maybe update anchor) and persist the
// whole triggers array back to the JSON file.
static void rescheduleAfterFire(long id, const char* automationJson, time_t firedAt) {
  Trigger triggers[MAX_TRIGGERS];
  int n = triggersFromJson(automationJson, triggers, MAX_TRIGGERS);
  if (n == 0) return;  // Malformed — scheduler will skip on next rebuild.

  // For each trigger that was due, advance it. Leave others untouched.
  for (int i = 0; i < n; i++) {
    if (triggers[i].nextAt > 0 && triggers[i].nextAt <= firedAt) {
      onFired(triggers[i], firedAt);  // mutates anchor if needed
      triggers[i].nextAt = nextFire(triggers[i], firedAt);  // 0 = disarm
    }
  }

  // Rewrite the triggers array in the file.
  String json;
  if (!readText(AUTOMATIONS_JSON_FILE, json)) return;
  PSRAM_JSON_DOC(doc);
  if (deserializeJson(doc, json)) return;
  JsonArray autos = doc["automations"].as<JsonArray>();
  if (autos.isNull()) return;
  for (JsonObject automation : autos) {
    if (automation["id"].as<long>() != id) continue;
    JsonArray arr = automation["triggers"].as<JsonArray>();
    if (arr.isNull()) break;
    int idx = 0;
    for (JsonVariant t : arr) {
      if (idx >= n) break;
      JsonObject tobj = t.as<JsonObject>();
      tobj["nextAt"] = (unsigned long)triggers[idx].nextAt;
      if (triggers[idx].type == Trigger::TIME && triggers[idx].weekInterval > 1 && triggers[idx].anchor > 0) {
        tobj["anchor"] = (unsigned long)triggers[idx].anchor;
      }
      idx++;
    }
    break;
  }
  json = "";
  serializeJsonPretty(doc, json);
  writeAutomationsJsonAtomic(json);
}

// Thin wrapper preserving the existing public API. Returns the min nextAt
// across all triggers of the automation, or 0 if none are scheduled.
time_t computeNextRunTime(const char* automationJson, time_t fromTime) {
  Trigger triggers[MAX_TRIGGERS];
  int n = triggersFromJson(automationJson, triggers, MAX_TRIGGERS);
  if (n == 0) return 0;
  // For triggers without persisted nextAt, compute fresh ones.
  time_t minAt = 0;
  for (int i = 0; i < n; i++) {
    time_t t = (triggers[i].nextAt > 0) ? triggers[i].nextAt : nextFire(triggers[i], fromTime);
    if (t > 0 && (minAt == 0 || t < minAt)) minAt = t;
  }
  return minAt;
}

// Validate condition syntax (const char* input)
const char* validateConditionSyntax(const char* condition) {
  const char* cond = condition;
  size_t len = strlen(condition);
  
  DEBUGF(DEBUG_AUTOMATIONS, "[validate] Input condition: '%s'", condition);
  
  // Skip leading whitespace
  while (*cond == ' ' || *cond == '\t') { cond++; len--; }
  
  // Must start with IF (case-insensitive)
  if (len < 3 || 
      (cond[0] != 'I' && cond[0] != 'i') ||
      (cond[1] != 'F' && cond[1] != 'f') ||
      cond[2] != ' ') {
    DEBUGF(DEBUG_AUTOMATIONS, "[validate] FAIL: Condition must start with 'IF'");
    return "Condition must start with 'IF'";
  }

  // Find THEN (case-insensitive)
  const char* thenPos = nullptr;
  for (size_t i = 3; i < len - 5; i++) {
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
    DEBUGF(DEBUG_AUTOMATIONS, "[validate] FAIL: Condition must contain 'THEN'");
    return "Condition must contain 'THEN'";
  }

  // Check condition part has content
  size_t condLen = thenPos - (cond + 3);
  if (condLen == 0) {
    DEBUGF(DEBUG_AUTOMATIONS, "[validate] FAIL: Missing condition after 'IF'");
    return "Missing condition after 'IF'";
  }

  // Check command part has content
  const char* cmdStart = thenPos + 5;
  while (*cmdStart == ' ' || *cmdStart == '\t') cmdStart++;
  if (*cmdStart == '\0') {
    DEBUGF(DEBUG_AUTOMATIONS, "[validate] FAIL: Missing command after 'THEN'");
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
    DEBUGF(DEBUG_AUTOMATIONS, "[validate] FAIL: No operator found in condition");
    return "Condition must contain an operator (>, <, =, >=, <=, !=, CONTAINS)";
  }

  DEBUGF(DEBUG_AUTOMATIONS, "[validate] PASS: Found operator '%s'", foundOp);
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
  
  DEBUGF(DEBUG_AUTOMATIONS, "[eval] Parsing condition: '%s'", condStart);
  
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
      strncpy(op, operators[i], sizeof(op) - 1);
      op[sizeof(op) - 1] = '\0';
      
      // Extract value
      const char* valStart = pos + strlen(operators[i]);
      while (*valStart == ' ' || *valStart == '\t') valStart++;
      strncpy(value, valStart, sizeof(value) - 1);
      value[sizeof(value) - 1] = '\0';
      // Trim value
      char* vEnd = value + strlen(value) - 1;
      while (vEnd > value && (*vEnd == ' ' || *vEnd == '\t')) *vEnd-- = '\0';
      
      opFound = operators[i];
      break;
    }
  }
  
  if (!opFound) {
    DEBUGF(DEBUG_AUTOMATIONS, "[eval] FAIL: No operator found in parsed condition");
    return false;
  }
  
  DEBUGF(DEBUG_AUTOMATIONS, "[eval] Parsed: sensor='%s' op='%s' value='%s'", sensor, op, value);

  // Get current sensor value
  float currentValue = 0;
  bool isNumeric = true;
  char currentStringValue[32] = "";

  if (strcmp(sensor, "TEMP") == 0) {
 #if ENABLE_THERMAL_SENSOR
    float v = 0.0f;
    bool ok = false;
    {
      SensorCacheGuard g(gThermalCache.mutex, pdMS_TO_TICKS(50), "automation.thermalAvgRead");
      if (g.held) {
        ok = gThermalCache.thermalDataValid;
        v = gThermalCache.thermalAvgTemp;
      }
    }
    if (!ok) return false;
    currentValue = v;
 #else
    return false;
 #endif
  } else if (strcmp(sensor, "HUMIDITY") == 0) {
    DEBUGF(DEBUG_AUTOMATIONS, "[condition] Humidity sensor not available");
    return false;
  } else if (strcmp(sensor, "DISTANCE") == 0) {
    // Special handling for distance - check if ANY valid object meets the condition
    float targetValue = atof(value);
#if ENABLE_TOF_SENSOR
    bool anyObjectMeetsCondition = false;
    int tofTotal = 0;
    TofCache::TofObject objs[4];
    for (int i = 0; i < 4; i++) objs[i] = gTofCache.tofObjects[i];
    {
      SensorCacheGuard g(gTofCache.mutex, pdMS_TO_TICKS(50), "automation.tofObjectsRead");
      if (g.held) {
        tofTotal = gTofCache.tofTotalObjects;
        for (int i = 0; i < 4; i++) objs[i] = gTofCache.tofObjects[i];
      }
    }

    DEBUGF(DEBUG_AUTOMATIONS, "[condition] distance: checking %d objects against %s%.1f",
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

        DEBUGF(DEBUG_AUTOMATIONS, "[condition] obj[%d]: %.1fcm %s %.1f = %s",
               j, objDistance, op, targetValue, objMeetsCondition ? "TRUE" : "FALSE");

        if (objMeetsCondition) {
          anyObjectMeetsCondition = true;
        }
      }
    }

    DEBUGF(DEBUG_AUTOMATIONS, "[condition] distance result: %s",
           anyObjectMeetsCondition ? "TRUE" : "FALSE");
    return anyObjectMeetsCondition;
 #else
    (void)targetValue;
    return false;
 #endif
  } else if (strcmp(sensor, "LIGHT") == 0) {
 #if ENABLE_APDS_SENSOR
    uint16_t clear = 0;
    bool ok = false;
    {
      SensorCacheGuard g(gApdsCache.mutex, pdMS_TO_TICKS(50), "automation.apdsLightRead");
      if (g.held) {
        ok = gApdsCache.apdsDataValid;
        clear = gApdsCache.apdsClear;
      }
    }
    if (!ok) return false;
    currentValue = (float)clear;
 #else
    return false;
 #endif
  } else if (strcmp(sensor, "MOTION") == 0) {
    isNumeric = false;
 #if ENABLE_APDS_SENSOR
    uint8_t prox = 0;
    bool ok = false;
    {
      SensorCacheGuard g(gApdsCache.mutex, pdMS_TO_TICKS(50), "automation.apdsMotionRead");
      if (g.held) {
        ok = gApdsCache.apdsDataValid;
        prox = gApdsCache.apdsProximity;
      }
    }
    if (!ok) return false;
    strncpy(currentStringValue, (prox > 50) ? "DETECTED" : "NONE", sizeof(currentStringValue) - 1);
 #else
    return false;
 #endif
  } else if (strcmp(sensor, "TIME") == 0) {
    isNumeric = false;
    time_t now = time(nullptr);
    struct tm* timeinfo = localtime(&now);
    int hour = timeinfo->tm_hour;
    if (hour >= 6 && hour < 12) strncpy(currentStringValue, "MORNING", sizeof(currentStringValue) - 1);
    else if (hour >= 12 && hour < 18) strncpy(currentStringValue, "AFTERNOON", sizeof(currentStringValue) - 1);
    else if (hour >= 18 && hour < 24) strncpy(currentStringValue, "EVENING", sizeof(currentStringValue) - 1);
    else strncpy(currentStringValue, "NIGHT", sizeof(currentStringValue) - 1);
  } else if (strcmp(sensor, "ROOM") == 0) {
    // ESP-NOW metadata: room assignment
    isNumeric = false;
    if (gSettings.espnowRoom.length() > 0) {
      strncpy(currentStringValue, gSettings.espnowRoom.c_str(), sizeof(currentStringValue) - 1);
      currentStringValue[sizeof(currentStringValue) - 1] = '\0';
      // Uppercase for comparison
      for (char* p = currentStringValue; *p; p++) {
        if (*p >= 'a' && *p <= 'z') *p -= 32;
      }
    } else {
      strncpy(currentStringValue, "NONE", sizeof(currentStringValue) - 1);
    }
    DEBUGF(DEBUG_AUTOMATIONS, "[eval] ROOM: current='%s' (from setting='%s')", 
           currentStringValue, gSettings.espnowRoom.c_str());
  } else if (strcmp(sensor, "ZONE") == 0) {
    // ESP-NOW metadata: zone assignment
    isNumeric = false;
    if (gSettings.espnowZone.length() > 0) {
      strncpy(currentStringValue, gSettings.espnowZone.c_str(), sizeof(currentStringValue) - 1);
      currentStringValue[sizeof(currentStringValue) - 1] = '\0';
      // Uppercase for comparison
      for (char* p = currentStringValue; *p; p++) {
        if (*p >= 'a' && *p <= 'z') *p -= 32;
      }
    } else {
      strncpy(currentStringValue, "NONE", sizeof(currentStringValue) - 1);
    }
    DEBUGF(DEBUG_AUTOMATIONS, "[eval] ZONE: current='%s' (from setting='%s')", 
           currentStringValue, gSettings.espnowZone.c_str());
  } else if (strcmp(sensor, "TAGS") == 0) {
    // ESP-NOW metadata: tags (supports CONTAINS operator)
    isNumeric = false;
    if (gSettings.espnowTags.length() > 0) {
      strncpy(currentStringValue, gSettings.espnowTags.c_str(), sizeof(currentStringValue) - 1);
      currentStringValue[sizeof(currentStringValue) - 1] = '\0';
      // Uppercase for comparison
      for (char* p = currentStringValue; *p; p++) {
        if (*p >= 'a' && *p <= 'z') *p -= 32;
      }
    } else {
      strncpy(currentStringValue, "NONE", sizeof(currentStringValue) - 1);
    }
    DEBUGF(DEBUG_AUTOMATIONS, "[eval] TAGS: current='%s' (from setting='%s')", 
           currentStringValue, gSettings.espnowTags.c_str());
  } else {
    DEBUGF(DEBUG_AUTOMATIONS, "[condition] Unknown sensor: %s", sensor);
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
    DEBUGF(DEBUG_AUTOMATIONS, "[eval] Numeric: %.2f %s %.2f = %s", 
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
      DEBUGF(DEBUG_AUTOMATIONS, "[eval] String: '%s' = '%s' = %s", 
             currentStringValue, value, result ? "TRUE" : "FALSE");
    } else if (strcmp(op, "!=") == 0) {
      result = strcmp(currentStringValue, value) != 0;
      DEBUGF(DEBUG_AUTOMATIONS, "[eval] String: '%s' != '%s' = %s", 
             currentStringValue, value, result ? "TRUE" : "FALSE");
    } else if (strcmp(op, "CONTAINS") == 0) {
      // Check if currentStringValue contains value (case-insensitive substring match)
      result = strstr(currentStringValue, value) != nullptr;
      DEBUGF(DEBUG_AUTOMATIONS, "[eval] String: '%s' CONTAINS '%s' = %s", 
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
  bool sawIF = false;
  bool sawELSE = false;
  size_t position = 0;

  while (position < len) {
    // Skip whitespace
    while (position < len && input[position] == ' ') position++;
    if (position >= len) break;

    // Check for keywords
    bool isIF = (strncmp(input + position, "IF ", 3) == 0);
    bool isELSEIF = (position + 8 <= len && strncmp(input + position, "ELSE IF ", 8) == 0);
    bool isELSE = (strncmp(input + position, "ELSE ", 5) == 0);

    if (isIF) {
      if (position > 0) {
        return "Error: 'IF' can only appear at the beginning of a conditional chain";
      }
      sawIF = true;

      // Check for THEN
      const char* thenPos = strstr(input + position + 3, " THEN ");
      if (!thenPos) {
        return "Error: 'IF' statement missing 'THEN' keyword";
      }

      position = (thenPos - input) + 6;

    } else if (isELSEIF) {
      if (!sawIF) {
        return "Error: 'ELSE IF' must follow 'IF' statement";
      }
      if (sawELSE) {
        return "Error: Cannot use 'ELSE IF' after 'ELSE' (ELSE must be terminal)";
      }

      // Check for THEN
      const char* thenPos = strstr(input + position + 8, " THEN ");
      if (!thenPos) {
        return "Error: 'ELSE IF' statement missing 'THEN' keyword";
      }

      position = (thenPos - input) + 6;

    } else if (isELSE) {
      if (!sawIF) {
        return "Error: 'ELSE' must follow 'IF' statement";
      }
      if (sawELSE) {
        return "Error: Multiple 'ELSE' clauses not allowed";
      }
      sawELSE = true;

      position += 5;
      position = len;  // ELSE is terminal

    } else {
      position++;
    }
  }

  if (!sawIF) {
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
    bool isIF = (strncmp(input + position, "IF ", 3) == 0);
    bool isELSEIF = (position + 8 <= len && strncmp(input + position, "ELSE IF ", 8) == 0);
    bool isELSE = (strncmp(input + position, "ELSE ", 5) == 0);

    if (isIF || isELSEIF) {
      // Extract condition and action
      size_t condStart = position + (isELSEIF ? 8 : 3);
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
      for (size_t i = actionStart; i < len - 7; i++) {
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
    } else if (isELSE) {
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
  static char errorBuf[128];  // Static buffer for error messages
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
        strncpy(errorBuf, "Error: PRINT requires a message", sizeof(errorBuf) - 1);
        errorBuf[sizeof(errorBuf) - 1] = '\0';
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
      strncpy(errorBuf, "Error: 'ELSE IF' cannot be used as a standalone command", sizeof(errorBuf) - 1);
      errorBuf[sizeof(errorBuf) - 1] = '\0';
      return errorBuf;
    }
    if (cmdLen >= 5 && strncmp(prefix, "ELSE ", 5) == 0) {
      strncpy(errorBuf, "Error: 'ELSE' cannot be used as a standalone command", sizeof(errorBuf) - 1);
      errorBuf[sizeof(errorBuf) - 1] = '\0';
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
      // Find THEN position (case-insensitive)
      int thenPos = -1;
      for (size_t i = 3; i < cmdLen - 5; i++) {
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
        strncpy(errorBuf, "Error: Conditional command missing THEN", sizeof(errorBuf) - 1);
        errorBuf[sizeof(errorBuf) - 1] = '\0';
        return errorBuf;
      }

      // Find ELSE position (case-insensitive)
      int elsePos = -1;
      for (size_t i = thenPos + 5; i < cmdLen - 5; i++) {
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
      if (condLen >= sizeof(conditionBuf)) condLen = sizeof(conditionBuf) - 1;
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
      if (thenLen >= sizeof(thenBuf)) thenLen = sizeof(thenBuf) - 1;
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
        if (elseLen >= sizeof(elseBuf)) elseLen = sizeof(elseBuf) - 1;
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

      DEBUGF(DEBUG_AUTOMATIONS, "[conditional] condition='%s' result=%s",
             condStart, conditionMet ? "TRUE" : "FALSE");

      // Execute appropriate command via async queue (avoids deadlock)
      if (conditionMet) {
        if (thenCmdStart[0] != '\0') {
          DEBUGF(DEBUG_AUTOMATIONS, "[conditional] queuing THEN: %s", thenCmdStart);
          queueAutomationSubCommand(thenCmdStart, owner, autoName);
          return "Conditional THEN queued";
        }
      } else {
        if (elseBuf[0] != '\0') {
          DEBUGF(DEBUG_AUTOMATIONS, "[conditional] queuing ELSE: %s", elseBuf);
          queueAutomationSubCommand(elseBuf, owner, autoName);
          return "Conditional ELSE queued";
        }
      }

      return "Conditional command completed";
    }
  }
  
  // Regular command - queue through FreeRTOS command queue (async, non-blocking)
  queueAutomationSubCommand(command, owner, autoName);
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

    bool foundIF = (strncmp(input + position, "IF ", 3) == 0);
    bool foundELSEIF = (position + 8 <= len && strncmp(input + position, "ELSE IF ", 8) == 0);
    bool foundELSE = (strncmp(input + position, "ELSE ", 5) == 0);

    switch (state) {
      case EXPECTING_IF:
        if (!foundIF) {
          return "Error: Expected IF statement at beginning";
        }
        state = EXPECTING_ELSE_OR_END;
        position += 3;
        break;

      case EXPECTING_ELSE_OR_END:
        if (foundELSEIF) {
          state = EXPECTING_ELSE_OR_END;
          position += 8;
        } else if (foundELSE) {
          state = EXPECTING_END;
          position += 5;
        } else {
          const char* thenPos = strstr(input + position, "THEN");
          if (!thenPos) {
            return "Error: Missing THEN keyword";
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
        if (foundIF || foundELSEIF || foundELSE) {
          return "Error: No additional conditions allowed after ELSE";
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

// NOTE: appendAutoLogEntry is implemented in system_utils.cpp
// It returns bool to indicate success/failure of log write
extern bool appendAutoLogEntry(const char* type, const String& message);

// ============================================================================
// Automation Logging Command
// ============================================================================

const char* cmd_autolog(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  CommandArgs a(argsInput);
  String subcmd = a.arg(0);
  subcmd.toLowerCase();

  if (subcmd == "start") {
    String filename;
    const char* qerr = requireQuotedToken(a, 1, filename);
    if (qerr) return qerr;

    // Capture the starter's identity BEFORE flipping gAutoLogActive — every
    // subsequent appendAutoLogEntry write is gated on this ctx via
    // VFS::openGuarded. If the caller can't write the path, the LOG_START
    // line below fails and we roll back cleanly.
    gAutoLogOwnerCtx = currentAuthContext();

    gAutoLogActive = true;
    gAutoLogFile = filename;

    if (!appendAutoLogEntry("LOG_START", "Automation logging started")) {
      gAutoLogActive = false;
      gAutoLogFile = "";
      gAutoLogOwnerCtx = AuthContext{};  // clear captured identity on failure
      snprintf(getDebugBuffer(), 1024, "Error: Failed to create log file: %s", filename.c_str());
      return getDebugBuffer();
    }

    snprintf(getDebugBuffer(), 1024, "Automation logging started: %s", filename.c_str());
    return getDebugBuffer();

  } else if (subcmd == "stop") {
    if (!gAutoLogActive) return "Automation logging is not active";

    appendAutoLogEntry("LOG_STOP", "Automation logging stopped");

    snprintf(getDebugBuffer(), 1024, "Automation logging stopped: %s", gAutoLogFile.c_str());
    gAutoLogActive = false;
    gAutoLogFile = "";
    gAutoLogOwnerCtx = AuthContext{};  // clear captured identity

    return getDebugBuffer();

  } else if (subcmd == "status") {
    if (gAutoLogActive) {
      snprintf(getDebugBuffer(), 1024, "Automation logging ACTIVE: %s", gAutoLogFile.c_str());
      return getDebugBuffer();
    } else {
      return "Automation logging INACTIVE";
    }

  } else {
    return "Usage: autolog start \"<filename>\" | autolog stop | autolog status";
  }
}

// Validate conditions command
const char* cmd_validate_conditions(const String& argsInput) {
  String conditions = argsInput;
  conditions.trim();
  const char* validationResult = validateConditionalHierarchy(conditions.c_str());
  // If we're in validation mode and validation passes, return "VALID"
  // Otherwise return the actual validation result (which could be an error)
  if (gCLIValidateOnly && validationResult && strcmp(validationResult, "VALID") == 0) {
    return "VALID";
  }
  broadcastOutput(validationResult);
  return "OK";
}

// Automation scheduler (runs from main loop)
// ============================================================================

// NOTE: cmd_downloadautomation, cmd_autolog, and cmd_conditional are implemented
// in the main .ino file to avoid duplication and linker conflicts.

// Notify the automation scheduler to run on next main loop iteration and
// invalidate the in-RAM cache so it gets rebuilt from the file.
void notifyAutomationScheduler() {
  gAutosDirty = true;
  gAutoCacheValid = false;
}

// triggerMode="once" edge state (RAM-only). For an automation whose top-level
// condition uses triggerMode "once", the condition fires only on the false->true
// crossing, then re-arms when it goes false. RAM-only is deliberate: a reboot
// re-baselines, so a condition that's already true at boot does NOT spuriously
// fire — the first poll just records the baseline.
struct CondEdgeState { long id; bool lastTrue; bool seen; };
static CondEdgeState gCondEdge[16];
static uint8_t gCondEdgeCount = 0;
static CondEdgeState* condEdgeFor(long id) {
  for (uint8_t i = 0; i < gCondEdgeCount; i++) {
    if (gCondEdge[i].id == id) return &gCondEdge[i];
  }
  if (gCondEdgeCount < (uint8_t)(sizeof(gCondEdge) / sizeof(gCondEdge[0]))) {
    CondEdgeState* e = &gCondEdge[gCondEdgeCount++];
    e->id = id; e->lastTrue = false; e->seen = false;
    return e;
  }
  return nullptr;  // table full → caller falls back to level behavior (safe)
}

// Core scheduler logic - extracted for reuse
void schedulerTickMinute() {
  // Only valid if time is synced
  time_t now = time(nullptr);
  if (now <= 0) return;

  DEBUGF(DEBUG_AUTOMATIONS, "[automations] tick now=%lu", (unsigned long)now);

  // Load automations.json
  String json;
  if (!readText(AUTOMATIONS_JSON_FILE, json)) return;
  DEBUGF(DEBUG_AUTOMATIONS, "[automations] json size=%d", json.length());

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

    // Duplicate-id guard
    bool dupSeen = false;
    for (int i = 0; i < seenCount; ++i) {
      if (seenIds[i] == id) {
        dupSeen = true;
        break;
      }
    }
    if (dupSeen) {
      DEBUGF(DEBUG_AUTOMATIONS, "[autos] duplicate id detected at runtime id=%ld; skipping and queuing sanitize", id);
      queueSanitize = true;
      pos = objEnd + 1;
      continue;
    }
    if (seenCount < (int)(sizeof(seenIds) / sizeof(seenIds[0]))) { seenIds[seenCount++] = id; }

    evaluated++;

    // Check if enabled
    bool enabled = (obj.indexOf("\"enabled\": true") >= 0) || (obj.indexOf("\"enabled\":true") >= 0);
    if (!enabled) {
      DEBUGF(DEBUG_AUTOMATIONS, "[autos] id=%ld skip: disabled", id);
      pos = objEnd + 1;
      continue;
    }

    // Parse nextAt field
    time_t nextAt = 0;
    int nextAtPos = obj.indexOf("\"nextAt\"");
    if (nextAtPos >= 0) {
      int nextAtColon = obj.indexOf(':', nextAtPos);
      int nextAtComma = obj.indexOf(',', nextAtColon);
      int nextAtBrace = obj.indexOf('}', nextAtColon);
      int nextAtEnd = (nextAtComma > 0 && (nextAtBrace < 0 || nextAtComma < nextAtBrace)) ? nextAtComma : nextAtBrace;
      if (nextAtEnd > nextAtColon) {
        String nextAtStr = obj.substring(nextAtColon + 1, nextAtEnd);
        nextAtStr.trim();
        if (nextAtStr != "null" && nextAtStr.length() > 0) {
          nextAt = (time_t)nextAtStr.toInt();
        }
      }
    }

    // If nextAt is missing or invalid, compute it now
    if (nextAt <= 0) {
      nextAt = computeNextRunTime(obj.c_str(), now);
      if (nextAt > 0) {
        updateAutomationNextAt(id, nextAt);
        DEBUGF(DEBUG_AUTOMATIONS, "[autos] id=%ld computed missing nextAt=%lu", id, (unsigned long)nextAt);
      } else {
        DEBUGF(DEBUG_AUTOMATIONS, "[autos] id=%ld skip: could not compute nextAt", id);
        pos = objEnd + 1;
        continue;
      }
    }

    // Check if it's time to run
    if (now >= nextAt) {
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

        // Evaluate global condition gate if present.
        if (condition.length() > 0) {
          String wrapped = "IF " + condition + " THEN _";
          bool conditionMet = evaluateCondition(wrapped.c_str());
          DEBUGF(DEBUG_AUTOMATIONS, "[autos] id=%ld condition='%s' result=%s",
                 id, condition.c_str(), conditionMet ? "TRUE" : "FALSE");

          // triggerMode: "once" fires only on the false->true crossing (edge),
          // then re-arms when the condition goes false. Missing/"repeat" fires
          // every poll while true (legacy). Edge state is RAM-only (gCondEdge).
          char trigModeBuf[16] = "";
          extractJsonString(obj.c_str(), "\"triggerMode\"", trigModeBuf, sizeof(trigModeBuf));
          bool fire = conditionMet;
          if (strcmp(trigModeBuf, "once") == 0) {
            CondEdgeState* st = condEdgeFor(id);
            fire = conditionMet && st && st->seen && !st->lastTrue;  // rising edge only
            if (st) { st->lastTrue = conditionMet; st->seen = true; }
          }

          if (!fire) {
            if (gAutoLogActive) {
              char skipBuf[256];
              snprintf(skipBuf, sizeof(skipBuf), "Scheduled automation skipped: ID=%ld Name=%s Condition not met: %s", id, autoName.c_str(), condition.c_str());
              appendAutoLogEntry("AUTO_SKIP", skipBuf);
            }
            DEBUGF(DEBUG_AUTOMATIONS, "[autos] id=%ld condition-gate skip (mode=%s met=%d): %s",
                   id, trigModeBuf[0] ? trigModeBuf : "repeat", conditionMet ? 1 : 0, condition.c_str());
            pos = objEnd + 1;
            continue;
          }
        }

        char _createdByBuf[64];
        extractJsonString(obj.c_str(), "\"createdBy\"", _createdByBuf, sizeof(_createdByBuf));

        // Log scheduled automation start if logging is active
        if (gAutoLogActive) {
          char startBuf[256];
          snprintf(startBuf, sizeof(startBuf), "Scheduled automation started: ID=%ld Name=%s User=%s", id, autoName.c_str(), _createdByBuf);
          appendAutoLogEntry("AUTO_START", startBuf);
        }

        // Execute commands (with conditional logic support)
        for (int ci = 0; ci < cmdsCount; ++ci) {
          DEBUGF(DEBUG_AUTOMATIONS, "[autos] id=%ld run cmd[%d]='%s'", id, ci, cmdsList[ci].c_str());

          // Queue command for execution (async, non-blocking)
          const char* result = executeConditionalCommand(cmdsList[ci].c_str(), _createdByBuf, autoName.c_str());

          // Output the result (skip internal status messages - actual output comes from queue)
          if (!isAutoInternalResult(result)) {
            {
              char schedBuf[256];
              snprintf(schedBuf, sizeof(schedBuf), "[Scheduled Automation %ld] %s", id, result);
              broadcastOutput(schedBuf);
            }
          }
        }
        executed++;

        // Log scheduled automation end if logging is active
        if (gAutoLogActive) {
          char endBuf[256];
          snprintf(endBuf, sizeof(endBuf), "Scheduled automation completed: ID=%ld Name=%s Commands=%d", id, autoName.c_str(), cmdsCount);
          appendAutoLogEntry("AUTO_END", endBuf);
        }

        // Update next run time via the unified post-fire helper.
        rescheduleAfterFire(id, obj.c_str(), now);
      } else {
        DEBUGF(DEBUG_AUTOMATIONS, "[autos] id=%ld skip: no commands found", id);
      }
    } else {
      DEBUGF(DEBUG_AUTOMATIONS, "[autos] id=%ld wait: nextAt=%lu now=%lu", id, (unsigned long)nextAt, (unsigned long)now);
    }

    pos = objEnd + 1;
  }

  DEBUGF(DEBUG_AUTOMATIONS, "[autos] evaluated=%d executed=%d", evaluated, executed);

  // Handle duplicate sanitization
  static unsigned long s_lastAutoSanitizeMs = 0;
  if (queueSanitize) {
    unsigned long nowMs = millis();
    if (nowMs - s_lastAutoSanitizeMs > 5000UL) {
      String fix;
      if (readText(AUTOMATIONS_JSON_FILE, fix)) {
        if (sanitizeAutomationsJson(fix)) {
          writeAutomationsJsonAtomic(fix);
          gAutosDirty = true;
          DEBUGF(DEBUG_AUTOMATIONS, "[autos] Runtime sanitize applied after duplicate detection; scheduler refresh queued");
        } else {
          DEBUGF(DEBUG_AUTOMATIONS, "[autos] Runtime sanitize: no changes needed");
        }
      }
      s_lastAutoSanitizeMs = nowMs;
    } else {
      DEBUGF(DEBUG_AUTOMATIONS, "[autos] Runtime sanitize skipped (debounced)");
    }
  }

  // Rebuild the in-RAM cache from the (possibly post-fire updated) file so the
  // fast due-check in the main loop can skip a rescan until something changes.
  rebuildAutoCache();
}

// Start the automation scheduler (now runs from main loop, no dedicated task)
bool startAutomationScheduler() {
  DEBUGF(DEBUG_AUTOMATIONS, "[automations] Scheduler enabled (runs from main loop)");
  return true;
}

// Stop the automation scheduler (no-op, runs from main loop)
void stopAutomationScheduler() {
  DEBUGF(DEBUG_AUTOMATIONS, "[automations] Scheduler disabled");
}

// ============================================================================
// Print / Broadcast Command
// ============================================================================

static const char* cmd_print(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (argsInput.length() == 0) return "Usage: print <message>";
  broadcastOutput(argsInput.c_str());
  return "Message printed";
}

// ============================================================================
// Automation Command Registry
// ============================================================================

// CommandEntry struct is defined in system_utils.h (included via automation_system.h)

// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
const CommandEntry automationCommands[] = {
  // Primary dispatcher: "automation <subcommand> [args]"
  // Subcommands: system enable|disable|status, list, add, enable, disable, delete, run, sanitize, recompute
  { "automation", "Automation system: automation <subcommand> [args].", false, cmd_automation,
    "Usage: automation <system enable|disable|status | list | add | enable | disable | delete | run | trigger | sanitize | recompute>" },

  // Single-word aliases for common operations (follow naming convention)
  { "automationlist", "List all automations.", false, cmd_automation_list },
  { "automationadd", "Add automation (same as 'automation add').", false, cmd_automation_add },
  { "automationrun", "Run automation by ID: automationrun id=<id>.", false, cmd_automation_run },
  { "automationtrigger", "Arm afterDelay automation timer: automationtrigger id=<id>.", false, cmd_automation_trigger },

  // Utility commands
  { "autolog", "Automation logging: autolog start \"<file>\" | stop | status.", false, cmd_autolog, "Usage: autolog start \"<filename>\" | autolog stop | autolog status" },
  { "validate-conditions", "Validate conditional automation syntax: validate-conditions IF temp>75 THEN ledcolor red.", true, cmd_validate_conditions },
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
  { "automationsEnabled", SETTING_BOOL, &gSettings.automationsEnabled, true, 0, nullptr, 0, 1, "Automations Enabled", nullptr, false, nullptr, nullptr }
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
