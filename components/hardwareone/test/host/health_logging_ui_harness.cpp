// Shipping Health handlers replace the markers; only external boundaries are
// mocked. No logger decision is reproduced in this harness.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static unsigned checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
  std::abort(); } } while (false)
class String {
 public:
  String(const char* value = "") : value_(value ? value : "") {}
  String(const std::string& value) : value_(value) {}
  const char* c_str() const { return value_.c_str(); }
  size_t length() const { return value_.size(); }
  char charAt(size_t at) const { return at < value_.size() ? value_[at] : '\0'; }
  int indexOf(const String& value, size_t start = 0) const {
    const auto pos = value_.find(value.value_, start);
    return pos == std::string::npos ? -1 : static_cast<int>(pos);
  }
  int indexOf(char value, size_t start = 0) const {
    const auto pos = value_.find(value, start);
    return pos == std::string::npos ? -1 : static_cast<int>(pos);
  }
  String substring(size_t start, size_t end) const { return value_.substr(start, end - start); }
  String operator+(const String& other) const { return value_ + other.value_; }
  bool operator==(const String& other) const { return value_ == other.value_; }
  bool operator!=(const String& other) const { return value_ != other.value_; }
  bool startsWith(const char* prefix) const { return value_.rfind(prefix, 0) == 0; }
  void replace(const char* from, const char* to) {
    size_t pos = 0;
    while ((pos = value_.find(from, pos)) != std::string::npos) {
      value_.replace(pos, std::strlen(from), to); pos += std::strlen(to);
    }
  }
  void toLowerCase() {
    std::transform(value_.begin(), value_.end(), value_.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  }
 private:
  std::string value_;
};

// INSERT_TYPES

static G2HealthNav sNav = HEALTH_NAV_MAIN;
static G2HealthMetric sSelected = HEALTH_METRIC_OVERVIEW;
static bool sLoggingToggleRequest, sPollRequest, sHistoryRefreshRequest;
static bool sHistoryRefreshForce, sLoggedHint, sPageActive, sRefreshSupportedLast;
static uint32_t sPollUiMs, sLastSyncMs;
enum { POLL_UI_IDLE, POLL_UI_ACTIVE };
static uint8_t sPollUi;
static unsigned generation, graphPushes, historyRequests, unsupportedNotes;
static bool refreshSupported = true, loggerActive;
static unsigned millisCalls;
static uint32_t nextMillis = 1234;
static uint32_t millis() { ++millisCalls; return nextMillis++; }
static void bumpMenuGen() { ++generation; }
static void g2HealthClearGraphPush() { graphPushes = 0; }
static void g2HealthArmGraphPush(uint32_t) { ++graphPushes; }
static bool healthLoggingIsActive() { return loggerActive; }
static void syncFromTelemetry() {}
static bool g2RingHealthPageRefreshSupported() { return refreshSupported; }
static void requestHistoryRefresh(bool) { ++historyRequests; }
static void enterTrendsNav() { sNav = HEALTH_NAV_TRENDS; }
static void leaveTrendsNav() { sNav = HEALTH_NAV_MAIN; }
static void g2HealthNotePollUnsupported() { ++unsupportedNotes; }
static G2HealthNav g2HealthNav() { return sNav; }
static unsigned diagnosticMessages;
template <typename... Args> static void diagnostic(const char*, Args...) { ++diagnosticMessages; }
#define DEBUG_G2_PAGESF(...) diagnostic(__VA_ARGS__)
#define DEBUG_RING_HEALTHF(...) diagnostic(__VA_ARGS__)
#define DEBUG_HTTPF(...) diagnostic(__VA_ARGS__)

using G2Completion = void (*)(bool, const char*, const G2CmdCookie&, void*);
struct G2Request { std::string command; G2CmdCookie cookie; G2Completion done; void* user; };
static std::vector<G2Request> g2Requests;
static bool g2QueueAccepts = true;
static bool g2SubmitHijackCommand(const char* command, const G2CmdCookie& cookie,
                                 G2Completion done, void* user) {
  if (!g2QueueAccepts) return false;
  g2Requests.push_back({command, cookie, done, user}); return true;
}

using esp_err_t = int;
using TransportSessionEpoch = uint32_t;
static constexpr int ESP_OK = 0, HTTPD_RESP_USE_STRLEN = -1;
static constexpr int HTTPD_SOCK_ERR_FAIL = -1, HTTPD_SOCK_ERR_TIMEOUT = -3;  // esp_http_server.h values
struct httpd_req_t {
  std::string input, output, status = "200 OK", type;
  size_t content_len = 0, received = 0;
  unsigned sends = 0;
};
static int authOpaqueToken;
static AuthContext fixtureWebIdentity() {
  AuthContext auth{};
  auth.transport = SOURCE_WEB;
  auth.user = "alice";
  auth.sid = "session-a";
  auth.ip = "local";
  auth.path = "/api/fixture";
  auth.scope = "/fixture-scope";
  auth.opaque = &authOpaqueToken; // Deliberately distinct from the request pointer.
  return auth;
}
static AuthContext webIdentity = fixtureWebIdentity();
static httpd_req_t* expectedRequest;
static TransportSessionEpoch capturedEpoch = 42;
static unsigned captureCalls;
static bool authenticated = true, guestAllowed = true, guestUser, adminUser;
static bool liveSession = true, revokeOnExecute, commandOk = true;
static bool routeCommands;  // true: web/OLED commands run through registryExecute
static bool registryExecute(const AuthContext& who, const String& line, String& out);
static String commandOutput = "SUCCESS: Health logging ON";
static std::vector<Command> webRequests;
static std::vector<std::string> broadcasts;
static AuthContext makeWebAuthCtx(httpd_req_t* req) {
  expectedRequest = req;
  return webIdentity;
}
static bool tgRequireAuth(const AuthContext&) { return authenticated; }
static bool webGuestAccessAllowed(httpd_req_t*, const AuthContext&) { return guestAllowed; }
static bool isGuestUser(const String&) { return guestUser; }
static bool isAdminUser(const String&) { return adminUser; }
static void httpd_resp_set_type(httpd_req_t* r, const char* type) { r->type = type; }
static void httpd_resp_set_status(httpd_req_t* r, const char* status) { r->status = status; }
static void httpd_resp_send(httpd_req_t* r, const char* body, int) { r->output = body; ++r->sends; }
// Empty script: every call delivers up to 3 bytes. Otherwise one step per call:
// 'T' idle wait (recv_wait_timeout), 'E' socket error, 'D' deliver up to 3 bytes.
static std::string recvScript;
static unsigned recvCalls;
static int httpd_req_recv(httpd_req_t* r, char* out, size_t length) {
  ++recvCalls;
  if (!recvScript.empty()) {
    CHECK(recvCalls <= recvScript.size());  // an unbounded retry loop walks past the script
    const char step = recvScript[recvCalls - 1];
    if (step == 'T') return HTTPD_SOCK_ERR_TIMEOUT;
    if (step == 'E') return HTTPD_SOCK_ERR_FAIL;
  }
  const size_t count = std::min({length, r->input.size() - r->received, size_t(3)});
  std::memcpy(out, r->input.data() + r->received, count); r->received += count;
  return static_cast<int>(count);
}
static TransportSessionEpoch captureTransportSessionEpoch(const AuthContext& ctx) {
  ++captureCalls;
  CHECK(ctx.user == webIdentity.user && ctx.sid == webIdentity.sid && ctx.opaque == webIdentity.opaque);
  return ctx.sid.length() ? capturedEpoch : 0;
}
static bool transportSessionEpochIsLive(CommandSource source, TransportSessionEpoch epoch) {
  CHECK(source == SOURCE_WEB && epoch == capturedEpoch);
  return epoch != 0 && liveSession;
}
static void checkWebExecutionContext(const CommandContext& ctx) {
  CHECK(ctx.origin == ORIGIN_WEB && ctx.auth.transport == webIdentity.transport);
  CHECK(ctx.auth.user == webIdentity.user && ctx.auth.sid == webIdentity.sid && ctx.auth.ip == webIdentity.ip);
  CHECK(ctx.auth.path == webIdentity.path && ctx.auth.scope == webIdentity.scope && ctx.auth.opaque == webIdentity.opaque);
  CHECK(ctx.httpReq == expectedRequest && static_cast<void*>(ctx.httpReq) != ctx.auth.opaque);
  CHECK(ctx.outputMask == (MSG_ROUTE_WEB | MSG_ROUTE_FILE));
  CHECK(ctx.behaviorFlags == (COMMAND_CONTEXT_MODE_INDEPENDENT |
        (webIdentity.sid.length() ? COMMAND_CONTEXT_REQUIRE_LIVE_SESSION : COMMAND_CONTEXT_DEFAULT)));
  CHECK(ctx.timestampMs == ctx.id + 1 && nextMillis == ctx.timestampMs + 1);
  CHECK(!ctx.validateOnly && !ctx.captureOutput && ctx.replyHandle == nullptr);
  CHECK(ctx.settingsBatchId == 0 && ctx.authoritySessionEpoch == 0 && ctx.authorityId == 0);
  CHECK(std::all_of(std::begin(ctx.automationName), std::end(ctx.automationName), [](char c) { return c == 0; }));
}
static bool submitAndExecuteSync(const Command& command, String& out) {
  checkWebExecutionContext(command.ctx);
  webRequests.push_back(command);
  if (routeCommands) return registryExecute(command.ctx.auth, command.line, out);
  out = commandOutput;
  if (revokeOnExecute) liveSession = false;
  return commandOk;
}
static bool cliModeOwnedBySession(int, TransportSessionEpoch) { return false; }
static String redactOutputForLog(const String& value) { return value; }
static void broadcastOutput(const String& value, const CommandContext&) { broadcasts.emplace_back(value.c_str()); }

enum G2RingDesiredState { G2_RING_PRESERVE, G2_RING_OFF, G2_RING_ON };
enum { G2_RING_ERR_NONE, G2_RING_ERR_FEATURE_UNSUPPORTED };
struct G2RingControlStatus {
  bool protocolProfileKnown = true;
  int lowPowerLastError = G2_RING_ERR_NONE, healthLastError = G2_RING_ERR_NONE;
  G2RingDesiredState healthDesired = G2_RING_PRESERVE, lowPowerDesired = G2_RING_PRESERVE;
};
static G2RingControlStatus ringStatus;
static bool ringConnected = true, pollAccepted = true, ringAccepted = true;
static unsigned ringMutations, pollRequests;
static bool healthStartPollBurst() { ++pollRequests; return pollAccepted; }
static bool g2RingIsConnected() { return ringConnected; }
static void g2RingGetControlStatus(G2RingControlStatus& out) { out = ringStatus; }
static bool g2RingRefreshControlStatus() { ++ringMutations; return ringAccepted; }
static std::vector<std::string> ringChanges;  // what reached the ring owner, in order
static const char* desiredName(G2RingDesiredState d) {
  return d == G2_RING_ON ? "on" : d == G2_RING_OFF ? "off" : "preserve";
}
static bool g2RingRequestHistoryRefresh(bool force) {
  ++ringMutations;
  if (force && ringAccepted) ringChanges.push_back("history=force");
  return ringAccepted;
}
static bool g2RingSetHealthCollectionDesired(G2RingDesiredState d) {
  ++ringMutations; ringChanges.push_back(std::string("collection=") + desiredName(d));
  ringStatus.healthDesired = d; return true;
}
static bool g2RingSetLowPowerDesired(G2RingDesiredState d) {
  ++ringMutations; ringChanges.push_back(std::string("lowpower=") + desiredName(d));
  ringStatus.lowPowerDesired = d; return true;
}

// Command-layer doubles for the ring/health command handlers.
static bool gCLIValidateOnly;
static AuthContext execIdentity;  // identity installed for the running command
static bool currentExecIsAdmin() { return isAdminUser(execIdentity.user); }
class CommandArgs {
 public:
  explicit CommandArgs(const String& input) {
    const std::string text = input.c_str();
    size_t at = 0;
    while (at < text.size()) {
      while (at < text.size() && text[at] == ' ') ++at;
      const size_t end = std::min(text.find(' ', at), text.size());
      if (end > at) args_.emplace_back(text.substr(at, end - at));
      at = end;
    }
  }
  const String& arg(int index) const {
    return index >= 0 && index < count() ? args_[static_cast<size_t>(index)] : empty_;
  }
  int count() const { return static_cast<int>(args_.size()); }
  bool has(int index) const { return index >= 0 && index < count(); }
 private:
  std::vector<String> args_;
  String empty_;
};
static const char* notExercised() { return "Error: not exercised by this harness"; }
static const char* cmd_ringstatus(const String&) { return notExercised(); }
static const char* cmd_ringscan(const String&) { return notExercised(); }
static const char* cmd_ringconnect(const String&) { return notExercised(); }
static const char* cmd_ringdisconnect(const String&) { return notExercised(); }
static const char* cmd_ringquery(const String&) { return notExercised(); }
static const char* cmd_healthlogging(const String&) { return notExercised(); }
static const char* cmd_healthlogmerge(const String&) { return notExercised(); }

static uint8_t sHealthSel;
static struct { bool up = false, down = false; } gNavEvents;
static bool oledGuest;
static std::vector<std::string> oledRequests;
static bool oledGuestBlocksMutate() { return oledGuest; }
static void healthKickPollBurst() { ++pollRequests; }
[[maybe_unused]] static bool healthRequireAdmin() { return adminUser; }  // 0.99.97.4 OLED gate
static void executeOLEDCommand(const String& command) { oledRequests.emplace_back(command.c_str()); }
[[maybe_unused]] static uint32_t sHealthDeniedUntilMs;
[[maybe_unused]] static unsigned oledDirtyMarks;
[[maybe_unused]] static void oledMarkDirty() { ++oledDirtyMarks; }
static String oledUser = "bob";
static std::string lastOledResult;
// OLED_Utils.cpp: the display identity, the shared executor, the result copied
// into the caller's buffer.
[[maybe_unused]] static bool executeOLEDCommandWithResult(const String& command, char* out,
                                                          size_t outSize) {
  oledRequests.emplace_back(command.c_str());
  AuthContext display{};
  display.transport = SOURCE_LOCAL_DISPLAY; display.user = oledUser; display.ip = "oled";
  String result;
  const bool ok = registryExecute(display, command, result);
  std::snprintf(out, outSize, "%s", result.c_str());
  lastOledResult = out;
  return ok;
}
enum { INPUT_BUTTON_A = 1, INPUT_BUTTON_X = 2, INPUT_BUTTON_B = 4 };
#define INPUT_CHECK(value, button) ((value) & (button))

// INSERT_PRODUCTION

// Registry executor double for System_Utils.cpp executeCommand: longest
// whole-word prefix over the production ring and health tables, the registry
// admin gate and its refusal text (authorizeCommand), a refusal record
// (logAuthAttempt + SYSEVT_COMMAND_DENIED), the identity currentExecIsAdmin()
// reads, the audit record of logCommandExecution (user@source line -> OK/FAIL)
// and the OK: stamp.
struct AuditRecord { std::string who, line; bool ok; };
static std::vector<AuditRecord> audit;
static std::vector<std::string> deniedCommands;
static const char* sourceName(CommandSource source) {
  switch (source) {
    case SOURCE_WEB: return "web";
    case SOURCE_SERIAL: return "serial";
    case SOURCE_LOCAL_DISPLAY: return "display";
    default: return "other";
  }
}
static const CommandEntry* findRow(const std::string& line, size_t& matched) {
  const CommandEntry* entry = nullptr;
  matched = 0;
  auto consider = [&](const CommandEntry* rows, size_t count) {
    for (size_t i = 0; i < count; ++i) {
      const size_t n = std::strlen(rows[i].name);
      if (n > matched && line.compare(0, n, rows[i].name) == 0 &&
          (line.size() == n || line[n] == ' ')) {
        entry = &rows[i]; matched = n;
      }
    }
  };
  consider(g2RingCommands, sizeof(g2RingCommands) / sizeof(g2RingCommands[0]));
  consider(healthCommands, sizeof(healthCommands) / sizeof(healthCommands[0]));
  return entry;
}
static bool registryExecute(const AuthContext& who, const String& lineIn, String& out) {
  const std::string line = lineIn.c_str();
  const std::string actor = std::string(who.user.c_str()) + "@" + sourceName(who.transport);
  size_t matched = 0;
  const CommandEntry* entry = findRow(line, matched);
  if (!entry) { out = "Error: Unknown command"; return false; }
  if ((entry->requiresAdmin || entry->requiresSuperAdmin) && !isAdminUser(who.user)) {
    out = std::string("Error: Admin access required for command '") +
          line.substr(0, line.find(' ')) + "'. Contact an administrator.";
    deniedCommands.push_back(actor + " " + line);
    return false;
  }
  execIdentity = who;
  std::string result = entry->handler(String(matched < line.size() ? line.substr(matched + 1) : ""));
  execIdentity = AuthContext{};
  const bool ok = result.rfind("Error", 0) != 0;
  if (!gCLIValidateOnly) audit.push_back({actor, line, ok});
  if (ok && !gCLIValidateOnly && result.rfind("SUCCESS", 0) != 0 && result.rfind("OK", 0) != 0)
    result = "OK: " + result;
  out = result;
  return ok;
}

static httpd_req_t request(const char* body) {
  httpd_req_t r; r.input = body; r.content_len = r.input.size();
  const unsigned beforeMillis = millisCalls, beforeCapture = captureCalls;
  const size_t beforeCommands = webRequests.size();
  CHECK(handleR1HealthAction(&r) == ESP_OK);
  if (webRequests.size() > beforeCommands) {
    CHECK(webRequests.size() == beforeCommands + 1);
    CHECK(millisCalls == beforeMillis + 2 && captureCalls == beforeCapture + 1);
  }
  return r;
}

static httpd_req_t scripted(const char* body, size_t contentLen, const char* script) {
  httpd_req_t r; r.input = body; r.content_len = contentLen;
  recvScript = script; recvCalls = 0;
  CHECK(handleR1HealthAction(&r) == ESP_OK);
  recvScript.clear();
  return r;
}

int main() {
  CHECK(!disabled::g2HealthConsumeLoggingToggleRequest());
  g2HealthOpen();
  CHECK(!g2HealthConsumeLoggingToggleRequest());
  CHECK(sHistoryRefreshRequest && !sPollRequest);
  g2HealthAction(10); g2HealthAction(10);
  CHECK(g2Requests.empty());
  drainLoggingRequest(); drainLoggingRequest();
  CHECK(g2Requests.size() == 1);
  CHECK(sLoggingCmdPending && g2HealthLoggingStatusLine() == nullptr);  // queued: neutral label, no message
  CHECK(g2Requests.back().command == "healthlogging toggle");
  CHECK(g2Requests.back().cookie.targetPage == G2_HIJACK_PAGE_HEALTH);
  CHECK(g2Requests.back().cookie.targetNetSub == HEALTH_NAV_MAIN);
  CHECK(g2Requests.back().done != nullptr && g2Requests.back().user == nullptr);
  CHECK(!loggerActive);
  const unsigned beforeDiagnostic = diagnosticMessages;
  g2Requests.back().done(false, "Error: denied", g2Requests.back().cookie, nullptr);
  CHECK(!sLoggingCmdPending && g2HealthLoggingStatusLine() &&
        std::string(g2HealthLoggingStatusLine()) == "Error: denied");  // the lens can say why
  g2Requests.back().done(true, "SUCCESS: Health logging ON", g2Requests.back().cookie, nullptr);
  CHECK(!sLoggingCmdPending && g2HealthLoggingStatusLine() == nullptr);
  CHECK(diagnosticMessages == beforeDiagnostic + 2);
  CHECK(!loggerActive && g2Requests.size() == 1);
  g2HealthAction(10); g2HealthOpen(); drainLoggingRequest();
  CHECK(g2Requests.size() == 1);
  g2HealthAction(10); g2HealthSetPageActive(false); drainLoggingRequest();
  CHECK(g2Requests.size() == 1 && !sPageActive);
  g2HealthSetPageActive(true); g2HealthAction(10);
  g2QueueAccepts = false; drainLoggingRequest();
  CHECK(g2Requests.size() == 1 && !g2HealthConsumeLoggingToggleRequest());
  CHECK(!sLoggingCmdPending && g2HealthLoggingStatusLine() &&
        std::string(g2HealthLoggingStatusLine()) == "Not paired");  // executor refusal is visible
  g2QueueAccepts = true;
  g2HealthAction(3); g2HealthAction(10); drainLoggingRequest();
  CHECK(g2Requests.size() == 1 && sNav == HEALTH_NAV_TRENDS);
  g2HealthAction(0); g2HealthAction(9);
  CHECK(sNav == HEALTH_NAV_MAIN && sPollRequest);

  authenticated = false;
  CHECK(request("action=logging-toggle").sends == 0 && webRequests.empty());
  authenticated = true; guestAllowed = false;
  CHECK(request("action=logging-toggle").sends == 0 && webRequests.empty());
  guestAllowed = true; guestUser = true;
  CHECK(request("action=logging-toggle").status == "403 Forbidden" && webRequests.empty());
  guestUser = false;
  CHECK(request("").status == "400 Bad Request");
  CHECK(request("action=unknown").status == "400 Bad Request" && webRequests.empty());
  auto r = request("xaction=wrong&action=LOGGING-TOGGLE");
  CHECK(r.status == "200 OK" && r.output == "SUCCESS: Health logging ON");
  CHECK(r.type == "text/plain" && r.sends == 1 && webRequests.size() == 1);
  CHECK(webRequests.back().line == "healthlogging toggle");
  CHECK(webRequests.back().ctx.auth.user == "alice" && webRequests.back().ctx.auth.sid == "session-a");
  CHECK(webRequests.back().ctx.auth.transport == SOURCE_WEB && webRequests.back().ctx.origin == ORIGIN_WEB);
  CHECK(webRequests.back().ctx.transportSessionEpoch == 42);
  CHECK(webRequests.back().ctx.behaviorFlags == (COMMAND_CONTEXT_REQUIRE_LIVE_SESSION | COMMAND_CONTEXT_MODE_INDEPENDENT));
  CHECK(webRequests.back().ctx.outputMask == (MSG_ROUTE_WEB | MSG_ROUTE_FILE));
  CHECK(!webRequests.back().ctx.validateOnly && !webRequests.back().ctx.replyHandle);
  CHECK(ringMutations == 0 && pollRequests == 0);
  commandOutput = "Error: storage unavailable";
  CHECK(request("action=logging-toggle").status == "409 Conflict");
  commandOutput = "SUCCESS: but helper failed"; commandOk = false;
  CHECK(request("action=logging-toggle").status == "409 Conflict");
  commandOk = true; commandOutput = "SUCCESS: Health logging OFF"; revokeOnExecute = true;
  const size_t broadcastCount = broadcasts.size();
  r = request("action=logging-toggle");
  CHECK(r.status == "409 Conflict" && r.output == "Error: web session changed before command result delivery.");
  CHECK(broadcasts.size() == broadcastCount);
  revokeOnExecute = false; liveSession = true;
  capturedEpoch = 0; commandOk = false;
  r = request("action=logging-toggle");
  CHECK(r.status == "409 Conflict" && r.output == "Error: web session changed before command result delivery.");
  CHECK(webRequests.back().ctx.transportSessionEpoch == 0);
  CHECK(webRequests.back().ctx.behaviorFlags == (COMMAND_CONTEXT_REQUIRE_LIVE_SESSION | COMMAND_CONTEXT_MODE_INDEPENDENT));
  CHECK(broadcasts.size() == broadcastCount);
  webIdentity.sid = ""; capturedEpoch = 42; commandOk = true;
  r = request("action=logging-toggle");
  CHECK(r.status == "200 OK" && r.output == "SUCCESS: Health logging OFF");
  CHECK(webRequests.back().ctx.transportSessionEpoch == 0);
  CHECK(webRequests.back().ctx.behaviorFlags == COMMAND_CONTEXT_MODE_INDEPENDENT);
  CHECK(broadcasts.size() == broadcastCount + 1);
  webIdentity.sid = "session-a";
  const size_t loggingRequests = webRequests.size();
  CHECK(request("action=poll").status == "202 Accepted");
  CHECK(webRequests.size() == loggingRequests);

  // Action body reader: two consecutive idle waits end the request (half-open
  // socket), one idle wait between chunks is retried, a socket error never is.
  const unsigned pollBefore = pollRequests, ringBefore = ringMutations, diagBefore = diagnosticMessages;
  auto t = scripted("", 40, "TT");  // headers with Content-Length, body never arrives
  CHECK(t.status == "400 Bad Request" && t.output == "invalid action body");
  CHECK(recvCalls == 2 && t.received == 0 && t.sends == 1);
  t = scripted("action=poll", 11, "TDTDTDTD");
  CHECK(t.status == "202 Accepted" && recvCalls == 8 && t.received == 11);
  t = scripted("action=poll", 11, "DTT");  // half-open after the first chunk
  CHECK(t.status == "400 Bad Request" && recvCalls == 3 && t.received == 3);
  t = scripted("action=poll", 11, "E");
  CHECK(t.status == "400 Bad Request" && recvCalls == 1 && t.received == 0);
  CHECK(pollRequests == pollBefore + 1 && ringMutations == ringBefore);
  CHECK(diagnosticMessages == diagBefore + 3 && webRequests.size() == loggingRequests);

  sHealthSel = HEALTH_ACTION_LOGGING;
  oledGuest = true;
  CHECK(healthInputHandler(0, 0, INPUT_BUTTON_A) && oledRequests.empty());
  oledGuest = false;
  CHECK(healthInputHandler(0, 0, INPUT_BUTTON_A));
  CHECK(healthInputHandler(0, 0, INPUT_BUTTON_X));
  CHECK(oledRequests.size() == 2 && oledRequests[0] == "healthlogging toggle" && oledRequests[1] == "healthlogging toggle");
  CHECK(!healthInputHandler(0, 0, INPUT_BUTTON_B) && oledRequests.size() == 2);

  // Ring controls and the forced history refresh: web, OLED and CLI submit the
  // same command under their own identity. The registry row (ringcontrol) or
  // the healthstatus handler decides admin; nothing else does.
  routeCommands = true;
  adminUser = false;
  AuthContext serial{};
  serial.transport = SOURCE_SERIAL; serial.user = "root"; serial.ip = "local";
  size_t matched = 0;
  const CommandEntry* ringcontrolRow = findRow("ringcontrol collection on", matched);
  const std::string refusal =
      "Error: Admin access required for command 'ringcontrol'. Contact an administrator.";
  size_t webBefore = webRequests.size();
  r = request("action=collection&value=on");
  const bool webRingControlIsACommand = webRequests.size() == webBefore + 1 &&
      webRequests.back().line == "ringcontrol collection on";
  CHECK(webRingControlIsACommand);
  CHECK(ringcontrolRow && ringcontrolRow->requiresAdmin && matched == std::strlen("ringcontrol"));
  CHECK(r.status == "403 Forbidden" && r.output == refusal);
  r = request("action=low-power&value=off");
  CHECK(r.status == "403 Forbidden" && webRequests.back().line == "ringcontrol lowpower off");
  sHealthSel = HEALTH_ACTION_COLLECTION;
  size_t oledBefore = oledRequests.size();
  CHECK(healthInputHandler(0, 0, INPUT_BUTTON_A));
  CHECK(oledRequests.size() == oledBefore + 1 && oledRequests.back() == "ringcontrol collection on");
  CHECK(lastOledResult == refusal && sHealthDeniedUntilMs != 0 && oledDirtyMarks == 1);
  sHealthSel = HEALTH_ACTION_LOW_POWER;
  CHECK(healthInputHandler(0, 0, INPUT_BUTTON_X));
  CHECK(oledRequests.back() == "ringcontrol lowpower on" && lastOledResult == refusal);
  String out;
  CHECK(!registryExecute(serial, "ringcontrol collection on", out) && out == refusal);
  gCLIValidateOnly = true;  // the validate pass is refused at the same row
  CHECK(!registryExecute(serial, "ringcontrol lowpower on", out) && out == refusal);
  gCLIValidateOnly = false;
  CHECK((deniedCommands == std::vector<std::string>{
      "alice@web ringcontrol collection on", "alice@web ringcontrol lowpower off",
      "bob@display ringcontrol collection on", "bob@display ringcontrol lowpower on",
      "root@serial ringcontrol collection on", "root@serial ringcontrol lowpower on"}));
  CHECK(ringChanges.empty() && audit.empty());
  // healthstatus force-history: the same handler refusal from every transport.
  const std::string forceRefusal = "Error: admin required for forced history refresh";
  r = request("action=history-force");
  CHECK(webRequests.back().line == "healthstatus force-history");
  CHECK(r.status == "403 Forbidden" && r.output == forceRefusal);
  sHealthSel = HEALTH_ACTION_HISTORY_FORCE; sHealthDeniedUntilMs = 0;
  CHECK(healthInputHandler(0, 0, INPUT_BUTTON_A));
  CHECK(oledRequests.back() == "healthstatus force-history" && lastOledResult == forceRefusal);
  CHECK(sHealthDeniedUntilMs != 0);
  CHECK(!registryExecute(serial, "healthstatus force-history", out) && out == forceRefusal);
  CHECK(ringChanges.empty() && audit.size() == 3 && !audit[0].ok && !audit[1].ok && !audit[2].ok);
  CHECK(audit[0].who == "alice@web" && audit[1].who == "bob@display" && audit[2].who == "root@serial");

  // A bad form value never reaches a command line; bad CLI arguments are errors.
  adminUser = true; audit.clear();
  webBefore = webRequests.size();
  r = request("action=collection&value=on+reboot");
  CHECK(r.status == "400 Bad Request" && r.output == "value must be preserve, off, or on");
  CHECK(webRequests.size() == webBefore);
  CHECK(!registryExecute(serial, "ringcontrol collection maybe", out) &&
        out == "Error: value must be preserve, off, or on");
  CHECK(!registryExecute(serial, "ringcontrol volume on", out) && out.startsWith("Error: invalid arguments"));
  CHECK(!registryExecute(serial, "ringcontrol collection", out) && out.startsWith("Error: invalid arguments"));
  CHECK(!registryExecute(serial, "ringcontrol collection on extra", out) && out.startsWith("Error: invalid arguments"));
  CHECK(ringChanges.empty() && audit.size() == 4);
  gCLIValidateOnly = true;
  CHECK(registryExecute(serial, "ringcontrol collection on", out) && out == "VALID" && ringChanges.empty());
  gCLIValidateOnly = false;

  // Admin: every transport's change reaches the owner and is audited.
  audit.clear();
  r = request("action=collection&value=on");
  CHECK(r.status == "202 Accepted" && r.output ==
        "OK: health collection policy accepted; 0x0E has no proven readback, so ACK is not observed state");
  ringStatus.lowPowerLastError = G2_RING_ERR_FEATURE_UNSUPPORTED;
  r = request("action=low-power&value=OFF");
  CHECK(r.status == "202 Accepted" && webRequests.back().line == "ringcontrol lowpower off" && r.output ==
        "OK: low-power preference saved; unsupported on the active ring profile, so no command was sent");
  ringStatus.lowPowerLastError = G2_RING_ERR_NONE;
  sHealthDeniedUntilMs = 0;
  sHealthSel = HEALTH_ACTION_COLLECTION;  // on -> off
  CHECK(healthInputHandler(0, 0, INPUT_BUTTON_A) && oledRequests.back() == "ringcontrol collection off");
  sHealthSel = HEALTH_ACTION_LOW_POWER;   // off -> preserve
  CHECK(healthInputHandler(0, 0, INPUT_BUTTON_A) && oledRequests.back() == "ringcontrol lowpower preserve");
  CHECK(lastOledResult == "OK: low-power policy accepted; awaiting verified readback observation");
  CHECK(sHealthDeniedUntilMs == 0);
  CHECK(registryExecute(serial, "ringcontrol Collection Preserve", out) &&
        out == "OK: health collection policy accepted; 0x0E has no proven readback, so ACK is not observed state");
  r = request("action=history-force");
  CHECK(r.status == "202 Accepted" && r.output == "SUCCESS: R1 forced typed history refresh queued");
  sHealthSel = HEALTH_ACTION_HISTORY_FORCE;
  CHECK(healthInputHandler(0, 0, INPUT_BUTTON_A) && sHealthDeniedUntilMs == 0);
  CHECK(registryExecute(serial, "healthstatus force-history", out));
  CHECK((ringChanges == std::vector<std::string>{
      "collection=on", "lowpower=off", "collection=off", "lowpower=preserve", "collection=preserve",
      "history=force", "history=force", "history=force"}));
  CHECK(audit.size() == 8 && std::all_of(audit.begin(), audit.end(), [](const AuditRecord& a) { return a.ok; }));
  CHECK(audit[0].who == "alice@web" && audit[0].line == "ringcontrol collection on");
  CHECK(audit[2].who == "bob@display" && audit[2].line == "ringcontrol collection off");
  CHECK(audit[4].who == "root@serial" && audit[4].line == "ringcontrol Collection Preserve");
  CHECK(audit[6].who == "bob@display" && audit[6].line == "healthstatus force-history");
  CHECK(deniedCommands.size() == 6);
  // A refusal that is not about the role keeps its own status and still flashes on OLED.
  ringAccepted = false;
  r = request("action=history-force");
  CHECK(r.status == "409 Conflict" && r.output.rfind("Error: forced history refresh not queued", 0) == 0);
  CHECK(healthInputHandler(0, 0, INPUT_BUTTON_A) && sHealthDeniedUntilMs != 0);
  ringAccepted = true; routeCommands = false;
  std::printf("health_logging_ui: %u checks passed\n", checks);
}
