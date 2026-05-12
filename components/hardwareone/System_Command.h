#ifndef COMMAND_SYSTEM_H
#define COMMAND_SYSTEM_H

#include "System_Utils.h"
#include <Arduino.h>

// ============================================================================
// Command System - Centralized command registry and execution
// ============================================================================

// Maximum number of commands that can be registered.
// Cost: +4 B BSS per slot on 32-bit (one const-ptr per entry). At 1024 the
// table is 4 KB, vs ~30 KB of typical free heap — negligible. Was 512;
// bumped after the registry silently dropped `g2scan`, all of `even_r1`,
// and onward when the firmware crossed the old ceiling mid-`g2Commands`
// (see registerCommand below — `gCommandRegistryDropped` now surfaces a
// boot-time WARN if it ever happens again).
#define MAX_COMMANDS 1024

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

// ============================================================================
// Command Argument Parser
// ============================================================================
//
// Unified argument parser for command handlers. Replaces ad-hoc
// indexOf/substring chains with a consistent API.
//
// The parser only tokenizes — it never prints, logs, or broadcasts,
// so it is safe to use with sensitive args (passwords, credentials)
// before any redaction occurs.
//
// Usage:
//   CommandArgs a(argsInput);
//   if (!a.hasMinArgs(2)) return "Usage: cmd <arg1> <arg2>";
//   String first = a.arg(0);
//   int    num   = a.argInt(1, 42);       // default 42
//   bool   flag  = a.argBool(2, false);   // default false
//   String rest  = a.remaining(1);        // everything after arg 1
//
class CommandArgs {
public:
  explicit CommandArgs(const String& input);

  // Positional access (0-indexed)
  const String& arg(int index) const;     // empty string if out of range
  int            count()        const { return argCount_; }
  bool           has(int index) const { return index >= 0 && index < argCount_; }
  bool           hasMinArgs(int n) const { return argCount_ >= n; }

  // Typed extraction with defaults
  int   argInt(int index, int defaultVal = 0)     const;
  float argFloat(int index, float defaultVal = 0) const;
  bool  argBool(int index, bool defaultVal = false) const;  // on/off/true/false/1/0/enable/disable
  bool  argMac(int index, uint8_t mac[6])          const;

  // Everything after arg N, preserving original spacing.
  // remaining(0) = everything after arg 0 = args 1..N as raw text.
  // remaining(-1) = the full raw input.
  String remaining(int afterIndex) const;

  // Key=value access (searches raw string for key=...)
  String value(const String& key)  const;
  bool   hasKey(const String& key) const;

  // Raw input (trimmed)
  const String& raw() const { return raw_; }

private:
  static constexpr int MAX_ARGS = 10;

  String raw_;                   // original input, trimmed
  String args_[MAX_ARGS];       // parsed tokens
  int    offsets_[MAX_ARGS];    // byte offset of each token in raw_
  int    argCount_ = 0;

  static const String empty_;   // returned by arg() on out-of-range

  void parse();
};

#endif // COMMAND_SYSTEM_H
