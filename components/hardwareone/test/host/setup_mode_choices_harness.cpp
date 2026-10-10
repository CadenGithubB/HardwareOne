#include <Arduino.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
void webBatchTestStringAppend() {}
// INSERT_PRODUCTION
static unsigned checks;
static void checked(bool condition, int line) {
  ++checks;
  if (!condition) std::fprintf(stderr, "setup mode choices line %d\n", line);
  assert(condition);
}
#define check(condition) checked((condition), __LINE__)
int main() {
  constexpr bool hasImport = ENABLE_MIGRATION_TOOL && ENABLE_WIFI;
  check(getSetupModeCount() == (hasImport ? 3u : 2u));
  check(getSetupModeOption(0)->mode == SETUP_MODE_BASIC);
  check(getSetupModeOption(1)->mode == SETUP_MODE_ADVANCED);
  if (hasImport) check(getSetupModeOption(2)->mode == SETUP_MODE_IMPORT);
  for (size_t i = 0; i < getSetupModeCount(); ++i) {
    const auto* option = getSetupModeOption(i);
    check(option && option->label && option->label[0]);
    check(option->description && option->description[0]);
    char selection[8]; std::snprintf(selection, sizeof(selection), "%u", static_cast<unsigned>(i + 1));
    SetupMode chosen = SETUP_MODE_IMPORT;
    check(parseSetupModeInput(selection, chosen));
    check(chosen == option->mode); // Rendered menu position and accepted number agree.
  }
  check(getSetupModeOption(getSetupModeCount()) == nullptr);
  check(getSetupModeOption(std::numeric_limits<size_t>::max()) == nullptr);
  for (const char* text : {"", "   ", "1", " basic ", "BaSiC"}) {
    SetupMode chosen = SETUP_MODE_IMPORT;
    check(parseSetupModeInput(text, chosen) && chosen == SETUP_MODE_BASIC);
  }
  for (const char* text : {"2", " advanced ", "AdVaNcEd"}) {
    SetupMode chosen = SETUP_MODE_BASIC;
    check(parseSetupModeInput(text, chosen) && chosen == SETUP_MODE_ADVANCED);
  }
  for (const char* text : {"3", " import ", "ReStOrE", "restore", "IMPORT"}) {
    SetupMode chosen = SETUP_MODE_ADVANCED;
    check(parseSetupModeInput(text, chosen) == hasImport);
    check(chosen == (hasImport ? SETUP_MODE_IMPORT : SETUP_MODE_ADVANCED));
  }
  for (const char* text : {"0", "4", "-1", "01", "1junk", "2.0", "1 2", "+1", "restored", "basic advanced", "cancel"}) {
    SetupMode chosen = SETUP_MODE_ADVANCED;
    check(!parseSetupModeInput(text, chosen));
    check(chosen == SETUP_MODE_ADVANCED);
  }
  std::printf("setup mode choices (migration=%d wifi=%d http=%d): %u checks passed\n",
              ENABLE_MIGRATION_TOOL, ENABLE_WIFI, ENABLE_HTTP_SERVER, checks);
}
