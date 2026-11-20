#!/usr/bin/env python3
"""A web page's own status polls leave no audit line; everything else still does.

The ESP-NOW page poll runs nine read-only status commands and the Logging
page poll two, through executeInternalStatusCommand (System_Utils.cpp), as the
internal system identity. Each used to write a command-audit.log line and a
[CMD] echo, so an open page filled the audit log with commands nobody ran.

The mark is one bit on the execution's own context
(COMMAND_CONTEXT_INTERNAL_STATUS_POLL, System_CommandTypes.h), set by
executeInternalStatusCommand and read by commandExecutionLogSuppressed.

Part 1 compiles the shipping executeInternalStatusCommand, the suppression
check, both audit entry points and the real CommandContextBehavior enum, with
a double of the executor that copies the context, installs it as the running
command's context and logs the outcome the way executeCommand does:

  - executeInternalStatusCommand("espnowstatus json", ...) writes no audit
    line and no echo, whether the command succeeded or failed;
  - the same command from a user's web session writes both;
  - an automation's command and a trusted system command (same origin,
    transport and user as the poll, without the bit) write both;
  - a user command the executor runs between two polls is audited: the mark
    travels with each execution, it is not a state of the caller;
  - the direct entry (logCommandExecution: confirmations, web login) follows
    the same rule, and with no running command it always writes;
  - validate-only runs and the quiet poll verbs stay suppressed as before.

Part 2 reads the sources: the bit is its own bit; the helper sets it; the
suppression reads it from the running context; and every caller passes a
fixed command from the list below. That list is the review point: a command
added to it runs unaudited, so it must be a read-only status command.

--command-types <file> runs the test against another copy of
System_CommandTypes.h.

Not covered: the real executor queue, the audit file itself (rotation and
caps are test_capped_log_writes.py), or the echo's routing on a device.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

FLAG = "COMMAND_CONTEXT_INTERNAL_STATUS_POLL"

# Every command a page handler may run through executeInternalStatusCommand.
# These leave no audit line: add a command only if it reads state and changes
# nothing, and rename an entry here when the command is renamed.
READ_ONLY_POLLS = {
    "WebServer_Server.cpp": ["time json", "sensorlog status json"],
    "WebPage_ESPNow.cpp": ["espnowstatus json", "espnowmode json", "bondstatus json", "espnowlist",
                           "espnowmeshes listjson", "espnowdeviceinfo json", "espnowmeshrole json",
                           "espnowmeshstatus", "espnowstatus"],
}

HARNESS = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

class String {
  std::string text_;
 public:
  String() = default;
  String(const char* s) : text_(s ? s : "") {}
  String& operator=(const char* s) { text_ = s ? s : ""; return *this; }
  const char* c_str() const { return text_.c_str(); }
};

enum CommandSource { SOURCE_WEB = 0, SOURCE_SERIAL = 1, SOURCE_INTERNAL = 2 };
enum CommandOrigin { ORIGIN_SERIAL, ORIGIN_WEB, ORIGIN_AUTOMATION, ORIGIN_SYSTEM };
struct AuthContext {
  CommandSource transport = SOURCE_WEB;
  String user, ip, path, sid;
};
// INSERT_BEHAVIOR_ENUM
struct CommandContext {
  CommandOrigin origin = ORIGIN_WEB;
  AuthContext auth;
  uint32_t id = 0;
  uint32_t timestampMs = 0;
  uint32_t outputMask = 0;
  uint8_t behaviorFlags = COMMAND_CONTEXT_DEFAULT;
  bool validateOnly = false;
};
struct Command { String line; CommandContext ctx; };
struct CommandResolution {};

static constexpr const char* kSystemUser = "system";
static constexpr uint8_t MSG_ROUTE_FILE = 0x04, MSG_ROUTE_BLE = 0x10, MSG_ROUTE_ALL = 0x3F;
static uint32_t gNow = 5000;
static uint32_t millis() { return gNow; }
namespace SelfDevice { static unsigned long uptimeSeconds() { return 77; } }
static const char* commandSourceName(CommandSource s) {
  return s == SOURCE_WEB ? "web" : s == SOURCE_SERIAL ? "serial" : "internal";
}
static CommandResolution resolveCommand(const String&) { return {}; }
static String redactCmdForAudit(const String& command, const CommandResolution&) { return command; }

struct Written { std::string path, line; size_t cap; };
static std::vector<Written> audit;
static std::vector<std::pair<std::string, uint8_t>> echoes;
static bool appendLineWithCap(const char* path, const char* line, size_t length, size_t cap) {
  audit.push_back({path, std::string(line, length), cap});
  return true;
}
static void broadcastOutputCore_Routed(const char* text, size_t length, uint8_t route) {
  echoes.push_back({std::string(text, length), route});
}

// The running command's context, as cmd_exec installs it (System_AuthIdentity.cpp).
static void* gCurrentContext = nullptr;
static void* currentCommandContext() { return gCurrentContext; }
bool currentCommandValidateOnly();
bool submitAndExecuteSync(const Command& cmd, String& out);

// INSERT_PRODUCTION

// Executor double. Like cmd_exec it runs the request's own copy of the
// context (ExecReq::ctx), installs it for the duration of the command and,
// like executeCommand, logs the outcome through logCommandExecutionPrepared.
struct Script {
  bool ok = true;
  const char* result = "{\"schema\":1}";
  // A command another producer queued; cmd_exec runs it right after this one.
  const Command* queuedBehind = nullptr;
};
static Script script;
static std::vector<uint8_t> flagsSeen;
static void runOnExecutor(const Command& cmd, bool ok, const char* result) {
  CommandContext running = cmd.ctx;
  void* previous = gCurrentContext;
  gCurrentContext = &running;
  flagsSeen.push_back(running.behaviorFlags);
  logCommandExecutionPrepared(running.auth, cmd.line.c_str(), String(cmd.line.c_str()), ok, result);
  gCurrentContext = previous;
}
bool submitAndExecuteSync(const Command& cmd, String& out) {
  runOnExecutor(cmd, script.ok, script.result);
  out = script.result;
  if (script.queuedBehind) {
    const Command* next = script.queuedBehind;
    script.queuedBehind = nullptr;
    runOnExecutor(*next, true, "done");
  }
  return script.ok;
}

static Command userCommand(const char* line) {
  Command c;
  c.line = line;
  c.ctx.origin = ORIGIN_WEB;
  c.ctx.auth.transport = SOURCE_WEB;
  c.ctx.auth.user = "alice";
  c.ctx.auth.sid = "session";
  c.ctx.behaviorFlags |= COMMAND_CONTEXT_REQUIRE_LIVE_SESSION;
  return c;
}
static void clear() { audit.clear(); echoes.clear(); flagsSeen.clear(); script = Script{}; }

int main() {
  // The bit is its own: it shares nothing with the three older flags.
  static_assert((COMMAND_CONTEXT_INTERNAL_STATUS_POLL &
                 (COMMAND_CONTEXT_MODE_INDEPENDENT | COMMAND_CONTEXT_REQUIRE_LIVE_SESSION |
                  COMMAND_CONTEXT_REQUIRE_G2_EVENAI_AUTHORITY)) == 0, "a bit of its own");
  static_assert(COMMAND_CONTEXT_INTERNAL_STATUS_POLL != 0 &&
                (COMMAND_CONTEXT_INTERNAL_STATUS_POLL &
                 (COMMAND_CONTEXT_INTERNAL_STATUS_POLL - 1)) == 0, "exactly one bit");
  static_assert(COMMAND_CONTEXT_INTERNAL_STATUS_POLL <= 0x80, "fits behaviorFlags (uint8_t)");

  char out[64];

  // A page poll: the reply is copied out, nothing is audited or echoed.
  clear();
  assert(executeInternalStatusCommand("espnowstatus json", out, sizeof(out)));
  assert(std::strcmp(out, "{\"schema\":1}") == 0);
  assert(audit.empty() && echoes.empty());
  assert(flagsSeen.size() == 1 && (flagsSeen[0] & COMMAND_CONTEXT_INTERNAL_STATUS_POLL));
  assert(flagsSeen[0] & COMMAND_CONTEXT_MODE_INDEPENDENT);

  // A poll that fails is not audited either.
  clear();
  script.ok = false;
  script.result = "Error: ESP-NOW is off";
  assert(!executeInternalStatusCommand("espnowstatus json", out, sizeof(out)));
  assert(std::strcmp(out, "Error: ESP-NOW is off") == 0);
  assert(audit.empty() && echoes.empty());

  // The same command from a user's web session: one audit line, one echo.
  clear();
  String reply;
  assert(submitAndExecuteSync(userCommand("espnowstatus json"), reply));
  assert(audit.size() == 1 && echoes.size() == 1);
  assert(audit[0].path == "/system/sys_logs/command-audit.log");
  assert(audit[0].line == "[77] alice@web espnowstatus json -> OK");
  assert(audit[0].cap == 500u * 1024u);
  assert(echoes[0].first == "[CMD] alice@web: espnowstatus json -> OK");
  assert(echoes[0].second == (MSG_ROUTE_ALL & ~MSG_ROUTE_BLE));

  // An automation's command: audited.
  clear();
  Command automation;
  automation.line = "espnowstatus json";
  automation.ctx.origin = ORIGIN_AUTOMATION;
  automation.ctx.auth.transport = SOURCE_INTERNAL;
  automation.ctx.auth.user = "alice";
  automation.ctx.behaviorFlags |= COMMAND_CONTEXT_MODE_INDEPENDENT;
  assert(submitAndExecuteSync(automation, reply));
  assert(audit.size() == 1 && audit[0].line == "[77] alice@internal espnowstatus json -> OK");
  assert(echoes.size() == 1);

  // A trusted system command (runUnifiedSystemCommand): the poll's origin,
  // transport and user, without the bit. Audited: only the bit decides.
  clear();
  Command systemCommand;
  systemCommand.line = "espnowstatus json";
  systemCommand.ctx.origin = ORIGIN_SYSTEM;
  systemCommand.ctx.auth.transport = SOURCE_INTERNAL;
  systemCommand.ctx.auth.user = kSystemUser;
  assert(submitAndExecuteSync(systemCommand, reply));
  assert(audit.size() == 1 && audit[0].line == "[77] system@internal espnowstatus json -> OK");
  assert(echoes.size() == 1);

  // The executor runs a user's command right behind a poll, while the page
  // handler is still in its loop of polls: the user's command is audited and
  // the polls are not.
  clear();
  const Command queued = userCommand("espnow off");
  script.queuedBehind = &queued;
  assert(executeInternalStatusCommand("espnowstatus json", out, sizeof(out)));
  assert(executeInternalStatusCommand("espnowmode json", out, sizeof(out)));
  assert(flagsSeen.size() == 3);
  assert(audit.size() == 1 && audit[0].line == "[77] alice@web espnow off -> OK");
  assert(echoes.size() == 1 && echoes[0].first == "[CMD] alice@web: espnow off -> OK");
  assert(gCurrentContext == nullptr);

  // The direct entry (confirmations, web login) follows the running command.
  clear();
  AuthContext who;
  who.user = "bob";
  logCommandExecution(who, "login bob", true, "Login successful");  // no running command
  assert(audit.size() == 1 && audit[0].line == "[77] bob@web login bob -> OK");
  CommandContext poll;
  poll.behaviorFlags = COMMAND_CONTEXT_INTERNAL_STATUS_POLL;
  gCurrentContext = &poll;
  logCommandExecution(who, "espnowstatus json", true, "ok");
  gCurrentContext = nullptr;
  assert(audit.size() == 1 && echoes.size() == 1);

  // Unchanged: validate-only runs and the quiet poll verbs.
  clear();
  Command dryRun = userCommand("reboot");
  dryRun.ctx.validateOnly = true;
  assert(submitAndExecuteSync(dryRun, reply));
  assert(submitAndExecuteSync(userCommand("g2status json"), reply));
  assert(submitAndExecuteSync(userCommand("transcription status"), reply));
  assert(audit.empty() && echoes.empty());
  assert(submitAndExecuteSync(userCommand("g2statusx"), reply));
  assert(audit.size() == 1);

  // Argument checks of the helper.
  clear();
  assert(!executeInternalStatusCommand(nullptr, out, sizeof(out)));
  assert(!executeInternalStatusCommand("time json", nullptr, 0));
  assert(flagsSeen.empty());

  std::puts("internal status polls: no audit line and no echo; user, automation "
            "and system commands still audited");
  return 0;
}
'''


def source_contracts(utils: str, types: str) -> None:
    enum = extract_block(types, "enum CommandContextBehavior : uint8_t")
    require(re.search(rf"\b{FLAG}\s*=\s*1u << 3,", enum) is not None,
            f"System_CommandTypes.h: enum CommandContextBehavior needs the line "
            f"'{FLAG} = 1u << 3,' after COMMAND_CONTEXT_REQUIRE_G2_EVENAI_AUTHORITY "
            f"(integration line of the 2026-11-20 behaviour batch, group web-server-and-reports)")
    bits = re.findall(r"=\s*1u << (\d+),", enum)
    require(len(bits) == len(set(bits)), f"each behaviour flag has its own bit: {bits}")

    helper = extract_block(utils, "bool executeInternalStatusCommand(")
    require(f"uc.ctx.behaviorFlags |= {FLAG};" in helper, "the helper marks its execution")
    require(utils.count(f"|= {FLAG}") == 1, "only executeInternalStatusCommand sets the mark")

    reader = extract_block(utils, "static bool currentCommandIsInternalStatusPoll()")
    require("currentCommandContext()" in reader and FLAG in reader,
            "the mark is read from the running command's context")
    suppressed = extract_block(utils, "static bool commandExecutionLogSuppressed(")
    require("currentCommandIsInternalStatusPoll()" in suppressed, "the suppression reads the mark")
    require("isQuietPollCommand(cmd)" in suppressed and "currentCommandValidateOnly()" in suppressed,
            "the older two rules stay")
    for entry in ("static void logCommandExecutionPrepared(", "void logCommandExecution("):
        require("commandExecutionLogSuppressed(" in extract_block(utils, entry),
                f"{entry} asks the one suppression check")
    # The verb list did not grow to cover the page polls.
    quiet = extract_block(utils, "static bool isQuietPollCommand(")
    for commands in READ_ONLY_POLLS.values():
        for command in commands:
            require(f'"{command.split()[0]}"' not in quiet,
                    f"isQuietPollCommand must not list the page poll '{command}'")

    # Every caller passes a fixed command from READ_ONLY_POLLS.
    callers = {}
    for path in sorted(COMPONENT.glob("*.cpp")) + sorted(COMPONENT.glob("*.h")):
        text = path.read_text(errors="replace")
        code = re.sub(r"//[^\n]*", "", text)
        calls = re.findall(r"executeInternalStatusCommand\(([^,]*),", code)
        if path.name in ("System_Utils.cpp", "System_Utils.h"):
            calls = [c for c in calls if not c.strip().startswith("const char*")]
        if calls:
            callers[path.name] = (calls, text)
    require(set(callers) == set(READ_ONLY_POLLS),
            f"callers of executeInternalStatusCommand: {sorted(callers)} (expected "
            f"{sorted(READ_ONLY_POLLS)}; a new caller's commands run unaudited: review them and "
            f"add them to READ_ONLY_POLLS)")
    for name, (calls, text) in callers.items():
        found = []
        for argument in calls:
            argument = argument.strip()
            literal = re.fullmatch(r'"([^"\\]*)"', argument)
            if literal:
                found.append(literal.group(1))
                continue
            table = re.fullmatch(r"(\w+)\[\w+\]", argument)
            require(table is not None, f"{name}: executeInternalStatusCommand({argument}, ...) "
                                       f"is not a fixed command")
            body = re.search(rf"static const char\* {table.group(1)}\[\] = \{{(.*?)\}};", text, re.S)
            require(body is not None, f"{name}: {table.group(1)} is not a fixed table of literals")
            items = [item.strip() for item in re.sub(r"//[^\n]*", "", body.group(1)).split(",")
                     if item.strip()]
            for item in items:
                require(re.fullmatch(r'"[^"\\]*"', item) is not None,
                        f"{name}: {table.group(1)} holds something that is not a literal: {item}")
            found.extend(item.strip('"') for item in items)
        require(found == READ_ONLY_POLLS[name],
                f"{name}: internal polls are {found}; READ_ONLY_POLLS says {READ_ONLY_POLLS[name]}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--command-types", type=Path, default=COMPONENT / "System_CommandTypes.h")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    utils = (COMPONENT / "System_Utils.cpp").read_text()
    types = args.command_types.read_text()
    source_contracts(utils, types)

    production = "\n\n".join(extract_block(utils, marker) for marker in (
        "static bool isPassiveTranscriptionCommand(", "static bool isQuietPollCommand(",
        "static bool currentCommandIsInternalStatusPoll()",
        "static bool commandExecutionLogSuppressed(", "static void logCommandExecutionPrepared(",
        "void logCommandExecution(", "bool currentCommandValidateOnly()",
        "bool executeInternalStatusCommand("))
    harness = HARNESS.replace("// INSERT_BEHAVIOR_ENUM",
                              extract_block(types, "enum CommandContextBehavior : uint8_t") + ";")
    harness = harness.replace("// INSERT_PRODUCTION", production)
    with tempfile.TemporaryDirectory(prefix="hw1-poll-audit-") as directory:
        source = Path(directory) / "test.cpp"
        binary = Path(directory) / "test"
        source.write_text(harness)
        subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                        "-Wno-unused-function", "-Wno-unused-const-variable",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        str(source), "-o", str(binary)], check=True, timeout=300)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
