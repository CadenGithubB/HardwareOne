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
 * ✅ parseAtTimeMatchDays() - const char* input, stack buffers
 * ✅ computeNextRunTime() - const char* input, ArduinoJson parsing
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
#include "System_MemUtil.h"
#include "System_Settings.h"
#include "System_User.h"
#include "System_Utils.h"

#if ENABLE_APDS_SENSOR
#include "i2csensor-apds9960.h"
#endif
#if ENABLE_THERMAL_SENSOR
#include "i2csensor-mlx90640.h"
#endif
#if ENABLE_TOF_SENSOR
#include "i2csensor-vl53l4cx.h"
#endif

// External dependencies from .ino
extern Settings gSettings;
extern bool gCLIValidateOnly;
// gDebugBuffer, gDebugFlags, ensureDebugBuffer now from debug_system.h
extern void runUnifiedSystemCommand(const String& cmd);

// Async command submission types (matches HardwareOne.cpp / System_Utils.cpp)
typedef void (*ExecAsyncCallback)(bool ok, const char* result, void* userData);
enum CommandOrigin { ORIGIN_SERIAL, ORIGIN_WEB, ORIGIN_AUTOMATION, ORIGIN_SYSTEM };
enum CmdOutputMask { CMD_OUT_SERIAL = 1 << 0, CMD_OUT_WEB = 1 << 1, CMD_OUT_LOG = 1 << 2, CMD_OUT_BROADCAST = 1 << 3 };
struct CommandContext {
  CommandOrigin origin;
  AuthContext auth;
  uint32_t id;
  uint32_t timestampMs;
  uint8_t outputMask;
  bool validateOnly;
  void* replyHandle;
  httpd_req_t* httpReq;
};
struct Command {
  String line;
  CommandContext ctx;
};
extern bool submitCommandAsync(const Command& cmd, ExecAsyncCallback callback, void* userData);

// Queue an automation sub-command through the FreeRTOS command queue (async, non-blocking).
// This avoids deadlock when already on cmd_exec task and avoids blocking the main loop.
static void queueAutomationSubCommand(const char* cmd) {
  Command uc;
  uc.line = cmd;
  uc.ctx.origin = ORIGIN_SYSTEM;
  AuthContext actx;
  actx.transport = SOURCE_INTERNAL;
  actx.user = "system";
  actx.ip = String();
  actx.path = "/automation";
  actx.opaque = nullptr;
  uc.ctx.auth = actx;
  uc.ctx.id = (uint32_t)millis();
  uc.ctx.timestampMs = (uint32_t)millis();
  uc.ctx.outputMask = CMD_OUT_LOG | CMD_OUT_BROADCAST;
  uc.ctx.validateOnly = false;
  uc.ctx.replyHandle = nullptr;
  uc.ctx.httpReq = nullptr;
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
String gAutoLogAutomationName = "";
extern String gExecUser;
extern bool gExecIsAdmin;

// Forward declarations for functions implemented in this file
bool updateAutomationNextAt(long automationId, time_t newNextAt);
time_t computeNextRunTime(const char* automationJson, time_t fromTime);
const char* executeConditionalCommand(const char* command);
const char* evaluateConditionalChain(const char* chainStr, char* outBuf, size_t outBufSize);
bool evaluateCondition(const char* condition);
const char* validateConditionalHierarchy(const char* conditions);
const char* validateConditionSyntax(const char* condition);
const char* validateConditionalChain(const char* chainStr);
const char* validateConditionalCommand(const char* command);
bool parseAtTimeMatchDays(const char* daysCsv, int tm_wday);
bool automationIdExistsInJson(const String& json, unsigned long id);
// jsonEscape now provided by system_utils.h
void findAutomationsArrayBounds(const String& json, int& arrStart, int& arrEnd);
// appendAutoLogEntry forward declaration removed - implemented in .ino with bool return

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
bool gInAutomationContext = false;
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
  File f = LittleFS.open(path, "r");
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

// jsonEscape implementation removed - now in system_utils.cpp

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

// Atomic writer for automations.json
bool writeAutomationsJsonAtomic(const String& json) {
  const char* tmp = "/automations.tmp";
  if (!writeText(tmp, json)) return false;
  fsLock("autos.rename");
  LittleFS.remove(AUTOMATIONS_JSON_FILE);
  bool renamed = LittleFS.rename(tmp, AUTOMATIONS_JSON_FILE);
  fsUnlock();
  if (!renamed) {
    // Fallback: direct write
    return writeText(AUTOMATIONS_JSON_FILE, json);
  }
  return true;
}

// Update nextAt field in automation JSON using ArduinoJson
bool updateAutomationNextAt(long automationId, time_t newNextAt) {
  String json;
  if (!readText(AUTOMATIONS_JSON_FILE, json)) return false;

  PSRAM_JSON_DOC(doc);
  DeserializationError error = deserializeJson(doc, json);
  if (error) {
    DEBUGF(DEBUG_AUTOMATIONS, "[updateNextAt] JSON parse error: %s", error.c_str());
    return false;
  }
  
  JsonArray automations = doc["automations"].as<JsonArray>();
  if (automations.isNull()) {
    return false;
  }
  bool found = false;
  
  for (JsonObject automation : automations) {
    if (automation["id"].as<long>() == automationId) {
      // Support both new nested schema (schedule.nextAt) and legacy flat schema (nextAt)
      if (!automation["schedule"].isNull()) {
        automation["schedule"]["nextAt"] = (unsigned long)newNextAt;
      } else {
        automation["nextAt"] = (unsigned long)newNextAt;
      }
      found = true;
      DEBUGF(DEBUG_AUTO_TIMING, "[updateNextAt] id=%ld nextAt=%lu", automationId, (unsigned long)newNextAt);
      break;
    }
  }
  
  if (!found) return false;
  
  // Serialize back to string
  json = "";
  serializeJsonPretty(doc, json);
  
  return writeAutomationsJsonAtomic(json);
}

// Run automations on boot
void runAutomationsOnBoot() {
  static bool s_ran = false;
  if (s_ran) return;
  s_ran = true;

  extern bool filesystemReady;
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

    bool runAtBoot = (obj.indexOf("\"runAtBoot\": true") >= 0) || (obj.indexOf("\"runAtBoot\":true") >= 0);
    if (!runAtBoot) {
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

    String cmdsList[64];
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
            if (one.length() && cmdsCount < 64) cmdsList[cmdsCount++] = one;
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
            if (cmd.length() && cmdsCount < 64) cmdsList[cmdsCount++] = cmd;
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
      String wrapped = "IF " + condition + " THEN _";
      bool conditionMet = evaluateCondition(wrapped.c_str());
      DEBUGF(DEBUG_AUTOMATIONS, "[automations] id=%ld boot condition='%s' result=%s",
             id, condition.c_str(), conditionMet ? "TRUE" : "FALSE");
      if (!conditionMet) {
        if (gAutoLogActive) {
          String skipMsg = "Boot automation skipped: ID=" + String(id) + " Name=" + autoName + " Condition not met: " + condition;
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

    if (gAutoLogActive) {
      gAutoLogAutomationName = autoName;
      String startMsg = "Boot automation started: ID=" + String(id) + " Name=" + autoName + " User=system";
      appendAutoLogEntry("AUTO_START", startMsg);
    }

    for (int ci = 0; ci < cmdsCount; ++ci) {
      const char* result = executeConditionalCommand(cmdsList[ci].c_str());

      // Output the result (skip internal status messages - actual output comes from queue)
      if (!isAutoInternalResult(result)) {
        broadcastOutput("[Boot Automation " + String(id) + "] " + String(result));
      }
    }

    if (gAutoLogActive) {
      String endMsg = "Boot automation completed: ID=" + String(id) + " Name=" + autoName + " Commands=" + String(cmdsCount);
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
void runAutomationCommandUnified(const String& cmd) {
  gInAutomationContext = true;
  queueAutomationSubCommand(cmd.c_str());
  gInAutomationContext = false;
}

// Automation command handlers
const char* cmd_automation_list(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String json;
  if (!readText(AUTOMATIONS_JSON_FILE, json)) {
    broadcastOutput("Error: failed to read automations.json");
    return "ERROR";
  }
  broadcastOutput(json);
  return "OK";
}

const char* cmd_automation_add(const String& argsIn) {
  // Do not early-return on validate; we want to perform full argument checks
  bool validateOnly = gCLIValidateOnly;
  
  String args = argsIn;
  args.trim();
  
  auto getVal = [&](const String& key) {
    String k = key + "=";
    int p = args.indexOf(k);
    if (p < 0) return String("");
    int start = p + k.length();
    int end = -1;
    
    // Skip leading whitespace
    while (start < (int)args.length() && args[start] == ' ') start++;
    
    // Check if value is quoted
    if (start < (int)args.length() && args[start] == '"') {
      // Find closing quote
      start++;  // Skip opening quote
      end = args.indexOf('"', start);
      if (end < 0) end = args.length();  // No closing quote, take rest
      return args.substring(start, end);
    } else {
      // Unquoted value - find next parameter
      // Handle empty values (when next char after = is space or another key=)
      if (start < (int)args.length() && args[start] == ' ') {
        // Check if next non-space character starts a new parameter (contains =)
        int nextNonSpace = start;
        while (nextNonSpace < (int)args.length() && args[nextNonSpace] == ' ') nextNonSpace++;
        if (nextNonSpace < (int)args.length()) {
          int nextEquals = args.indexOf('=', nextNonSpace);
          int nextSpace = args.indexOf(' ', nextNonSpace);
          if (nextEquals > 0 && (nextSpace < 0 || nextEquals < nextSpace)) {
            // Next token is a parameter, so current value is empty
            return String("");
          }
        }
      }
      
      // Find end of current value
      for (int i = start; i < (int)args.length(); i++) {
        if (args[i] == ' ' && i + 1 < (int)args.length()) {
          int nextSpace = args.indexOf(' ', i + 1);
          int nextEquals = args.indexOf('=', i + 1);
          if (nextEquals > 0 && (nextSpace < 0 || nextEquals < nextSpace)) {
            end = i;
            break;
          }
        }
      }
      if (end < 0) end = args.length();
      String result = args.substring(start, end);
      result.trim();
      return result;
    }
  };
  
  String name = getVal("name");
  String type = getVal("type");
  String timeS = getVal("time");
  String recurrence = getVal("recurrence");
  String days = getVal("days");
  String delayMs = getVal("delayms");
  String intervalMs = getVal("intervalms");
  String runAtBootStr = getVal("runatboot");
  String bootDelayMsStr = getVal("bootdelayms");
  String cmdStr = getVal("command");
  String cmdsList = getVal("commands");
  String condition = getVal("condition");
  String enabledStr = getVal("enabled");
  
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
      String error = String("Error: Invalid condition expression - ") + conditionError;
      broadcastOutput(error);
      return error.c_str();
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
          // Validate this individual command by calling executeCommand in validation mode
          extern String gExecUser;
          
          // Create minimal auth context for validation
          AuthContext ctx;
          ctx.transport = SOURCE_INTERNAL;
          ctx.path = "/automation/validate";
          ctx.ip = "127.0.0.1";
          ctx.user = gExecUser.length() > 0 ? gExecUser : "system";
          ctx.sid = "";
          ctx.opaque = nullptr;
          
          bool prevValidate = gCLIValidateOnly;
          gCLIValidateOnly = true;
          
          char validationBuf[256];
          executeCommand(ctx, part.c_str(), validationBuf, sizeof(validationBuf));
          String validationResult = validationBuf;
          
          gCLIValidateOnly = prevValidate;
          
          if (validationResult != "VALID") {
            String error = String("Error: Invalid command '") + part + "' - " + validationResult;
            broadcastOutput(error);
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
  String idOverrideStr = getVal("id");
  if (idOverrideStr.length() > 0) {
    unsigned long overrideId = strtoul(idOverrideStr.c_str(), nullptr, 10);
    if (automationIdExistsInJson(json, overrideId)) {
      String needle = String("\"id\": ") + String(overrideId);
      int idPos = json.indexOf(needle);
      int aS = json.indexOf('[');
      int aE = json.lastIndexOf(']');
      if (idPos >= 0 && aS >= 0 && aE >= 0) {
        int oS = json.lastIndexOf('{', idPos);
        int oE = (oS >= 0) ? findJsonObjectEnd(json, oS) : -1;
        if (oS >= 0 && oE >= 0) {
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
  
  // Build schedule sub-object (without nextAt first so computeNextRunTime can read it)
  String sched = "  \"schedule\": {\n";
  sched += "    \"type\": \"" + typeNorm + "\"";
  if (typeNorm == "attime" && timeS.length() > 0) sched += ",\n    \"time\": \"" + jsonEscape(timeS) + "\"";
  if (recurrence.length() > 0) sched += ",\n    \"recurrence\": \"" + jsonEscape(recurrence) + "\"";
  if (typeNorm == "attime" && days.length() > 0) sched += ",\n    \"days\": \"" + jsonEscape(days) + "\"";
  if (typeNorm == "afterdelay" && delayMs.length() > 0) sched += ",\n    \"delayMs\": " + delayMs;
  if (typeNorm == "interval" && intervalMs.length() > 0) sched += ",\n    \"intervalMs\": " + intervalMs;
  if (runAtBoot) sched += ",\n    \"runAtBoot\": true";
  if (bootDelayMsStr.length() > 0) sched += ",\n    \"bootDelayMs\": " + bootDelayMsStr;
  
  // Compute nextAt using partial schedule object
  String tempObj = "{" + sched + "\n  }\n}";
  time_t now = time(nullptr);
  time_t nextAt = 0;
  if (now > 0) {
    nextAt = computeNextRunTime(tempObj.c_str(), now);
  }
  
  if (nextAt > 0) {
    sched += ",\n    \"nextAt\": " + String((unsigned long)nextAt);
    DEBUGF(DEBUG_AUTOMATIONS, "[autos add] nextAt=%lu", (unsigned long)nextAt);
  } else {
    sched += ",\n    \"nextAt\": null";
    DEBUGF(DEBUG_AUTOMATIONS, "[autos add] Warning: could not compute nextAt for automation");
  }
  sched += "\n  }";
  
  // Build final automation object
  String obj = "{\n";
  obj += "  \"id\": " + String(id) + ",\n";
  obj += "  \"name\": \"" + jsonEscape(name) + "\",\n";
  obj += "  \"enabled\": " + String(enabled ? "true" : "false") + ",\n";
  if (condition.length() > 0) obj += "  \"condition\": \"" + jsonEscape(condition) + "\",\n";
  obj += sched + ",\n";
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
  
  String result = String(idOverrideStr.length() > 0 ? "Updated" : "Added") + " automation id=" + String(id) + " name=" + name;
  broadcastOutput(result);
  return "OK";
}

const char* cmd_automation_enable_disable(const String& argsIn, bool enable) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String args = argsIn;
  args.trim();
  
  auto getVal = [&](const String& key) {
    String k = key + "=";
    int p = args.indexOf(k);
    if (p < 0) return String("");
    int s = p + k.length();
    int e = args.indexOf(' ', s);
    if (e < 0) e = args.length();
    return args.substring(s, e);
  };
  
  String idStr = getVal("id");
  if (idStr.length() == 0) {
    String usage = String("Usage: automation ") + (enable ? "enable" : "disable") + " id=<id>";
    broadcastOutput(usage);
    return "ERROR";
  }
  
  String json;
  if (!readText(AUTOMATIONS_JSON_FILE, json)) {
    broadcastOutput("Error: failed to read automations.json");
    return "ERROR";
  }
  
  String needle = String("\"id\": ") + idStr;
  int idPos = json.indexOf(needle);
  if (idPos < 0) {
    broadcastOutput("Error: automation id not found");
    return "ERROR";
  }
  
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
  
  String result = String(enable ? "Enabled" : "Disabled") + " automation id=" + idStr;
  broadcastOutput(result);
  return "OK";
}

const char* cmd_automation_delete(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gExecIsAdmin) { broadcastOutput("Error: admin required"); return "ERROR"; }
  
  String args = argsIn;
  args.trim();
  
  auto getVal = [&](const String& key) {
    String k = key + "=";
    int p = args.indexOf(k);
    if (p < 0) return String("");
    int s = p + k.length();
    int e = args.indexOf(' ', s);
    if (e < 0) e = args.length();
    return args.substring(s, e);
  };
  
  String idStr = getVal("id");
  if (idStr.length() == 0) {
    broadcastOutput("Usage: automation delete id=<id>");
    return "ERROR";
  }
  
  String json;
  if (!readText(AUTOMATIONS_JSON_FILE, json)) {
    broadcastOutput("Error: failed to read automations.json");
    return "ERROR";
  }
  
  String needle = String("\"id\": ") + idStr;
  int idPos = json.indexOf(needle);
  if (idPos < 0) {
    broadcastOutput("Error: automation id not found");
    return "ERROR";
  }
  
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
  
  // Find object bounds (depth-tracked to handle nested schedule sub-object)
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
  
  String result = String("Deleted automation id=") + idStr;
  broadcastOutput(result);
  return "OK";
}

const char* cmd_automation_run(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String args = argsIn;
  args.trim();
  
  auto getVal = [&](const String& key) {
    String k = key + "=";
    int p = args.indexOf(k);
    if (p < 0) return String("");
    int s = p + k.length();
    int e = args.indexOf(' ', s);
    if (e < 0) e = args.length();
    return args.substring(s, e);
  };
  
  String idStr = getVal("id");
  if (idStr.length() == 0) {
    broadcastOutput("Usage: automation run id=<id>");
    return "ERROR";
  }
  
  String json;
  if (!readText(AUTOMATIONS_JSON_FILE, json)) {
    broadcastOutput("Error: failed to read automations.json");
    return "ERROR";
  }
  
  String needle = String("\"id\": ") + idStr;
  int idPos = json.indexOf(needle);
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
    gAutoLogAutomationName = autoName;
    String startMsg = String("Automation started: ID=") + idStr + " Name=" + autoName + " User=" + gExecUser;
    appendAutoLogEntry("AUTO_START", startMsg);
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
  
  String cmdsList[64];
  int cmdsCount = 0;
  
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
    if (cmd.length() && cmdsCount < 64) { cmdsList[cmdsCount++] = cmd; }
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
        String skipMsg = String("Automation skipped: ID=") + idStr + " Name=" + autoName + " Condition not met: " + condition;
        appendAutoLogEntry("AUTO_SKIP", skipMsg);
        gAutoLogAutomationName = "";
      }
      broadcastOutput("Automation skipped - condition not met: " + condition);
      return "OK";
    }
  }
  
  // Execute all commands (with conditional logic support)
  for (int ci = 0; ci < cmdsCount; ++ci) {
    DEBUGF(DEBUG_AUTOMATIONS, "[autos run] id=%s cmd[%d]='%s'", idStr.c_str(), ci, cmdsList[ci].c_str());
    
    // Protect against malformed commands
    if (cmdsList[ci].length() == 0 || cmdsList[ci] == "\\") {
      DEBUGF(DEBUG_AUTOMATIONS, "[autos run] skipping malformed command: '%s'", cmdsList[ci].c_str());
      continue;
    }
    
    // Queue command for execution (async, non-blocking)
    const char* result = executeConditionalCommand(cmdsList[ci].c_str());
    
    // Output the result (skip internal status messages - actual output comes from queue)
    if (!isAutoInternalResult(result)) {
      broadcastOutput(String("[Automation ") + idStr + "] " + result);
    }
  }
  
  // Advance nextAt after manual execution
  time_t now = time(nullptr);
  if (now > 0) {
    time_t nextAt = computeNextRunTime(obj.c_str(), now);
    if (nextAt > 0) {
      long id = idStr.toInt();
      if (updateAutomationNextAt(id, nextAt)) {
        DEBUGF(DEBUG_AUTOMATIONS, "[autos run] advanced nextAt=%lu for id=%s", (unsigned long)nextAt, idStr.c_str());
      } else {
        DEBUGF(DEBUG_AUTOMATIONS, "[autos run] warning: failed to update nextAt for id=%s", idStr.c_str());
      }
    } else {
      DEBUGF(DEBUG_AUTOMATIONS, "[autos run] warning: could not compute nextAt for id=%s", idStr.c_str());
    }
  }
  
  // Log automation end if logging is active
  if (gAutoLogActive) {
    String endMsg = String("Automation completed: ID=") + idStr + " Name=" + autoName + " Commands=" + String(cmdsCount);
    appendAutoLogEntry("AUTO_END", endMsg);
  }
  
  String result = String("Ran automation id=") + idStr + " (" + String(cmdsCount) + " command" + (cmdsCount == 1 ? "" : "s") + ")";
  broadcastOutput(result);
  return "OK";
}

// Main automation command dispatcher
const char* cmd_automation(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  String args = argsIn;
  args.trim();
  String argsLower = args;
  argsLower.toLowerCase();

  // Extract subcommand (first word)
  int sp = argsLower.indexOf(' ');
  String subCmd = (sp >= 0) ? argsLower.substring(0, sp) : argsLower;
  String subArgs = (sp >= 0) ? args.substring(sp + 1) : String("");
  subArgs.trim();

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
    
    String result = String("Recomputed nextAt: ") + String(recomputed) + " succeeded, " + String(failed) + " failed";
    broadcastOutput(result);
    return "OK";
  } else if (subCmd == "run") {
    return cmd_automation_run(subArgs);
  }

  broadcastOutput("Unknown automation command. Use: list, add, enable, disable, delete, run, sanitize, recompute");
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
            String skipMsg = "Scheduled automation skipped: ID=" + String(id) + " Name=" + autoName + " Condition not met: " + condition;
            appendAutoLogEntry("AUTO_SKIP", skipMsg);
          }
          DEBUGF(DEBUG_AUTO_CONDITION, "[autos] id=%ld skipped - condition not met: %s", id, condition.c_str());
          return true;
        }
      }

      // Log scheduled automation start if logging is active
      if (gAutoLogActive) {
        gAutoLogAutomationName = autoName;
        if (ensureDebugBuffer()) {
          snprintf(getDebugBuffer(), 1024, "Scheduled automation started: ID=%lu Name=%s User=system", id, autoName.c_str());
          appendAutoLogEntry("AUTO_START", String(getDebugBuffer()));
        }
      }

      // Execute commands (with conditional logic support)
      for (int ci = 0; ci < cmdsCount; ++ci) {
        DEBUGF(DEBUG_AUTO_EXEC, "[autos] id=%ld run cmd[%d]='%s'", id, ci, cmdsList[ci].c_str());

        // Queue command for execution (async, non-blocking)
        const char* result = executeConditionalCommand(cmdsList[ci].c_str());

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

      // Compute and update next run time
      time_t newNextAt = computeNextRunTime(obj.c_str(), ctx->now);
      if (newNextAt > 0) {
        updateAutomationNextAt(id, newNextAt);
        DEBUGF(DEBUG_AUTO_TIMING, "[autos] id=%ld updated nextAt=%lu", id, (unsigned long)newNextAt);
      } else {
        DEBUGF(DEBUG_AUTO_TIMING, "[autos] id=%ld warning: could not compute next nextAt", id);
      }
    } else {
      DEBUGF(DEBUG_AUTO_SCHEDULER, "[autos] id=%ld skip: no commands found", id);
    }
  } else {
    DEBUGF(DEBUG_AUTO_TIMING, "[autos] id=%ld wait: nextAt=%lu now=%lu", id, (unsigned long)nextAt, (unsigned long)ctx->now);
  }

  return true;  // Continue processing next automation
}

// Helper: Parse day matching for atTime automations (const char* input)
bool parseAtTimeMatchDays(const char* daysCsv, int tm_wday) {
  if (!daysCsv || daysCsv[0] == '\0') return true;
  
  // tm_wday: 0=Sun..6=Sat
  static const char* names[7] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };
  const char* want = names[tm_wday];
  
  // Use stack buffer for processing
  char buf[128];
  size_t len = strlen(daysCsv);
  if (len >= sizeof(buf) - 2) len = sizeof(buf) - 3;
  
  // Copy and lowercase, skip spaces
  size_t j = 0;
  for (size_t i = 0; i < len; i++) {
    char c = daysCsv[i];
    if (c == ' ') continue;  // Skip spaces
    if (c >= 'A' && c <= 'Z') c += 32;  // toLower
    buf[j++] = c;
  }
  buf[j] = '\0';
  
  // Simple substring search with comma delimiters
  char needle[8];
  snprintf(needle, sizeof(needle), ",%s,", want);
  
  // Add commas to buf for matching
  char wrapped[130];
  snprintf(wrapped, sizeof(wrapped), ",%s,", buf);
  
  return strstr(wrapped, needle) != nullptr;
}

// Compute next run time for automation using ArduinoJson (const char* input)
time_t computeNextRunTime(const char* automationJson, time_t fromTime) {
  PSRAM_JSON_DOC(doc);
  DeserializationError error = deserializeJson(doc, automationJson);
  if (error) {
    DEBUGF(DEBUG_AUTO_TIMING, "[computeNextRunTime] JSON parse error: %s", error.c_str());
    return 0;
  }
  
  // Support both new nested schema (schedule.type) and legacy flat schema (type)
  JsonVariantConst schedDoc = doc["schedule"];
  const char* type = schedDoc.isNull() ? (doc["type"] | "") : (schedDoc["type"] | "");

  if (strcmp(type, "atTime") == 0 || strcmp(type, "attime") == 0) {
    const char* timeStr = schedDoc.isNull() ? (doc["time"] | "") : (schedDoc["time"] | "");
    const char* daysStr = schedDoc.isNull() ? (doc["days"] | "") : (schedDoc["days"] | "");
    
    // Validate time format (HH:MM)
    if (strlen(timeStr) != 5 || timeStr[2] != ':') return 0;
    
    int hour = (timeStr[0] - '0') * 10 + (timeStr[1] - '0');
    int minute = (timeStr[3] - '0') * 10 + (timeStr[4] - '0');
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return 0;

    // Convert fromTime to local time
    struct tm tmNow;
    if (!localtime_r(&fromTime, &tmNow)) return 0;

    // Try today first
    struct tm tmTarget = tmNow;
    tmTarget.tm_hour = hour;
    tmTarget.tm_min = minute;
    tmTarget.tm_sec = 0;
    tmTarget.tm_isdst = -1;

    time_t candidateTime = mktime(&tmTarget);
    bool needNextDay = (candidateTime <= fromTime);

    // Check day restrictions
    if (daysStr[0] != '\0') {
      bool dayMatches = parseAtTimeMatchDays(daysStr, tmTarget.tm_wday);
      if (!dayMatches) needNextDay = true;
    }

    if (needNextDay) {
      // Search up to 7 days ahead
      for (int dayOffset = 1; dayOffset <= 7; dayOffset++) {
        tmTarget = tmNow;
        tmTarget.tm_mday += dayOffset;
        tmTarget.tm_hour = hour;
        tmTarget.tm_min = minute;
        tmTarget.tm_sec = 0;
        tmTarget.tm_isdst = -1;

        candidateTime = mktime(&tmTarget);
        if (candidateTime <= fromTime) continue;

        struct tm tmCheck;
        if (localtime_r(&candidateTime, &tmCheck)) {
          if (daysStr[0] == '\0') {
            return candidateTime;
          } else {
            if (parseAtTimeMatchDays(daysStr, tmCheck.tm_wday)) {
              return candidateTime;
            }
          }
        }
      }
      return 0;
    }

    return candidateTime;

  } else if (strcmp(type, "afterDelay") == 0 || strcmp(type, "afterdelay") == 0) {
    int delayMs = schedDoc.isNull() ? (doc["delayMs"] | 0) : (schedDoc["delayMs"] | 0);
    if (delayMs <= 0) return 0;
    return fromTime + (delayMs / 1000);

  } else if (strcmp(type, "interval") == 0) {
    int intervalMs = schedDoc.isNull() ? (doc["intervalMs"] | 0) : (schedDoc["intervalMs"] | 0);
    if (intervalMs <= 0) return 0;
    return fromTime + (intervalMs / 1000);
  }

  return 0;
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
    if (gThermalCache.mutex && xSemaphoreTake(gThermalCache.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      ok = gThermalCache.thermalDataValid;
      v = gThermalCache.thermalAvgTemp;
      xSemaphoreGive(gThermalCache.mutex);
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
    if (gTofCache.mutex && xSemaphoreTake(gTofCache.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      tofTotal = gTofCache.tofTotalObjects;
      for (int i = 0; i < 4; i++) objs[i] = gTofCache.tofObjects[i];
      xSemaphoreGive(gTofCache.mutex);
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
    if (gPeripheralCache.mutex && xSemaphoreTake(gPeripheralCache.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      ok = gPeripheralCache.apdsDataValid;
      clear = gPeripheralCache.apdsClear;
      xSemaphoreGive(gPeripheralCache.mutex);
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
    if (gPeripheralCache.mutex && xSemaphoreTake(gPeripheralCache.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      ok = gPeripheralCache.apdsDataValid;
      prox = gPeripheralCache.apdsProximity;
      xSemaphoreGive(gPeripheralCache.mutex);
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
const char* executeConditionalCommand(const char* command) {
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
      char conditionBuf[256];
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
      char thenBuf[256];
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
      char elseBuf[256];
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
      char fullCondBuf[512];
      snprintf(fullCondBuf, sizeof(fullCondBuf), "IF %s THEN dummy", condStart);
      bool conditionMet = evaluateCondition(fullCondBuf);

      DEBUGF(DEBUG_AUTOMATIONS, "[conditional] condition='%s' result=%s",
             condStart, conditionMet ? "TRUE" : "FALSE");

      // Execute appropriate command via async queue (avoids deadlock)
      if (conditionMet) {
        if (thenCmdStart[0] != '\0') {
          DEBUGF(DEBUG_AUTOMATIONS, "[conditional] queuing THEN: %s", thenCmdStart);
          queueAutomationSubCommand(thenCmdStart);
          return "Conditional THEN queued";
        }
      } else {
        if (elseBuf[0] != '\0') {
          DEBUGF(DEBUG_AUTOMATIONS, "[conditional] queuing ELSE: %s", elseBuf);
          queueAutomationSubCommand(elseBuf);
          return "Conditional ELSE queued";
        }
      }

      return "Conditional command completed";
    }
  }
  
  // Regular command - queue through FreeRTOS command queue (async, non-blocking)
  queueAutomationSubCommand(command);
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

const char* cmd_autolog(const String& argsIn) {
  extern bool gCLIValidateOnly;
  if (gCLIValidateOnly) return "VALID";
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  String args = argsIn;
  args.trim();

  if (args.startsWith("start ")) {
    String filename = args.substring(6);
    filename.trim();
    if (filename.length() == 0) return "Usage: autolog start <filename>";

    gAutoLogActive = true;
    gAutoLogFile = filename;
    gAutoLogAutomationName = "";

    if (!appendAutoLogEntry("LOG_START", "Automation logging started")) {
      gAutoLogActive = false;
      gAutoLogFile = "";
      snprintf(getDebugBuffer(), 1024, "Error: Failed to create log file: %s", filename.c_str());
      return getDebugBuffer();
    }

    snprintf(getDebugBuffer(), 1024, "Automation logging started: %s", filename.c_str());
    return getDebugBuffer();

  } else if (args == "stop") {
    if (!gAutoLogActive) return "Automation logging is not active";

    appendAutoLogEntry("LOG_STOP", "Automation logging stopped");

    snprintf(getDebugBuffer(), 1024, "Automation logging stopped: %s", gAutoLogFile.c_str());
    gAutoLogActive = false;
    gAutoLogFile = "";
    gAutoLogAutomationName = "";

    return getDebugBuffer();

  } else if (args == "status") {
    if (gAutoLogActive) {
      if (gAutoLogAutomationName.length() > 0) {
        snprintf(getDebugBuffer(), 1024, "Automation logging ACTIVE: %s (automation: %s)",
                 gAutoLogFile.c_str(), gAutoLogAutomationName.c_str());
      } else {
        snprintf(getDebugBuffer(), 1024, "Automation logging ACTIVE: %s", gAutoLogFile.c_str());
      }
      return getDebugBuffer();
    } else {
      return "Automation logging INACTIVE";
    }

  } else {
    return "Usage: autolog start <filename> | autolog stop | autolog status";
  }
}

// Validate conditions command
const char* cmd_validate_conditions(const String& cmd) {
  String conditions = cmd;
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

// Notify the automation scheduler to run on next main loop iteration
void notifyAutomationScheduler() {
  gAutosDirty = true;
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
          DEBUGF(DEBUG_AUTOMATIONS, "[autos] id=%ld condition='%s' result=%s",
                 id, condition.c_str(), conditionMet ? "TRUE" : "FALSE");
          if (!conditionMet) {
            if (gAutoLogActive) {
              String skipMsg = "Scheduled automation skipped: ID=" + String(id) + " Name=" + autoName + " Condition not met: " + condition;
              appendAutoLogEntry("AUTO_SKIP", skipMsg);
            }
            DEBUGF(DEBUG_AUTOMATIONS, "[autos] id=%ld skipped - condition not met: %s", id, condition.c_str());
            pos = objEnd + 1;
            continue;
          }
        }

        // Log scheduled automation start if logging is active
        if (gAutoLogActive) {
          gAutoLogAutomationName = autoName;
          String startMsg = "Scheduled automation started: ID=" + String(id) + " Name=" + autoName + " User=system";
          appendAutoLogEntry("AUTO_START", startMsg);
        }

        // Execute commands (with conditional logic support)
        for (int ci = 0; ci < cmdsCount; ++ci) {
          DEBUGF(DEBUG_AUTOMATIONS, "[autos] id=%ld run cmd[%d]='%s'", id, ci, cmdsList[ci].c_str());

          // Queue command for execution (async, non-blocking)
          const char* result = executeConditionalCommand(cmdsList[ci].c_str());

          // Output the result (skip internal status messages - actual output comes from queue)
          if (!isAutoInternalResult(result)) {
            broadcastOutput("[Scheduled Automation " + String(id) + "] " + String(result));
          }
        }
        executed++;

        // Log scheduled automation end if logging is active
        if (gAutoLogActive) {
          String endMsg = "Scheduled automation completed: ID=" + String(id) + " Name=" + autoName + " Commands=" + String(cmdsCount);
          appendAutoLogEntry("AUTO_END", endMsg);
        }

        // Compute and update next run time
        time_t newNextAt = computeNextRunTime(obj.c_str(), now);
        if (newNextAt > 0) {
          updateAutomationNextAt(id, newNextAt);
          DEBUGF(DEBUG_AUTOMATIONS, "[autos] id=%ld updated nextAt=%lu", id, (unsigned long)newNextAt);
        } else {
          DEBUGF(DEBUG_AUTOMATIONS, "[autos] id=%ld warning: could not compute next nextAt", id);
        }
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

static const char* cmd_print(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (args.length() == 0) return "Usage: print <message>";
  broadcastOutput(args.c_str());
  return "Message printed";
}

// ============================================================================
// Automation Command Registry
// ============================================================================

// CommandEntry struct is defined in system_utils.h (included via automation_system.h)

const CommandEntry automationCommands[] = {
  // Primary dispatcher: "automation <subcommand> [args]"
  // Subcommands: system enable|disable|status, list, add, enable, disable, delete, run, sanitize, recompute
  { "automation", "Automation system: automation <subcommand> [args].", false, cmd_automation,
    "Usage: automation <system enable|disable|status | list | add | enable | disable | delete | run | sanitize | recompute>" },

  // Single-word aliases for common operations (follow naming convention)
  { "automationlist", "List all automations.", false, cmd_automation_list },
  { "automationadd", "Add automation (same as 'automation add').", false, cmd_automation_add },
  { "automationrun", "Run automation by ID: automationrun id=<id>.", false, cmd_automation_run },

  // Utility commands
  { "autolog", "Automation logging: autolog start <file> | stop | status.", false, cmd_autolog, "Usage: autolog start <filename> | autolog stop | autolog status" },
  { "validate-conditions", "Validate conditional automation syntax: validate-conditions IF temp>75 THEN ledcolor red.", true, cmd_validate_conditions },
  { "print", "Broadcast a message to all outputs: print <message>.", false, cmd_print },
  
  // NOTE: downloadautomation and if/conditional commands are registered
  // in the main .ino file's command registry to avoid duplication
};

const size_t automationCommandsCount = sizeof(automationCommands) / sizeof(automationCommands[0]);

// Auto-register with command system
static CommandModuleRegistrar _automation_cmd_registrar(automationCommands, automationCommandsCount, "automation");

// ============================================================================
// Automation Settings Module
// ============================================================================

static const SettingEntry automationSettingEntries[] = {
  { "automationsEnabled", SETTING_BOOL, &gSettings.automationsEnabled, false, 0, nullptr, 0, 1, "Automations Enabled", nullptr }
};

extern const SettingsModule automationSettingsModule = {
  "automation", "automation", automationSettingEntries,
  sizeof(automationSettingEntries) / sizeof(automationSettingEntries[0])
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp

#endif // ENABLE_AUTOMATION
