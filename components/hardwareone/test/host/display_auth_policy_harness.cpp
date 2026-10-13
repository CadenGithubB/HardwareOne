// test_display_auth_policy.py inserts the production settings rows and command
// tables that hold the login policies (serial, UART link, local display), the
// production SettingEntry/CommandEntry types, and generated link fixtures: a
// gSettings built from the rows, one stub per table handler, and the list of
// handlers whose bodies write a policy. Only table metadata is compiled; no
// handler body runs here.
#include <Arduino.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>

// Every row and command that can exist on some board is part of the contract.
#define ENABLE_WIFI 1
#define ENABLE_ESPNOW 1
#define ENABLE_HTTP_SERVER 1
#define ENABLE_HTTPS 1
#define UART_LINK_PORT 1
#define UART_LINK_BAUD_MAX 921600
#include "System_CommandRegistry.h"

// INSERT_TYPES

// INSERT_SETTINGS

// INSERT_STUBS

// INSERT_TABLES

struct RowTable { const char* name; const SettingEntry* rows; size_t count; };
struct CommandTable { const char* name; const CommandEntry* rows; size_t count; };
struct Writer { const char* name; const char* (*handler)(const String&); };

// INSERT_LISTS

struct PolicyField { const char* name; const void* field; };
static const PolicyField kPolicies[] = {
  {"localDisplayRequireAuth", &gSettings.localDisplayRequireAuth},
  {"serialRequireAuth", &gSettings.serialRequireAuth},
  {"uartRequireAuth", &gSettings.uartRequireAuth},
};

static unsigned checks, failures;
static void check(bool good, const char* message) {
  ++checks;
  if (!good) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", message);
  }
}

static const PolicyField* policyOf(const void* field) {
  for (const auto& policy : kPolicies) if (policy.field == field) return &policy;
  return nullptr;
}

// Command names match case-insensitively, as the registry and
// findSettingByCmdKey() do. Every matching row counts, not only the first.
static unsigned countCommands(const char* name, unsigned* nonSuper) {
  unsigned found = 0;
  for (const auto& table : kCommandTables) {
    for (size_t i = 0; i < table.count; ++i) {
      if (strcasecmp(table.rows[i].name, name) != 0) continue;
      ++found;
      if (!table.rows[i].requiresSuperAdmin) ++*nonSuper;
      std::printf("  command %s.%s super=%d\n", table.name, table.rows[i].name,
                  (int)table.rows[i].requiresSuperAdmin);
    }
  }
  return found;
}

// A persisted row is edited through its cmdKey (OLED settings editor, web
// settings cards). A policy row must name a command that only super-admins run.
static void policyRowsUseSuperAdminCommands() {
  char message[256];
  for (const auto& table : kRowTables) {
    for (size_t i = 0; i < table.count; ++i) {
      const SettingEntry& row = table.rows[i];
      const PolicyField* policy = policyOf(row.valuePtr);
      if (!policy) continue;
      std::printf("row %s.%s (%s) cmdKey=%s\n", table.name, row.jsonKey, policy->name,
                  row.cmdKey ? row.cmdKey : "(none)");
      unsigned nonSuper = 0;
      const unsigned found = row.cmdKey ? countCommands(row.cmdKey, &nonSuper) : 0;
      std::snprintf(message, sizeof(message),
                    "policy row %s.%s writes %s through cmdKey '%s', which resolves to %u command(s), "
                    "%u without requiresSuperAdmin: a plain admin can change the login policy",
                    table.name, row.jsonKey, policy->name, row.cmdKey ? row.cmdKey : "(none)",
                    found, nonSuper);
      check(found > 0 && nonSuper == 0, message);
    }
  }
}

// One persisted row (one JSON key) per policy, so load order cannot decide it.
static void onePersistedRowPerPolicy() {
  char message[256];
  for (const auto& policy : kPolicies) {
    unsigned rows = 0;
    for (const auto& table : kRowTables) {
      for (size_t i = 0; i < table.count; ++i) {
        if (table.rows[i].valuePtr != policy.field) continue;
        ++rows;
      }
    }
    std::printf("rows persisting %s: %u\n", policy.name, rows);
    std::snprintf(message, sizeof(message),
                  "%s is persisted by %u settings rows (JSON keys); exactly one owner row is required",
                  policy.name, rows);
    check(rows == 1, message);
  }
}

// Every handler that writes a policy is registered, and only as super-admin.
static void policyWritersAreSuperAdmin() {
  char message[256];
  for (const Writer* writer = kPolicyWriters; writer->name; ++writer) {
    unsigned registrations = 0;
    for (const auto& table : kCommandTables) {
      for (size_t i = 0; i < table.count; ++i) {
        const CommandEntry& command = table.rows[i];
        if (command.handler != writer->handler) continue;
        ++registrations;
        std::printf("writer %s registered as %s.%s super=%d\n", writer->name, table.name,
                    command.name, (int)command.requiresSuperAdmin);
        std::snprintf(message, sizeof(message),
                      "command %s.%s runs policy writer %s without requiresSuperAdmin",
                      table.name, command.name, writer->name);
        check(command.requiresSuperAdmin, message);
      }
    }
    std::snprintf(message, sizeof(message),
                  "policy writer %s is not registered in any compiled command table", writer->name);
    check(registrations > 0, message);
  }
}

// An OLED row whose command is gone would leave a dead editor/web control.
static void oledRowsResolve() {
  char message[256];
  for (const auto& table : kRowTables) {
    if (std::strcmp(table.name, "oledSettingEntries") != 0) continue;
    for (size_t i = 0; i < table.count; ++i) {
      const SettingEntry& row = table.rows[i];
      if (!row.cmdKey) continue;
      unsigned found = 0;
      for (const auto& commands : kCommandTables) {
        for (size_t j = 0; j < commands.count; ++j) {
          if (strcasecmp(commands.rows[j].name, row.cmdKey) == 0) ++found;
        }
      }
      std::snprintf(message, sizeof(message), "OLED row %s names command '%s', which does not exist",
                    row.jsonKey, row.cmdKey);
      check(found > 0, message);
    }
  }
}

int main() {
  policyRowsUseSuperAdminCommands();
  onePersistedRowPerPolicy();
  policyWritersAreSuperAdmin();
  oledRowsResolve();
  std::printf("display auth policy: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
