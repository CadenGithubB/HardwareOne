// Drives the production users.json and pending_users.json writers from
// System_User.cpp (ban, unban, promote/demote, delete, lastSeen, creation-time
// resolution, useradd, approve, deny, request) against the shared fake VFS with
// real ArduinoJson. Hashing, sessions, events and the per-user settings writer
// are doubles; the FS lock guard, the text writers and the settings path are
// the production ones.
#define FAKE_VFS_PROVIDE_VFS_NAMESPACE
#define FAKE_VFS_LOCK_DOUBLE
#include <Arduino.h>
#include "fake_vfs.h"
#include <ArduinoJson.h>
#include "System_PollPause.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <strings.h>
#include <string>
#include <vector>

void webBatchTestStringAppend() {}
volatile bool gSensorPollingPaused = false;
static unsigned pauseDepth = 0;
void pollPause(uint8_t) { ++pauseDepth; }
void pollResume(uint8_t) { if (pauseDepth) --pauseDepth; }
bool pollPaused(uint8_t) { return pauseDepth != 0; }

static unsigned checks = 0;
static void checked(bool ok, const char* text, int line) {
  ++checks;
  if (ok) return;
  std::fprintf(stderr, "user store check failed at line %d: %s\n", line, text);
  std::exit(1);
}
#define check(condition) checked((condition), #condition, __LINE__)

// ---------------------------------------------------------------------------
// Doubles for everything System_User.cpp reaches outside the store itself.
// ---------------------------------------------------------------------------
bool filesystemReady = true;
uint32_t gNTPAnchorId = 8;
uint32_t gBootCounter = 5;
const char* USERS_JSON_FILE = "/system/users/users.json";
const char* BOOT_ANCHORS_FILE = "/system/boot_anchors.json";
#define PENDING_USERS_FILE "/system/users/pending_users.json"
#define ENABLE_HTTP_SERVER 0

static int allocationFailureAt = -1, allocationCalls = 0;
static bool allocationFailed = false;
class TestAllocator final : public ArduinoJson::Allocator {
 public:
  void* allocate(size_t size) override {
    if (allocationCalls++ == allocationFailureAt) { allocationFailed = true; return nullptr; }
    return std::malloc(size);
  }
  void deallocate(void* ptr) override { std::free(ptr); }
  // Only fresh allocations fail: the vendored ArduinoJson saves a string whose
  // resize returned null (StringBuilder::save -> StringPool::add), which is a
  // library fault no caller can guard, not a users.json writer behaviour.
  void* reallocate(void* ptr, size_t size) override { return std::realloc(ptr, size); }
};
static TestAllocator allocator;
#define PSRAM_JSON_DOC(name) JsonDocument name(&allocator)

#define DEBUG_USERSF(...) ((void)0)
#define DEBUG_SYSTEMF(...) ((void)0)
#define DEBUG_NTP_RESOLVEF(...) ((void)0)
#define DEBUG_CMD_FLOWF(...) ((void)0)
#define INFO_SESSIONF(...) ((void)0)
#define ERROR_STORAGEF(...) ((void)0)

static std::vector<std::string> output, events, logLines, warnings;
static unsigned identityBumps = 0;
static void recordFormatted(std::vector<std::string>& into, const char* format, va_list args) {
  char line[512];
  std::vsnprintf(line, sizeof(line), format, args);
  into.emplace_back(line);
}
static void warnSession(const char* format, ...) {
  va_list args; va_start(args, format); recordFormatted(warnings, format, args); va_end(args);
}
#define WARN_SESSIONF(...) warnSession(__VA_ARGS__)
static void broadcastPrintf(const char* format, ...) {
  va_list args; va_start(args, format); recordFormatted(output, format, args); va_end(args);
}
#define BROADCAST_PRINTF(...) broadcastPrintf(__VA_ARGS__)
static void broadcastOutput(const String& message) { output.emplace_back(message.c_str()); }
static void logSystemEvent(const char* category, const char* format, ...) {
  va_list args; va_start(args, format); recordFormatted(logLines, format, args); va_end(args);
  logLines.back() = std::string(category) + ": " + logLines.back();
}
enum : int {
  SYSEVT_USER_ADDED = 1, SYSEVT_USER_APPROVED, SYSEVT_USER_REJECTED, SYSEVT_USER_BANNED,
  SYSEVT_USER_DELETED, SYSEVT_USER_REQUEST, SYSEVT_AUTH_DB_FAULT
};
static void systemEventPost(int kind, const char* subject = nullptr, const char* detail = nullptr) {
  (void)detail;
  events.emplace_back(std::to_string(kind) + ":" + (subject ? subject : ""));
}
static void bumpIdentityGeneration(const char*) { ++identityBumps; }

static unsigned long nowMs = 4242;
static unsigned long millis() { return nowMs; }
static time_t wallTime = 0;
static time_t testWallTime(time_t*) { return wallTime; }
#define time testWallTime
namespace Clock {
static bool isValidEpoch(time_t epoch) { return epoch >= 1577836800; }  // 2020-01-01
static time_t epochSeconds() { return wallTime; }
}  // namespace Clock

static AuthContext gAuth;
static const AuthContext& currentAuthContext() { return gAuth; }
struct AuthPerfScope { explicit AuthPerfScope(const char*) {} };
enum class AllocPref { PreferPSRAM };
static void* ps_alloc(size_t size, AllocPref, const char*) { return std::malloc(size); }

// The hash double never contains its input, so a plaintext found in any file
// or output line is a real leak.
static std::vector<std::string> plaintexts;
static std::string fakeHash(const std::string& plaintext) {
  return "hash:" + std::string(plaintext.rbegin(), plaintext.rend());
}
static String hashUserPassword(const String& password) {
  plaintexts.emplace_back(password.c_str());
  return String(fakeHash(password.c_str()));
}

// Session and transport doubles: the ban and delete tails revoke sessions.
static bool serialTransportSessionSnapshot(String& user, bool& authed) { user = ""; authed = false; return true; }
static void serialTransportSessionCleared() {}
static bool uartLinkSessionClearIfUser(const String&) { return false; }
static bool localDisplayTransportSessionSnapshot(String& user, bool& authed) { user = ""; authed = false; return true; }
static void localDisplayTransportSessionCleared() {}
static bool g2PairedUserClearIfMatches(const String&) { return false; }
static int bleRevokeUserSessions(const String&) { return 0; }
static int revokeUserSessions(const String&, const char*) { return 0; }

// Command-line double for cmd_user_request: whitespace-separated arguments.
class CommandArgs {
 public:
  explicit CommandArgs(const String& raw) {
    std::string text(raw.c_str());
    size_t pos = 0;
    while (pos < text.size()) {
      while (pos < text.size() && text[pos] == ' ') ++pos;
      if (pos >= text.size()) break;
      const size_t end = text.find(' ', pos);
      args_.emplace_back(text.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
      if (end == std::string::npos) break;
      pos = end + 1;
    }
  }
  const String& arg(int index) const { return has(index) ? args_[index] : empty_; }
  bool has(int index) const { return index >= 0 && index < static_cast<int>(args_.size()); }
  bool hasMinArgs(int n) const { return static_cast<int>(args_.size()) >= n; }
 private:
  std::vector<String> args_;
  String empty_;
};
static char debugBuffer[1024];
static bool ensureDebugBuffer() { return true; }
static char* getDebugBuffer() { return debugBuffer; }

void resolvePendingUserCreationTimes();
void cleanupOldBootAnchors(void* usersDocPtr = nullptr);

// INSERT_FS_GUARD
// INSERT_TEXT_IO

// The per-user settings writer is a double over the fake: a checked atomic
// write of the credential document, refusable by the suite.
static bool credentialOk = true;
static unsigned credentialSaves = 0;
static bool saveUserSettings(uint32_t userId, const JsonDocument& doc) {
  ++credentialSaves;
  if (!credentialOk || doc.overflowed()) return false;
  String json;
  serializeJson(doc, json);
  FsLockGuard guard("user_settings.save");
  return writeTextAtomic(getUserSettingsPath(userId).c_str(), json);
}

// INSERT_TYPES
// INSERT_PRODUCTION
// INSERT_BAN_ADAPTER

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------
static const char* SETTINGS_DIR = "/system/users/user_settings";

static std::string userRecord(unsigned id, const std::string& name, const char* role, bool pending,
                              const std::string& extra = "") {
  std::string record = "{\"id\":" + std::to_string(id) + ",\"username\":\"" + name + "\"";
  if (role) record += std::string(",\"role\":\"") + role + "\"";
  if (pending) {
    record += ",\"createdAt\":null,\"createdAtSource\":\"pending\",\"createdMs\":1000,\"ntpAnchorId\":7,\"bootCount\":5";
  } else {
    record += ",\"createdAt\":\"2023-01-02T03:04:05Z\",\"createdAtSource\":\"ntp_resolved\",\"createdMs\":1000,"
              "\"ntpAnchorId\":1,\"bootCount\":1,\"lastSeen\":\"2023-02-03T04:05:06Z\"";
  }
  record += ",\"createdBy\":\"admin\"";
  record += extra;
  return record + "}";
}

static std::string settingsPath(unsigned id) { return std::string(SETTINGS_DIR) + "/" + std::to_string(id) + ".json"; }

static fake_vfs::Fs& resetStore() {
  auto& fs = fake_vfs::fs();
  fs.reset();
  fake_vfs::wireLockDouble();
  gFakeFsSemaphore = FakeFsSemaphore{};
  fs.mkdirAll(SETTINGS_DIR);
  output.clear(); events.clear(); logLines.clear(); warnings.clear(); plaintexts.clear();
  identityBumps = 0; credentialOk = true; credentialSaves = 0;
  allocationFailureAt = -1; allocationCalls = 0; allocationFailed = false;
  gAuth = AuthContext{}; gAuth.user = "fixture-admin";
  nowMs = 4242; wallTime = 0; gNTPAnchorId = 8; gBootCounter = 5;
  return fs;
}

// owner (1, superadmin), alice (2), bob (3), carol (4, admin); nextId 5.
static void putStandardRoster(fake_vfs::Fs& fs, bool withNextId = true, unsigned nextId = 5) {
  std::string json = "{\"version\":1,";
  if (withNextId) json += "\"nextId\":" + std::to_string(nextId) + ",";
  json += "\"users\":[" + userRecord(1, "owner", "superadmin", false) + "," +
          userRecord(2, "alice", "user", false) + "," + userRecord(3, "bob", "user", false) + "," +
          userRecord(4, "carol", "admin", false) + "]}";
  fs.put(USERS_JSON_FILE, json);
  for (unsigned id = 1; id <= 4; ++id) {
    fs.put(settingsPath(id), "{\"theme\":\"light\",\"password\":\"hash:user" + std::to_string(id) + "\"}");
  }
}

static bool parseRoster(JsonDocument& doc) {
  const std::string text = fake_vfs::fs().text(USERS_JSON_FILE);
  return !deserializeJson(doc, text) && doc["users"].is<JsonArray>();
}

static JsonObject rosterUser(JsonDocument& doc, const char* name) {
  for (JsonObject user : doc["users"].as<JsonArray>()) {
    if (std::strcmp(user["username"] | "", name) == 0) return user;
  }
  return JsonObject();
}

static bool rosterHas(const char* name) {
  JsonDocument doc;
  return parseRoster(doc) && !rosterUser(doc, name).isNull();
}

static bool pendingHas(const char* name) {
  auto& fs = fake_vfs::fs();
  if (!fs.exists(PENDING_USERS_FILE)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, fs.text(PENDING_USERS_FILE))) return false;
  for (JsonObject entry : doc.as<JsonArray>()) {
    if (std::strcmp(entry["username"] | "", name) == 0) return true;
  }
  return false;
}

static bool callsLockedSince(size_t index, bool (*wanted)(const fake_vfs::Call&) = nullptr) {
  const auto& log = fake_vfs::fs().log;
  for (size_t i = index; i < log.size(); ++i) {
    if (wanted && !wanted(log[i])) continue;
    if (log[i].locked) continue;
    std::fprintf(stderr, "unlocked call: %s %s %s\n", log[i].op.c_str(), log[i].path.c_str(), log[i].other.c_str());
    return false;
  }
  return true;
}
static bool touchesRosterOrSettings(const fake_vfs::Call& call) {
  return call.path.rfind("/system/users/user", 0) == 0;  // users.json(.tmp) and user_settings/*
}
static bool touchesRoster(const fake_vfs::Call& call) {
  return call.path.rfind(USERS_JSON_FILE, 0) == 0;  // users.json and its temporary
}

static bool contains(const std::vector<std::string>& lines, const char* needle) {
  for (const auto& line : lines) if (line.find(needle) != std::string::npos) return true;
  return false;
}

static void noPlaintextAnywhere() {
  auto& fs = fake_vfs::fs();
  for (const auto& path : fs.allPaths()) {
    const std::string text = fs.text(path);
    for (const auto& secret : plaintexts) check(text.find(secret) == std::string::npos);
  }
  for (const auto& line : output) {
    for (const auto& secret : plaintexts) check(line.find(secret) == std::string::npos);
  }
}

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------
static void banReasonsThenEdit() {
  auto& fs = resetStore();
  putStandardRoster(fs);
  String err;
  const size_t start = fs.log.size();
  check(banUser("alice", true, "spam}", err));
  check(banUser("bob", true, "[spam]", err));
  JsonDocument doc;
  check(parseRoster(doc));
  check(rosterUser(doc, "alice")["banned"].as<bool>() && std::string(rosterUser(doc, "alice")["banReason"] | "") == "spam}");
  check(std::string(rosterUser(doc, "bob")["banReason"] | "") == "[spam]");
  check(events.size() == 2 && identityBumps == 2);

  // Delete the account whose reason holds a brace: the file must still parse
  // and the other records must be intact.
  check(deleteUserInternal("alice", err));
  check(parseRoster(doc));
  check(rosterUser(doc, "alice").isNull() && doc["users"].size() == 3);
  check(std::string(rosterUser(doc, "bob")["banReason"] | "") == "[spam]");
  check(std::string(rosterUser(doc, "carol")["role"] | "") == "admin");
  check(std::string(rosterUser(doc, "owner")["lastSeen"] | "") == "2023-02-03T04:05:06Z");
  check(!fs.exists(settingsPath(2)) && fs.exists(settingsPath(3)) && fs.exists(settingsPath(4)));
  check(contains(output, "Deleted user: alice"));

  // Promote and demote the bracketed account and a later one.
  check(setUserRoleInternal("bob", "admin", err));
  check(setUserRoleInternal("carol", "user", err));
  check(parseRoster(doc));
  check(std::string(rosterUser(doc, "bob")["role"] | "") == "admin");
  check(std::string(rosterUser(doc, "bob")["banReason"] | "") == "[spam]");
  check(std::string(rosterUser(doc, "carol")["role"] | "") == "user");
  check(setUserRoleInternal("bob", "user", err));
  check(deleteUserInternal("bob", err));
  check(parseRoster(doc) && doc["users"].size() == 2 && !rosterUser(doc, "carol").isNull());
  check(doc["nextId"].as<int>() == 5);

  // Unban keeps the record and drops both fields.
  check(banUser("carol", true, "x}y]z", err) && banUser("carol", false, "", err));
  check(parseRoster(doc));
  check(!rosterUser(doc, "carol")["banned"].is<bool>() && rosterUser(doc, "carol")["banReason"].isNull());

  // Every users.json read and write ran under the filesystem lock (the
  // settings-file removal after a delete is outside it, as before).
  check(callsLockedSince(start, touchesRoster));
  check(fs.liveHandles() == 0);
}

static void legacyRolelessEntry() {
  auto& fs = resetStore();
  fs.put(USERS_JSON_FILE, "{\"version\":1,\"nextId\":4,\"users\":[" + userRecord(1, "owner", "superadmin", false) + "," +
                              userRecord(2, "legacy", nullptr, false, ",\"banned\":true,\"banReason\":\"x}y\"") + "," +
                              userRecord(3, "carol", "user", false) + "]}");
  String err;
  check(setUserRoleInternal("legacy", "admin", err));
  JsonDocument doc;
  check(parseRoster(doc));
  check(std::string(rosterUser(doc, "legacy")["role"] | "") == "admin");
  check(std::string(rosterUser(doc, "legacy")["banReason"] | "") == "x}y");
  check(std::string(rosterUser(doc, "carol")["role"] | "") == "user");
}

static void founderAndMissingAccounts() {
  auto& fs = resetStore();
  putStandardRoster(fs);
  const std::string before = fs.text(USERS_JSON_FILE);
  String err;
  check(!banUser("owner", true, "no", err));
  check(!setUserRoleInternal("owner", "user", err) && err == "Cannot modify the first admin account");
  check(!deleteUserInternal("owner", err) && err == "Cannot delete the first admin account");
  check(!banUser("nobody", true, "no", err));
  check(!setUserRoleInternal("nobody", "admin", err) && err == "User not found");
  check(!deleteUserInternal("nobody", err) && err == "User not found");
  check(fs.text(USERS_JSON_FILE) == before);
  check(fs.count("rename") == 0 && identityBumps == 0 && events.empty());
  check(fs.exists(settingsPath(1)));
}

static void lastSeenBesideBraces() {
  auto& fs = resetStore();
  putStandardRoster(fs);
  String err;
  check(banUser("alice", true, "a}b", err));
  wallTime = 1700000000;
  const size_t start = fs.log.size();
  updateUserLastSeen("carol");
  JsonDocument doc;
  check(parseRoster(doc));
  check(std::string(rosterUser(doc, "carol")["lastSeen"] | "") == "2023-11-14T22:13:20Z");
  check(std::string(rosterUser(doc, "alice")["banReason"] | "") == "a}b");
  check(callsLockedSince(start));
  const std::string before = fs.text(USERS_JSON_FILE);
  updateUserLastSeen("nobody");
  check(fs.text(USERS_JSON_FILE) == before);
}

static std::string bigRoster(unsigned count, bool (*pending)(unsigned)) {
  std::string json = "{\"version\":1,\"nextId\":" + std::to_string(count + 1) + ",\"users\":[" +
                     userRecord(1, "owner", "superadmin", false);
  for (unsigned id = 2; id <= count; ++id) {
    json += "," + userRecord(id, "account-" + std::to_string(id), "user", pending(id));
  }
  return json + "]}";
}

static void putAnchor(fake_vfs::Fs& fs) {
  fs.put(BOOT_ANCHORS_FILE, "{\"bootAnchors\":[{\"ntpAnchorId\":7,\"epochAtSync\":1700000000,\"millisAtSync\":5000}]}");
}

static void resolveWholeRoster() {
  auto& fs = resetStore();
  const std::string roster = bigRoster(60, [](unsigned id) { return id >= 2 && id <= 5; });
  check(roster.size() > 10 * 1024);
  fs.put(USERS_JSON_FILE, roster);
  putAnchor(fs);
  wallTime = 1700000100;
  const size_t start = fs.log.size();
  resolvePendingUserCreationTimes();
  JsonDocument doc;
  check(parseRoster(doc));
  check(doc["users"].size() == 60 && doc["nextId"].as<int>() == 61);
  for (unsigned id = 2; id <= 5; ++id) {
    JsonObject user = rosterUser(doc, ("account-" + std::to_string(id)).c_str());
    check(std::string(user["createdAt"] | "") == "2023-11-14T22:13:16Z");
    check(std::string(user["createdAtSource"] | "") == "ntp_resolved");
    check(std::string(user["createdBy"] | "") == "admin");
  }
  JsonObject last = rosterUser(doc, "account-60");
  check(std::string(last["lastSeen"] | "") == "2023-02-03T04:05:06Z" && last["id"].as<int>() == 60);
  check(fs.text(USERS_JSON_FILE).size() > 10 * 1024);
  check(callsLockedSince(start));
  // Idempotent: a second pass changes nothing.
  const std::string written = fs.text(USERS_JSON_FILE);
  const unsigned renames = fs.count("rename");
  resolvePendingUserCreationTimes();
  check(fs.text(USERS_JSON_FILE) == written && fs.count("rename") == renames);
}

static void resolvePastEightKb() {
  auto& fs = resetStore();
  fs.put(USERS_JSON_FILE, bigRoster(60, [](unsigned id) { return id == 60; }));
  check(fs.text(USERS_JSON_FILE).size() > 8192);
  putAnchor(fs);
  wallTime = 1700000100;
  resolvePendingUserCreationTimes();
  JsonDocument doc;
  check(parseRoster(doc) && doc["users"].size() == 60);
  JsonObject last = rosterUser(doc, "account-60");
  check(std::string(last["createdAt"] | "") == "2023-11-14T22:13:16Z");
  check(std::string(last["createdAtSource"] | "") == "ntp_resolved");
}

static void useraddOrderAndFailures() {
  auto& fs = resetStore();
  putStandardRoster(fs);
  String err;
  // Success: credential first, roster last, hash only.
  size_t start = fs.log.size();
  check(adminCreateUser("dave", "dave-secret", true, "fixture-admin", err, "admin"));
  JsonDocument doc;
  check(parseRoster(doc));
  JsonObject dave = rosterUser(doc, "dave");
  check(dave["id"].as<int>() == 5 && std::string(dave["role"] | "") == "admin" && dave["createdAt"].isNull());
  check(std::string(dave["createdBy"] | "") == "fixture-admin" && std::string(dave["createdAtSource"] | "") == "pending");
  check(dave["createdMs"].as<unsigned long>() == nowMs && dave["ntpAnchorId"].as<uint32_t>() == gNTPAnchorId);
  check(doc["nextId"].as<int>() == 6 && !dave["password"].is<const char*>());
  check(fs.text(settingsPath(5)).find("\"password\":\"" + fakeHash("dave-secret") + "\"") != std::string::npos);
  check(fs.text(settingsPath(5)).find("\"mustChangePassword\":true") != std::string::npos);
  check(callsLockedSince(start, touchesRosterOrSettings));
  check(identityBumps == 1 && events.size() == 1);
  // The credential write precedes the roster promotion.
  int credentialRename = -1, rosterRename = -1;
  for (size_t i = start; i < fs.log.size(); ++i) {
    if (fs.log[i].op != "rename") continue;
    if (fs.log[i].other == settingsPath(5) && credentialRename < 0) credentialRename = static_cast<int>(i);
    if (fs.log[i].other == USERS_JSON_FILE && rosterRename < 0) rosterRename = static_cast<int>(i);
  }
  check(credentialRename >= 0 && rosterRename >= 0 && credentialRename < rosterRename);

  // Unknown role collapses to user; reserved and invalid names are refused.
  check(adminCreateUser("erin", "erin-secret", false, "", err, "wizard"));
  check(parseRoster(doc) && std::string(rosterUser(doc, "erin")["role"] | "") == "user");
  check(std::string(rosterUser(doc, "erin")["createdBy"] | "") == "admin");
  std::string before = fs.text(USERS_JSON_FILE);
  check(!adminCreateUser("bond-admin", "bond-secret", false, "x", err) && err == "Invalid username");
  check(!adminCreateUser("System", "sys-secret", false, "x", err) && err == "Invalid username");
  check(!adminCreateUser("dave", "dave-secret", false, "x", err) && err == "Username already exists");
  check(!adminCreateUser("ok.name", "short", false, "x", err));
  check(fs.text(USERS_JSON_FILE) == before && !fs.exists(settingsPath(7)));

  // Refused credential write: nothing published, nothing left behind.
  credentialOk = false;
  check(!adminCreateUser("frank", "frank-secret", false, "x", err));
  check(fs.text(USERS_JSON_FILE) == before && !rosterHas("frank") && !fs.exists(settingsPath(7)));
  credentialOk = true;

  // Refused roster write: nothing published and the credential is withdrawn.
  fs.faults.rename.add(std::string(USERS_JSON_FILE) + ".tmp");
  check(!adminCreateUser("frank", "frank-secret", false, "x", err));
  check(fs.text(USERS_JSON_FILE) == before && !fs.exists(settingsPath(7)));
  check(!fs.exists(std::string(USERS_JSON_FILE) + ".tmp"));
  fs.faults.clear();
  check(adminCreateUser("frank", "frank-secret", false, "x", err) && rosterHas("frank"));
  check(fs.exists(settingsPath(7)));
  noPlaintextAnywhere();
  check(fs.liveHandles() == 0);
}

static void uniqueIdWithoutNextId() {
  auto& fs = resetStore();
  putStandardRoster(fs, /*withNextId=*/false);
  String err;
  check(adminCreateUser("dave", "dave-secret", false, "x", err));
  JsonDocument doc;
  check(parseRoster(doc));
  check(rosterUser(doc, "dave")["id"].as<int>() == 5 && doc["nextId"].as<int>() == 6);
  for (unsigned id = 1; id <= 4; ++id) {
    check(fs.text(settingsPath(id)) == "{\"theme\":\"light\",\"password\":\"hash:user" + std::to_string(id) + "\"}");
  }
  check(fs.text(settingsPath(5)).find(fakeHash("dave-secret")) != std::string::npos);

  // A lagging counter (restored backup) must not reuse an id either.
  resetStore();
  putStandardRoster(fs, true, /*nextId=*/2);
  check(adminCreateUser("dave", "dave-secret", false, "x", err));
  check(parseRoster(doc) && rosterUser(doc, "dave")["id"].as<int>() == 5 && doc["nextId"].as<int>() == 6);
  check(fs.text(settingsPath(2)) == "{\"theme\":\"light\",\"password\":\"hash:user2\"}");
}

static void putPending(fake_vfs::Fs& fs, bool two) {
  std::string json = "[{\"username\":\"erin\",\"password\":\"hash:erin-secret\",\"timestamp\":1}";
  if (two) json += ",{\"username\":\"frank\",\"password\":\"hash:frank-secret\",\"timestamp\":2}";
  fs.put(PENDING_USERS_FILE, json + "]");
}

static void approveOrderAndFailures() {
  auto& fs = resetStore();
  putStandardRoster(fs);
  putPending(fs, false);
  wallTime = 1700000100;
  String err;
  const std::string rosterBefore = fs.text(USERS_JSON_FILE);
  const std::string pendingBefore = fs.text(PENDING_USERS_FILE);

  // Refused credential write: the request is kept, nothing is published.
  credentialOk = false;
  check(!approvePendingUserInternal("erin", err) && err.length() > 0);
  check(fs.text(USERS_JSON_FILE) == rosterBefore && fs.text(PENDING_USERS_FILE) == pendingBefore);
  check(!rosterHas("erin") && pendingHas("erin") && !fs.exists(settingsPath(5)));
  check(output.empty() && events.empty() && identityBumps == 0);
  credentialOk = true;

  // Refused roster write: same outcome, and the credential is withdrawn.
  fs.faults.rename.add(std::string(USERS_JSON_FILE) + ".tmp");
  check(!approvePendingUserInternal("erin", err) && err.length() > 0);
  check(fs.text(USERS_JSON_FILE) == rosterBefore && fs.text(PENDING_USERS_FILE) == pendingBefore);
  check(!fs.exists(settingsPath(5)) && output.empty() && identityBumps == 0);
  fs.faults.clear();

  // Unknown request.
  check(!approvePendingUserInternal("zoe", err) && err == "User not found in pending list");
  check(fs.text(PENDING_USERS_FILE) == pendingBefore);

  // Success: account live with the request's hash, request retired, event and
  // broadcast posted, every call under the lock.
  const size_t start = fs.log.size();
  check(approvePendingUserInternal("erin", err));
  JsonDocument doc;
  check(parseRoster(doc));
  JsonObject erin = rosterUser(doc, "erin");
  check(erin["id"].as<int>() == 5 && std::string(erin["role"] | "") == "user");
  check(std::string(erin["createdBy"] | "") == "approved:fixture-admin");
  check(fs.text(settingsPath(5)).find("\"password\":\"hash:erin-secret\"") != std::string::npos);
  check(!fs.exists(PENDING_USERS_FILE));
  check(contains(output, "Approved user: erin") && contains(logLines, "approved (id=5)"));
  check(identityBumps == 1 && events.size() == 1);
  check(callsLockedSince(start));
  check(fs.liveHandles() == 0);
  // Approving again is refused as a duplicate.
  check(!approvePendingUserInternal("erin", err) && err == "User not found in pending list");
}

static void approveReportsPendingListFailure() {
  auto& fs = resetStore();
  putStandardRoster(fs);
  putPending(fs, true);
  String err;
  fs.faults.rename.add(std::string(PENDING_USERS_FILE) + ".tmp");
  // The account exists and was announced; the stale request is reported, not hidden.
  check(!approvePendingUserInternal("erin", err));
  check(err == "User approved, but the pending list could not be updated");
  check(rosterHas("erin") && fs.exists(settingsPath(5)) && contains(output, "Approved user: erin"));
  check(pendingHas("erin") && pendingHas("frank"));
  // A retry is refused as a duplicate and publishes nothing new.
  const std::string roster = fs.text(USERS_JSON_FILE);
  check(!approvePendingUserInternal("erin", err) && err == "Username already exists");
  check(fs.text(USERS_JSON_FILE) == roster && pendingHas("erin"));
  fs.faults.clear();
  check(denyPendingUserInternal("erin", err) && !pendingHas("erin") && pendingHas("frank"));
}

static void denyAndRequest() {
  auto& fs = resetStore();
  putStandardRoster(fs);
  putPending(fs, true);
  String err;
  size_t start = fs.log.size();
  check(!denyPendingUserInternal("zoe", err) && err == "User not found in pending list");
  check(denyPendingUserInternal("erin", err));
  check(!pendingHas("erin") && pendingHas("frank"));
  check(denyPendingUserInternal("frank", err));
  check(!fs.exists(PENDING_USERS_FILE) && events.size() == 2);
  check(callsLockedSince(start));

  start = fs.log.size();
  const char* reply = cmd_user_request("gina gina-secret1 gina-secret1");
  check(std::string(reply) == "Request submitted for 'gina' (JSON)");
  check(pendingHas("gina"));
  check(fs.text(PENDING_USERS_FILE).find(fakeHash("gina-secret1")) != std::string::npos);
  check(callsLockedSince(start));
  check(std::string(cmd_user_request("gina other-secret1")) == "Error: Username already pending approval");
  check(std::string(cmd_user_request("hank short")) == "Error: Password must be at least 6 characters");
  check(std::string(cmd_user_request("bond-admin bond-secret1")) == "Error: Invalid username");
  check(std::string(cmd_user_request("hank hank-secret1")).rfind("Request submitted", 0) == 0);
  check(pendingHas("gina") && pendingHas("hank"));
  check(!fs.exists(std::string(PENDING_USERS_FILE) + ".tmp"));
  noPlaintextAnywhere();
  check(fs.liveHandles() == 0);
}

static bool hookFired = false, hookDeferred = false;
static void banAliceFromOtherTask() {
  String err;
  check(banUser("alice", true, "landed}", err));
}

static void banLandsInsideApprove() {
  auto& fs = resetStore();
  putStandardRoster(fs);
  putPending(fs, false);
  hookFired = hookDeferred = false;
  // Another task's ban arrives at the moment approve has read users.json. If
  // approve holds the filesystem lock, that task blocks until approve is done
  // (modelled by running the ban afterwards); otherwise it commits in between.
  fs.onCall = [](const fake_vfs::Call& call) {
    if (hookFired || call.op != "close" || call.path != USERS_JSON_FILE) return;
    hookFired = true;
    if (call.locked) { hookDeferred = true; return; }
    banAliceFromOtherTask();
  };
  String err;
  check(approvePendingUserInternal("erin", err));
  check(hookFired);
  fs.onCall = nullptr;
  if (hookDeferred) banAliceFromOtherTask();
  JsonDocument doc;
  check(parseRoster(doc));
  check(!rosterUser(doc, "erin").isNull());
  check(rosterUser(doc, "alice")["banned"].as<bool>());
  check(std::string(rosterUser(doc, "alice")["banReason"] | "") == "landed}");
  check(doc["users"].size() == 5);
}

static void allocationFailuresNeverTruncate() {
  auto& fs = resetStore();
  putStandardRoster(fs);
  String err;
  check(banUser("alice", true, "why}", err));
  const int successfulAllocations = allocationCalls;
  check(successfulAllocations > 0);
  for (int failAt = 0; failAt < successfulAllocations; ++failAt) {
    resetStore();
    putStandardRoster(fs);
    const std::string before = fs.text(USERS_JSON_FILE);
    allocationFailureAt = failAt;
    const bool ok = banUser("alice", true, "why}", err);
    allocationFailureAt = -1;
    // Either the ban failed and the roster is untouched, or it was published
    // complete; a document that lost records is never published.
    JsonDocument doc;
    check(parseRoster(doc) && doc["users"].size() == 4);
    if (!ok) check(fs.text(USERS_JSON_FILE) == before);
    check(!fs.exists(std::string(USERS_JSON_FILE) + ".tmp"));
  }
}

struct Case { const char* name; void (*run)(); };
static const Case kCases[] = {
  {"banReasonsThenEdit", banReasonsThenEdit},
  {"legacyRolelessEntry", legacyRolelessEntry},
  {"founderAndMissingAccounts", founderAndMissingAccounts},
  {"lastSeenBesideBraces", lastSeenBesideBraces},
  {"resolveWholeRoster", resolveWholeRoster},
  {"resolvePastEightKb", resolvePastEightKb},
  {"useraddOrderAndFailures", useraddOrderAndFailures},
  {"uniqueIdWithoutNextId", uniqueIdWithoutNextId},
  {"approveOrderAndFailures", approveOrderAndFailures},
  {"approveReportsPendingListFailure", approveReportsPendingListFailure},
  {"denyAndRequest", denyAndRequest},
  {"banLandsInsideApprove", banLandsInsideApprove},
  {"allocationFailuresNeverTruncate", allocationFailuresNeverTruncate},
};

// With no argument every case runs; a case name runs that one alone, which is
// how the runner reports each case separately against an older tree.
int main(int argc, char** argv) {
  unsigned ran = 0;
  for (const Case& c : kCases) {
    if (argc > 1 && std::strcmp(argv[1], c.name) != 0) continue;
    c.run();
    ++ran;
  }
  if (ran == 0) { std::fprintf(stderr, "unknown case: %s\n", argv[1]); return 2; }
  std::printf("user store mutations: %u checks passed in %u case(s)\n", checks, ran);
  return 0;
}
