#ifndef SYSTEM_AUTOMATION_H
#define SYSTEM_AUTOMATION_H

#include <Arduino.h>
#include <time.h>

#include "System_BuildConfig.h"
#include "System_CommandRegistry.h"

#if ENABLE_AUTOMATION

// Automation command registry
extern const CommandEntry automationCommands[];
extern const size_t automationCommandsCount;

// Forward declarations
struct Settings;

// Forward declarations from main system
extern bool gCLIValidateOnly;
void fsLock(const char* reason);
void fsUnlock();

// Automation callback type
typedef bool (*AutomationCallback)(const char* autoJson, size_t jsonLen, void* userData);

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

// Runtime state is separate from automationEnabled (permission) and
// automationAutoStart (boot intent). Only the owner changes it.
bool automationSchedulerRunning();
// Call from the main task while automationEnabled permits execution. The
// owner checks runtime state, due work, file changes and event throttling.
void automationTick();
void notifyAutomationScheduler();

// Event-bus hook: called by systemEventPost() from ANY task. Sets the
// events-pending flag for subscribed kinds, or conservatively while the cache
// needs refreshing. Valid unsubscribed kinds do not force a tick. Atomic
// reads/writes only — no I/O, no locks.
void automationOnSystemEvent(uint8_t kind);

// Automation file operations
extern const char* AUTOMATIONS_JSON_FILE;
bool sanitizeAutomationsJson(String& jsonRef);
// Redact credential-bearing command strings in an automations JSON document
// before it crosses a CLI/HTTP/log boundary. Returns an empty String on parse
// failure so callers fail closed instead of echoing malformed raw storage.
String redactAutomationsJsonForResponse(const String& json);
bool writeAutomationsJsonAtomic(const String& json);
bool streamParseAutomations(const char* path, AutomationCallback callback, void* userData);
bool updateAutomationNextAt(long automationId, time_t newNextAt);
// Write a specific trigger's nextAt (by zero-based index in the triggers array).
bool updateAutomationTriggerNextAt(long automationId, int triggerIdx, time_t newNextAt);

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
inline void automationTick() {}
inline void notifyAutomationScheduler() {}
inline void automationOnSystemEvent(uint8_t) {}
inline bool sanitizeAutomationsJson(String&) { return false; }
inline String redactAutomationsJsonForResponse(const String&) { return String(); }
inline bool writeAutomationsJsonAtomic(const String&) { return false; }
inline const char* executeConditionalCommand(const char*, const char*) { return "disabled"; }
inline bool evaluateCondition(const char*) { return false; }
inline bool automationLogActive() { return false; }
inline bool appendAutoLogEntry(const char*, const String&) { return false; }
inline const char* cmd_autolog(const String&) { return "Error: Automation support is not enabled in this build"; }

#endif // ENABLE_AUTOMATION

#endif // SYSTEM_AUTOMATION_H
