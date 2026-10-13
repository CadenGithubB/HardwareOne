// Runs the production command registration (registerCommand(s) and
// initializeCommandSystem from System_Command.cpp) over fixture modules and
// checks the CMDREG report of a command name registered twice. The module
// catalogue and the system log are doubles.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

class String {};
#include "System_CommandRegistry.h"
#include "System_CommandLookupCore.h"

static unsigned checks = 0;
static void checked(bool ok, const char* text, int line) {
  ++checks;
  if (ok) return;
  std::fprintf(stderr, "command name uniqueness check failed at line %d: %s\n", line, text);
  std::exit(1);
}
#define check(condition) checked((condition), #condition, __LINE__)

#define EXT_RAM_BSS_ATTR
#define DEBUG_COMMAND_SYSTEMF(...) ((void)0)
// INSERT_COMMAND_HEADER

static std::vector<std::string> logLines;
static void logSystemEvent(const char* category, const char* format, ...) {
  char line[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  logLines.push_back(std::string(category) + ": " + line);
}

static const char* fixtureHandler(const String&) { return "ok"; }

static const CommandEntry systemRows[] = {
  {"status", "", false, fixtureHandler},
  {"pendinglist", "", true, fixtureHandler},
  {"cm5", "", false, fixtureHandler},
};
static const CommandEntry settingsRows[] = {
  {"serialrequireauth", "", true, fixtureHandler},
  {"cm5 status", "", false, fixtureHandler},
};
static const CommandEntry usersRows[] = {
  {"login", "", false, fixtureHandler},
  {"PendingList", "", true, fixtureHandler},
  {"serialrequireauth", "", true, fixtureHandler},
};
static const size_t systemCount = 3, settingsCount = 2, usersCount = 3, noneCount = 0;
static const CommandModule duplicateModules[] = {
  {"system", nullptr, nullptr, systemRows, &systemCount, 0, nullptr},
  {"empty", nullptr, nullptr, nullptr, &noneCount, 0, nullptr},
  {"settings", nullptr, nullptr, settingsRows, &settingsCount, 0, nullptr},
  {"users", nullptr, nullptr, usersRows, &usersCount, 0, nullptr},
};
static const CommandModule uniqueModules[] = {
  {"system", nullptr, nullptr, systemRows, &systemCount, 0, nullptr},
  {"settings", nullptr, nullptr, settingsRows, &settingsCount, 0, nullptr},
};
static const CommandModule* activeModules = duplicateModules;
static size_t activeCount = 4;
const CommandModule* getCommandModules(size_t& count) {
  count = activeCount;
  return activeModules;
}

// INSERT_REGISTRATION

static size_t duplicateLines() {
  size_t n = 0;
  for (const std::string& line : logLines) {
    if (line.rfind("CMDREG: ", 0) == 0 && line.find("DUPLICATE") != std::string::npos) ++n;
  }
  return n;
}

static bool logged(const char* a, const char* b, const char* c) {
  for (const std::string& line : logLines) {
    if (line.rfind("CMDREG: ", 0) == 0 && line.find(a) != std::string::npos &&
        line.find(b) != std::string::npos && line.find(c) != std::string::npos) return true;
  }
  return false;
}

static void duplicateNamesReported() {
  logLines.clear();
  activeModules = duplicateModules;
  activeCount = 4;
  initializeCommandSystem();
  // Registration is unchanged: every row is in the registry, in order.
  check(gCommandsCount == 8);
  check(gCommands[1] == &systemRows[1] && gCommands[6] == &usersRows[1]);
  // One line per shadowed row, naming the row and both modules; case is
  // ignored the way lookup ignores it, and a longer key is not a duplicate.
  check(duplicateLines() == 2);
  check(logged("'PendingList'", "'users'", "'system'"));
  check(logged("'serialrequireauth'", "'users'", "'settings'"));
  // Running it again reports again, once per row.
  initializeCommandSystem();
  check(duplicateLines() == 4);
}

static void uniqueNamesQuiet() {
  logLines.clear();
  activeModules = uniqueModules;
  activeCount = 2;
  initializeCommandSystem();
  check(gCommandsCount == 5 && duplicateLines() == 0 && logLines.empty());
}

struct Case { const char* name; void (*run)(); };
static const Case kCases[] = {
  {"duplicateNamesReported", duplicateNamesReported},
  {"uniqueNamesQuiet", uniqueNamesQuiet},
};

int main(int argc, char** argv) {
  unsigned ran = 0;
  for (const Case& c : kCases) {
    if (argc > 1 && std::strcmp(argv[1], c.name) != 0) continue;
    c.run();
    ++ran;
  }
  if (ran == 0) { std::fprintf(stderr, "unknown case: %s\n", argv[1]); return 2; }
  std::printf("command registration duplicates: %u checks passed in %u case(s)\n", checks, ran);
  return 0;
}
