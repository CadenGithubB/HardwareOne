/**
 * Command System Implementation
 * 
 * Centralized command registry and execution system
 * Moved from main .ino to improve modularity and organization
 */

#include <Arduino.h>
#include <string.h>

#include "System_CLI.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_Settings.h"

// Forward declaration for exitToNormalBanner function (defined in cli_system.cpp)
String exitToNormalBanner();

// ============================================================================
// Command Registry Storage
// ============================================================================

// Static storage for all commands (avoid dynamic allocation on embedded systems)
// Maximum 256 commands should be sufficient for the entire system
static const CommandEntry* commandRegistry[MAX_COMMANDS];
static size_t commandRegistrySize = 0;

// Maximum number of command modules we can track for debug summary
#define MAX_MODULES 32

// Module tracking for debug summary
static ModuleInfo registeredModules[MAX_MODULES];
static size_t registeredModuleCount = 0;

// Global access pointers (extern declarations in header)
const CommandEntry** gCommands = nullptr;
size_t gCommandsCount = 0;

// ============================================================================
// Command Registration Functions
// ============================================================================

void registerCommand(const CommandEntry* command) {
  if (!command || commandRegistrySize >= MAX_COMMANDS) {
    // IMPORTANT: do not log here. This function may be called before
    // Serial/logging is ready.
    return;
  }

  commandRegistry[commandRegistrySize] = command;
  commandRegistrySize++;

  // Update global access pointers after each registration
  gCommands = commandRegistry;
  gCommandsCount = commandRegistrySize;
}

void registerCommands(const CommandEntry* commands, size_t count) {
  if (!commands || count == 0) {
    return;
  }

  for (size_t i = 0; i < count; i++) {
    registerCommand(&commands[i]);
  }
}

// ============================================================================
// Command Lookup Functions
// ============================================================================

// Find command using longest-prefix matching
// e.g., "user list json" matches "user list" (not just "user")
const CommandEntry* findCommand(const String& cmdLine) {
  if (cmdLine.length() == 0) {
    return nullptr;
  }
  
  String lc = cmdLine;
  lc.toLowerCase();
  lc.trim();
  
  const CommandEntry* bestMatch = nullptr;
  size_t bestLen = 0;
  
  for (size_t i = 0; i < commandRegistrySize; i++) {
    const char* entryName = commandRegistry[i]->name;
    size_t entryLen = strlen(entryName);
    
    // Convert entry name to lowercase for case-insensitive comparison
    String lcEntry = String(entryName);
    lcEntry.toLowerCase();
    
    // Check if command line starts with this entry name
    if (lc.startsWith(lcEntry)) {
      // Ensure it's a complete word match (followed by space, end, or nothing)
      if (lc.length() == entryLen || lc.charAt(entryLen) == ' ') {
        // Prefer longer matches (e.g., "user list" over "user")
        if (entryLen > bestLen) {
          bestMatch = commandRegistry[i];
          bestLen = entryLen;
        }
      }
    }
  }
  return bestMatch;
}

// Check if a command should remain in help mode rather than exiting it.
// Returns true for commands in the CLI module (help/back/exit/clear) and
// any command whose name matches a registered module name (help navigation).
bool isHelpModeCommand(const char* cmdName) {
  size_t moduleCount;
  const CommandModule* modules = getCommandModules(moduleCount);

  // Check if command belongs to the CLI module (CMD_MODULE_CORE)
  for (size_t i = 0; i < moduleCount; i++) {
    if (!(modules[i].flags & CMD_MODULE_CORE)) continue;
    for (size_t j = 0; j < modules[i].count; j++) {
      if (strcasecmp(modules[i].commands[j].name, cmdName) == 0) return true;
    }
  }

  // Check if command name matches any module name (help navigation shortcut)
  for (size_t i = 0; i < moduleCount; i++) {
    if (strcasecmp(modules[i].name, cmdName) == 0) return true;
  }

  return false;
}

// Resolve the canonical registry command key from a full command line
// Uses longest-prefix matching to find the command name
String resolveRegistryCommandKey(const String& command) {
  String cmd = command;
  cmd.trim();
  
  if (cmd.length() == 0) {
    return "";
  }
  
  // Use findCommand() which does longest-prefix matching
  const CommandEntry* found = findCommand(cmd);
  if (found) {
    return String(found->name);
  }
  
  return "";
}

// ============================================================================
// Command Execution
// ============================================================================

String executeCommandThroughRegistry(const String& argsInput) {
  String command = argsInput;
  command.trim();

  if (command.length() == 0) {
    return "Empty command";
  }

  // Reject binary/non-printable input — garbled bytes from a torn-down connection
  // can reach this path and cause an InstructionFetchError via a bad function lookup
  for (size_t i = 0; i < (size_t)command.length(); i++) {
    unsigned char c = (unsigned char)command[i];
    // Allow printable ASCII (0x20-0x7E) plus common whitespace (tab, CR, LF)
    if (c < 0x20 && c != '\t' && c != '\r' && c != '\n') {
      return "Error: command contains non-printable characters";
    }
    if (c > 0x7E) {
      return "Error: command contains non-ASCII characters";
    }
  }

  DEBUG_COMMAND_SYSTEMF("CommandSystem: Executing command '%s'", command.c_str());

  // Step 1: Resolve canonical command key once (case-insensitive, args preserved)
  String resolvedKey = resolveRegistryCommandKey(command);

  // Prepare original for argument slicing
  String originalForArgs = command;

  // Step 2: Split key vs args (do not alter dispatch yet)
  String resolvedArgs;
  size_t resolvedLen = 0;
  if (resolvedKey.length() > 0) {
    resolvedLen = resolvedKey.length();
    resolvedArgs = originalForArgs.substring(resolvedLen);
    resolvedArgs.trim();
  }

  // Step 3: Find handler by exact key and rebuild normalized command (single source of truth)
  const CommandEntry* found = nullptr;
  if (resolvedKey.length() > 0) {
    for (size_t i = 0; i < commandRegistrySize; ++i) {
      if (resolvedKey == String(commandRegistry[i]->name)) {
        // Use this entry (help navigation is now in cli_system module)
        found = commandRegistry[i];
        break;
      }
    }
  }

  if (found) {
    // Step 4: Rebuild command using canonical key + trailing args (arguments preserved)
    command = String(found->name);
    if (resolvedArgs.length() > 0) {
      command += " ";
      command += resolvedArgs;
    }

    // Handle help mode exit and command reprocessing
    if (gCLIState != CLI_NORMAL) {
      if (!isHelpModeCommand(found->name)) {
        // User typed a regular command while in help mode
        // Exit help first, then execute the command
        String exitBanner = exitToNormalBanner();
        broadcastOutput(exitBanner);
        const char* commandResult = found->handler(resolvedArgs);
        return String(commandResult);
      }
    }

    // Execute through registry handler - pass only args, not full command
    DEBUGF(DEBUG_CLI, "[registry_exec] executing: %s (args: %s)", command.c_str(), resolvedArgs.c_str());
    const char* result = found->handler(resolvedArgs);
    
    // Check if result indicates an error or usage issue
    // If so, append the stored usage string if available
    if (result && found->usage) {
      String resultStr = String(result);
      bool isError = resultStr.startsWith("Usage:") || 
                     resultStr.startsWith("Error:") || 
                     resultStr.startsWith("Invalid");
      
      if (isError) {
        // Handler returned an error - append stored usage if not already present
        if (resultStr.indexOf(found->usage) < 0) {
          resultStr += "\n\nDetailed usage:\n";
          resultStr += found->usage;
        }
        return resultStr;
      }
    }
    
    return String(result);
  } else {
    // Command not found in registry
    return "Unknown command: " + command + "\nType 'help' for available commands";
  }
}

// ============================================================================
// System Initialization
// ============================================================================

void initializeCommandSystem() {
  // Reset registry
  commandRegistrySize = 0;
  memset(commandRegistry, 0, sizeof(commandRegistry));

  // Clear tracked modules
  registeredModuleCount = 0;

  // Dynamically discover all command modules from centralized registry
  size_t moduleCount = 0;
  const CommandModule* modules = getCommandModules(moduleCount);

  DEBUG_COMMAND_SYSTEMF("[CMDREG] Total modules to process: %zu", moduleCount);

  for (size_t i = 0; i < moduleCount; ++i) {
    DEBUG_COMMAND_SYSTEMF("[CMDREG] Module[%zu] '%s': commands=%p count=%zu",
                          i, modules[i].name, modules[i].commands, modules[i].count);

    if (!modules[i].commands || modules[i].count == 0) {
      DEBUG_COMMAND_SYSTEMF("[CMDREG] SKIPPING module '%s' (commands=%p count=%zu)",
                          modules[i].name, modules[i].commands, modules[i].count);
      continue;
    }
    
    DEBUG_COMMAND_SYSTEMF("[CommandSystem] Registering module '%s' with %zu commands", modules[i].name, modules[i].count);
    
    registerCommands(modules[i].commands, modules[i].count);
    if (registeredModuleCount < MAX_MODULES) {
      registeredModules[registeredModuleCount].name = modules[i].name;
      registeredModules[registeredModuleCount].commands = modules[i].commands;
      registeredModules[registeredModuleCount].count = modules[i].count;
      registeredModuleCount++;
    }
  }

  DEBUG_COMMAND_SYSTEMF("[REG_INIT] Registry initialized with %d commands", commandRegistrySize);
  
  // Update global pointers
  gCommands = commandRegistry;
  gCommandsCount = commandRegistrySize;
}

// Print debug summary of registered modules
void printCommandModuleSummary() {
  DEBUG_COMMAND_SYSTEMF("[CommandSystem] %zu modules registered", registeredModuleCount);
  for (size_t i = 0; i < registeredModuleCount; i++) {
    DEBUG_COMMAND_SYSTEMF("[CommandSystem]   Module '%s': %zu commands", 
                  registeredModules[i].name, registeredModules[i].count);
  }
  DEBUG_COMMAND_SYSTEMF("[CommandSystem] Total: %zu commands available", commandRegistrySize);
  
  // Print all registered commands to debug
  DEBUG_COMMAND_SYSTEMF("[CommandSystem] All registered commands:");
  for (size_t i = 0; i < commandRegistrySize; i++) {
    DEBUG_COMMAND_SYSTEMF("[CommandSystem]   [%zu] '%s'", i, commandRegistry[i]->name);
  }
}

// ============================================================================
// Command Argument Parser Implementation
// ============================================================================

const String CommandArgs::empty_;

CommandArgs::CommandArgs(const String& input) : raw_(input) {
  raw_.trim();
  parse();
}

void CommandArgs::parse() {
  int len = raw_.length();
  int pos = 0;

  while (pos < len && argCount_ < MAX_ARGS) {
    // skip spaces
    while (pos < len && raw_[pos] == ' ') pos++;
    if (pos >= len) break;

    int start = pos;

    if (raw_[pos] == '"') {
      // quoted token — store content without quotes
      pos++;          // skip opening quote
      start = pos;
      while (pos < len && raw_[pos] != '"') pos++;
      args_[argCount_]    = raw_.substring(start, pos);
      offsets_[argCount_] = start - 1;  // offset points to the opening quote
      argCount_++;
      if (pos < len) pos++;  // skip closing quote
    } else {
      // unquoted token
      while (pos < len && raw_[pos] != ' ') pos++;
      args_[argCount_]    = raw_.substring(start, pos);
      offsets_[argCount_] = start;
      argCount_++;
    }
  }
}

const String& CommandArgs::arg(int index) const {
  if (index < 0 || index >= argCount_) return empty_;
  return args_[index];
}

int CommandArgs::argInt(int index, int defaultVal) const {
  if (!has(index)) return defaultVal;
  const String& a = args_[index];
  if (a.length() == 0) return defaultVal;
  if (a == "0") return 0;
  int v = a.toInt();
  return (v == 0) ? defaultVal : v;  // toInt returns 0 on failure
}

float CommandArgs::argFloat(int index, float defaultVal) const {
  if (!has(index)) return defaultVal;
  const String& a = args_[index];
  if (a.length() == 0) return defaultVal;
  // toFloat returns 0 on failure; distinguish from literal "0"
  if (a == "0" || a == "0.0" || a == "0.00") return 0.0f;
  float v = a.toFloat();
  return (v == 0.0f) ? defaultVal : v;
}

bool CommandArgs::argBool(int index, bool defaultVal) const {
  if (!has(index)) return defaultVal;
  // Delegate to the existing project-wide helper
  int r = parseBoolArg(args_[index]);
  if (r < 0) return defaultVal;
  return r == 1;
}

bool CommandArgs::argMac(int index, uint8_t mac[6]) const {
  if (!has(index)) return false;
  extern bool parseMacAddress(const String& macStr, uint8_t mac[6]);
  return parseMacAddress(args_[index], mac);
}

String CommandArgs::remaining(int afterIndex) const {
  if (afterIndex < -1) return raw_;
  if (afterIndex == -1) return raw_;

  // Find byte position right after the token at afterIndex
  if (afterIndex >= argCount_) return String();

  int pos = offsets_[afterIndex];
  // skip past the token itself
  if (raw_[pos] == '"') {
    pos++;  // opening quote
    while (pos < (int)raw_.length() && raw_[pos] != '"') pos++;
    if (pos < (int)raw_.length()) pos++;  // closing quote
  } else {
    while (pos < (int)raw_.length() && raw_[pos] != ' ') pos++;
  }
  // skip spaces between token and remainder
  while (pos < (int)raw_.length() && raw_[pos] == ' ') pos++;

  if (pos >= (int)raw_.length()) return String();
  return raw_.substring(pos);
}

String CommandArgs::value(const String& key) const {
  String needle = key + "=";
  int pos = raw_.indexOf(needle);
  if (pos < 0) return String();

  int start = pos + needle.length();
  if (start >= (int)raw_.length()) return String();

  // quoted value?
  if (raw_[start] == '"') {
    start++;
    int end = raw_.indexOf('"', start);
    if (end < 0) end = raw_.length();
    return raw_.substring(start, end);
  }

  // unquoted — find next space
  int end = raw_.indexOf(' ', start);
  if (end < 0) end = raw_.length();
  return raw_.substring(start, end);
}

bool CommandArgs::hasKey(const String& key) const {
  String needle = key + "=";
  return raw_.indexOf(needle) >= 0;
}

// ============================================================================
// CLI Settings Module
// ============================================================================

// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry cliSettingsEntries[] = {
  { "oledHistorySize", SETTING_INT, &gSettings.oledCliHistorySize, 50, 0, nullptr, 10, 100, "OLED History", nullptr, false, nullptr, nullptr }
};

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule cliSettingsModule = {
  "cli",
  "system.cli",
  cliSettingsEntries,
  sizeof(cliSettingsEntries) / sizeof(cliSettingsEntries[0]),
  nullptr,
  "CLI history and display settings"
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp
