// Runs the auth log front door as shipped: logAuthAttempt with its path test
// and authLogPath/appendAuthLogLine (System_Debug.cpp), on the real Arduino
// String with its heap calls counted. The capped file append and the time
// stamp are doubles. The runner pastes the production text at the marker.
#include <Arduino.h>
#include "string_heap_count.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static unsigned checks = 0;
static void checked(bool ok, const char* text, int line) {
  ++checks;
  if (ok) return;
  std::fprintf(stderr, "auth log paths check failed at line %d: %s\n", line, text);
  std::exit(1);
}
#define check(condition) checked((condition), #condition, __LINE__)

static const char* const LOG_OK_FILE = "/system/sys_logs/successful_login.log";
static const char* const LOG_FAIL_FILE = "/system/sys_logs/failed_login.log";
static constexpr size_t LOG_CAP_BYTES = 696969;

struct Written { std::string file, line; size_t cap; };
static std::vector<Written> written;
static bool appendLineWithCap(const char* path, const String& line, size_t capBytes) {
  written.push_back({path, line.c_str(), capBytes});
  return true;
}
static void getTimestampPrefixMsCached(char* out, size_t outSize) {
  std::snprintf(out, outSize, "%s", "[2026-11-15 10:00:00.000] | ");
}

// INSERT_PRODUCTION

struct Case { const char* path; bool success; const char* user; const char* ip; const char* reason;
              const char* file; const char* line; };

int main() {
  // The expected lines are those the function wrote before the path test
  // moved ahead of the copies (the old tree gives the same lines).
  static const char* const kOk = "/system/sys_logs/successful_login.log";
  static const char* const kFail = "/system/sys_logs/failed_login.log";
  static const Case kCases[] = {
    {"/login", true, "alice", "client", "",
     kOk, "[2026-11-15 10:00:00.000] | SUCCESS | user=alice | ip=client | /login"},
    {"web/login", false, "bob", "::FFFF:client", "Invalid credentials",
     kFail, "[2026-11-15 10:00:00.000] | FAILED | user=bob | ip=client | web/login | reason=Invalid credentials"},
    {"serial/login", true, "carol", "local", "cmd=login carol ***",
     kOk, "[2026-11-15 10:00:00.000] | SUCCESS | user=carol | ip=local | serial/login | reason=cmd=login carol ***"},
    {"g2/pair", true, "dave", "ble", "G2 glasses paired",
     kOk, "[2026-11-15 10:00:00.000] | SUCCESS | user=dave | ip=ble | g2/pair | reason=G2 glasses paired"},
    {"espnow/bond", false, "peer-name", "espnow", "Bond session active (role=master)",
     kFail, "[2026-11-15 10:00:00.000] | FAILED | user=peer-name | ip=espnow | espnow/bond | reason=Bond session active (role=master)"},
    // URL escapes are undone before the test and in the line.
    {"%2Flogin", true, "erin", "client", "",
     kOk, "[2026-11-15 10:00:00.000] | SUCCESS | user=erin | ip=client | /login"},
    {"/web%2Flogin", false, "frank", "client", "x",
     kFail, "[2026-11-15 10:00:00.000] | FAILED | user=frank | ip=client | /web/login | reason=x"},
    {"g2%2Fpair", true, "gina", "ble", "",
     kOk, "[2026-11-15 10:00:00.000] | SUCCESS | user=gina | ip=ble | g2/pair"},
    {"espnow%2Fbond", true, "hal", "espnow", "",
     kOk, "[2026-11-15 10:00:00.000] | SUCCESS | user=hal | ip=espnow | espnow/bond"},
    {"/my%20dir/login", true, "ivy", "client", "",
     kOk, "[2026-11-15 10:00:00.000] | SUCCESS | user=ivy | ip=client | /my dir/login"},
    // A lower-case escape is not undone; the path still ends in /login.
    {"/a%2fb/login", true, "jo", "client", "",
     kOk, "[2026-11-15 10:00:00.000] | SUCCESS | user=jo | ip=client | /a%2fb/login"},
    // Not security events: nothing is written.
    {"/api/sensors/status/with-updates", true, "alice", "client", "", nullptr, nullptr},
    {"/api/cli", false, "", "client", "unauthorized", nullptr, nullptr},
    {"/login-help", true, "alice", "client", "", nullptr, nullptr},
    {"/configure-login-page", true, "alice", "client", "", nullptr, nullptr},
    {"login", true, "alice", "client", "", nullptr, nullptr},
    {"/LOGIN", true, "alice", "client", "", nullptr, nullptr},
    {"/login/", true, "alice", "client", "", nullptr, nullptr},
    {"g2/pair/x", true, "alice", "ble", "", nullptr, nullptr},
    {"xg2/pair", true, "alice", "ble", "", nullptr, nullptr},
    {"espnow bond", true, "alice", "espnow", "", nullptr, nullptr},
    {"espnow%20bond", true, "alice", "espnow", "", nullptr, nullptr},
    {"/x/login%20", true, "alice", "client", "", nullptr, nullptr},
    {"%2F", true, "alice", "client", "", nullptr, nullptr},
    {"", true, "alice", "client", "", nullptr, nullptr},
    {nullptr, true, "alice", "client", "", nullptr, nullptr},
  };
  for (const Case& c : kCases) {
    written.clear();
    logAuthAttempt(c.success, c.path, String(c.user), String(c.ip), c.reason);
    if (!c.file) {
      if (!written.empty()) std::fprintf(stderr, "unexpected line for %s\n", c.path ? c.path : "(null)");
      check(written.empty());
      continue;
    }
    if (written.size() != 1 || written[0].line != c.line)
      std::fprintf(stderr, "path %s wrote %s\n", c.path, written.empty() ? "nothing" : written[0].line.c_str());
    check(written.size() == 1 && written[0].file == c.file && written[0].line == c.line &&
          written[0].cap == LOG_CAP_BYTES);
  }

  // The common call: a web request or a command whose path is not a
  // security event, with a reason of more than the inline capacity. Before,
  // the cleaned path copy and the reason were made and dropped every time.
  const String user("alice"), ip("client");
  char auditBuf[180];
  std::snprintf(auditBuf, sizeof(auditBuf), "cmd=%.170s", "sensors status all --verbose");
  gStringHeap = StringHeapCount{};
  written.clear();
  logAuthAttempt(true, "/api/sensors/status/with-updates", user, ip, "");
  logAuthAttempt(false, "serial/command/sensors-status", user, ip, auditBuf);
  check(written.empty());
  std::printf("no security event: %lu String blocks for two calls\n", gStringHeap.made);
  check(gStringHeap.made == 0);

  std::printf("auth log paths passed (%u checks)\n", checks);
  return 0;
}
