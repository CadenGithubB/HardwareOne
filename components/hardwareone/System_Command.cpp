/**
 * Command System Implementation
 * 
 * Centralized command registry and execution system
 * Moved from main .ino to improve modularity and organization
 */

#include <Arduino.h>
#include <ctype.h>
#include <esp_attr.h>
#include <string.h>

#include "System_CLI.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_Settings.h"
#include "System_Utils.h"

// Forward declaration for exitToNormalBanner function (defined in cli_system.cpp)
String exitToNormalBanner();

// ============================================================================
// Command Registry Storage
// ============================================================================

// Static storage for all commands (avoid dynamic allocation on embedded systems).
// MAX_COMMANDS lives in the header — bump it there if `gCommandRegistryDropped`
// fires (see initializeCommandSystem boot WARN).
EXT_RAM_BSS_ATTR static const CommandEntry* commandRegistry[MAX_COMMANDS];
static size_t commandRegistrySize = 0;

// Count of registerCommand() calls that were dropped because the registry was
// full. Bumped silently in the hot path (which runs during early boot before
// the debug queue is reliably up); a single summary line is emitted at the end
// of initializeCommandSystem so the failure mode is loud the next boot but the
// registration loop itself stays log-free.
//
// That summary uses logSystemEvent(), NOT a WARN macro. initializeCommandSystem
// runs at HardwareOne.cpp:1372 but initDebugSystem() not until :1476, so every
// DEBUGF/WARN variant here lands in debugQueuePrintf's `if (!getDebugQueue())
// return;` (System_Debug.cpp:777) and is discarded — the WARN_COMMANDF that used
// to be here could never have fired, and WARN_COMMANDF is additionally gated on
// getLogLevel() >= LOG_LEVEL_WARN. logSystemEvent echoes to Serial immediately,
// replays into the durable event log once the queue exists, and ignores both
// debug flags and log level.
static size_t gCommandRegistryDropped = 0;

// Same idea one table over. Registration itself is NOT affected by this cap —
// registerCommands() runs before the guard — but a truncated registeredModules[]
// makes printCommandModuleSummary list fewer modules than exist, while
// cmd_commandmodulesummary's header still reports the true count from
// getCommandModules(). A listing that disagrees with its own total is the most
// confusing shape this failure could take, so count the drops.
static size_t gModuleSummaryDropped = 0;

// Maximum number of command modules we can track for the debug summary.
// gCommandModules[] has 45 rows in source (28 compile in the committed FeatherS3
// config), so 64 is permanent headroom even with every feature turned on. Cost is
// 12 B per slot — three words — and it lands in PSRAM, not internal DRAM: the
// array below is EXT_RAM_BSS_ATTR and CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
// is set, so 32 -> 64 is +384 B of external RAM and zero DRAM (verified against
// the ELF: _ZL17registeredModules was 0x180 at 0x3c50d918). Overflow should now
// be unreachable, but it is still counted rather than dropped in silence.
#define MAX_MODULES 64

// Module tracking for debug summary
EXT_RAM_BSS_ATTR static ModuleInfo registeredModules[MAX_MODULES];
static size_t registeredModuleCount = 0;

// Global access pointers (extern declarations in header)
const CommandEntry** gCommands = nullptr;
size_t gCommandsCount = 0;

// ============================================================================
// Command Registration Functions
// ============================================================================

void registerCommand(const CommandEntry* command) {
  if (!command) return;
  if (commandRegistrySize >= MAX_COMMANDS) {
    // IMPORTANT: do not log here — this runs during early boot before the
    // debug queue is reliably up. Bump a counter; initializeCommandSystem
    // emits a single loud WARN at the end if it's non-zero.
    gCommandRegistryDropped++;
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
      if (lc.length() == entryLen ||
          isspace(static_cast<unsigned char>(lc.charAt(entryLen)))) {
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

  const String safeCommandForTrace = redactCmdForAudit(command);
  DEBUG_COMMAND_SYSTEMF("CommandSystem: Executing command '%s'", safeCommandForTrace.c_str());

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

    // Execute through registry handler - pass only args, not full command
    DEBUGF(DEBUG_CLI, "[registry_exec] executing: %s", found->name);
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
    return "Unknown command: " + redactCmdForAudit(command) +
           "\nType 'help' for available commands";
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
    } else {
      gModuleSummaryDropped++;
    }
  }

  DEBUG_COMMAND_SYSTEMF("[REG_INIT] Registry initialized with %d commands", commandRegistrySize);

  // If we hit either cap, surface it loudly — silent drops are how `g2scan`,
  // all of `even_r1`, and downstream modules vanished from the registry last
  // time. logSystemEvent rather than a WARN macro because the debug queue does
  // not exist this early in boot; see gCommandRegistryDropped above for why.
  if (gCommandRegistryDropped > 0) {
    logSystemEvent("CMDREG", "DROPPED %zu commands: registry full at MAX_COMMANDS=%d. "
                   "Trailing modules in gCommandModules are partially or fully missing — "
                   "bump MAX_COMMANDS in System_Command.h.",
                   gCommandRegistryDropped, (int)MAX_COMMANDS);
  }
  if (gModuleSummaryDropped > 0) {
    logSystemEvent("CMDREG", "DROPPED %zu modules from the summary table at MAX_MODULES=%d. "
                   "Their commands ARE registered and dispatch normally, but "
                   "printCommandModuleSummary lists fewer modules than commandmodulesummary "
                   "counts — bump MAX_MODULES in System_Command.cpp.",
                   gModuleSummaryDropped, (int)MAX_MODULES);
  }

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
  
  // Print all registered commands to debug. This is 500+ lines emitted in a
  // tight loop; without pacing it outruns the single debugOutputTask and the
  // tail overflows the 192-slot pool (observed: ~452 dropped lines at boot when
  // every debug flag is on). Apply backpressure every 16 lines so the pool
  // drains. No-op when DEBUG_COMMAND_SYSTEM is off (nothing is emitted).
  DEBUG_COMMAND_SYSTEMF("[CommandSystem] All registered commands:");
  for (size_t i = 0; i < commandRegistrySize; i++) {
    DEBUG_COMMAND_SYSTEMF("[CommandSystem]   [%zu] '%s'", i, commandRegistry[i]->name);
    if ((i & 0x0F) == 0x0F) debugQueueBackpressure();
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
    // Use the same ASCII-whitespace grammar as findCommand(). Keeping the
    // registry boundary and argument parser aligned prevents a tab-delimited
    // command from being authorized as one shape and executed as another.
    while (pos < len &&
           isspace(static_cast<unsigned char>(raw_[pos]))) pos++;
    if (pos >= len) break;

    int start = pos;

    if (raw_[pos] == '"') {
      // quoted token — store content without quotes
      pos++;          // skip opening quote
      start = pos;
      while (pos < len && raw_[pos] != '"') pos++;
      args_[argCount_]    = raw_.substring(start, pos);
      offsets_[argCount_] = start - 1;  // offset points to the opening quote
      quoted_[argCount_]  = true;
      if (pos < len) {
        pos++;  // skip closing quote
      } else {
        unterminatedQuote_ = true;  // ran off the end with no closing quote
      }
      argCount_++;
    } else {
      // unquoted token
      while (pos < len &&
             !isspace(static_cast<unsigned char>(raw_[pos]))) pos++;
      args_[argCount_]    = raw_.substring(start, pos);
      offsets_[argCount_] = start;
      quoted_[argCount_]  = false;
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
    while (pos < (int)raw_.length() &&
           !isspace(static_cast<unsigned char>(raw_[pos]))) pos++;
  }
  // skip whitespace between token and remainder
  while (pos < (int)raw_.length() &&
         isspace(static_cast<unsigned char>(raw_[pos]))) pos++;

  if (pos >= (int)raw_.length()) return String();
  return raw_.substring(pos);
}

String CommandArgs::value(const String& key) const {
  String needle = key + "=";
  int pos = raw_.indexOf(needle);
  if (pos < 0) return String();

  int start = pos + needle.length();
  if (start >= (int)raw_.length()) return String();

  // quoted value? Honor backslash-escaped quotes (\") — the web UI emits
  // them for nested JSON (secondarytriggers=), quoted text inside commands=,
  // and event match= patterns. The old plain indexOf('"') scan truncated all
  // of those at the first inner quote, which silently dropped every
  // web-created secondary trigger.
  if (raw_[start] == '"') {
    start++;
    String out;
    int i = start;
    while (i < (int)raw_.length()) {
      char c = raw_[i];
      if (c == '\\' && i + 1 < (int)raw_.length() && raw_[i + 1] == '"') {
        out += '"';  // unescape \" -> " ; other backslashes pass through untouched
        i += 2;
        continue;
      }
      if (c == '"') break;
      out += c;
      i++;
    }
    return out;
  }

  // unquoted — find the next command whitespace boundary
  int end = start;
  while (end < (int)raw_.length() &&
         !isspace(static_cast<unsigned char>(raw_[end]))) end++;
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
  { "oledHistorySize", SETTING_INT, &gSettings.oledCliHistorySize, 50, 0, nullptr, 10, 100, "OLED History", nullptr, false, nullptr, "oledclihistorysize" }
};

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule cliSettingsModule = {
  "cli",
  "system.cli",
  cliSettingsEntries,
  sizeof(cliSettingsEntries) / sizeof(cliSettingsEntries[0]),
  nullptr,
  "CLI history and display"
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp
