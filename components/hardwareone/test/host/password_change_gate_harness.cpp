// Drives the forced-password-change owners with real ArduinoJson: the per-user
// settings merge and its editable-key list (System_Settings.cpp), the cached
// userMustChangePassword decision (System_User.cpp), the web gate that
// tgRequireAuth runs (WebServer_Utils.cpp) and createAccount's guest rule
// (System_User.cpp). The runner pastes the production definitions in; the
// settings store, roster lookups, the httpd request/response and the users.json
// writer are doubles.
#include <Arduino.h>
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
#include <ArduinoJson.h>
#include "System_UserSettings.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

void webBatchTestStringAppend() {}

static unsigned checks = 0;
static void checked(bool ok, const char* text, int line) {
  ++checks;
  if (ok) return;
  std::fprintf(stderr, "password change gate check failed at line %d: %s\n", line, text);
  std::exit(1);
}
#define check(condition) checked((condition), #condition, __LINE__)

#define PSRAM_JSON_DOC(name) JsonDocument name
#define DEBUG_USERSF(...) ((void)0)
#define EXT_RAM_BSS_ATTR
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(mux) (mux)->lock()
#define portEXIT_CRITICAL(mux) (mux)->unlock()
inline constexpr size_t kPublicUsernameMaxLen = 64;
std::atomic<uint32_t> gIdentityGeneration{1};
bool filesystemReady = true;

// ---------------------------------------------------------------------------
// Per-user settings store double: files by user id, counted loads and saves.
// ---------------------------------------------------------------------------
static std::map<uint32_t, std::string> stored;
static unsigned loads = 0, saves = 0;

bool loadUserSettings(uint32_t userId, JsonDocument& doc) {
  ++loads;
  doc.clear();
  const auto it = stored.find(userId);
  if (it == stored.end()) {
    doc.to<JsonObject>();
    return true;
  }
  if (deserializeJson(doc, it->second) || !doc.is<JsonObject>()) {
    doc.clear();
    return false;
  }
  return true;
}

UserSettingsTxnStatus runUserSettingsTransaction(uint32_t userId, UserSettingsMutator mutator, void* context) {
  if (!mutator) return UserSettingsTxnStatus::InvalidArgument;
  JsonDocument base;
  if (!loadUserSettings(userId, base)) return UserSettingsTxnStatus::LoadFailed;
  if (!mutator(base, context) || base.overflowed()) return UserSettingsTxnStatus::MutationRejected;
  std::string out;
  serializeJson(base, out);
  stored[userId] = out;
  ++saves;
  return UserSettingsTxnStatus::Committed;
}

// ---------------------------------------------------------------------------
// Roster doubles: id and role by name; "owner" is the legacy recovery owner.
// ---------------------------------------------------------------------------
struct Account { uint32_t id; const char* role; };
static std::map<std::string, Account> roster;
static unsigned roleReads = 0;

bool getUserIdByUsername(const String& username, uint32_t& outUserId) {
  outUserId = 0;
  const auto it = roster.find(username.c_str());
  if (it == roster.end()) return false;
  outUserId = it->second.id;
  return true;
}
bool getUserAuthorizationRole(const String& username, String& roleOut) {
  ++roleReads;
  const auto it = roster.find(username.c_str());
  if (it == roster.end()) return false;
  roleOut = it->second.role;
  return true;
}
bool isSuperAdminUser(const String& who) { return std::strcmp(who.c_str(), "owner") == 0; }

// ---------------------------------------------------------------------------
// httpd doubles for the web gate.
// ---------------------------------------------------------------------------
using esp_err_t = int;
static constexpr esp_err_t ESP_OK = 0;
static constexpr esp_err_t ESP_FAIL = -1;
enum httpd_method { HTTP_GET = 1, HTTP_HEAD, HTTP_POST, HTTP_OPTIONS };
struct httpd_req_t {
  int method;
  char uri[128];
};
static std::string respStatus, respType, respBody, respLocation, acceptHeader;
static void resetResponse() { respStatus.clear(); respType.clear(); respBody.clear(); respLocation.clear(); }
static esp_err_t httpd_resp_set_status(httpd_req_t*, const char* status) { respStatus = status; return ESP_OK; }
static esp_err_t httpd_resp_set_type(httpd_req_t*, const char* type) { respType = type; return ESP_OK; }
static esp_err_t httpd_resp_set_hdr(httpd_req_t*, const char* name, const char* value) {
  if (std::strcmp(name, "Location") == 0) respLocation = value;
  return ESP_OK;
}
static esp_err_t httpd_resp_sendstr(httpd_req_t*, const char* body) { respBody = body; return ESP_OK; }
static esp_err_t httpd_resp_send(httpd_req_t*, const char* body, long length) {
  respBody.assign(body, static_cast<size_t>(length));
  return ESP_OK;
}
static esp_err_t httpd_req_get_hdr_value_str(httpd_req_t*, const char* name, char* out, size_t size) {
  if (std::strcmp(name, "Accept") != 0 || acceptHeader.empty()) return ESP_FAIL;
  std::snprintf(out, size, "%s", acceptHeader.c_str());
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// Account-creation doubles: the users.json writer and VFS are observed only.
// ---------------------------------------------------------------------------
static unsigned publications = 0;
bool isValidPublicUsername(const String& username, String* errorOut) {
  if (username.length() == 0 && errorOut) *errorOut = "Invalid username";
  return username.length() != 0;
}
struct FsLockGuard { explicit FsLockGuard(const char*) {} };
int createAccountMutation = 0;
template <typename Mutation>
static bool mutateUsersJson(const char*, Mutation, void*, String&) { ++publications; return true; }
namespace VFS {
static int systemAuth(const char*) { return 0; }
namespace Scopes {  // the production Scopes (System_VFS.h); keep in step
  inline constexpr const char* VIDEOS = "/sd/VIDEOS"; inline constexpr const char* CERTS = "/system/certs";
  inline constexpr const char* OTA = "/system/ota"; inline constexpr const char* ESPNOW_RECEIVED = "/espnow/received";
  inline constexpr const char* CAPTURES = "/logging_captures"; inline constexpr const char* CAPTURES_SD = "/sd/logging_captures";
  inline constexpr const char* STT_MODELS = "/STT Models"; inline constexpr const char* STT_MODELS_SD = "/sd/STT Models";
  inline constexpr const char* SYS_LOGS = "/system/sys_logs"; inline constexpr const char* SYS_LOGS_SD = "/sd/system/sys_logs";
  inline constexpr const char* SYSTEM_STATE = "/system"; inline constexpr const char* USERS = "/system/users";
  inline constexpr const char* MAPS = "/maps"; inline constexpr const char* EI_MODELS = "/EI Models";
  inline constexpr const char* RECORDINGS = "/recordings"; inline constexpr const char* RECORDINGS_SD = "/sd/recordings";
  inline constexpr const char* ESPNOW_STATE = "/system/espnow"; inline constexpr const char* ESPNOW_CAPTURES = "/sd/espnow";
  inline constexpr const char* LLM_MODELS = "/system/llm"; inline constexpr const char* LLM_MODELS_SD = "/sd/llm";
  inline constexpr const char* ESPSR = "/ESPSR"; inline constexpr const char* ESPSR_SD = "/sd/ESPSR";
  inline constexpr const char* PICTURES_SD = "/sd/PICTURES";
}  // namespace Scopes
static int systemAuth(const char*, const char* reason) { return systemAuth(reason); }
// Tier pickers mirroring System_VFS.h (accept const char* or any String-like).
inline const char* fakeVfsCStr(const char* p) { return p; }
template <class S> inline const char* fakeVfsCStr(const S& s) { return s.c_str(); }
template <class P> inline bool fakeVfsOnSd(const P& p) { return std::strncmp(fakeVfsCStr(p), "/sd/", 4) == 0; }
template <class P> inline const char* captureScopeFor(const P& p) { return fakeVfsOnSd(p) ? Scopes::CAPTURES_SD : Scopes::CAPTURES; }
template <class P> inline const char* sysLogScopeFor(const P& p) { return fakeVfsOnSd(p) ? Scopes::SYS_LOGS_SD : Scopes::SYS_LOGS; }
template <class P> inline const char* recordingScopeFor(const P& p) { return fakeVfsOnSd(p) ? Scopes::RECORDINGS_SD : Scopes::RECORDINGS; }
template <class P> inline const char* llmModelScopeFor(const P& p) { return fakeVfsOnSd(p) ? Scopes::LLM_MODELS_SD : Scopes::LLM_MODELS; }
template <class P> inline const char* espsrScopeFor(const P& p) { return fakeVfsOnSd(p) ? Scopes::ESPSR_SD : Scopes::ESPSR; }
template <class P> inline const char* sttModelScopeFor(const P& p) { return fakeVfsOnSd(p) ? Scopes::STT_MODELS_SD : Scopes::STT_MODELS; }
// end tier pickers
static bool removeGuarded(const char*, int) { return true; }
}  // namespace VFS
String getUserSettingsPath(uint32_t) { return String("/system/users/user_settings/0.json"); }

// INSERT_PRODUCTION

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------
static const char* kBob = "{\"theme\":\"light\",\"password\":\"PBKDF2:bob\","
                          "\"gamepad_password\":\"PBKDF2:pad\",\"mustChangePassword\":true}";

static bool merge(const char* patchJson) {
  JsonDocument patch;
  check(!deserializeJson(patch, patchJson));
  return mergeAndSaveUserSettings(7, patch);
}

static void mergeCases() {
  // Credentials and the forced-change flag are refused whatever the value;
  // a JSON null must not erase the stored hash or clear the flag.
  static const char* const kRefused[] = {
    "{\"mustChangePassword\":false}", "{\"mustChangePassword\":null}",
    "{\"password\":null}", "{\"gamepad_password\":null}",
    "{\"password\":\"plaintext\"}", "{\"theme\":\"dark\",\"password\":null}",
    "{\"theme\":\"dark\",\"mustChangePassword\":false}", "{\"notAPreference\":1}",
  };
  for (const char* patch : kRefused) {
    stored[7] = kBob;
    saves = 0;
    if (merge(patch)) std::fprintf(stderr, "merge accepted %s\n", patch);
    check(saves == 0);
    check(stored[7] == kBob);
  }

  stored[7] = kBob;
  check(merge("{\"theme\":\"dark\",\"notifyLevel\":2,\"dashboardSystemHidden\":null,"
              "\"dashboardConnLayout\":[\"wifi\"]}"));
  JsonDocument doc;
  check(!deserializeJson(doc, stored[7]));
  check(doc["theme"] == "dark" && doc["notifyLevel"] == 2 && doc["dashboardConnLayout"][0] == "wifi");
  check(doc["password"] == "PBKDF2:bob" && doc["gamepad_password"] == "PBKDF2:pad" &&
        doc["mustChangePassword"] == true);

#if HAS_REFUSED_KEY
  JsonDocument patch;
  check(!deserializeJson(patch, "{\"theme\":\"x\",\"password\":null}"));
  const char* refused = userSettingsPatchRefusedKey(patch);
  check(refused && std::strcmp(refused, "password") == 0);
  check(!deserializeJson(patch, "{\"theme\":\"x\"}"));
  check(userSettingsPatchRefusedKey(patch) == nullptr);
  check(!deserializeJson(patch, "[1]"));
  refused = userSettingsPatchRefusedKey(patch);
  check(refused && refused[0] == '\0');
#endif
}

static void mustChangeCases() {
  roster = {{"bob", {7, "user"}}, {"gina", {8, "guest"}}, {"owner", {1, "guest"}},
            {"carl", {9, "user"}}, {"bad", {10, "user"}}, {"dave", {12, "user"}}};
  stored[7] = kBob;
  stored[8] = "{\"password\":\"PBKDF2:g\",\"mustChangePassword\":true}";
  stored[1] = "{\"password\":\"PBKDF2:o\",\"mustChangePassword\":true}";
  stored[9] = "{\"password\":\"PBKDF2:c\",\"mustChangePassword\":false}";
  stored[10] = "{broken";
  stored[12] = "{\"mustChangePassword\":true}";

  check(userMustChangePassword("bob"));
  check(!userMustChangePassword("carl"));
  check(!userMustChangePassword("nobody"));
  check(!userMustChangePassword(""));
  check(!userMustChangePassword("bad"));
  // A guest can never change its own password, so the flag does not bind one;
  // the legacy recovery owner is not a guest for this rule (as in the web gate).
  check(!userMustChangePassword("gina"));
  check(userMustChangePassword("owner"));

#if HAS_CACHE
  // Answers are cached per account until a password write or identity change.
  loads = 0;
  check(userMustChangePassword("bob") && userMustChangePassword("bob"));
  check(loads == 0);
  stored[7] = "{\"password\":\"PBKDF2:new\",\"mustChangePassword\":false}";
  must_change_cache::sWrites.fetch_add(1);
  check(!userMustChangePassword("bob"));
  check(loads == 1);
  check(!userMustChangePassword("bob") && loads == 1);
  stored[7] = kBob;
  gIdentityGeneration.fetch_add(1);
  check(userMustChangePassword("bob") && loads == 2);
  // A role change bumps the identity generation: the new role is read.
  roster["bob"].role = "guest";
  gIdentityGeneration.fetch_add(1);
  check(!userMustChangePassword("bob"));
  roster["bob"].role = "user";
  gIdentityGeneration.fetch_add(1);
  check(userMustChangePassword("bob"));
  // Unreadable settings are not cached.
  loads = 0;
  check(!userMustChangePassword("bad") && !userMustChangePassword("bad"));
  check(loads == 2);
  // More accounts than slots: every answer stays right.
  for (int round = 0; round != 3; ++round) {
    check(userMustChangePassword("bob") && !userMustChangePassword("carl") &&
          !userMustChangePassword("gina") && userMustChangePassword("owner") &&
          userMustChangePassword("dave"));
  }
  // A name longer than a cache slot is answered uncached.
  const std::string longName(80, 'l');
  roster[longName] = {11, "user"};
  stored[11] = "{\"mustChangePassword\":true}";
  loads = 0;
  check(userMustChangePassword(String(longName)) && userMustChangePassword(String(longName)));
  check(loads == 2);
#endif
}

#if HAS_GATE
static bool gate(int method, const char* uri, const char* user, const char* accept = "") {
  httpd_req_t req{};
  req.method = method;
  std::snprintf(req.uri, sizeof(req.uri), "%s", uri);
  acceptHeader = accept;
  resetResponse();
  return webPasswordChangeAllows(&req, String(user));
}

static bool forbiddenJson() {
  return respStatus == "403 Forbidden" && respType == "application/json" &&
         respBody.find("\"error\":\"password_change_required\"") != std::string::npos;
}

static void gateCases() {
  stored[7] = kBob;
  gIdentityGeneration.fetch_add(1);
  // APIs and other pages are refused while the change is pending.
  check(!gate(HTTP_GET, "/api/system", "bob") && forbiddenJson());
  check(!gate(HTTP_POST, "/api/user/settings", "bob") && forbiddenJson());
  check(!gate(HTTP_POST, "/api/cli", "bob") && forbiddenJson());
  check(!gate(HTTP_GET, "/api/events?x=1", "bob") && forbiddenJson());
  check(!gate(HTTP_POST, "/mqtt/save", "bob") && forbiddenJson());
  check(!gate(HTTP_GET, "/dashboard", "bob", "application/json") && forbiddenJson());
  check(!gate(HTTP_GET, "/mqtt", "bob", "text/html"));
  check(respStatus == "303 See Other" && respLocation == "/account/password-change");
  check(!gate(HTTP_GET, "/account/password-changed", "bob"));
  // The change form and logout stay open.
  check(gate(HTTP_GET, "/account/password-change", "bob") && respStatus.empty());
  check(gate(HTTP_POST, "/account/password-change?from=login", "bob") && respStatus.empty());
  check(gate(HTTP_GET, "/logout", "bob") && respStatus.empty());
  // Everyone else passes untouched.
  check(gate(HTTP_POST, "/api/cli", "carl") && respStatus.empty());
  check(gate(HTTP_GET, "/dashboard", "gina") && respStatus.empty());
  check(gate(HTTP_GET, "/api/system", "") && respStatus.empty());
}
#endif

#if HAS_CREATE
static void createCases() {
  NewAccount account;
  account.username = "gwen";
  account.passwordHash = "PBKDF2:gwen";
  account.role = "guest";
  account.mustChangePassword = true;
  uint32_t id = 0;
  String err;
  publications = 0;
  check(!createAccount(account, id, err));
  check(publications == 0 && id == 0);
  check(err == "A guest account cannot be required to change its password");
  account.mustChangePassword = false;
  check(createAccount(account, id, err) && publications == 1);
  account.role = "user";
  account.mustChangePassword = true;
  check(createAccount(account, id, err) && publications == 2);
}
#endif

int main() {
  mergeCases();
  mustChangeCases();
#if HAS_GATE
  gateCases();
#endif
#if HAS_CREATE
  createCases();
#endif
  if (!(HAS_REFUSED_KEY && HAS_CACHE && HAS_GATE && HAS_CREATE)) {
    std::fprintf(stderr, "password change gate: production owner missing (refusedKey=%d cache=%d gate=%d create=%d)\n",
                 HAS_REFUSED_KEY, HAS_CACHE, HAS_GATE, HAS_CREATE);
    return 1;
  }
  std::printf("password_change_gate: %u checks passed\n", checks);
  return 0;
}
