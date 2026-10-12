// Executes the shipping log-level owner (applyLogLevel), its CLI caller
// (cmd_loglevel), the boot clamp block from applySettings() and the real
// setDebugSetting template. DebugManager, esp_log_level_set and the debug
// settings persistence request are recorded boundaries, so every check is about
// which calls happen and in which order.
#include "Arduino.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int checks = 0;
#define CHECK(condition) do { \
  if (!(condition)) { std::fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #condition); std::abort(); } \
  ++checks; } while (0)

static std::vector<std::string> events;
typedef enum { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE } esp_log_level_t;
static const char* const kEspNames[] = {"NONE", "ERROR", "WARN", "INFO", "DEBUG", "VERBOSE"};
void esp_log_level_set(const char* tag, esp_log_level_t level) {
  events.push_back(std::string("esp:") + tag + "=" + kEspNames[level]);
}
struct DebugManager {
  static DebugManager& getInstance() { static DebugManager instance; return instance; }
  void setLogLevel(uint8_t level) { events.push_back("manager=" + std::to_string(level)); }
};
// INSERT_MACROS

struct HostSettings { int logLevel = 2; } gSettings;
bool requestDebugSettingsPersist() { events.push_back("persist"); return true; }
// INSERT_SET_DEBUG_SETTING

bool gCLIValidateOnly = false;
#define RETURN_VALID_IF_VALIDATE_CSTR() do { if (::gCLIValidateOnly) return "VALID"; } while (0)
static bool bufferAvailable = true;
static char* gDebugBuffer = nullptr;
static bool ensureDebugBuffer() {
  static char storage[1024];
  if (!bufferAvailable) return false;
  gDebugBuffer = storage;
  return true;
}

void applyLogLevel(uint8_t level);
// INSERT_PRODUCTION

static void bootApplyLogLevel() {
// INSERT_BOOT_BLOCK
}

static std::vector<std::string> applied(int manager, const char* esp) {
  const std::string level = esp;
  return {"manager=" + std::to_string(manager), "esp:wifi=" + level, "esp:wifi_init=" + level,
          "esp:phy_init=" + level};
}
static std::vector<std::string> withPersist(std::vector<std::string> calls) {
  calls.insert(calls.begin(), "persist");
  return calls;
}
static std::string run(const char* args) {
  events.clear();
  return cmd_loglevel(String(args));
}

int main() {
  // Owner mapping: ERROR/WARN/INFO map one-to-one, everything else is DEBUG;
  // exactly the Wi-Fi and PHY tags follow it (TLS/HTTP tags have their own owner).
  const struct { int level; const char* esp; } mapping[] = {
    {LOG_LEVEL_ERROR, "ERROR"}, {LOG_LEVEL_WARN, "WARN"}, {LOG_LEVEL_INFO, "INFO"},
    {LOG_LEVEL_DEBUG, "DEBUG"}, {4, "DEBUG"}, {255, "DEBUG"}};
  for (const auto& row : mapping) {
    events.clear();
    applyLogLevel(static_cast<uint8_t>(row.level));
    CHECK(events == applied(row.level, row.esp));
  }

  // Bare command reports the clamped saved level and changes nothing.
  const struct { int saved; const char* text; } reports[] = {
    {2, "Current log level: info (2) (0=error, 1=warn, 2=info, 3=debug)"},
    {-5, "Current log level: error (0) (0=error, 1=warn, 2=info, 3=debug)"},
    {9, "Current log level: debug (3) (0=error, 1=warn, 2=info, 3=debug)"},
    {1, "Current log level: warn (1) (0=error, 1=warn, 2=info, 3=debug)"}};
  for (const auto& row : reports) {
    gSettings.logLevel = row.saved;
    CHECK(std::string(run("   ")) == row.text);
    CHECK(events.empty() && gSettings.logLevel == row.saved);
  }

  // Every accepted spelling, case and surrounding space; the saved value is
  // requested first, then the owner applies it.
  const struct { const char* arg; int level; const char* esp; const char* name; } sets[] = {
    {"error", 0, "ERROR", "error"}, {"E", 0, "ERROR", "error"}, {"0", 0, "ERROR", "error"},
    {"warn", 1, "WARN", "warn"}, {"Warning", 1, "WARN", "warn"}, {"w", 1, "WARN", "warn"},
    {"1", 1, "WARN", "warn"}, {"info", 2, "INFO", "info"}, {" I ", 2, "INFO", "info"},
    {"2", 2, "INFO", "info"}, {"debug", 3, "DEBUG", "debug"}, {"\tDeBuG \r\n", 3, "DEBUG", "debug"},
    {"d", 3, "DEBUG", "debug"}, {"3", 3, "DEBUG", "debug"}};
  for (const auto& row : sets) {
    gSettings.logLevel = row.level == 0 ? 2 : 0;  // always a change
    const std::string reply = run(row.arg);
    CHECK(reply == std::string("Log level set to: ") + row.name + " (" + std::to_string(row.level) + ") and saved");
    CHECK(gSettings.logLevel == row.level);
    CHECK(events == withPersist(applied(row.level, row.esp)));
  }

  // Unchanged value: no persistence request, but the owner still re-applies it.
  gSettings.logLevel = 3;
  CHECK(std::string(run("debug")) == "Log level set to: debug (3) and saved");
  CHECK(events == applied(3, "DEBUG"));

  // Refusals change nothing.
  for (const char* bad : {"verbose", "4", "-1", "warn now", "0x1"}) {
    gSettings.logLevel = 2;
    CHECK(std::string(run(bad)) == "Error: Invalid level. Use: error(0), warn(1), info(2), or debug(3)");
    CHECK(events.empty() && gSettings.logLevel == 2);
  }
  bufferAvailable = false;
  CHECK(std::string(run("debug")) == "Error: Debug buffer unavailable");
  CHECK(events.empty() && gSettings.logLevel == 2);
  bufferAvailable = true;
  gCLIValidateOnly = true;
  CHECK(std::string(run("debug")) == "VALID");
  CHECK(events.empty() && gSettings.logLevel == 2);
  gCLIValidateOnly = false;

  // Boot: the saved level is clamped into range, stored back without a
  // persistence request, and applied through the same owner.
  const struct { int saved; int level; const char* esp; } boots[] = {
    {-3, 0, "ERROR"}, {0, 0, "ERROR"}, {1, 1, "WARN"}, {2, 2, "INFO"}, {3, 3, "DEBUG"}, {7, 3, "DEBUG"}};
  for (const auto& row : boots) {
    gSettings.logLevel = row.saved;
    events.clear();
    bootApplyLogLevel();
    CHECK(gSettings.logLevel == row.level);
    CHECK(events == applied(row.level, row.esp));
  }

  std::fprintf(stderr, "checks=%d\n", checks);
  std::puts("LOG_LEVEL_APPLICATION PASS");
  return 0;
}
