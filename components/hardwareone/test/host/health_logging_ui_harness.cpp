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
static uint32_t millis() { return 1234; }
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
static constexpr int ESP_OK = 0, HTTPD_RESP_USE_STRLEN = -1, HTTPD_SOCK_ERR_TIMEOUT = -2;
static constexpr int SOURCE_WEB = 1, ORIGIN_WEB = 2, MSG_ROUTE_WEB = 1, MSG_ROUTE_FILE = 2;
static constexpr int COMMAND_CONTEXT_REQUIRE_LIVE_SESSION = 1, COMMAND_CONTEXT_MODE_INDEPENDENT = 2;
struct AuthContext { String user = "alice", sid = "session-a", ip = "local"; int transport = SOURCE_WEB; };
struct CommandContext {
  int origin = 0, outputMask = 0, behaviorFlags = 0;
  AuthContext auth;
  uint32_t id = 0, timestampMs = 0;
  TransportSessionEpoch transportSessionEpoch = 0;
  bool validateOnly = false;
  void* replyHandle = nullptr; void* httpReq = nullptr;
};
struct Command { String line; CommandContext ctx; };
struct httpd_req_t {
  std::string input, output, status = "200 OK", type;
  size_t content_len = 0, received = 0;
  unsigned sends = 0;
};
static AuthContext webIdentity;
static bool authenticated = true, guestAllowed = true, guestUser, adminUser;
static bool liveSession = true, revokeOnExecute, commandOk = true;
static String commandOutput = "SUCCESS: Health logging ON";
static std::vector<Command> webRequests;
static std::vector<std::string> broadcasts;
static AuthContext makeWebAuthCtx(httpd_req_t*) { return webIdentity; }
static bool tgRequireAuth(const AuthContext&) { return authenticated; }
static bool webGuestAccessAllowed(httpd_req_t*, const AuthContext&) { return guestAllowed; }
static bool isGuestUser(const String&) { return guestUser; }
static bool isAdminUser(const String&) { return adminUser; }
static void httpd_resp_set_type(httpd_req_t* r, const char* type) { r->type = type; }
static void httpd_resp_set_status(httpd_req_t* r, const char* status) { r->status = status; }
static void httpd_resp_send(httpd_req_t* r, const char* body, int) { r->output = body; ++r->sends; }
static int httpd_req_recv(httpd_req_t* r, char* out, size_t length) {
  const size_t count = std::min({length, r->input.size() - r->received, size_t(3)});
  std::memcpy(out, r->input.data() + r->received, count); r->received += count;
  return static_cast<int>(count);
}
static TransportSessionEpoch captureTransportSessionEpoch(const AuthContext& ctx) {
  return ctx.sid.length() ? 42 : 0;
}
static bool transportSessionEpochIsLive(int source, TransportSessionEpoch epoch) {
  CHECK(source == SOURCE_WEB && epoch == 42); return liveSession;
}
static bool submitAndExecuteSync(const Command& command, String& out) {
  webRequests.push_back(command); out = commandOutput;
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
static bool g2RingRequestHistoryRefresh(bool) { ++ringMutations; return ringAccepted; }
static bool g2RingSetHealthCollectionDesired(G2RingDesiredState) { ++ringMutations; return ringAccepted; }
static bool g2RingSetLowPowerDesired(G2RingDesiredState) { ++ringMutations; return ringAccepted; }

static uint8_t sHealthSel;
static struct { bool up = false, down = false; } gNavEvents;
static bool oledGuest;
static std::vector<std::string> oledRequests;
static bool oledGuestBlocksMutate() { return oledGuest; }
static void healthKickPollBurst() { ++pollRequests; }
static bool healthRequireAdmin() { return adminUser; }
static void executeOLEDCommand(const String& command) { oledRequests.emplace_back(command.c_str()); }
enum { INPUT_BUTTON_A = 1, INPUT_BUTTON_X = 2, INPUT_BUTTON_B = 4 };
#define INPUT_CHECK(value, button) ((value) & (button))

// INSERT_PRODUCTION

static httpd_req_t request(const char* body) {
  httpd_req_t r; r.input = body; r.content_len = r.input.size();
  CHECK(handleR1HealthAction(&r) == ESP_OK); return r;
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
  CHECK(g2Requests.back().command == "healthlogging toggle");
  CHECK(g2Requests.back().cookie.targetPage == G2_HIJACK_PAGE_HEALTH);
  CHECK(g2Requests.back().cookie.targetNetSub == HEALTH_NAV_MAIN);
  CHECK(g2Requests.back().done != nullptr && g2Requests.back().user == nullptr);
  CHECK(!loggerActive);
  const unsigned beforeDiagnostic = diagnosticMessages;
  g2Requests.back().done(false, "Error: denied", g2Requests.back().cookie, nullptr);
  g2Requests.back().done(true, "SUCCESS: Health logging ON", g2Requests.back().cookie, nullptr);
  CHECK(diagnosticMessages == beforeDiagnostic + 2);
  CHECK(!loggerActive && g2Requests.size() == 1);
  g2HealthAction(10); g2HealthOpen(); drainLoggingRequest();
  CHECK(g2Requests.size() == 1);
  g2HealthAction(10); g2HealthSetPageActive(false); drainLoggingRequest();
  CHECK(g2Requests.size() == 1 && !sPageActive);
  g2HealthSetPageActive(true); g2HealthAction(10);
  g2QueueAccepts = false; drainLoggingRequest();
  CHECK(g2Requests.size() == 1 && !g2HealthConsumeLoggingToggleRequest());
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
  const size_t loggingRequests = webRequests.size();
  CHECK(request("action=poll").status == "202 Accepted");
  CHECK(request("action=history-force").status == "403 Forbidden");
  adminUser = true;
  CHECK(request("action=history-force").status == "202 Accepted");
  CHECK(request("action=collection&value=invalid").status == "400 Bad Request");
  CHECK(request("action=collection&value=on").status == "202 Accepted");
  CHECK(webRequests.size() == loggingRequests);

  sHealthSel = HEALTH_ACTION_LOGGING;
  oledGuest = true;
  CHECK(healthInputHandler(0, 0, INPUT_BUTTON_A) && oledRequests.empty());
  oledGuest = false;
  CHECK(healthInputHandler(0, 0, INPUT_BUTTON_A));
  CHECK(healthInputHandler(0, 0, INPUT_BUTTON_X));
  CHECK(oledRequests.size() == 2 && oledRequests[0] == "healthlogging toggle" && oledRequests[1] == "healthlogging toggle");
  CHECK(!healthInputHandler(0, 0, INPUT_BUTTON_B) && oledRequests.size() == 2);
  std::printf("health_logging_ui: %u checks passed\n", checks);
}
