// Only transport/auth/executor boundaries are mocked; routing and JSON emission
// below are extracted from shipping sources by test_map_live_ui.py.
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
#include <Arduino.h>
#include <algorithm>
#include <ArduinoJson.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
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
// INSERT_COMMAND_TYPES
struct httpd_req_t {
  std::string query, output, status = "200 OK", type;
  unsigned sends = 0, chunks = 0, finishes = 0;
};
static std::function<void()> onFirstChunk;
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
static unsigned captureCalls, millisCalls;
static uint32_t nextMillis = 1234;
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
static uint32_t millis() { ++millisCalls; return nextMillis++; }
static AuthContext makeWebAuthCtx(httpd_req_t* req) {
  expectedRequest = req;
  return webIdentity;
}
static bool tgRequireAuth(const AuthContext&) { return authenticated; }
static bool webGuestAccessAllowed(httpd_req_t*, const AuthContext&) { return guestAllowed; }
static void httpd_resp_set_type(httpd_req_t* req, const char* type) { req->type = type; }
static void httpd_resp_set_status(httpd_req_t* req, const char* status) { req->status = status; }
static void httpd_resp_sendstr(httpd_req_t* req, const char* body) { req->output = body; ++req->sends; }
static void httpd_resp_sendstr_chunk(httpd_req_t* req, const char* body) {
  if (!body) { ++req->finishes; return; }
  req->output += body;
  if (++req->chunks == 1 && onFirstChunk) onFirstChunk();
}
// The Guest mutation gate (WebServer_Utils): a Guest session gets the 403 reply.
static bool guestSession;
static unsigned forbidden;
static bool webGuestMutationAllowed(httpd_req_t* req, const AuthContext&) {
  if (!guestSession) return true;
  ++forbidden;
  httpd_resp_set_status(req, "403 Forbidden");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"guest_forbidden\",\"message\":\"Guest accounts are view-only\"}");
  return false;
}
static String urlDecode(const String& value) { return value; }
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

// INSERT_TRACK_TYPES

static GPSTrackStatus currentTrack;
static std::vector<GPSTrackPoint> suppliedPoints;
static int snapshotCalls, statusCalls, loadCalls, statusLoadCalls, saveCalls, requestedMaxPoints;
static bool snapshotOk = true, loadOk = true, saveOk = true;
static String loadError = "File not found";
static int savedCount = 37;
static bool rejectOutside;
static std::string loadedPath;
static std::function<void()> onTrackLoaded;
static TrackValidation messageValidation;
static float messageCoverage;
// Return a supplied owner snapshot. The owner suite tests how it is captured;
// this suite verifies that a consumer retains it throughout callbacks/output.
static void supplySnapshot(GPSTrackSnapshot& out) {
  out = {};
  out.status = currentTrack;
  out.pointCount = static_cast<int>(suppliedPoints.size());
  if (!suppliedPoints.empty()) {
    out.points.reset(static_cast<GPSTrackPoint*>(std::malloc(sizeof(GPSTrackPoint) * suppliedPoints.size())));
    CHECK(out.points != nullptr);
    std::memcpy(out.points.get(), suppliedPoints.data(), sizeof(GPSTrackPoint) * suppliedPoints.size());
  }
}
struct GPSTrackManager {
  static bool saveTrack(char* path, size_t length, int* count = nullptr) {
    ++saveCalls;
    if (count) *count = saveOk ? savedCount : 0;
    std::snprintf(path, length, "%s", saveOk ? "/logging_captures/tracks/saved.csv" : "");
    return saveOk;
  }
  static GPSTrackStatus readStatus() { ++statusCalls; return currentTrack; }
  static bool readSnapshot(GPSTrackSnapshot& out, int maxPoints = MAX_TRACK_POINTS) {
    CHECK(identityDepth == 1 && installedIdentity);
    ++snapshotCalls; requestedMaxPoints = maxPoints;
    if (!snapshotOk) { out = {}; return false; }
    supplySnapshot(out); return true;
  }
  static bool loadTrack(const char* path, String& error, GPSTrackSnapshot& out,
                        bool rejectOutOfBounds = false) {
    CHECK(identityDepth == 1 && installedIdentity);
    ++loadCalls; loadedPath = path; rejectOutside = rejectOutOfBounds;
    if (!loadOk) { error = loadError; out = {}; return false; }
    supplySnapshot(out);
    if (onTrackLoaded) onTrackLoaded();
    return true;
  }
  // Status-only load: publishes, returns the published status, allocates no copy.
  static bool loadTrack(const char* path, String& error, GPSTrackStatus& out,
                        bool rejectOutOfBounds = false) {
    CHECK(identityDepth == 1 && installedIdentity);
    ++loadCalls; ++statusLoadCalls; loadedPath = path; rejectOutside = rejectOutOfBounds;
    if (!loadOk) { error = loadError; out = {}; return false; }
    out = currentTrack;
    if (onTrackLoaded) onTrackLoaded();
    return true;
  }
  static String getValidationMessage(TrackValidation validation, float coverage) {
    messageValidation = validation; messageCoverage = coverage;
    return "Track snapshot validation";
  }
};

static constexpr int DISPLAY_COLOR_WHITE = 1, OLED_CONTENT_START_Y = 0, OLED_CONTENT_HEIGHT = 54;
static std::function<void()> onFirstDraw;
struct TestDisplay {
  std::vector<std::string> text;
  void setTextSize(int value) {
    CHECK(value == 1);
    if (onFirstDraw) onFirstDraw();
  }
  void setTextColor(int value) { CHECK(value == DISPLAY_COLOR_WHITE); }
  void setCursor(int, int) {}
  void print(const char* value) { text.emplace_back(value); }
};
static TestDisplay display;
static TestDisplay* oledDisplay = &display;

// INSERT_PRODUCTION

static httpd_req_t request(const char* query) {
  httpd_req_t req; req.query = query;
  const unsigned beforeMillis = millisCalls, beforeCapture = captureCalls;
  const size_t beforeCommands = webRequests.size();
  CHECK(handleGPSTracksAPI(&req) == ESP_OK);
  if (webRequests.size() > beforeCommands) {
    CHECK(webRequests.size() == beforeCommands + 1);
    CHECK(millisCalls == beforeMillis + 2 && captureCalls == beforeCapture + 1);
  }
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

static void webContextHelperTests() {
#if HAVE_WEB_CONTEXT_HELPER
  // The factory copies identity and supplies defaults only. Session capture,
  // mode policy and execution remain with its caller.
  for (const char* sid : {"session-a", ""}) {
    for (TransportSessionEpoch epoch : {TransportSessionEpoch(42), TransportSessionEpoch(0)}) {
      httpd_req_t req;
      AuthContext auth = fixtureWebIdentity();
      auth.sid = sid;
      auth.transport = SOURCE_ESPNOW; // Copy the provided identity without rewriting it.
      capturedEpoch = epoch;
      const unsigned beforeCapture = captureCalls, beforeMillis = millisCalls;
      const uint32_t firstMillis = nextMillis;
      const auto ctx = makeWebCommandContext(&req, auth);
      CHECK(captureCalls == beforeCapture && millisCalls == beforeMillis + 2);
      CHECK(ctx.id == firstMillis && ctx.timestampMs == firstMillis + 1);
      CHECK(ctx.origin == ORIGIN_WEB && ctx.auth.transport == SOURCE_ESPNOW);
      CHECK(ctx.auth.user == auth.user && ctx.auth.sid == auth.sid && ctx.auth.ip == auth.ip);
      CHECK(ctx.auth.path == auth.path && ctx.auth.scope == auth.scope && ctx.auth.opaque == auth.opaque);
      CHECK(ctx.httpReq == &req && static_cast<void*>(ctx.httpReq) != auth.opaque);
      CHECK(ctx.outputMask == (MSG_ROUTE_WEB | MSG_ROUTE_FILE));
      CHECK(ctx.behaviorFlags == (auth.sid.length() ? COMMAND_CONTEXT_REQUIRE_LIVE_SESSION : COMMAND_CONTEXT_DEFAULT));
      CHECK(ctx.transportSessionEpoch == 0 && ctx.settingsBatchId == 0);
      CHECK(ctx.authoritySessionEpoch == 0 && ctx.authorityId == 0);
      CHECK(!ctx.validateOnly && !ctx.captureOutput && ctx.replyHandle == nullptr);
      CHECK(std::all_of(std::begin(ctx.automationName), std::end(ctx.automationName), [](char c) { return c == 0; }));
      auth.user = "changed-user"; auth.path = "/changed-path"; auth.scope = "/changed-scope"; auth.sid = "changed-session";
      CHECK(ctx.auth.user == "alice" && ctx.auth.path == "/api/fixture" && ctx.auth.scope == "/fixture-scope");
      CHECK(ctx.auth.sid == sid);
    }
  }
#endif
  capturedEpoch = 42; nextMillis = 1234; captureCalls = millisCalls = 0;
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
  CHECK(ctx.transportSessionEpoch == 42 && ctx.id == 1234 && ctx.timestampMs == 1235);
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
  // A cookie whose epoch cannot be captured must still carry REQUIRE. The
  // executor owns rejection; this facade must also suppress stale delivery.
  webIdentity.sid = "session-a"; capturedEpoch = 0;
  const unsigned beforeFailedCapture = broadcasts;
  checkError(request("live=start"), "Error: web session changed before command result delivery.");
  CHECK(webRequests.back().ctx.transportSessionEpoch == 0);
  CHECK(webRequests.back().ctx.behaviorFlags == (COMMAND_CONTEXT_REQUIRE_LIVE_SESSION | COMMAND_CONTEXT_MODE_INDEPENDENT));
  CHECK(broadcasts == beforeFailedCapture);
  // Basic Auth remains intentionally stateless and can receive its result.
  webIdentity.sid = ""; capturedEpoch = 42;
  commandOk = true; commandOutput = "SUCCESS: Live tracking started";
  CHECK(request("live=start").output == "{\"live\":true,\"started\":true}");
  CHECK(webRequests.back().ctx.transportSessionEpoch == 0);
  CHECK(webRequests.back().ctx.behaviorFlags == COMMAND_CONTEXT_MODE_INDEPENDENT);
  const size_t count = webRequests.size();
  request("other=start"); request("live=start-extra"); request("");
  CHECK(webRequests.size() == count);

  // A Guest session keeps the read-only surface: recording control, saving
  // and loading are refused by the facade before any owner or executor call.
  guestSession = true;
  const size_t commandsBefore = webRequests.size();
  const int loadsBefore = loadCalls, savesBefore = saveCalls, snapshotsBefore = snapshotCalls;
  for (const char* query : {"live=start", "live=stop", "live=save", "file=/tracks/original.csv"}) {
    const auto denied = request(query);
    CHECK(denied.status == "403 Forbidden" && denied.type == "application/json");
    CHECK(denied.sends == 1 && denied.chunks == 0 && denied.finishes == 0);
    CHECK(denied.output.find("guest_forbidden") != std::string::npos);
  }
  CHECK(forbidden == 4 && webRequests.size() == commandsBefore);
  CHECK(loadCalls == loadsBefore && saveCalls == savesBefore && snapshotCalls == snapshotsBefore);
  const auto readOnly = request("live=1");
  CHECK(readOnly.status == "200 OK" && readOnly.finishes == 1 && readOnly.sends == 0);
  CHECK(forbidden == 4 && snapshotCalls == snapshotsBefore + 1);
  guestSession = false;
}
static void setTrackFixture() {
  currentTrack = {};
  currentTrack.pointCount = 503;
  currentTrack.live = true;
  currentTrack.lastUpdateMs = 9123;
  currentTrack.stats = {1200.0f, 120.0f, 10.0f, true};
  currentTrack.validation = TRACK_PARTIAL;
  currentTrack.coveragePercent = 75.0f;
  std::snprintf(currentTrack.filename, sizeof(currentTrack.filename), "/tracks/original.csv");
  suppliedPoints.clear();
  for (int i = 0; i < 500; ++i) {
    suppliedPoints.push_back({static_cast<float>(i % 80), static_cast<float>(i % 170),
                             static_cast<unsigned long>(i)});
  }
}
static void replaceTrack() {
  currentTrack = {};
  currentTrack.pointCount = 1;
  std::snprintf(currentTrack.filename, sizeof(currentTrack.filename), "/tracks/replacement.csv");
  suppliedPoints = {{-20.0f, -50.0f, 999}};
}
static void dataTests() {
  setTrackFixture();
  onFirstChunk = replaceTrack;
  const auto beforeSnapshot = snapshotCalls;
  auto req = request("live=1");
  CHECK(snapshotCalls == beforeSnapshot + 1);
  CHECK(req.status == "200 OK" && req.type == "application/json");
  CHECK(req.sends == 0 && req.chunks == 502 && req.finishes == 1);
  CHECK(requestedMaxPoints == 500);
  JsonDocument parsed;
  CHECK(!deserializeJson(parsed, req.output));
  CHECK(parsed["live"].as<bool>() && parsed["count"].as<int>() == 503);
  CHECK(parsed["distance"].as<float>() == 1200.0f && parsed["duration"].as<float>() == 120.0f);
  CHECK(parsed["speed"].as<float>() == 10.0f && parsed["lastUpdate"].as<int>() == 9123);
  CHECK(parsed["points"].size() == 500);
  for (int i = 0; i < 500; ++i) {
    CHECK(parsed["points"][i]["lat"].as<int>() == i % 80);
    CHECK(parsed["points"][i]["lon"].as<int>() == i % 170);
  }
  CHECK(currentTrack.pointCount == 1 && !currentTrack.live);

  // A failed copy must finish before streaming starts and cannot masquerade
  // as an empty successful track.
  snapshotOk = false;
  req = request("live=1");
  CHECK(req.status == "503 Service Unavailable" && req.sends == 1);
  CHECK(req.chunks == 0 && req.finishes == 0);
  CHECK(!deserializeJson(parsed, req.output) && parsed["error"].is<const char*>());
  CHECK(!parsed["points"].is<JsonArray>());
  snapshotOk = true; onFirstChunk = nullptr;
  currentTrack = {}; suppliedPoints.clear();
  req = request("live=1");
  CHECK(!deserializeJson(parsed, req.output));
  CHECK(parsed["count"].as<int>() == 0 && parsed["points"].size() == 0);

  // The file reply publishes through the status-only load (freeing the old
  // track first), then copies the published track once and describes that
  // one snapshot: status and points together. Replacing the owner at the
  // first chunk cannot change what was copied.
  setTrackFixture();
  suppliedPoints.resize(3); currentTrack.pointCount = 3;
  onFirstChunk = replaceTrack;
  const int loadsBefore = loadCalls, statusLoadsBefore = statusLoadCalls, snapshotsBefore = snapshotCalls;
  req = request("file=/tracks/original.csv");
  CHECK(loadCalls == loadsBefore + 1 && statusLoadCalls == statusLoadsBefore + 1);
  CHECK(snapshotCalls == snapshotsBefore + 1 && requestedMaxPoints == MAX_TRACK_POINTS);
  CHECK(messageValidation == TRACK_PARTIAL && messageCoverage == 75.0f);
  CHECK(loadedPath == "/tracks/original.csv" && !rejectOutside);
  CHECK(req.chunks == 5 && req.finishes == 1 && req.sends == 0);
  CHECK(!deserializeJson(parsed, req.output));
  CHECK(parsed["success"].as<bool>() && parsed["count"].as<int>() == 3);
  CHECK(std::strcmp(parsed["validation"].as<const char*>(), "partial") == 0);
  CHECK(parsed["coverage"].as<float>() == 75.0f);
  CHECK(parsed["points"].size() == 3 && parsed["points"][2]["lat"].as<int>() == 2);
  CHECK(std::strcmp(parsed["message"].as<const char*>(), "Track snapshot validation") == 0);
  CHECK(currentTrack.pointCount == 1);
  onFirstChunk = nullptr;
  // An owner replaced between publish and copy still yields one coherent
  // reply: the replacement's status and points, not a mix of generations.
  setTrackFixture();
  suppliedPoints.resize(3); currentTrack.pointCount = 3;
  onTrackLoaded = replaceTrack;
  req = request("file=/tracks/original.csv");
  CHECK(!deserializeJson(parsed, req.output));
  CHECK(parsed["success"].as<bool>() && parsed["count"].as<int>() == 1);
  CHECK(std::strcmp(parsed["validation"].as<const char*>(), "valid") == 0);
  CHECK(parsed["coverage"].as<float>() == 0.0f);
  CHECK(parsed["points"].size() == 1 && parsed["points"][0]["lat"].as<int>() == -20);
  CHECK(currentTrack.validation == TRACK_EMPTY && messageValidation == currentTrack.validation);
  CHECK(messageCoverage == currentTrack.coveragePercent);
  onTrackLoaded = nullptr;
  // A load that published but whose reply copy failed is reported as such,
  // never streamed as an empty success.
  setTrackFixture();
  snapshotOk = false;
  const int loadsBeforeCopyFailure = loadCalls;
  req = request("file=/tracks/original.csv");
  CHECK(loadCalls == loadsBeforeCopyFailure + 1);
  CHECK(req.status == "503 Service Unavailable" && req.sends == 1 && req.chunks == 0);
  CHECK(!deserializeJson(parsed, req.output) && parsed["error"].is<const char*>());
  CHECK(!parsed["points"].is<JsonArray>());
  snapshotOk = true;
  const auto beforeLoad = loadCalls;
  req = request("file=relative.csv");
  CHECK(loadCalls == beforeLoad && req.sends == 1 && req.chunks == 0);
  loadOk = false;
  req = request("file=/missing.csv");
  CHECK(!deserializeJson(parsed, req.output));
  CHECK(std::strcmp(parsed["error"].as<const char*>(), "File not found") == 0);
  CHECK(req.sends == 1 && req.chunks == 0);
  loadOk = true;

  // The saved count belongs to saveTrack, not whatever track is current when
  // the response is emitted.
  req = request("live=save");
  CHECK(!deserializeJson(parsed, req.output));
  CHECK(parsed["saved"].as<bool>() && parsed["points"].as<int>() == savedCount);
  CHECK(savedCount != currentTrack.pointCount);
  saveOk = false;
  req = request("live=save");
  CHECK(!deserializeJson(parsed, req.output));
  CHECK(!parsed["saved"].as<bool>() && parsed["error"].is<const char*>());
  CHECK(std::strcmp(parsed["error"].as<const char*>(), "Unable to save track data") == 0);
  saveOk = true;
}
static void oledStatusTests() {
  setTrackFixture();
  const auto before = statusCalls;
  display.text.clear();
  onFirstDraw = replaceTrack;
  drawTrackStatus();
  CHECK(statusCalls == before + 1);
  const std::vector<std::string> expected = {
    "LIVE TRACKING", "original.csv", "Points: 503", "Dist: 1.20km", "Time: 2:00",
    "10.0m/s", "Coverage: 75%", "B:OK"};
  CHECK(display.text == expected);
  CHECK(currentTrack.pointCount == 1);
  onFirstDraw = nullptr;
  currentTrack = {}; display.text.clear();
  drawTrackStatus();
  CHECK(display.text == std::vector<std::string>({"Track Status", "No track loaded", "B:OK"}));
  currentTrack.live = true; display.text.clear();
  drawTrackStatus();
  CHECK(display.text.front() == "LIVE TRACKING");
  CHECK(display.text[2] == "Points: 0");
  oledDisplay = nullptr;
  const auto noDisplay = statusCalls;
  drawTrackStatus();
  CHECK(statusCalls == noDisplay);
  oledDisplay = &display;
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
  gShowTrackStatus = false; saveOk = false;
  const auto beforeToast = toasts.size();
  executeSubmenuAction(MENU_CAT_TRACKS, 5);
  CHECK(!gShowTrackStatus && toasts.size() == beforeToast + 1);
  CHECK(toasts.back() == "Save track failed");
  saveOk = true;
  executeSubmenuAction(MENU_CAT_TRACKS, 5);
  CHECK(gShowTrackStatus && toasts.size() == beforeToast + 1);
}
int main() {
  webContextHelperTests(); webTests(); dataTests(); oledStatusTests(); oledTests();
  std::printf("Map live UI (GPS=%d): %u production-path assertions passed\n", ENABLE_GPS_SENSOR, checks);
}
