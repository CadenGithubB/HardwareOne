#ifndef COMMAND_SYSTEM_H
#define COMMAND_SYSTEM_H

#include "System_Utils.h"

// ============================================================================
// Command System - Centralized command registry and execution
// ============================================================================

// Maximum number of commands that can be registered
#define MAX_COMMANDS 512

// Command registry functions
void registerCommand(const CommandEntry* command);
void registerCommands(const CommandEntry* commands, size_t count);
const CommandEntry* findCommand(const String& name);
String executeCommandThroughRegistry(const String& argsInput);
String resolveRegistryCommandKey(const String& command);

// Global access to command registry
extern const CommandEntry** gCommands;
extern size_t gCommandsCount;

// Command system initialization
void initializeCommandSystem();

// Debug summary of auto-registered modules (call after debug flags applied)
void printCommandModuleSummary();

// Module tracking for debug summary
struct ModuleInfo {
  const char* name;
  const CommandEntry* commands;
  size_t count;
};

#endif // COMMAND_SYSTEM_H
