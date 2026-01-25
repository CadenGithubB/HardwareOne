#ifndef AUTOMATION_SYSTEM_H
#define AUTOMATION_SYSTEM_H

#include <Arduino.h>
#include <time.h>

#include "System_BuildConfig.h"
#include "System_Utils.h"

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

// Automation system constants
#define kAutoMemoCap 128

// Context structure for streaming automation callback
struct SchedulerContext {
  time_t now;
  int evaluated;
  int executed;
  bool queueSanitize;
  long seenIds[128];
  int seenCount;
};

// Automation callback type
typedef bool (*AutomationCallback)(const char* autoJson, size_t jsonLen, void* userData);

// Core automation system functions
bool initAutomationSystem();
void suspendAutomationSystem();
void resumeAutomationSystem();
void runAutomationsOnBoot();

// Automation scheduler functions (runs from main loop, no dedicated task)
bool startAutomationScheduler();
void stopAutomationScheduler();
void notifyAutomationScheduler();

// Automation file operations
bool sanitizeAutomationsJson(String& jsonRef);
bool writeAutomationsJsonAtomic(const String& json);
bool streamParseAutomations(const char* path, AutomationCallback callback, void* userData);
bool updateAutomationNextAt(long automationId, time_t newNextAt);

// Automation command handlers
const char* cmd_automation(const String& originalCmd);
const char* cmd_automation_list(const String& cmd);
const char* cmd_automation_add(const String& originalCmd);
const char* cmd_automation_enable_disable(const String& originalCmd, bool enable);
const char* cmd_automation_delete(const String& originalCmd);
const char* cmd_automation_run(const String& originalCmd);
const char* cmd_validate_conditions(const String& cmd);
// NOTE: cmd_downloadautomation, cmd_autolog, and cmd_conditional are declared
// and implemented in the main .ino file to avoid duplication

// Automation execution
void runAutomationCommandUnified(const String& cmd);

// Automation scheduler tick (called from main loop)
void schedulerTickMinute();
bool processAutomationCallback(const char* autoJson, size_t jsonLen, void* userData);

// Helper function for automation processing (public)
time_t computeNextRunTime(const char* automationJson, time_t fromTime);

// Conditional command evaluation (modernized - no String returns)
const char* evaluateConditionalChain(const char* chainStr, char* outBuf, size_t outBufSize);
const char* executeConditionalCommand(const char* command);
bool evaluateCondition(const char* condition);
const char* validateConditionalHierarchy(const char* conditions);

// Global automation state
extern bool gInAutomationContext;
extern long* gAutoMemoId;
extern time_t* gAutoMemoNextAt;
extern int gAutoMemoCount;
extern bool gAutosDirty;

// Automation logging and execution context variables (defined in automation_system.cpp)
extern bool gAutoLogActive;
extern String gAutoLogFile;
extern String gAutoLogAutomationName;
extern String gExecUser;

#else // !ENABLE_AUTOMATION

// Stub declarations when Automation is disabled
inline bool initAutomationSystem() { return false; }
inline void suspendAutomationSystem() {}
inline void resumeAutomationSystem() {}
inline void runAutomationsOnBoot() {}
inline bool startAutomationScheduler() { return false; }
inline void stopAutomationScheduler() {}
inline void notifyAutomationScheduler() {}
inline bool sanitizeAutomationsJson(String&) { return false; }
inline bool writeAutomationsJsonAtomic(const String&) { return false; }
inline void schedulerTickMinute() {}
inline const char* executeConditionalCommand(const char*) { return "disabled"; }
inline bool evaluateCondition(const char*) { return false; }

// Global state stubs
static bool gInAutomationContext = false;
static bool gAutosDirty = false;
static bool gAutoLogActive = false;

#endif // ENABLE_AUTOMATION

#endif // AUTOMATION_SYSTEM_H
