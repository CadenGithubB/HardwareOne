#ifndef SYSTEM_AUTOMATION_H
#define SYSTEM_AUTOMATION_H

#include <Arduino.h>
#include <time.h>

#include "System_BuildConfig.h"
#include "System_CommandRegistry.h"

// automations.json format version, the file's top-level "version". A file with
// no "version" (older files) or this version or an older one loads as before;
// a newer one is refused and left unchanged (readAutomationsJson). `versions`
// reports it as automations_file.
constexpr uint32_t kAutomationsFileVersion = 2;

#if ENABLE_AUTOMATION

// Forward declarations
struct Settings;

// Forward declarations from main system
extern bool gCliValidateOnly;
void fsLock(const char* reason);
void fsUnlock();

// Core automation system functions
bool initAutomationSystem();
void suspendAutomationSystem();
void resumeAutomationSystem();
void runAutomationsOnBoot();

// Automation scheduler functions (runs from main loop, no dedicated task)
bool startAutomationScheduler();
// Prevents new passes and releases the cache. A pass already claimed by the
// main task may finish dispatching; stop does not cancel queued commands.
void stopAutomationScheduler();

// Runtime state is separate from automationAutoStart (boot intent). Only the
// owner changes it.
bool automationSchedulerRunning();
// Starts (enable) or stops (disable) the scheduler now; saves nothing.
// nullptr on success, else an error reply. The one owner transition.
const char* automationSetSystemEnabled(bool enable);
// Call from the main task on every loop. The owner checks runtime state, due
// work, file changes and event throttling.
void automationTick();
void notifyAutomationScheduler();

// Event-bus hook: called by systemEventPost() from ANY task. Sets the
// events-pending flag for subscribed kinds, or conservatively while the cache
// needs refreshing. Valid unsubscribed kinds do not force a tick. Atomic
// reads/writes only — no I/O, no locks.
void automationOnSystemEvent(uint8_t kind);

// Automation file operations
extern const char* AUTOMATIONS_JSON_FILE;
// What sanitizeAutomationsJson changed: ids given to a second holder of the
// same id, and ids above the range (System_AutomationId.h) given a new one.
struct AutomationSanitizeCounts {
  int duplicates = 0;
  int outOfRange = 0;
};
// True when it changed the document. `counts`, when given, says what it fixed.
bool sanitizeAutomationsJson(String& jsonRef, AutomationSanitizeCounts* counts = nullptr);
// Redact credential-bearing command strings in an automations JSON document
// before it crosses a CLI/HTTP/log boundary. Returns an empty String on parse
// failure so callers fail closed instead of echoing malformed raw storage.
// The result also shows each short interval's next run as the scheduler holds
// it in RAM; the file keeps an older value for those (see
// AutomationVolatileSchedule in System_Automation.cpp). While the clock is not
// set, interval and delay triggers count uptime and show no next run (null;
// AutomationUptimeSchedule).
String redactAutomationsJsonForResponse(const String& json);
// Reads automations.json. False when it is missing, unreadable or written by
// newer firmware (a "version" above kAutomationsFileVersion): such a file is
// not loaded, so nothing is changed and written back from it. The writer also
// refuses any document with a newer version.
bool readAutomationsJson(String& json);
// The same read; *newerVersion is the refused file's format, or 0 when the
// read did not refuse one (missing, unreadable or loaded). For callers that
// report why, such as the web Automations page.
bool readAutomationsJson(String& json, uint32_t* newerVersion);
// Writes a whole automations document a caller built (the empty store, an
// import). Refuses a newer format and a document that does not parse or has no
// "automations" array. It cannot make an earlier read current: do not pair it
// with readAutomationsJson to change the store; use automationsSanitizeStore
// or a command.
bool writeAutomationsJsonAtomic(const String& json);
// Read, sanitize the ids and write back under one lock: the one-call form of
// readAutomationsJson + sanitizeAutomationsJson + writeAutomationsJsonAtomic.
// False when the file could not be read (missing, unreadable, newer format)
// or the repaired document could not be written. *changedOut says whether
// anything was repaired, `counts` what.
bool automationsSanitizeStore(AutomationSanitizeCounts* counts = nullptr, bool* changedOut = nullptr);
// Disables every enabled automation whose createdBy is exactly `username`, in
// one locked write (userdelete calls it: steps run as their creator, so a
// re-added name would otherwise inherit them). Nothing re-enables them.
// Returns how many were disabled, or -1 when automations.json could not be
// read, parsed or written.
int automationDisableOwnedBy(const String& username);
// An empty store in the current format (the first-time setup seed).
String automationsEmptyStoreJson();
bool updateAutomationNextAt(long automationId, time_t newNextAt);
// Write a specific trigger's nextAt (by zero-based index in the triggers array).
bool updateAutomationTriggerNextAt(long automationId, int triggerIdx, time_t newNextAt);

// Automation list summaries for the OLED and G2 pages. Each is a copy built by
// the owner with the trigger parser the scheduler fires from, so pages never
// open or parse automations.json themselves.
enum class AutomationTriggerKind : uint8_t {
  None,      // no triggers
  Invalid,   // no trigger the scheduler accepts
  Time,      // daily or weekly at HH:MM
  Monthly,
  Yearly,
  Interval,
  Manual,    // armed by "automation trigger", fires after its delay
  Boot,
  Event,
};

struct AutomationSummary {
  long id;
  uint16_t commandCount;
  uint8_t triggerCount;          // entries in the "triggers" array
  AutomationTriggerKind kind;    // the first trigger the scheduler accepts
  bool enabled;
  char name[32];                 // stored name, or "Auto #<id>"
  char trigger[40];              // "daily 07:30", "weekly 08:00 MoWe", "monthly 15 07:30",
                                 // "every 5m", "on <event>", ...; " +N" for N more triggers
};

// Copies up to maxCount automations, in file order. False when the file cannot
// be read or parsed (*count is then 0).
bool automationCopySummaries(AutomationSummary* out, size_t maxCount, size_t* count);
// The automation with this id. False when it is missing or the file cannot be read.
bool automationFindSummary(long id, AutomationSummary* out);

// Automation command handlers
const char* cmd_automation(const String& originalCmd);
const char* cmd_automation_list(const String& argsInput);
const char* cmd_automation_add(const String& originalCmd);
const char* cmd_automation_enable_disable(const String& originalCmd, bool enable);
const char* cmd_automation_delete(const String& originalCmd);
const char* cmd_automation_run(const String& originalCmd);
const char* cmd_automation_trigger(const String& originalCmd);
const char* cmd_validate_conditions(const String& argsInput);
const char* cmd_autolog(const String& argsInput);

// Automation execution
void runAutomationCommandUnified(const String& argsInput);

// Helper function for automation processing (public)
time_t computeNextRunTime(const char* automationJson, time_t fromTime);

// Conditional command evaluation (modernized - no String returns)
const char* evaluateConditionalChain(const char* chainStr, char* outBuf, size_t outBufSize);
const char* executeConditionalCommand(const char* command, const char* owner);
bool evaluateCondition(const char* condition);
const char* validateConditionalHierarchy(const char* conditions);

// The active check is a cheap hint for avoiding unused message formatting.
// Append rechecks the session and writes with its captured starter identity.
// True means the complete entry passed byte-count, write-error and size checks.
// A failed append can leave a partial line; it does not replace existing data.
bool automationLogActive();
bool appendAutoLogEntry(const char* type, const String& message);

#else // !ENABLE_AUTOMATION

// Stub declarations when Automation is disabled
inline bool initAutomationSystem() { return false; }
inline void suspendAutomationSystem() {}
inline void resumeAutomationSystem() {}
inline void runAutomationsOnBoot() {}
inline bool startAutomationScheduler() { return false; }
inline void stopAutomationScheduler() {}
inline bool automationSchedulerRunning() { return false; }
inline const char* automationSetSystemEnabled(bool) { return "Error: Automations are not compiled in this build"; }
inline void automationTick() {}
inline void notifyAutomationScheduler() {}
inline void automationOnSystemEvent(uint8_t) {}
inline bool sanitizeAutomationsJson(String&) { return false; }
inline String redactAutomationsJsonForResponse(const String&) { return String(); }
inline bool writeAutomationsJsonAtomic(const String&) { return false; }
struct AutomationSanitizeCounts;
inline bool automationsSanitizeStore(AutomationSanitizeCounts* = nullptr, bool* = nullptr) { return false; }
// No automations in this build: nothing to disable.
inline int automationDisableOwnedBy(const String&) { return 0; }
inline const char* executeConditionalCommand(const char*, const char*) { return "disabled"; }
inline bool evaluateCondition(const char*) { return false; }
inline bool automationLogActive() { return false; }
inline bool appendAutoLogEntry(const char*, const String&) { return false; }
inline const char* cmd_autolog(const String&) { return "Error: Automation support is not enabled in this build"; }

#endif // ENABLE_AUTOMATION

#endif // SYSTEM_AUTOMATION_H
