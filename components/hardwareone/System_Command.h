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
String executeCommandThroughRegistry(const String& cmd);
String resolveRegistryCommandKey(const String& command);

// Global access to command registry
extern const CommandEntry** gCommands;
extern size_t gCommandsCount;

// Command system initialization
void initializeCommandSystem();

// Debug summary of auto-registered modules (call after debug flags applied)
void printCommandModuleSummary();

// Automatic command discovery system
struct ModuleInfo {
  const char* name;
  const CommandEntry* commands;
  size_t count;
};

class CommandModuleRegistrar {
public:
  CommandModuleRegistrar(const CommandEntry* commands, size_t count, const char* moduleName);
};

// Macro for automatic registration in module files
#define REGISTER_COMMAND_MODULE(commands, count, name) \
  static CommandModuleRegistrar _cmd_registrar(commands, count, name)

#endif // COMMAND_SYSTEM_H
