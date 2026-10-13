// Drives the production account-authority code from System_User.cpp and
// HardwareOne.cpp against the shared fake VFS with real ArduinoJson: the
// admin/super predicates and their bond-admin branch, hasAdminPrivilege /
// hasSuperAdminPrivilege, userAccountRank and the mutation rank rule, the
// credential and id lookups, the ban/unban/sessionrevoke handlers, the
// targeted-logout owner, the userlist/pendinglist/sessionlist JSON replies and
// the read-only roster snapshot. The runner compiles it twice, with
// ENABLE_HTTP_SERVER 0 and 1 (HW1_TEST_HTTP). The ban writer, session fan-out,
// web session tables, password hashing and per-user settings loader are
// doubles that record their calls.
#define FAKE_VFS_PROVIDE_VFS_NAMESPACE
#define FAKE_VFS_LOCK_DOUBLE
#define FAKE_VFS_EXTERNAL_AUTH_CONTEXT
#include <Arduino.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <strings.h>
#include <vector>

// INSERT_AUTH_TYPES

#include "fake_vfs.h"
#include <ArduinoJson.h>
#include "System_PollPause.h"

void webBatchTestStringAppend() {}
volatile bool gSensorPollingPaused = false;
static unsigned pauseDepth = 0;
void pollPause(uint8_t) { ++pauseDepth; }
void pollResume(uint8_t) { if (pauseDepth) --pauseDepth; }
bool pollPaused(uint8_t) { return pauseDepth != 0; }

static unsigned checks = 0;
static const char* currentCase = "";
static void checked(bool ok, const char* text, int line) {
  ++checks;
  if (ok) return;
  std::fprintf(stderr, "user roster authority check failed in %s at line %d: %s\n", currentCase, line, text);
  std::exit(1);
}
#define check(condition) checked((condition), #condition, __LINE__)

// ---------------------------------------------------------------------------
// Build configuration and doubles for what System_User.cpp reaches outside
// the code under test.
// ---------------------------------------------------------------------------
#define ENABLE_HTTP_SERVER HW1_TEST_HTTP
#define ENABLE_BONDED_MODE 1
#define EXT_RAM_BSS_ATTR
#define RETURN_VALID_IF_VALIDATE_CSTR() ((void)0)
#define DEBUG_USERSF(...) ((void)0)
#define ERROR_SESSIONF(...) ((void)0)
#define ERROR_MEMORYF(...) ((void)0)
#define WARN_USERF(...) ((void)0)

bool filesystemReady = true;
const char* USERS_JSON_FILE = "/system/users/users.json";
#define PENDING_USERS_FILE "/system/users/pending_users.json"
static const char* SETTINGS_DIR = "/system/users/user_settings";
#define PSRAM_JSON_DOC(name) JsonDocument name

struct AuthPerfScope { explicit AuthPerfScope(const char*) {} };
enum : int { SYSEVT_AUTH_DB_FAULT = 1 };
static unsigned dbFaultEvents = 0;
static void systemEventPost(int, const char* = nullptr, const char* = nullptr) { ++dbFaultEvents; }
enum class AllocPref { PreferPSRAM };
// Reply buffers are tracked so a case can tell whether a returned pointer is
// one of them and how large it is.
static std::map<const void*, size_t> allocations;
static void* ps_alloc(size_t size, AllocPref, const char*) {
  void* block = std::malloc(size);
  allocations[block] = size;
  return block;
}

static std::vector<std::string> output;
static void broadcastOutput(const String& message) { output.emplace_back(message.c_str()); }
static void broadcastPrintf(const char* format, ...) {
  char line[512];
  va_list args; va_start(args, format); std::vsnprintf(line, sizeof(line), format, args); va_end(args);
  output.emplace_back(line);
}
#define BROADCAST_PRINTF(...) broadcastPrintf(__VA_ARGS__)
static char debugBuffer[1024];
static bool ensureDebugBuffer() { return true; }
static char* getDebugBuffer() { return debugBuffer; }

static bool argWantsJson(const String& args) {
  const std::string text = std::string(" ") + args.c_str() + " ";
  return text.find(" json ") != std::string::npos;
}

// Whitespace-separated arguments with double-quote grouping.
class CommandArgs {
 public:
  explicit CommandArgs(const String& raw) : raw_(raw.c_str()) {
    size_t pos = 0;
    while (pos < raw_.size()) {
      while (pos < raw_.size() && raw_[pos] == ' ') ++pos;
      if (pos >= raw_.size()) break;
      std::string token;
      size_t end = pos;
      if (raw_[pos] == '"') {
        end = raw_.find('"', pos + 1);
        if (end == std::string::npos) end = raw_.size();
        token = raw_.substr(pos + 1, end - pos - 1);
        if (end < raw_.size()) ++end;
      } else {
        end = raw_.find(' ', pos);
        if (end == std::string::npos) end = raw_.size();
        token = raw_.substr(pos, end - pos);
      }
      args_.emplace_back(token);
      ends_.push_back(end);
      pos = end;
    }
  }
  String arg(int index) const { return has(index) ? String(args_[index]) : String(); }
  bool has(int index) const { return index >= 0 && index < static_cast<int>(args_.size()); }
  bool hasMinArgs(int n) const { return static_cast<int>(args_.size()) >= n; }
  String remaining(int afterIndex) const {
    if (afterIndex < 0) return String(raw_);
    if (!has(afterIndex)) return String();
    size_t from = ends_[afterIndex];
    while (from < raw_.size() && raw_[from] == ' ') ++from;
    return String(raw_.substr(from));
  }
 private:
  std::string raw_;
  std::vector<std::string> args_;
  std::vector<size_t> ends_;
};

// The calling task's identity (System_AuthIdentity's TLS slot).
static AuthContext gAuth;
static const AuthContext& currentAuthContext() { return gAuth; }
static const String& currentExecUser() { return gAuth.user; }
static bool gBondLive = false;
bool isBondSessionTokenValid() { return gBondLive; }

static AuthContext contextOf(CommandSource transport, const char* user) {
  AuthContext ctx{};
  ctx.transport = transport;
  ctx.user = user;
  ctx.path = "/test";
  ctx.ip = "local";
  return ctx;
}
static void runAs(CommandSource transport, const char* user) { gAuth = contextOf(transport, user); }

// Credentials: the hash double never contains its input.
static String hashUserPassword(const String& password) {
  const std::string text(password.c_str());
  return String("PBKDF2:" + std::string(text.rbegin(), text.rend()));
}
static bool isUserBanned(const String&) { return false; }
static bool loadUserSettings(uint32_t userId, JsonDocument& doc) {
  const std::string path = std::string(SETTINGS_DIR) + "/" + std::to_string(userId) + ".json";
  if (!fake_vfs::fs().exists(path.c_str(), fake_vfs::Fs::Auth{})) return false;
  return !deserializeJson(doc, fake_vfs::fs().text(path));
}

// Session fan-out and ban writer doubles.
static std::vector<std::string> revokeCalls, banCalls;
static int revokeUserSessions(const String& username, const String&) {
  revokeCalls.emplace_back(username.c_str());
  return 1;
}
// INSERT_BAN_WRITER_DOUBLE

#if ENABLE_HTTP_SERVER
static bool webRevokeSessionBySid(const String&, const String&, String* who) { if (who) *who = "web"; return true; }
static bool banIp(const char*, const char* = nullptr) { return true; }
static bool unbanIp(const char*) { return true; }
static bool isIpBanned(const char*) { return false; }
static const char* banListJson() { return "{\"schema\":1,\"bans\":[]}"; }
static void broadcastBanList() {}
static int webBroadcastSessionsHuman() { return 0; }
static int sessionRows = 0;
static void buildAllSessionsJson(const String&, JsonArray& sessions) {
  for (int i = 0; i < sessionRows; ++i) {
    JsonObject row = sessions.add<JsonObject>();
    row["user"] = "session-user-with-a-long-name-" + std::to_string(i);
    row["transport"] = "";
    row["ip"] = "192.168.100.200";
    row["sid"] = "0123456789abcdef0123456789abcdef";
    row["createdAt"] = 1700000000000LL + i;
  }
}
#endif

// Targeted-logout owner doubles.
static std::map<int, std::string> transportUsers;
static std::vector<int> logoutCalls;
static String getTransportUser(CommandSource transport) {
  auto it = transportUsers.find(static_cast<int>(transport));
  return it == transportUsers.end() ? String() : String(it->second);
}
static bool userMayControlOtherSessions(const String& username) { return username.length() > 0; }
static void logoutTransport(CommandSource transport) { logoutCalls.push_back(static_cast<int>(transport)); }
namespace {
class NamedSourceUseGuard {
 public:
  NamedSourceUseGuard(CommandSource, const String&, TransportSessionEpoch) {}
  explicit operator bool() const { return true; }
};
}  // namespace

// INSERT_FS_GUARD

// Forward declarations the production file gets from System_User.h.
bool isAdminUser(const String& who);
bool isSuperAdminUser(const String& who);
bool getUserIdByUsername(const String& username, uint32_t& outUserId);
bool getUserRole(const String& username, String& outRole);
int userRoleRank(const String& role);
bool isKnownUserRole(const String& role);
// INSERT_PROTOTYPES
// Referenced here so a tree whose list handlers do not use them still builds.
static_assert(kPublicUsernameMaxLen > 0 && CMD_RESULT_MAX > 0, "account and reply limits");

// INSERT_PRODUCTION

static bool currentExecIsAdmin() { return isAdminUser(gAuth.user); }

// INSERT_PRIVILEGE

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------
static std::string record(unsigned id, const std::string& name, const char* role, bool banned = false,
                          const std::string& extra = "") {
  std::string text = "{\"id\":" + std::to_string(id) + ",\"username\":\"" + name + "\"";
  if (role) text += std::string(",\"role\":\"") + role + "\"";
  if (banned) text += ",\"banned\":true,\"banReason\":\"test\"";
  return text + extra + "}";
}

static fake_vfs::Fs& resetStore() {
  auto& fs = fake_vfs::fs();
  fs.reset();
  fake_vfs::wireLockDouble();
  gFakeFsSemaphore = FakeFsSemaphore{};
  fs.mkdirAll(SETTINGS_DIR);
  output.clear(); revokeCalls.clear(); banCalls.clear(); logoutCalls.clear(); transportUsers.clear();
  gBondLive = false;
  filesystemReady = true;
  runAs(SOURCE_SERIAL, "owner");
#if ENABLE_HTTP_SERVER
  sessionRows = 0;
#endif
  return fs;
}

static void putRoster(fake_vfs::Fs& fs, const std::vector<std::string>& records) {
  std::string json = "{\"version\":1,\"nextId\":99,\"users\":[";
  for (size_t i = 0; i < records.size(); ++i) json += (i ? "," : "") + records[i];
  fs.put(USERS_JSON_FILE, json + "]}");
}

static void putPassword(fake_vfs::Fs& fs, unsigned id, const char* password) {
  fs.put((std::string(SETTINGS_DIR) + "/" + std::to_string(id) + ".json").c_str(),
         "{\"password\":\"" + std::string(hashUserPassword(password).c_str()) + "\"}");
}

// owner (1, superadmin), alice (2), carol (3, admin), sam (4, superadmin,
// banned), dave (5, user, banned), bond-admin (6, user: an account that only
// a sync receiver or a hand edit could have written).
static void putAuthorityRoster(fake_vfs::Fs& fs) {
  putRoster(fs, {record(1, "owner", "superadmin"), record(2, "alice", "user"), record(3, "carol", "admin"),
                 record(4, "sam", "superadmin", true), record(5, "dave", "user", true),
                 record(6, "bond-admin", "user")});
  putPassword(fs, 1, "owner-pass");
  putPassword(fs, 6, "bond-pass");
}

static bool startsWith(const char* text, const char* prefix) {
  return text && std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

// A reply that lives in one of the tracked reply buffers must be terminated
// inside that buffer; a literal is terminated by construction.
static bool terminatedInPlace(const char* reply) {
  auto it = allocations.find(reply);
  if (it == allocations.end()) return reply != nullptr;
  return strnlen(reply, it->second) < it->second;
}

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------
static void reservedBondNameIsNeverAnAccount() {
  auto& fs = resetStore();
  putAuthorityRoster(fs);
  gBondLive = true;

  // The credential owner refuses the reserved name whatever users.json holds.
  check(isValidUser("owner", "owner-pass"));
  check(!isValidUser("bond-admin", "bond-pass"));
  uint32_t id = 0;
  check(getUserIdByUsername("alice", id) && id == 2);
  check(!getUserIdByUsername("bond-admin", id) && id == 0);
  check(!getUserIdByUsername("Bond-Admin", id));

  // A serial or web session carrying the name is an ordinary account at most,
  // even while a bond session is live.
  for (CommandSource transport : {SOURCE_SERIAL, SOURCE_WEB, SOURCE_UART, SOURCE_BLUETOOTH}) {
    runAs(transport, "bond-admin");
    check(!hasSuperAdminPrivilege(gAuth));
    check(!hasAdminPrivilege(gAuth));
    check(!isSuperAdminUser("bond-admin"));
    check(!isAdminUser("bond-admin"));  // e.g. the web approve handler's name-only check
    check(userAccountRank("bond-admin") <= kRoleRankUser);
    String err;
    check(!userMutationAllowed("owner", kRoleRankNoGrant, err));
    check(!userMutationAllowed("alice", kRoleRankSuperAdmin, err));
  }

  // The live ESP-NOW bond keeps its super tier, for the ctx and the bare name.
  runAs(SOURCE_ESPNOW, "bond-admin");
  check(hasSuperAdminPrivilege(gAuth));
  check(hasAdminPrivilege(gAuth));
  check(isSuperAdminUser("bond-admin"));
  check(isAdminUser("bond-admin"));
  check(userAccountRank("bond-admin") == kRoleRankSuperAdmin);
  {
    String err;
    check(userMutationAllowed("owner", kRoleRankNoGrant, err));
  }
  // A ctx naming the bond on ESP-NOW is judged on the ctx even from another task.
  runAs(SOURCE_SERIAL, "alice");
  check(hasSuperAdminPrivilege(contextOf(SOURCE_ESPNOW, "bond-admin")));
  check(!hasSuperAdminPrivilege(contextOf(SOURCE_SERIAL, "bond-admin")));

  // No live bond: the ESP-NOW tuple is nobody.
  gBondLive = false;
  runAs(SOURCE_ESPNOW, "bond-admin");
  check(!hasSuperAdminPrivilege(gAuth));
  check(!hasAdminPrivilege(gAuth));
  check(!isAdminUser("bond-admin"));

  // Ordinary accounts are judged as before.
  runAs(SOURCE_SERIAL, "carol");
  check(hasAdminPrivilege(gAuth) && !hasSuperAdminPrivilege(gAuth));
  runAs(SOURCE_WEB, "owner");
  check(hasAdminPrivilege(gAuth) && hasSuperAdminPrivilege(gAuth));
  runAs(SOURCE_SERIAL, "alice");
  check(!hasAdminPrivilege(gAuth));
  check(userAccountRank("sam") == kRoleRankSuperAdmin);
}

static void unbanAndRevokeFollowTheRankRule() {
  auto& fs = resetStore();
  putAuthorityRoster(fs);
  runAs(SOURCE_SERIAL, "carol");

  // A regular admin may not lift a super-admin's ban ...
  const char* reply = cmd_unbanuser("sam");
  check(startsWith(reply, "Error") && std::strstr(reply, "higher-privileged"));
  check(banCalls.empty());
  // ... nor sign that account out everywhere.
  reply = cmd_session_revoke("user sam");
  check(startsWith(reply, "Error") && std::strstr(reply, "higher-privileged"));
  check(revokeCalls.empty());
  reply = cmd_session_revoke("user owner");
  check(startsWith(reply, "Error") && revokeCalls.empty());

  // Equal or lower targets are still allowed.
  reply = cmd_unbanuser("dave");
  check(!startsWith(reply, "Error"));
  check(banCalls.size() == 1 && banCalls[0] == "dave:unban");
  reply = cmd_session_revoke("user alice");
  check(!startsWith(reply, "Error"));
  check(revokeCalls.size() == 1 && revokeCalls[0] == "alice");
  reply = cmd_banuser("alice spam");
  check(!startsWith(reply, "Error"));
  check(banCalls.size() == 2 && banCalls[1] == "alice:ban");
  check(startsWith(cmd_banuser("owner"), "Error"));

  // A super-admin may.
  runAs(SOURCE_SERIAL, "owner");
  check(!startsWith(cmd_unbanuser("sam"), "Error"));
  check(banCalls.back() == "sam:unban");
  check(!startsWith(cmd_session_revoke("user sam"), "Error"));
  check(revokeCalls.back() == "sam");
}

static void targetedLogoutFollowsTheRankRule() {
  auto& fs = resetStore();
  putAuthorityRoster(fs);
  const char* denial = nullptr;

  // A user on serial may not sign out the admin's display session.
  transportUsers[SOURCE_LOCAL_DISPLAY] = "carol";
  check(!LOGOUT_TARGET(SOURCE_LOCAL_DISPLAY, SOURCE_SERIAL, "alice", 7, &denial));
  check(logoutCalls.empty());
  check(denial && std::strstr(denial, "higher-privileged"));

  // Equal rank, an empty target and a higher caller are allowed.
  transportUsers[SOURCE_LOCAL_DISPLAY] = "dave";
  check(LOGOUT_TARGET(SOURCE_LOCAL_DISPLAY, SOURCE_SERIAL, "alice", 7, &denial));
  transportUsers.erase(SOURCE_LOCAL_DISPLAY);
  check(LOGOUT_TARGET(SOURCE_LOCAL_DISPLAY, SOURCE_SERIAL, "alice", 7, &denial));
  transportUsers[SOURCE_UART] = "carol";
  check(LOGOUT_TARGET(SOURCE_UART, SOURCE_SERIAL, "owner", 7, &denial));
  check(logoutCalls.size() == 3);
}

static void accountBansWorkWithoutHttp() {
#if !ENABLE_HTTP_SERVER
  auto& fs = resetStore();
  putAuthorityRoster(fs);
  runAs(SOURCE_SERIAL, "carol");
  check(!startsWith(cmd_banuser("alice spam"), "Error"));
  check(!startsWith(cmd_banuser("alice"), "Ban management"));
  check(!startsWith(cmd_unbanuser("alice"), "Ban management"));
  check(banCalls.size() == 3 && banCalls[0] == "alice:ban" && banCalls[2] == "alice:unban");
  const char* reply = cmd_session_revoke("user alice");
  check(!startsWith(reply, "Error") && !startsWith(reply, "Session management"));
  check(revokeCalls.size() == 1 && revokeCalls[0] == "alice");
  // Only the web pieces stay behind the HTTP server.
  check(std::strstr(cmd_session_revoke("sid 0123"), "HTTP") != nullptr);
  check(std::strstr(cmd_ban("192.168.1.9"), "HTTP") != nullptr);
#endif
}

static void listRepliesAreTerminatedOrRefused() {
  auto& fs = resetStore();
  const std::string fat = ",\"createdAt\":\"2023-01-02T03:04:05Z\",\"createdAtSource\":\"ntp_resolved\","
                          "\"createdMs\":1000,\"ntpAnchorId\":1,\"bootCount\":1,"
                          "\"lastSeen\":\"2023-02-03T04:05:06Z\",\"createdBy\":\"administrator\"";
  std::vector<std::string> records;
  for (unsigned i = 1; i <= 12; ++i) {
    char name[48];
    std::snprintf(name, sizeof(name), "account-%02u-with-a-rather-long-public-name", i);
    records.push_back(record(i, name, i == 1 ? "superadmin" : "user", i == 5, fat));
  }
  putRoster(fs, records);
  const char* reply = cmd_user_list("json");
  check(terminatedInPlace(reply));
  {
    JsonDocument doc;
    check(!deserializeJson(doc, reply));
    JsonArray users = doc["users"].as<JsonArray>();
    check(doc["schema"] == 1 && users.size() == 12);
    for (JsonObject user : users) {
      // Only what the Settings Users panel and the user-sync picker read.
      for (JsonPair field : user) {
        const std::string key = field.key().c_str();
        check(key == "id" || key == "username" || key == "role" || key == "banned" || key == "createdAt");
      }
      check(user["id"].is<unsigned>() && user["username"].is<const char*>() && user["role"].is<const char*>());
      check(user["banned"].is<bool>());
    }
    check(users[4]["banned"] == true && users[0]["role"] == "superadmin");
    check(std::string(users[2]["createdAt"] | "") == "2023-01-02T03:04:05Z");
  }

  // A roster too large for one reply is refused explicitly.
  records.clear();
  for (unsigned i = 1; i <= 40; ++i) {
    std::string name(60, 'n');
    name += std::to_string(1000 + i);
    records.push_back(record(i, name, "superadmin", false, fat));
  }
  putRoster(fs, records);
  reply = cmd_user_list("json");
  check(terminatedInPlace(reply));
  check(startsWith(reply, "Error:"));

  // pendinglist: 32 long-named requests still fit; a larger hand-made list is refused.
  std::string pending = "[";
  for (unsigned i = 0; i < 32; ++i) {
    std::string name(60, 'p');
    name += std::to_string(1000 + i);
    pending += (i ? "," : "") + std::string("{\"username\":\"") + name +
               "\",\"password\":\"hash:secret\",\"timestamp\":1700000000123}";
  }
  fs.put(PENDING_USERS_FILE, pending + "]");
  reply = cmd_pending_list("json");
  check(terminatedInPlace(reply));
  {
    JsonDocument doc;
    check(!deserializeJson(doc, reply));
    check(doc["pending"].as<JsonArray>().size() == 32);
    check(std::strstr(reply, "password") == nullptr && std::strstr(reply, "secret") == nullptr);
  }
  pending = "[";
  for (unsigned i = 0; i < 60; ++i) {
    std::string name(60, 'q');
    name += std::to_string(1000 + i);
    pending += (i ? "," : "") + std::string("{\"username\":\"") + name + "\",\"timestamp\":1700000000123}";
  }
  fs.put(PENDING_USERS_FILE, pending + "]");
  reply = cmd_pending_list("json");
  check(terminatedInPlace(reply));
  check(startsWith(reply, "Error:"));

#if ENABLE_HTTP_SERVER
  sessionRows = 3;
  reply = cmd_session_list("json");
  check(terminatedInPlace(reply));
  {
    JsonDocument doc;
    check(!deserializeJson(doc, reply) && doc["sessions"].as<JsonArray>().size() == 3);
  }
  sessionRows = 80;
  reply = cmd_session_list("json");
  check(terminatedInPlace(reply));
  check(startsWith(reply, "Error:"));
#endif
}

static void rosterSnapshotKeepsFullNames() {
#if HW1_HAS_ROSTER_SNAPSHOT
  auto& fs = resetStore();
  const std::string longName(30, 'a');
  const std::string prefix(23, 'a');
  const std::string tooLong(70, 'z');
  putRoster(fs, {record(1, "owner", nullptr), record(2, longName, "Admin"), record(3, prefix, "user"),
                 record(4, "eve", "guest", true), record(5, tooLong, "user"), record(6, "wiz", "wizard"),
                 record(7, "", "user")});
  UserRosterEntry rows[16];
  size_t count = 0, total = 0;
  check(readUserRoster(rows, 16, count, &total));
  check(count == 5 && total == 5);
  check(std::string(rows[0].username) == "owner" && std::string(rows[0].role) == "superadmin" && rows[0].id == 1);
  check(std::string(rows[1].username) == longName && std::string(rows[1].role) == "admin" && rows[1].id == 2);
  check(std::string(rows[2].username) == prefix && rows[2].id == 3);
  check(std::string(rows[3].role) == "guest" && rows[3].banned);
  check(std::string(rows[4].username) == "wiz" && std::string(rows[4].role) == "user");
  check(!rows[1].banned);

  check(readUserRoster(rows, 2, count, &total) && count == 2 && total == 5);
  fs.remove(USERS_JSON_FILE, fake_vfs::Fs::Auth{});
  check(!readUserRoster(rows, 16, count) && count == 0);
  fs.put(USERS_JSON_FILE, "{\"users\":[");
  check(!readUserRoster(rows, 16, count) && count == 0);

  // userlist prints the full names from the same snapshot.
  putRoster(fs, {record(1, "owner", "superadmin"), record(2, longName, "admin"), record(3, prefix, "user")});
  output.clear();
  check(std::string(cmd_user_list("")) == "OK");
  bool sawLong = false;
  for (const auto& line : output) sawLong = sawLong || line == "  " + longName + " (admin)";
  check(sawLong);
#else
  std::fprintf(stderr, "user roster authority check failed in %s: System_User exposes no roster snapshot "
               "(readUserRoster)\n", currentCase);
  std::exit(1);
#endif
}

struct Case { const char* name; void (*run)(); };
static const Case kCases[] = {
  {"reservedBondNameIsNeverAnAccount", reservedBondNameIsNeverAnAccount},
  {"unbanAndRevokeFollowTheRankRule", unbanAndRevokeFollowTheRankRule},
  {"targetedLogoutFollowsTheRankRule", targetedLogoutFollowsTheRankRule},
  {"accountBansWorkWithoutHttp", accountBansWorkWithoutHttp},
  {"listRepliesAreTerminatedOrRefused", listRepliesAreTerminatedOrRefused},
  {"rosterSnapshotKeepsFullNames", rosterSnapshotKeepsFullNames},
};

int main(int argc, char** argv) {
  unsigned ran = 0;
  for (const Case& c : kCases) {
    if (argc > 1 && std::strcmp(argv[1], c.name) != 0) continue;
    currentCase = c.name;
    c.run();
    ++ran;
  }
  if (ran == 0) { std::fprintf(stderr, "no such case\n"); return 2; }
  for (auto& entry : allocations) std::free(const_cast<void*>(entry.first));
  std::printf("user roster authority (http=%d): %u checks passed in %u case(s)\n", HW1_TEST_HTTP, checks, ran);
  return 0;
}
