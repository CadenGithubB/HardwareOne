// Only transport/auth/executor boundaries are mocked; routing and JSON emission
// below are extracted from shipping sources by test_map_live_ui.py.
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static unsigned checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
  std::abort(); } } while (false)
void webBatchTestStringAppend() {}
#define PSRAM_JSON_DOC(name) JsonDocument name
using esp_err_t = int;
using TransportSessionEpoch = uint32_t;
static constexpr int ESP_OK = 0, ESP_FAIL = -1;
static constexpr int SOURCE_WEB = 1, ORIGIN_WEB = 2;
static constexpr int MSG_ROUTE_WEB = 1, MSG_ROUTE_FILE = 2;
static constexpr int COMMAND_CONTEXT_REQUIRE_LIVE_SESSION = 1, COMMAND_CONTEXT_MODE_INDEPENDENT = 2;
struct AuthContext { String user = "alice", sid = "session-a", ip = "local"; int transport = SOURCE_WEB; };
struct CommandContext {
  int origin = 0, outputMask = 0, behaviorFlags = 0;
  AuthContext auth;
  uint32_t id = 0, timestampMs = 0;
  TransportSessionEpoch transportSessionEpoch = 0;
  bool validateOnly = false;
  void* replyHandle = nullptr;
  void* httpReq = nullptr;
};
struct Command { String line; CommandContext ctx; };
struct httpd_req_t {
  std::string query, output, status = "200 OK", type;
  unsigned sends = 0;
};
static AuthContext webIdentity;
static bool authenticated = true, guestAllowed = true, liveSession = true;
static bool revokeOnExecute, commandOk = true;
static String commandOutput = "SUCCESS: Live tracking started";
static std::vector<Command> webRequests;
static unsigned broadcasts, identityDepth;
static const AuthContext* installedIdentity;
struct ExecIdentityGuard {
  const AuthContext* previous;
  explicit ExecIdentityGuard(const AuthContext& ctx) : previous(installedIdentity) {
    installedIdentity = &ctx; ++identityDepth;
  }
  ~ExecIdentityGuard() { installedIdentity = previous; --identityDepth; }
};
static uint32_t millis() { return 1234; }
static AuthContext makeWebAuthCtx(httpd_req_t*) { return webIdentity; }
static bool tgRequireAuth(const AuthContext&) { return authenticated; }
static bool webGuestAccessAllowed(httpd_req_t*, const AuthContext&) { return guestAllowed; }
static void httpd_resp_set_type(httpd_req_t* req, const char* type) { req->type = type; }
static void httpd_resp_set_status(httpd_req_t* req, const char* status) { req->status = status; }
static void httpd_resp_sendstr(httpd_req_t* req, const char* body) { req->output = body; ++req->sends; }
static int httpd_req_get_url_query_str(httpd_req_t* req, char* out, size_t length) {
  if (req->query.empty() || req->query.size() >= length) return ESP_FAIL;
  std::snprintf(out, length, "%s", req->query.c_str()); return ESP_OK;
}
static int httpd_query_key_value(const char* query, const char* key, char* out, size_t length) {
  const std::string text(query), prefix = std::string(key) + "=";
  size_t start = 0;
  while (start < text.size()) {
    const size_t end = text.find('&', start);
    const std::string entry = text.substr(start, end - start);
    if (entry.rfind(prefix, 0) == 0) {
      const std::string value = entry.substr(prefix.size());
      if (value.size() >= length) return ESP_FAIL;
      std::snprintf(out, length, "%s", value.c_str()); return ESP_OK;
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return ESP_FAIL;
}
static TransportSessionEpoch captureTransportSessionEpoch(const AuthContext& ctx) {
  return ctx.sid.length() ? 42 : 0;
}
static bool transportSessionEpochIsLive(int source, TransportSessionEpoch epoch) {
  CHECK(source == SOURCE_WEB && epoch == 42); return liveSession;
}
static bool submitAndExecuteSync(const Command& command, String& out) {
  CHECK(identityDepth == 1 && installedIdentity);
  CHECK(installedIdentity->user == command.ctx.auth.user);
  CHECK(installedIdentity->sid == command.ctx.auth.sid);
  CHECK(command.ctx.httpReq != nullptr);
  webRequests.push_back(command); out = commandOutput;
  if (revokeOnExecute) liveSession = false;
  return commandOk;
}
static bool cliModeOwnedBySession(int, TransportSessionEpoch) { return false; }
static String redactOutputForLog(const String& value) { return value; }
static void broadcastOutput(const String&, const CommandContext&) { ++broadcasts; }

enum { MENU_CAT_WAYPOINTS = 3, MENU_CAT_TRACKS = 4, MENU_CAT_GPS = 5 };
enum class OledUIIcon { ERROR };
static bool oledGuest, oledExecuted = true, sForceRender, gShowTrackStatus;
static String oledResult = "SUCCESS: Live tracking started";
static std::vector<std::string> oledRequests, toasts;
static int gTrackFileCount, gTrackSelectMode, gTrackFileIdx;
static bool oledGuestBlocksMutate() { return oledGuest; }
static bool executeOLEDCommandWithResult(const char* command, char* out, size_t length) {
  oledRequests.emplace_back(command); std::snprintf(out, length, "%s", oledResult.c_str());
  return oledExecuted;
}
static void oledToastShow(const char* text, unsigned duration, OledUIIcon icon) {
  CHECK(duration == 3000 && icon == OledUIIcon::ERROR); toasts.emplace_back(text);
}
static void scanTrackFiles() {}
static void goBackToMainMenu() {}
struct GPSTrackManager { static bool saveTrack(char*, size_t) { return true; } };

// INSERT_PRODUCTION

static httpd_req_t request(const char* query) {
  httpd_req_t req; req.query = query;
  CHECK(handleGPSTracksAPI(&req) == ESP_OK);
  CHECK(identityDepth == 0 && installedIdentity == nullptr);
  return req;
}
static void checkError(const httpd_req_t& req, const char* expected) {
  CHECK(req.status == "409 Conflict" && req.type == "application/json" && req.sends == 1);
  JsonDocument parsed;
  CHECK(!deserializeJson(parsed, req.output));
  CHECK(parsed["error"].is<const char*>());
  CHECK(std::strcmp(parsed["error"].as<const char*>(), expected) == 0);
  CHECK(!parsed["live"].is<bool>() && !parsed["started"].is<bool>() && !parsed["stopped"].is<bool>());
}
static void webTests() {
  authenticated = false;
  CHECK(request("live=start").sends == 0 && webRequests.empty());
  authenticated = true; guestAllowed = false;
  CHECK(request("live=stop").sends == 0 && webRequests.empty());
  guestAllowed = true;
  auto req = request("other=x&live=start");
  CHECK(req.status == "200 OK" && req.type == "application/json");
  CHECK(req.output == "{\"live\":true,\"started\":true}" && req.sends == 1);
  CHECK(webRequests.size() == 1 && webRequests.back().line == "gpstrack live start");
  const auto& ctx = webRequests.back().ctx;
  CHECK(ctx.auth.user == "alice" && ctx.auth.sid == "session-a" && ctx.auth.ip == "local");
  CHECK(ctx.auth.transport == SOURCE_WEB && ctx.origin == ORIGIN_WEB);
  CHECK(ctx.transportSessionEpoch == 42 && ctx.id == 1234 && ctx.timestampMs == 1234);
  CHECK(ctx.outputMask == (MSG_ROUTE_WEB | MSG_ROUTE_FILE));
  CHECK(ctx.behaviorFlags == (COMMAND_CONTEXT_REQUIRE_LIVE_SESSION | COMMAND_CONTEXT_MODE_INDEPENDENT));
  CHECK(!ctx.validateOnly && !ctx.replyHandle);
  commandOutput = "SUCCESS: Live tracking stopped";
  req = request("live=stop");
  CHECK(req.output == "{\"live\":false,\"stopped\":true}");
  CHECK(webRequests.size() == 2 && webRequests.back().line == "gpstrack live stop");
  // The executor, not the HTTP facade, owns command permissions. Its rejection
  // must retain guest/session identity and cannot be translated to success.
  webIdentity.user = "guest"; commandOk = false; commandOutput = "Error: guest cannot mutate";
  checkError(request("live=start"), commandOutput.c_str());
  CHECK(webRequests.back().ctx.auth.user == "guest");
  webIdentity.user = "alice"; commandOk = true;
  commandOutput = "Error: path \"bad\"\nGPS unavailable\\retry";
  checkError(request("live=start"), commandOutput.c_str());
  commandOutput = "";
  checkError(request("live=stop"), "Live track command failed");
  commandOutput = "Queued: command pending";
  checkError(request("live=start"), commandOutput.c_str());
  commandOutput = "SUCCESS: but executor failed"; commandOk = false;
  checkError(request("live=stop"), commandOutput.c_str());
  commandOk = true; commandOutput = "SUCCESS: started"; revokeOnExecute = true;
  const unsigned before = broadcasts;
  checkError(request("live=start"), "Error: web session changed before command result delivery.");
  CHECK(broadcasts == before);
  revokeOnExecute = false; liveSession = true;
  webIdentity.sid = ""; commandOk = false; commandOutput = "Error: missing session";
  checkError(request("live=stop"), commandOutput.c_str());
  CHECK(webRequests.back().ctx.transportSessionEpoch == 0);
  CHECK(webRequests.back().ctx.behaviorFlags == COMMAND_CONTEXT_MODE_INDEPENDENT);
  const size_t count = webRequests.size();
  request("other=start"); request("live=start-extra"); request("");
  CHECK(webRequests.size() == count);
}
static void oledAction(int action, const char* result, bool executed, bool success, const char* error = nullptr) {
  oledResult = result; oledExecuted = executed; sForceRender = false;
  const auto before = oledRequests.size(), toastBefore = toasts.size();
  executeSubmenuAction(MENU_CAT_TRACKS, action);
  CHECK(oledRequests.size() == before + 1);
  CHECK(oledRequests.back() == (action == 1 ? "gpstrack clear" : "gpstrack live toggle"));
  CHECK(sForceRender == success);
  CHECK(toasts.size() == toastBefore + (success ? 0 : 1));
  if (error) CHECK(toasts.back() == error);
}
static void oledTests() {
  oledGuest = true;
  executeSubmenuAction(MENU_CAT_TRACKS, 4); executeSubmenuAction(MENU_CAT_TRACKS, 1);
  CHECK(oledRequests.empty() && toasts.empty() && !sForceRender);
  oledGuest = false;
  oledAction(4, "SUCCESS: Live tracking started", true, true);
  oledAction(4, "SUCCESS: Live tracking stopped", true, true);
  oledAction(4, "Error: logger in use", true, false, "Error: logger in use");
  oledAction(4, "SUCCESS: stale session", false, false, "SUCCESS: stale session");
  oledAction(4, "", false, false, "Live track failed");
  oledAction(4, "Queued: pending", true, false, "Queued: pending");
  oledAction(1, "GPS track cleared", true, true);
  oledAction(1, "OK: GPS track cleared", true, true);
  oledAction(1, "Error: Stop live tracking first", true, false, "Error: Stop live tracking first");
  oledAction(1, "GPS track cleared", false, false, "GPS track cleared");
  oledAction(1, "", false, false, "Clear track failed");
}
int main() {
  webTests(); oledTests();
  std::printf("Map live UI (GPS=%d): %u production-path assertions passed\n", ENABLE_GPS_SENSOR, checks);
}
