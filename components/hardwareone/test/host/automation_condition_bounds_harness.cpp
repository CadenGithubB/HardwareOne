// Host harness for the THEN / ELSE scans of the automation command parser.
//
// test_automation_condition_bounds.py inserts the production
// validateConditionSyntax and executeConditionalCommand (System_Automation.cpp).
// Host boundaries: the Arduino String stub, the log macro, the audit redactor
// (identity), the condition evaluator (a settable answer) and the sub-command
// queue and output (recorded).
//
// Each short command sits at the start of a larger buffer, and the bytes after
// its terminator spell " THEN go ELSE no". A scan that runs past the
// terminator finds that text, so the fault shows without a sanitizer; with
// --sanitize the command is also placed in an exactly sized heap block.
#include <Arduino.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

void webBatchTestStringAppend() {}

#define DEBUG_AUTOMATIONSF(...) ((void)0)
#define EXT_RAM_BSS_ATTR
static String redactAutomationCommandForAudit(const String& command) { return command; }

static std::string gPrinted, gQueued, gEvaluated;
static unsigned gQueues = 0;
static bool gConditionMet = true;
static void broadcastOutput(const char* message) { gPrinted = message; }
static bool evaluateCondition(const char* condition) { gEvaluated = condition; return gConditionMet; }
static bool gQueueAccepts = true;
static bool queueAutomationSubCommand(const char* cmd, const char*, const char* = nullptr) {
  if (!gQueueAccepts) return false;
  gQueued = cmd;
  ++gQueues;
  return true;
}

// INSERT_PRODUCTION

static int failures = 0;
static int checks = 0;
static void check(bool ok, const std::string& what) {
  ++checks;
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

// `command`, its terminator, then text a scan must never reach.
struct Planted {
  char bytes[64];
  explicit Planted(const char* command) {
    std::memset(bytes, 0, sizeof(bytes));
    const size_t length = std::strlen(command);
    std::memcpy(bytes, command, length);
    std::memcpy(bytes + length + 1, " THEN go ELSE no", 17);
  }
};

static std::string run(const char* command) {
  gQueued.clear();
  gQueues = 0;
  return executeConditionalCommand(command, "owner", "name");
}

int main() {
  const char kMissingThen[] = "Error: Conditional command missing THEN";

  // ---- executeConditionalCommand ---------------------------------------------
  // "IF " plus 0 or 1 characters: the bound cmdLen - 5 wrapped and the scan ran
  // on past the terminator.
  for (const char* shortCommand : {"IF ", "IF x", "if y", "IF  "}) {
    Planted planted(shortCommand);
    const std::string reply = run(planted.bytes);
    check(reply == kMissingThen, std::string("'") + shortCommand + "' has no THEN: " + reply);
    check(gQueues == 0, std::string("'") + shortCommand + "' queues nothing");
  }
  // The same commands in a heap block of exactly their size (for --sanitize).
  for (const char* shortCommand : {"IF ", "IF x"}) {
    const size_t size = std::strlen(shortCommand) + 1;
    char* exact = static_cast<char*>(std::malloc(size));
    std::memcpy(exact, shortCommand, size);
    check(run(exact) == kMissingThen, std::string("'") + shortCommand + "' in an exact block has no THEN");
    std::free(exact);
  }
  // Lengths 5 to 8 never wrapped; they still have no THEN.
  for (const char* command : {"IF ab", "IF abc", "IF a=1 ", "IF THEN "}) {
    Planted planted(command);
    check(run(planted.bytes) == kMissingThen, std::string("'") + command + "' has no THEN");
  }

  // Well-formed commands behave as before.
  gConditionMet = true;
  check(run("IF TEMP > 5 THEN ledon ELSE ledoff") == "Conditional THEN queued" && gQueued == "ledon",
        "a true condition queues the THEN command");
  check(gEvaluated == "IF TEMP > 5 THEN dummy", "the condition between IF and THEN is evaluated");
  gConditionMet = false;
  check(run("IF TEMP > 5 THEN ledon ELSE ledoff") == "Conditional ELSE queued" && gQueued == "ledoff",
        "a false condition queues the ELSE command");
  check(run("if temp > 5 then ledon") == "Conditional command completed" && gQueues == 0,
        "a false condition without ELSE queues nothing");
  gConditionMet = true;
  check(run("IF A=1 THEN x") == "Conditional THEN queued" && gQueued == "x",
        "the shortest complete command: THEN is found at the end of the scan range");
  check(run("IF A THEN ") == "Conditional command completed" && gQueues == 0,
        "THEN followed by nothing is found and has no command");
  check(run("IF A=1 THEN on ELSE of") == "Conditional THEN queued" && gQueued == "on",
        "ELSE near the end of the command is found");
  gConditionMet = false;
  check(run("IF A=1 THEN on ELSE of") == "Conditional ELSE queued" && gQueued == "of",
        "and its command is queued when the condition is false");
  check(run("ledon") == "Command queued" && gQueued == "ledon", "a plain command is queued");
  check(run("PRINT hello") == "Message printed" && gPrinted == "hello", "PRINT prints");

  // A step the executor did not take is a failure, for a plain command and for
  // either branch, and its text is not mistaken for an accepted step.
  const char kNotSubmitted[] =
      "Error: Command not submitted (executor busy or command too long). Run the automation again.";
  gQueueAccepts = false;
  check(run("ledon") == kNotSubmitted && gQueues == 0, "a plain command that is not submitted is an error");
  gConditionMet = true;
  check(run("IF A=1 THEN on ELSE of") == kNotSubmitted, "a THEN command that is not submitted is an error");
  gConditionMet = false;
  check(run("IF A=1 THEN on ELSE of") == kNotSubmitted, "an ELSE command that is not submitted is an error");
  check(std::strstr(kNotSubmitted, "queued") == nullptr, "the error does not read as an accepted step");
  gQueueAccepts = true;

  // ---- validateConditionSyntax ------------------------------------------------
  for (const char* shortCondition : {"IF ", "IF x", "  IF y", "IF  "}) {
    Planted planted(shortCondition);
    const std::string reply = validateConditionSyntax(planted.bytes);
    check(reply == "Condition must contain 'THEN'", std::string("validate '") + shortCondition + "': " + reply);
  }
  check(std::string(validateConditionSyntax("IF TEMP > 5 THEN ledon")).empty(), "a complete condition is valid");
  check(std::string(validateConditionSyntax("IF A=1 THEN x")).empty(), "the shortest complete condition is valid");
  check(std::string(validateConditionSyntax("IF TEMP 5 THEN ledon")) ==
            "Condition must contain an operator (>, <, =, >=, <=, !=, CONTAINS)",
        "a condition without an operator is refused");
  check(std::string(validateConditionSyntax("IF A=1 THEN ")) == "Missing command after 'THEN'",
        "THEN without a command is refused");
  check(std::string(validateConditionSyntax("ledon")) == "Condition must start with 'IF'",
        "a plain command is not a condition");

  if (failures != 0) {
    std::fprintf(stderr, "%d of %d condition bound checks failed\n", failures, checks);
    return 1;
  }
  std::printf("automation condition bounds: %d checks passed\n", checks);
  return 0;
}
