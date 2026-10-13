// Drives the production /register/submit handler (WebServer_Server.cpp) with
// the production form decoding and public account rules, plus the production
// authorizeCommand and the `userrequest` adapter (cmd_user_request). The httpd
// request, the lockout adapters, the role lookups and the account-request
// owner are doubles; the owner double records exactly what it receives. Run
// with no argument for every case, or with one case name.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <strings.h>
#include <vector>

static unsigned checks = 0;
static void checked(bool ok, const char* text, int line) {
  ++checks;
  if (ok) return;
  std::fprintf(stderr, "register submit check failed at line %d: %s\n", line, text);
  std::exit(1);
}
#define check(condition) checked((condition), #condition, __LINE__)

// Harness-local String: the members the extracted code calls, over std::string.
class String {
 public:
  String() = default;
  String(const char* text) : text_(text ? text : "") {}
  String(const std::string& text) : text_(text) {}
  String& operator=(const char* text) { text_ = text ? text : ""; return *this; }
  char operator[](size_t index) const { return text_[index]; }
  const char* c_str() const { return text_.c_str(); }
  size_t length() const { return text_.length(); }
  bool reserve(size_t capacity) { text_.reserve(capacity); return true; }
  int indexOf(char value, size_t from = 0) const {
    const auto pos = text_.find(value, from);
    return pos == std::string::npos ? -1 : static_cast<int>(pos);
  }
  int indexOf(const char* value, size_t from = 0) const {
    const auto pos = text_.find(value, from);
    return pos == std::string::npos ? -1 : static_cast<int>(pos);
  }
  String substring(size_t from) const { return text_.substr(from); }
  String substring(size_t from, size_t to) const { return text_.substr(from, to - from); }
  void trim() {
    const auto first = text_.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { text_.clear(); return; }
    text_ = text_.substr(first, text_.find_last_not_of(" \t\r\n") - first + 1);
  }
  String operator+(const char* other) const { return text_ + other; }
  String& operator+=(char c) { text_ += c; return *this; }
  String& operator+=(const char* other) { text_ += other; return *this; }
  bool operator==(const String& other) const { return text_ == other.text_; }
  bool operator!=(const String& other) const { return text_ != other.text_; }
  bool equalsIgnoreCase(const String& other) const {
    return strcasecmp(text_.c_str(), other.text_.c_str()) == 0;
  }
  const std::string& str() const { return text_; }

 private:
  std::string text_;
};

// Production types: the public limits, CommandSource, AuthContext and the
// account-request result.
// INSERT_TYPES
AccountRequestResult submitAccountRequest(const String& username, const String& password);

// ---------------------------------------------------------------------------
// httpd double: one request body, one captured response.
// ---------------------------------------------------------------------------
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define HTTPD_RESP_USE_STRLEN -1
enum httpd_err_code_t { HTTPD_400_BAD_REQUEST = 400 };
struct httpd_req_t {
  int content_len = 0;
  std::string body;
  size_t offset = 0;
  std::string response;
  std::string status = "200 OK";
};
static int httpd_req_recv(httpd_req_t* req, char* buf, size_t len) {
  const size_t n = std::min(len, req->body.size() - req->offset);
  std::memcpy(buf, req->body.data() + req->offset, n);
  req->offset += n;
  return static_cast<int>(n);
}
static void httpd_resp_set_status(httpd_req_t* req, const char* status) { req->status = status; }
static void httpd_resp_set_type(httpd_req_t*, const char*) {}
static void httpd_resp_send(httpd_req_t* req, const char* text, int) { req->response += text; }
static void httpd_resp_send_chunk(httpd_req_t* req, const char* text, int) { req->response += text; }
static void httpd_resp_send_err(httpd_req_t* req, httpd_err_code_t, const char* text) {
  req->status = "400";
  req->response += text;
}
static void streamBeginHtml(httpd_req_t* req, const char* title, bool, const char*, const char*) {
  req->response += "<title>";
  req->response += title;
  req->response += "</title>";
}
static void streamEndHtml(httpd_req_t*) {}

enum class AllocPref { PreferPSRAM };
static void* ps_alloc(size_t size, AllocPref, const char*) { return std::malloc(size); }

// Client and lockout adapters (WebServer_Server.cpp keys them by IP).
static const char* kClientIp = "192.0.2.7";
static void getClientIP(httpd_req_t*, String& out) { out = kClientIp; }
static void getClientIP(httpd_req_t*, char* buf, size_t size) { std::snprintf(buf, size, "%s", kClientIp); }
static bool ipBanned = false, ipLocked = false;
static unsigned failedLogins = 0;
static bool isIpBanned(const char*) { return ipBanned; }
static bool isLoginLocked(const char*, unsigned long* remainingMs) {
  if (remainingMs) *remainingMs = ipLocked ? 30000UL : 0UL;
  return ipLocked;
}
static void recordFailedLogin(const char*) { ++failedLogins; }

// Doubles for what the production authorizeCommand consults besides the
// identity: every named account is a plain user, no command needs a role.
enum { SYSEVT_COMMAND_DENIED = 1 };
static unsigned deniedAudits = 0;
static void logAuthAttempt(bool success, const char*, const String&, const String&, const char*) {
  if (!success) ++deniedAudits;
}
static void systemEventPost(int, const char* = nullptr, const char* = nullptr) {}
static bool isSuperAdminUser(const String&) { return false; }
static bool getUserAuthorizationRole(const String&, String& role) { role = "user"; return true; }
static bool commandAllowedForGuest(const String&) { return false; }
static bool hasAdminPrivilege(const AuthContext&) { return false; }
static bool hasSuperAdminPrivilege(const AuthContext&) { return false; }
static bool callerMayTargetAnotherSession(const AuthContext&, const char** denialOut) {
  if (denialOut) *denialOut = "Error: targeted session control denied.";
  return false;
}
struct CommandEntry { const char* name; bool requiresAdmin; bool requiresSuperAdmin; };
struct CommandResolution { const CommandEntry* entry = nullptr; };
// Registry rows as System_User.cpp / System_Utils.cpp declare them: neither
// needs admin.
static const CommandEntry kUserRequestEntry = {"userrequest", false, false};
static const CommandEntry kLoginEntry = {"login", false, false};

// Validate-only flag and reply buffer used by the command adapter.
static bool gCLIValidateOnly = false;
#define RETURN_VALID_IF_VALIDATE_CSTR() \
  do { \
    if (::gCLIValidateOnly) return "VALID"; \
  } while (0)
static char debugBuffer[1024];
static bool ensureDebugBuffer() { return true; }
static char* getDebugBuffer() { return debugBuffer; }

// The command path an older handler used: the web context the handler builds,
// then the production authorizeCommand (defined below).
static AuthContext makeWebAuthCtx(httpd_req_t*) {
  AuthContext ctx{};
  ctx.transport = SOURCE_WEB;
  ctx.path = "/register/submit";
  ctx.ip = kClientIp;
  return ctx;
}
static std::vector<std::string> commandLines;
bool executeUnifiedWebCommand(httpd_req_t*, AuthContext& ctx, const String& line, String& out);

// INSERT_PRODUCTION

static bool authorize(const AuthContext& ctx, const String& line, const CommandEntry* entry,
                      std::string& reply) {
  CommandResolution resolution;
  resolution.entry = entry;
  char out[256] = {};
  const bool ok = authorizeCommand(ctx, line, resolution, line, out, sizeof(out));
  reply = out;
  return ok;
}

// Runs a command line as the dispatcher would: authorization first, then
// `userrequest` reaches its adapter.
bool executeUnifiedWebCommand(httpd_req_t*, AuthContext& ctx, const String& line, String& out) {
  commandLines.push_back(line.str());
  std::string reply;
  if (!authorize(ctx, line, &kUserRequestEntry, reply)) {
    out = reply;
    return false;
  }
#ifndef HW1_NO_ACCOUNT_REQUEST_ADAPTER
  const std::string text = line.str();
  if (text.rfind("userrequest ", 0) == 0) {
    out = cmd_user_request(String(text.substr(12)));
    return std::strncmp(out.c_str(), "Error", 5) != 0;
  }
#endif
  out = "unexpected";
  return false;
}

// Recording account-request owner.
struct OwnerCall { std::string username, password; };
static std::vector<OwnerCall> ownerCalls;
static AccountRequestResult ownerReply = AccountRequestResult::Submitted;
AccountRequestResult submitAccountRequest(const String& username, const String& password) {
  ownerCalls.push_back({username.str(), password.str()});
  return ownerReply;
}

static void reset() {
  ownerCalls.clear();
  commandLines.clear();
  ownerReply = AccountRequestResult::Submitted;
  failedLogins = 0;
  ipBanned = ipLocked = false;
  gCLIValidateOnly = false;
  deniedAudits = 0;
}

static httpd_req_t post(const std::string& body) {
  httpd_req_t req;
  req.body = body;
  req.content_len = static_cast<int>(body.size());
  check(handleRegisterSubmit(&req) == ESP_OK);
  return req;
}

static std::string form(const std::string& user, const std::string& pass, const std::string& confirm) {
  return "username=" + user + "&password=" + pass + "&confirm_password=" + confirm;
}

static void explain() {
  for (const auto& line : commandLines) std::fprintf(stderr, "  command line built: %s\n", line.c_str());
}

// A passphrase with spaces reaches the owner as typed; nothing counts.
static void spacedPassphraseReachesOwner() {
  reset();
  const httpd_req_t req = post(form("alice", "correct+horse+battery+staple", "correct+horse+battery+staple"));
  if (ownerCalls.size() != 1) explain();
  check(ownerCalls.size() == 1);
  check(ownerCalls[0].username == "alice");
  check(ownerCalls[0].password == "correct horse battery staple");
  check(commandLines.empty());
  check(failedLogins == 0);
  check(req.response.find("Request Submitted") != std::string::npos);
}

// Repeated words were silently cut to the first one by the command line.
static void repeatedWordsAreNotCut() {
  reset();
  post(form("bob", "aaaaaa+aaaaaa", "aaaaaa+aaaaaa"));
  if (ownerCalls.size() != 1) explain();
  check(ownerCalls.size() == 1);
  check(ownerCalls[0].password == "aaaaaa aaaaaa");
  check(failedLogins == 0);
}

// Quotes, percent signs, '&' and '+' survive form decoding byte-exact.
static void quotesAndEscapesReachOwner() {
  reset();
  const std::string encoded = "pa%22ss+%27w%22rd+%25%26%2B";
  post(form("carol.d-1", encoded, encoded));
  if (ownerCalls.size() != 1) explain();
  check(ownerCalls.size() == 1);
  check(ownerCalls[0].username == "carol.d-1");
  check(ownerCalls[0].password == "pa\"ss 'w\"rd %&+");
  check(failedLogins == 0);
}

// The owner's refusals are shown with fixed text and are not login failures.
static void ownerRefusalsDoNotCount() {
  struct Case { AccountRequestResult reply; const char* text; };
  const Case cases[] = {
      {AccountRequestResult::AlreadyPending, "already pending approval"},
      {AccountRequestResult::QueueFull, "Too many pending requests"},
      {AccountRequestResult::WriteFailed, "Unable to submit request"},
      {AccountRequestResult::ReadFailed, "Unable to submit request"},
      {AccountRequestResult::NotReady, "Unable to submit request"},
  };
  for (const Case& c : cases) {
    reset();
    ownerReply = c.reply;
    const httpd_req_t req = post(form("dave", "dave-secret1", "dave-secret1"));
    if (ownerCalls.size() != 1) explain();
    check(ownerCalls.size() == 1);
    check(failedLogins == 0);
    check(req.response.find("Registration Failed") != std::string::npos);
    check(req.response.find(c.text) != std::string::npos);
  }
  // A field the owner rejects counts, like the handler's own field checks.
  reset();
  ownerReply = AccountRequestResult::InvalidUsername;
  post(form("erin", "erin-secret1", "erin-secret1"));
  check(ownerCalls.size() == 1);
  check(failedLogins == 1);
}

// Rejected form fields still count on this unauthenticated endpoint and never
// reach the owner.
static void rejectedFieldsStillCount() {
  reset();
  post(form("frank", "one+secret", "two+secret"));
  check(ownerCalls.empty() && failedLogins == 1);
  reset();
  post(form("bad%20name", "frank-secret1", "frank-secret1"));
  check(ownerCalls.empty() && failedLogins == 1);
  reset();
  post(form("bond-admin", "frank-secret1", "frank-secret1"));
  check(ownerCalls.empty() && failedLogins == 1);
  reset();
  post(form("frank", "short", "short"));
  check(ownerCalls.empty() && failedLogins == 1);
  reset();
  post(form("frank", "", ""));
  check(ownerCalls.empty() && failedLogins == 1);
  // A locked address reaches nothing and adds nothing.
  reset();
  ipLocked = true;
  const httpd_req_t req = post(form("frank", "frank-secret1", "frank-secret1"));
  check(ownerCalls.empty() && failedLogins == 0);
  check(req.response.find("Too many attempts") != std::string::npos);
  check(commandLines.empty());
}

// Registration does not open `userrequest` to anonymous callers: the
// production authorizeCommand still refuses it from every external transport
// without an identity, on the web CLI path as well as on the register path.
static void anonymousUserRequestStillRefused() {
  const CommandSource transports[] = {SOURCE_WEB, SOURCE_SERIAL, SOURCE_ESPNOW,
                                      SOURCE_LOCAL_DISPLAY, SOURCE_BLUETOOTH, SOURCE_MQTT,
                                      SOURCE_VOICE, SOURCE_G2_GLASSES, SOURCE_UART};
  const char* paths[] = {"/api/cli", "/register/submit"};
  for (CommandSource transport : transports) {
    for (const char* path : paths) {
      reset();
      AuthContext ctx{};
      ctx.transport = transport;
      ctx.path = path;
      ctx.ip = kClientIp;
      std::string reply;
      check(!authorize(ctx, "userrequest alice alice-secret1 alice-secret1", &kUserRequestEntry, reply));
      check(reply.find("Authentication required") != std::string::npos);
      check(deniedAudits == 1);
      check(ownerCalls.empty());
    }
  }
  // The pre-auth exception stays `login` alone, and a named caller may still
  // run `userrequest`.
  reset();
  AuthContext anonymous{};
  anonymous.transport = SOURCE_WEB;
  anonymous.path = "/api/cli";
  std::string reply;
  check(authorize(anonymous, "login alice alice-secret1", &kLoginEntry, reply));
  AuthContext named = anonymous;
  named.user = "alice";
  check(authorize(named, "userrequest bob bob-secret1", &kUserRequestEntry, reply));
}

#ifndef HW1_NO_ACCOUNT_REQUEST_ADAPTER
// Validate-only checks the arguments and stops before the owner: no pending
// list read or write, no event.
static void commandValidateOnlyHasNoEffect() {
  reset();
  gCLIValidateOnly = true;
  check(std::string(cmd_user_request("alice alice-secret1 alice-secret1")) == "VALID");
  check(std::string(cmd_user_request("alice \"two words\"")) == "VALID");
  check(ownerCalls.empty());
  // Argument errors are still reported in validate-only mode.
  check(std::string(cmd_user_request("alice")).rfind("Error: invalid arguments", 0) == 0);
  check(std::string(cmd_user_request("alice one-secret two-secret")) == "Error: passwords do not match");
  check(std::string(cmd_user_request("alice short")) == "Error: Password must be at least 6 characters");
  check(std::string(cmd_user_request("bad/name alice-secret1")) == "Error: Invalid username");
  check(ownerCalls.empty());
}

// The command adapter hands a quoted passphrase to the owner byte-exact and
// reports the owner's refusals.
static void commandQuotedPassphraseReachesOwner() {
  reset();
  const std::string reply = cmd_user_request("alice \"correct horse battery\" \"correct horse battery\"");
  check(ownerCalls.size() == 1);
  check(ownerCalls[0].username == "alice");
  check(ownerCalls[0].password == "correct horse battery");
  check(reply == "Request submitted for 'alice' (JSON)");
  reset();
  ownerReply = AccountRequestResult::QueueFull;
  check(std::string(cmd_user_request("bob bob-secret1")) == "Error: Too many pending requests");
  ownerReply = AccountRequestResult::AlreadyPending;
  check(std::string(cmd_user_request("bob bob-secret1")) == "Error: Username already pending approval");
  ownerReply = AccountRequestResult::WriteFailed;
  check(std::string(cmd_user_request("bob bob-secret1")) == "Error: could not write pending list");
  check(ownerCalls.size() == 3);
}
#else
static void noAdapter() {
  std::fprintf(stderr,
               "register submit check failed: cmd_user_request does not file through "
               "submitAccountRequest on this tree (it does its own pending-list I/O)\n");
  std::exit(1);
}
static void commandValidateOnlyHasNoEffect() { noAdapter(); }
static void commandQuotedPassphraseReachesOwner() { noAdapter(); }
#endif

int main(int argc, char** argv) {
  struct Case { const char* name; void (*run)(); };
  const Case cases[] = {
      {"web_spaced_passphrase", spacedPassphraseReachesOwner},
      {"web_repeated_words", repeatedWordsAreNotCut},
      {"web_quotes_and_escapes", quotesAndEscapesReachOwner},
      {"web_owner_refusals", ownerRefusalsDoNotCount},
      {"web_rejected_fields", rejectedFieldsStillCount},
      {"anonymous_userrequest_refused", anonymousUserRequestStillRefused},
      {"command_validate_only", commandValidateOnlyHasNoEffect},
      {"command_quoted_passphrase", commandQuotedPassphraseReachesOwner},
  };
  bool ran = false;
  for (const Case& c : cases) {
    if (argc > 1 && std::strcmp(argv[1], c.name) != 0) continue;
    c.run();
    ran = true;
  }
  if (!ran) {
    std::fprintf(stderr, "unknown case\n");
    return 2;
  }
  std::printf("register submit harness passed (%u checks)\n", checks);
  return 0;
}
