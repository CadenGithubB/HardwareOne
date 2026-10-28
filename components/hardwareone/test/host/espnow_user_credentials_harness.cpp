// Host harness for ESP-NOW's password checks (C8/D3) and the USER_SYNC
// receiver's account creation (A11 with C10). test_espnow_user_credentials.py
// inserts, unchanged: the credential attempt table and checkCredentialAttempt,
// the System_User account owner region (validators, mutateUsersJson,
// createAccount and the id record), the roster lookup and reserved-name rule,
// the real FsLockGuard, text writers and settings path, DeferredUserSyncWork
// and doUserSyncWork, and v4_handle_cmd up to its authentication decision.
// Storage is the shared fake VFS with vendored ArduinoJson; everything else
// those functions reach is a double below.
#define FAKE_VFS_PROVIDE_VFS_NAMESPACE
#define FAKE_VFS_LOCK_DOUBLE
#include <Arduino.h>
#include "fake_vfs.h"
#include <ArduinoJson.h>
#include "System_PollPause.h"

#include <cctype>
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

// ---------------------------------------------------------------------------
// Process-wide doubles
// ---------------------------------------------------------------------------
bool filesystemReady = true;
uint32_t gNTPAnchorId = 8;  // non-static: older code declares them extern locally
uint32_t gBootCounter = 5;
const char* USERS_JSON_FILE = "/system/users/users.json";
#define PENDING_USERS_FILE "/system/users/pending_users.json"
#define ENABLE_BONDED_MODE 1
#define PSRAM_JSON_DOC(name) JsonDocument name

static std::vector<std::string> output, logLines;
static void recordLine(std::vector<std::string>& into, const char* format, va_list args) {
  char line[512];
  std::vsnprintf(line, sizeof(line), format, args);
  into.emplace_back(line);
}
static void broadcastPrintf(const char* format, ...) {
  va_list args; va_start(args, format); recordLine(output, format, args); va_end(args);
}
static void logSink(const char*, ...) {}
#define BROADCAST_PRINTF(...) broadcastPrintf(__VA_ARGS__)
#define DEBUG_AUTHF(...) logSink(__VA_ARGS__)
#define DEBUG_USERSF(...) logSink(__VA_ARGS__)
#define ERROR_STORAGEF(...) logSink(__VA_ARGS__)
#define ERROR_ESPNOWF(...) logSink(__VA_ARGS__)
#define WARN_ESPNOWF(...) logSink(__VA_ARGS__)
#define INFO_ESPNOWF(...) logSink(__VA_ARGS__)
#define DEBUG_ESPNOWF(...) logSink(__VA_ARGS__)
#define DEBUGF(flag, ...) logSink(__VA_ARGS__)
static void broadcastOutput(const String& message) { output.emplace_back(message.c_str()); }
static void broadcastOutput(const char* message) { output.emplace_back(message ? message : ""); }
static void logSystemEvent(const char* category, const char* format, ...) {
  va_list args; va_start(args, format); recordLine(logLines, format, args); va_end(args);
  logLines.back() = std::string(category) + ": " + logLines.back();
}

enum : uint8_t {
  SYSEVT_LOGIN_OK = 1, SYSEVT_LOGIN_FAIL, SYSEVT_LOGIN_LOCKED, SYSEVT_USER_ADDED,
  SYSEVT_AUTH_DB_FAULT
};
struct Event { int kind; std::string subject; std::string detail; };
static std::vector<Event> events;
void systemEventPost(uint8_t kind, const char* subject = nullptr, const char* detail = nullptr) {
  events.push_back({kind, subject ? subject : "", detail ? detail : ""});
}
static unsigned identityBumps = 0;
void bumpIdentityGeneration(const char*) { ++identityBumps; }

// One host clock: the table reads esp_timer, the roster code millis() and the
// wall clock.
static uint64_t gClockMs = 1000;
static unsigned long millis() { return static_cast<unsigned long>(gClockMs); }
static int64_t esp_timer_get_time() { return static_cast<int64_t>(gClockMs * 1000ULL); }
static time_t wallTime = 0;
static time_t testWallTime(time_t*) { return wallTime; }
#define time testWallTime
namespace Clock {
static bool isValidEpoch(time_t epoch) { return epoch >= 1577836800; }  // 2020-01-01
}  // namespace Clock
#define EXT_RAM_BSS_ATTR
struct portMUX_TYPE { int owner; };
#define portMUX_INITIALIZER_UNLOCKED {0}
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
struct AuthPerfScope { explicit AuthPerfScope(const char*) {} };

// The hash double never contains its input.
static String hashUserPassword(const String& password) {
  std::string text(password.c_str());
  return String(("hash:" + std::string(text.rbegin(), text.rend())).c_str());
}

// INSERT_TYPES

// Password and role doubles: root/root-pass is this device's admin; carol is
// a valid non-admin account.
static int passwordChecks = 0;
bool isValidUser(const String& user, const String& password) {
  ++passwordChecks;
  return (user == "root" && password == "root-pass") ||
         (user == "carol" && password == "carol-pass");
}
bool isAdminUser(const String& user) { return user == "root"; }
void secureClearString(String& s) { s = ""; }

// INSERT_FS_GUARD
// INSERT_TEXT_IO

// The per-user settings writer: a checked atomic write, refusable.
static bool credentialOk = true;
static bool saveUserSettings(uint32_t userId, const JsonDocument& doc) {
  if (!credentialOk || doc.overflowed()) return false;
  String json;
  serializeJson(doc, json);
  FsLockGuard guard("user_settings.save");
  return writeTextAtomic(getUserSettingsPath(userId).c_str(), json, VFS::systemAuth(VFS::Scopes::USERS, "user_settings.save"));
}

// The account-id high-water mark (an NVS key) as a variable.
static uint32_t idMark = 0;
bool bootStateGetAccountIdHighWater(uint32_t& out) { out = idMark; return true; }
bool bootStateRaiseAccountIdHighWater(uint32_t id) {
  if (idMark < id) idMark = id;
  return true;
}

// INSERT_TABLE

// INSERT_ACCOUNTS

// ---------------------------------------------------------------------------
// ESP-NOW doubles
// ---------------------------------------------------------------------------
static struct { bool espnowUserSyncEnabled = true; } gSettings;
enum : uint8_t { ESPNOW_V4_TYPE_CMD_RESP = 51 };
enum : uint16_t { ESPNOW_V4_FLAG_ACK_REQ = 0x0001 };

struct Reply { uint32_t msgId; bool ok; std::string text; };
static std::vector<Reply> replies;  // CMD_RESP frames, both senders
static int acks = 0;
static bool v4_send_ack(const uint8_t*, uint32_t) { ++acks; return true; }
bool v4_send_command_response(const uint8_t*, uint32_t msgId, bool success, const char* text,
                              uint16_t length) {
  replies.push_back({msgId, success, std::string(text, length)});
  return true;
}
namespace espnowtx {
bool sendAead(const uint8_t*, uint8_t type, uint16_t, uint32_t msgId, const uint8_t* payload,
              uint16_t length, uint8_t) {
  if (type == ESPNOW_V4_TYPE_CMD_RESP && length >= 2) {
    replies.push_back({msgId, payload[0] != 0,
                       std::string(reinterpret_cast<const char*>(payload + 1))});
  }
  return true;
}
}  // namespace espnowtx

static String redactCmdForAudit(const String& line) { return line; }

// Bond-token doubles: the token "00..0" (32 hex zeros) is the live one.
static int bondFailures = 0;
static bool parseSessionToken(const char* text, uint8_t* token) {
  for (int i = 0; i < 32; ++i) if (!std::isxdigit(static_cast<unsigned char>(text[i]))) return false;
  std::memset(token, 0, 16);
  token[0] = std::strncmp(text, "00000000000000000000000000000000", 32) == 0 ? 0 : 1;
  return true;
}
static bool validateBondSessionToken(const uint8_t* token, size_t) { return token[0] == 0; }
static void logBondAuthFailure(const uint8_t*, const char*) { ++bondFailures; }

struct Accepted { uint32_t msgId; std::string user; std::string command; };
static std::vector<Accepted> accepted;
static void harnessCommandAccepted(const uint8_t*, uint32_t msgId, const char* user,
                                   const char* command) {
  accepted.push_back({msgId, user, command});
}

// INSERT_USER_SYNC

// INSERT_REMOTE_COMMAND

// ---------------------------------------------------------------------------
// Fixtures and scenarios
// ---------------------------------------------------------------------------
static int gChecks = 0;
static int gFailures = 0;
static bool gScenarioOk = true;
static void check(bool condition, const char* scenario, const char* what) {
  ++gChecks;
  if (!condition) {
    gScenarioOk = false;
    std::printf("  FAIL %s: %s\n", scenario, what);
  }
}
static void finish(const char* scenario) {
  if (gScenarioOk) {
    std::printf("ok   %s\n", scenario);
  } else {
    std::printf("FAIL %s\n", scenario);
    ++gFailures;
  }
}

static const char* SETTINGS_DIR = "/system/users/user_settings";
static const char* ROSTER =
    "{\"version\":1,\"nextId\":5,\"users\":["
    "{\"id\":1,\"username\":\"root\",\"role\":\"superadmin\",\"createdBy\":\"setup\"},"
    "{\"id\":2,\"username\":\"alice\",\"role\":\"user\",\"createdBy\":\"admin\"},"
    "{\"id\":3,\"username\":\"bob\",\"role\":\"user\",\"createdBy\":\"admin\"},"
    "{\"id\":4,\"username\":\"carol\",\"role\":\"user\",\"createdBy\":\"admin\"}]}";

static void begin() {
  auto& fs = fake_vfs::fs();
  fs.reset();
  fake_vfs::wireLockDouble();
  gFakeFsSemaphore = FakeFsSemaphore{};
  fs.mkdirAll(SETTINGS_DIR);
  fs.put(USERS_JSON_FILE, ROSTER);
  std::memset(credential_attempts::sEntries, 0, sizeof(credential_attempts::sEntries));
  credential_attempts::sTotalFailures = 0;
  output.clear(); logLines.clear(); events.clear(); replies.clear(); accepted.clear();
  acks = 0; passwordChecks = 0; bondFailures = 0; identityBumps = 0;
  credentialOk = true; idMark = 0; gClockMs = 1000; wallTime = 0;
  filesystemReady = true;
  gSettings.espnowUserSyncEnabled = true;
  gScenarioOk = true;
}

static const uint8_t kPeerA[6] = {0x02, 0, 0, 0, 0, 0xA1};
static const uint8_t kPeerB[6] = {0x02, 0, 0, 0, 0, 0xB2};

static void remoteCommand(const uint8_t* mac, uint32_t msgId, const char* payload,
                          bool encrypted = true) {
  v4_handle_cmd(mac, "peer-a", msgId, payload, encrypted);
}

static void userSync(uint32_t msgId, const std::string& json, bool encrypted = true) {
  const size_t need = sizeof(DeferredUserSyncWork) + json.size();
  auto* work = static_cast<DeferredUserSyncWork*>(std::calloc(1, need));
  std::memcpy(work->srcAddr, kPeerA, 6);
  work->msgId = msgId;
  work->isSessionEncrypted = encrypted;
  std::snprintf(work->deviceName, sizeof(work->deviceName), "%s", "peer-a");
  work->payloadLen = static_cast<uint16_t>(json.size());
  std::memcpy(work->payload, json.data(), json.size());
  doUserSyncWork(work);
  std::free(work);
}

static std::string syncJsonAs(const char* admin, const char* adminPass, const char* user,
                              const char* pass, const char* role = "user") {
  return std::string("{\"recv_admin_user\":\"") + admin + "\",\"recv_admin_pass\":\"" + adminPass +
         "\",\"target_user\":\"" + user + "\",\"target_pass\":\"" + pass + "\",\"role\":\"" +
         role + "\"}";
}
static std::string syncJson(const char* adminPass, const char* user, const char* pass,
                            const char* role = "user") {
  return syncJsonAs("root", adminPass, user, pass, role);
}

static bool lastReply(uint32_t msgId, bool ok, const char* text) {
  return !replies.empty() && replies.back().msgId == msgId && replies.back().ok == ok &&
         replies.back().text == text;
}

static int countEvents(int kind) {
  int count = 0;
  for (const Event& e : events) if (e.kind == kind) ++count;
  return count;
}

static bool rosterUser(const char* name, JsonDocument& out, JsonObject& user) {
  if (deserializeJson(out, fake_vfs::fs().text(USERS_JSON_FILE))) return false;
  for (JsonObject entry : out["users"].as<JsonArray>()) {
    if (std::strcmp(entry["username"] | "", name) == 0) {
      user = entry;
      return true;
    }
  }
  return false;
}

static void scenarioRemoteLockout() {
  const char* name = "remote command: the sixth password is refused unchecked";
  begin();
  for (uint32_t id = 1; id <= 5; ++id) {
    remoteCommand(kPeerA, id, "carol:wrong:status");
    check(lastReply(id, false, "Auth failed"), name, "a wrong password answers Auth failed with its msgId");
  }
  check(passwordChecks == 5 && countEvents(SYSEVT_LOGIN_FAIL) == 5, name,
        "five checks, five SYSEVT_LOGIN_FAIL posts");
  remoteCommand(kPeerB, 6, "root:root-pass:status");
  check(passwordChecks == 5, name, "another peer's right password is not checked while locked");
  check(accepted.empty(), name, "and runs nothing");
  check(lastReply(6, false, "Auth locked out. Retry in 30s"), name,
        "the lockout answers in the same CMD_RESP shape with the msgId echoed");
  check(countEvents(SYSEVT_LOGIN_LOCKED) == 1 && countEvents(SYSEVT_LOGIN_FAIL) == 5, name,
        "one lock event; the refusal is not a login failure");
  gClockMs += LOGIN_LOCKOUT_TIER1_MS;
  remoteCommand(kPeerB, 7, "root:root-pass:status");
  check(accepted.size() == 1 && accepted[0].user == "root" && accepted[0].command == "status",
        name, "the right password runs the command once the lock ends");
  finish(name);
}

static void scenarioPlaintextAndBond() {
  const char* name = "remote command: plaintext and bond-token paths stay outside the table";
  begin();
  for (uint32_t id = 1; id <= 6; ++id) remoteCommand(kPeerA, id, "carol:wrong:status", false);
  check(passwordChecks == 0, name, "a plaintext command is refused before any check");
  check(lastReply(6, false, "Remote command must be session-encrypted"), name,
        "with its existing refusal");
  for (uint32_t id = 10; id <= 15; ++id) {
    remoteCommand(kPeerA, id, "@BOND:ffffffffffffffffffffffffffffffff:status");
  }
  check(bondFailures == 6 && lastReply(15, false, "Session token auth failed"), name,
        "bad bond tokens still go to logBondAuthFailure with their own reply");
  check(credentialAttemptFailures(SOURCE_ESPNOW, "espnow") == 0, name,
        "bond-token failures do not count toward the password lock");
  for (uint32_t id = 20; id <= 24; ++id) remoteCommand(kPeerA, id, "carol:wrong:status");
  remoteCommand(kPeerA, 30, "@BOND:00000000000000000000000000000000:status");
  check(accepted.size() == 1 && accepted[0].user == "bond-admin", name,
        "a live bond token still runs its command while passwords are locked");
  finish(name);
}

static void scenarioSyncAdminLockout() {
  const char* name = "USER_SYNC: the admin check shares ESP-NOW's lock";
  begin();
  for (uint32_t id = 1; id <= 3; ++id) remoteCommand(kPeerA, id, "carol:wrong:status");
  userSync(4, syncJson("wrong", "dave", "dave-pass"));
  check(lastReply(4, false, "Admin authentication failed"), name, "a wrong admin password keeps its text");
  userSync(5, syncJson("wrong", "dave", "dave-pass"));
  const int checksAtLock = passwordChecks;
  userSync(6, syncJson("root-pass", "dave", "dave-pass"));
  check(passwordChecks == checksAtLock, name, "the locked admin check validates nothing");
  check(lastReply(6, false, "Admin authentication locked out. Retry in 30s"), name,
        "and answers with the lockout and the msgId");
  JsonDocument doc; JsonObject user;
  check(!rosterUser("dave", doc, user), name, "no account is created while locked");
  finish(name);
}

static void scenarioSyncRefusals() {
  const char* name = "USER_SYNC: reserved or malformed names and short passwords are refused";
  begin();
  const std::string before = fake_vfs::fs().text(USERS_JSON_FILE);
  uint32_t id = 1;
  for (const char* bad : {"bond-admin", "system", "AuthBypass", "bad:name", "two words"}) {
    userSync(id, syncJson("root-pass", bad, "long-enough"));
    check(lastReply(id, false, "Invalid username"), name, "a reserved or malformed name is refused");
    ++id;
  }
  userSync(id, syncJson("root-pass", "dave", "abc"));
  check(lastReply(id, false, "Password must be at least 6 characters"), name,
        "a short password is refused with the owner's text");
  check(fake_vfs::fs().text(USERS_JSON_FILE) == before, name, "users.json is untouched");
  check(countEvents(SYSEVT_USER_ADDED) == 0 && identityBumps == 0, name,
        "no account event or identity bump");
  finish(name);
}

static void scenarioSyncCreates() {
  const char* name = "USER_SYNC: createAccount allocates above the id mark and forces user";
  begin();
  idMark = 40;
  wallTime = 1700000000;  // 2023-11-14T22:13:20Z
  userSync(9, syncJson("root-pass", "dave", "dave-pass", "admin"));
  check(lastReply(9, true, "User 'dave' created (id=41)"), name,
        "the reply carries createAccount's id, above the mark");
  JsonDocument doc; JsonObject user;
  const bool found = rosterUser("dave", doc, user);
  check(found && user["id"].as<unsigned>() == 41, name, "the roster holds id 41, not nextId 5");
  check(found && std::strcmp(user["role"] | "", "user") == 0, name, "a synced admin lands as user");
  check(found && std::strcmp(user["createdBy"] | "", "espnow:peer-a") == 0, name,
        "provenance names the sending device");
  check(found && std::strcmp(user["createdAt"] | "", "2023-11-14T22:13:20Z") == 0 &&
            std::strcmp(user["createdAtSource"] | "", "clock") == 0,
        name, "createdAt is stamped from the valid clock");
  check(fake_vfs::fs().text(std::string(SETTINGS_DIR) + "/41.json").find("hash:ssap-evad") !=
            std::string::npos,
        name, "the credential is stored under the new id");
  check(idMark == 41, name, "the high-water mark is raised");
  check(countEvents(SYSEVT_USER_ADDED) == 1 && events.back().subject == "dave" &&
            events.back().detail == "espnow:peer-a",
        name, "SYSEVT_USER_ADDED names the account and the sender");
  check(identityBumps == 1, name, "the identity generation is bumped");
  bool logged = false;
  for (const auto& line : logLines) logged = logged || line.find("USERS: user 'dave' (id=41)") == 0;
  check(logged, name, "the USERS log line names the new id");
  userSync(10, syncJson("root-pass", "dave", "dave-pass"));
  check(lastReply(10, true, "User already exists (skipped)"), name,
        "a second sync of the same name is skipped with ok=true");
  finish(name);
}

static void scenarioSyncCheckedWrite() {
  const char* name = "USER_SYNC: a failed credential write publishes nothing";
  begin();
  credentialOk = false;
  const std::string before = fake_vfs::fs().text(USERS_JSON_FILE);
  userSync(3, syncJson("root-pass", "erin", "erin-pass"));
  check(lastReply(3, false, "Failed to write user settings"), name, "the sender hears the failure");
  check(fake_vfs::fs().text(USERS_JSON_FILE) == before, name, "users.json is untouched");
  check(countEvents(SYSEVT_USER_ADDED) == 0 && identityBumps == 0, name,
        "no account event or identity bump");
  finish(name);
}

static void scenarioSyncGates() {
  const char* name = "USER_SYNC: encryption and opt-in refusals are unchanged";
  begin();
  userSync(1, syncJson("root-pass", "dave", "dave-pass"), false);
  check(acks == 1 && replies.empty() && passwordChecks == 0, name,
        "a plaintext sync gets a bare ACK before any check");
  gSettings.espnowUserSyncEnabled = false;
  userSync(2, syncJson("root-pass", "dave", "dave-pass"));
  check(acks == 2 && replies.empty() && passwordChecks == 0, name,
        "a disabled sync gets a bare ACK before any check");
  gSettings.espnowUserSyncEnabled = true;
  userSync(3, syncJsonAs("carol", "carol-pass", "dave", "dave-pass"));
  check(lastReply(3, false, "Admin privileges required"), name, "a non-admin is refused");
  finish(name);
}

int main() {
  scenarioRemoteLockout();
  scenarioPlaintextAndBond();
  scenarioSyncAdminLockout();
  scenarioSyncRefusals();
  scenarioSyncCreates();
  scenarioSyncCheckedWrite();
  scenarioSyncGates();
  std::fprintf(stderr, "checks=%d\n", gChecks);
  std::printf("%d scenario(s) failed\n", gFailures);
  return gFailures == 0 ? 0 : 1;
}
