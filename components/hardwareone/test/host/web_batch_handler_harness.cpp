// Template compiled by test_web_batch_handlers.py, never as a separate target.
// Actual production definitions replace the marker below. Boundary mocks are
// deliberately small and do not reimplement either handler's control flow.
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

static unsigned assertions = 0;
static void check(bool condition, const char* message) {
  ++assertions;
  if (!condition) throw std::runtime_error(message);
}

class TestAllocator : public ArduinoJson::Allocator {
 public:
  size_t calls = 0;
  size_t failAt = static_cast<size_t>(-1);
  bool failed = false;
  std::unordered_map<void*, size_t> live;
  void reset() {
    check(live.empty(), "JSON allocations must be released after the handler");
    calls = 0; failAt = static_cast<size_t>(-1); failed = false;
  }
  bool reject() {
    if (calls++ != failAt) return false;
    failed = true;
    return true;  // One failure only; later allocation attempts can succeed.
  }
  void* allocate(size_t size) override {
    if (reject()) return nullptr;
    void* result = std::malloc(size);
    if (result) live[result] = size;
    return result;
  }
  void deallocate(void* ptr) override {
    if (!ptr) return;
    check(live.erase(ptr) == 1, "JSON allocation released exactly once");
    std::free(ptr);
  }
  void* reallocate(void* ptr, size_t size) override {
    if (!ptr) return allocate(size);
    if (reject()) return nullptr;
    const auto old = live.find(ptr);
    check(old != live.end(), "JSON realloc uses owned allocation");
    void* result = std::realloc(ptr, size);
    if (result) { live.erase(old); live[result] = size; }
    return result;
  }
};
static TestAllocator requestAllocator, responseAllocator;
static ArduinoJson::Allocator* testJsonAllocator(const char* name) {
  return std::strcmp(name, "respDoc") == 0 ? &responseAllocator : &requestAllocator;
}
#define PSRAM_JSON_DOC(name) JsonDocument name(testJsonAllocator(#name))

using esp_err_t = int;
using TransportSessionEpoch = uint32_t;
static constexpr int ESP_OK = 0, ESP_FAIL = -1;
static constexpr int HTTPD_500_INTERNAL_SERVER_ERROR = 500;
static constexpr int HTTPD_RESP_USE_STRLEN = -1;
static constexpr int SOURCE_WEB = 1, ORIGIN_WEB = 1, ORIGIN_SYSTEM = 2;
static constexpr int MSG_ROUTE_WEB = 1, MSG_ROUTE_FILE = 2;
static constexpr int COMMAND_CONTEXT_REQUIRE_LIVE_SESSION = 1;
static constexpr int COMMAND_CONTEXT_MODE_INDEPENDENT = 2;
enum class AllocPref { PreferPSRAM };
struct AuthContext { String sid = "session"; String user = "alice"; String ip = "local"; int transport = SOURCE_WEB; };
struct CommandContext {
  int origin = 0;
  AuthContext auth;
  uint32_t id = 0, timestampMs = 0, settingsBatchId = 0;
  int outputMask = 0, behaviorFlags = 0;
  TransportSessionEpoch transportSessionEpoch = 0;
  bool validateOnly = false;
  void* replyHandle = nullptr;
  void* httpReq = nullptr;
};
struct Command { String line; CommandContext ctx; };
struct httpd_req_t {
  std::string input, output, status = "200 OK", contentType;
  size_t content_len = 0, received = 0;
  int sends = 0;
};
struct Step {
  std::string output;
  bool succeeded = true, completed = true, revoke = false, stopServer = false;
};
struct Scenario {
  bool authenticated = true, guestAllowed = true, live = true, stateful = true;
  bool bondMaster = true, finalizerSucceeded = true, revokeOnSerialize = false;
  bool revokeOnFinalizer = false;
  size_t stepIndex = 0;
  int finalizers = 0, interactions = 0, delays = 0;
  std::vector<Step> steps;
  std::vector<Command> executed;
  std::vector<std::string> broadcasts;
};
static Scenario scenario;
static struct { bool bondModeEnabled = true; } gSettings;
static void* server = reinterpret_cast<void*>(1);
static int gBroadcastSkipSessionIdx = 17;
static bool gMeshActivitySuspended = false;

void webBatchTestStringAppend() {
  if (scenario.revokeOnSerialize) scenario.live = false;
}
static void* ps_alloc(size_t size, AllocPref, const char*) { return std::malloc(size); }
static AuthContext makeWebAuthCtx(httpd_req_t*) {
  AuthContext ctx;
  if (!scenario.stateful) ctx.sid = "";
  return ctx;
}
static bool tgRequireAuth(const AuthContext&) { return scenario.authenticated; }
static bool webGuestAccessAllowed(httpd_req_t*, const AuthContext&) { return scenario.guestAllowed; }
#define WEB_AUTH_JSON_OR_RETURN(req, ctx) \
  AuthContext ctx = makeWebAuthCtx(req); \
  if (!tgRequireAuth(ctx)) return ESP_OK
static void logAuthAttempt(bool, const char*, const String&, const String&, const char*) {}
static void httpd_resp_set_type(httpd_req_t* req, const char* type) { req->contentType = type; }
static void httpd_resp_set_status(httpd_req_t* req, const char* status) { req->status = status; }
static void httpd_resp_sendstr(httpd_req_t* req, const char* output) {
  check(server != nullptr, "no HTTP send after server destruction");
  check(!gMeshActivitySuspended, "mesh suspension restored before HTTP response");
  check(gBroadcastSkipSessionIdx == 17, "broadcast origin restored before HTTP response");
  req->output = output ? output : "";
  ++req->sends;
}
static void httpd_resp_send(httpd_req_t* req, const char* output, int) { httpd_resp_sendstr(req, output); }
static void httpd_resp_send_err(httpd_req_t* req, int, const char* output) {
  req->status = "500 Internal Server Error"; httpd_resp_sendstr(req, output);
}
static int httpd_req_recv(httpd_req_t* req, char* destination, size_t length) {
  const size_t count = std::min(length, req->input.size() - req->received);
  std::memcpy(destination, req->input.data() + req->received, count);
  req->received += count;
  return static_cast<int>(count);
}
static bool isBondMaster() { return scenario.bondMaster; }
static String getCookieSID(httpd_req_t*) { return scenario.stateful ? "session" : ""; }
static TransportSessionEpoch captureTransportSessionEpoch(const AuthContext&) { return scenario.stateful ? 42 : 0; }
static bool transportSessionEpochIsLive(int, TransportSessionEpoch) { return scenario.live; }
static bool webNoteSessionInteraction(const String&, TransportSessionEpoch) {
  ++scenario.interactions; return scenario.live;
}
static uint32_t allocateSettingsWriteBatchId() { return 73; }
static int findSessionIndexBySID(const String&) { return 3; }
static uint32_t millis() { return 123; }
static int pdMS_TO_TICKS(int value) { return value; }
static void vTaskDelay(int) { ++scenario.delays; }
static AuthContext systemIdentity(const char*) { return {}; }
#define ERROR_WEBF(...) ((void)0)
static void appendCommandToFeed(const char*, const String&, const String&, const String&) {}
static String redactCmdForAudit(const String& text) { return text; }
static String redactWebCommandResult(const String& output) {
  std::string text(output.c_str());
  for (size_t at = text.find("SECRET"); at != std::string::npos; at = text.find("SECRET"))
    text.replace(at, 6, "[redacted]");
  return String(text);
}
static bool cliModeOwnedBySession(int, TransportSessionEpoch) { return false; }
static void broadcastOutput(const String& output, const CommandContext&) {
  scenario.broadcasts.emplace_back(output.c_str());
}
static bool submitAndExecuteSync(const Command& cmd, String& output, bool* completed) {
  if (cmd.ctx.origin == ORIGIN_SYSTEM) {
    ++scenario.finalizers;
    check(cmd.ctx.settingsBatchId == 73, "finalizer retains request token");
    check(cmd.line == "savesettings", "finalizer uses savesettings command");
    check((cmd.ctx.behaviorFlags & COMMAND_CONTEXT_MODE_INDEPENDENT) != 0,
          "finalizer is mode independent");
    *completed = scenario.finalizerSucceeded;
    output = scenario.finalizerSucceeded ? "Settings saved" : "Error: finalizer pending";
    if (scenario.revokeOnFinalizer) scenario.live = false;
    return scenario.finalizerSucceeded;
  }
  scenario.executed.push_back(cmd);
  Step step;
  if (scenario.stepIndex < scenario.steps.size()) step = scenario.steps[scenario.stepIndex];
  else step.output = cmd.line == "savesettings" ? "Settings saved" : "OK: command";
  ++scenario.stepIndex;
  output = String(step.output);
  *completed = step.completed;
  if (step.revoke) scenario.live = false;
  if (step.stopServer) server = nullptr;
  return step.succeeded;
}
static bool executeUnifiedWebCommand(httpd_req_t*, const AuthContext& ctx,
                                     const String& text, String& output) {
  Command cmd; cmd.line = text; cmd.ctx.auth = ctx; cmd.ctx.origin = ORIGIN_WEB;
  bool completed = false;
  return submitAndExecuteSync(cmd, output, &completed);
}

// INSERT_PRODUCTION_HANDLERS_HERE

static void resetScenario() {
  requestAllocator.reset(); responseAllocator.reset(); scenario = Scenario{};
  gSettings.bondModeEnabled = true; server = reinterpret_cast<void*>(1);
  gBroadcastSkipSessionIdx = 17; gMeshActivitySuspended = false;
}
static httpd_req_t invoke(bool bond, const std::vector<std::string>& commands,
                         bool interactive = false) {
  JsonDocument request;
  JsonArray array = request["commands"].to<JsonArray>();
  for (const auto& cmd : commands) array.add(cmd);
  if (interactive) request["interactive"] = true;
  httpd_req_t req;
  serializeJson(request, req.input);
  req.content_len = req.input.size();
  if (bond) handleBondCliBatch(&req); else handleCliBatch(&req);
  check(!gMeshActivitySuspended && gBroadcastSkipSessionIdx == 17,
        "handler exit restores web globals");
  check(requestAllocator.live.empty() && responseAllocator.live.empty(),
        "handler releases request and response JSON allocations");
  return req;
}
static JsonDocument decode(const httpd_req_t& req) {
  check(req.sends == 1, "handler sends exactly one complete response");
  JsonDocument result;
  check(!deserializeJson(result, req.output), "handler response is valid JSON");
  return result;
}
static void expectError(const httpd_req_t& req, const char* status, const char* error) {
  check(req.status.rfind(status, 0) == 0, "expected HTTP error status");
  const JsonDocument doc = decode(req);
  check(doc["ok"] == false, "error response is not success");
  check(doc["error"] == error, "expected error code");
  check(!doc["results"].is<JsonArray>(), "failure does not expose partial results");
}
static void testNormalAndOwnership() {
  for (bool bond : {false, true}) {
    resetScenario();
    auto empty = decode(invoke(bond, {}));
    check(empty["ok"] == true && empty["count"] == 0, "empty batch succeeds");
    check(empty["results"].as<JsonArray>().size() == 0, "empty batch has empty results");
    resetScenario();
    const std::string special = "line\nquote\"slash\\tab\tUnicode: \xE2\x98\x83";
    scenario.steps = {{special}, {"Error: command refused", false}, {std::string(4095, 'x')}};
    const auto doc = decode(invoke(bond, {"", "  ", "first", "second", "third"}));
    check(doc["ok"] == true, "completed command-level errors keep batch ok");
    check(doc["count"] == 3, "count excludes blank commands");
    auto replies = doc["results"].as<JsonArrayConst>();
    check(replies.size() == 5, "blank placeholders preserve original positions");
    check(replies[0] == "" && replies[1] == "", "blank placeholders are strings");
    check(replies[2].as<std::string>() == special, "JSON escaping preserves reply text");
    check(replies[3] == "Error: command refused", "command error remains in result slot");
    check(replies[4].as<std::string>() == std::string(4095, 'x'), "large reply survives");
    check(scenario.executed.size() == 3, "all nonempty commands execute exactly once");
    check(scenario.executed[0].line == (bond ? "remote:first" : "first"), "routing stays unchanged");
    check(responseAllocator.calls > 0, "actual response uses supplied JSON allocator");
  }
  resetScenario(); scenario.steps = {{"SECRET first"}, {"second unrelated response"}};
  const auto redacted = decode(invoke(false, {"a", "b"}));
  check(redacted["results"][0] == "[redacted] first", "redaction precedes response storage");
  check(scenario.broadcasts[0] == "[redacted] first", "broadcast remains redacted");
  for (const auto& cmd : scenario.executed)
    check((cmd.ctx.behaviorFlags & COMMAND_CONTEXT_MODE_INDEPENDENT) != 0,
          "ordinary batch remains independent of interactive modes");
}
static void testSettingsAndTimeouts() {
  resetScenario();
  auto saved = decode(invoke(false, {"beginwrite", "set 1", "savesettings"}));
  check(saved["ok"] == true && scenario.finalizers == 0, "confirmed save disarms finalizer");
  for (const auto& cmd : scenario.executed)
    check(cmd.ctx.settingsBatchId == 73, "one settings token covers every request command");
  resetScenario();
  scenario.steps = {{"begin OK"}, {"", false, false}, {"Settings saved"}};
  auto timeout = decode(invoke(false, {"beginwrite", "set 1", "savesettings"}));
  check(timeout["ok"] == false && timeout["error"] == "batch_executor_unconfirmed",
        "executor timeout remains batch uncertainty");
  check(scenario.executed.size() == 3, "timeout does not suppress later commands");
  check(timeout["results"][1] == "Error: command was not accepted by the executor",
        "empty executor failure receives placeholder");
  resetScenario(); scenario.finalizerSucceeded = false;
  scenario.steps = {{"begin OK"}, {"set OK"}, {"Error: save failed", false}};
  auto failed = decode(invoke(false, {"beginwrite", "set 1", "savesettings"}));
  check(scenario.finalizers == 1, "failed terminal save runs finalizer exactly once");
  check(failed["ok"] == false && failed["error"] == "settings_batch_finalize_pending",
        "finalizer failure remains batch uncertainty");
  resetScenario();
  const auto invalid = invoke(false, {"beginwrite", "set 1"});
  check(invalid.status.rfind("400", 0) == 0 && scenario.executed.empty(), "unbounded begin rejected before execution");
  resetScenario();
  const auto nested = invoke(false, {"beginwrite", "beginwrite", "savesettings"});
  check(nested.status.rfind("400", 0) == 0 && scenario.executed.empty(), "nested wrapper rejected before execution");
}
static void testInteractive() {
  resetScenario(); scenario.steps = {{"Confirm?"}, {"OK: deleted"}};
  auto local = decode(invoke(false, {"userdelete bob", " YES "}, true));
  check(local["count"] == 2 && local["results"][1] == "OK: deleted", "local interactive has paired results");
  check(scenario.interactions == 1, "local interactive refreshes live session once");
  for (const auto& cmd : scenario.executed)
    check((cmd.ctx.behaviorFlags & COMMAND_CONTEXT_MODE_INDEPENDENT) == 0,
          "interactive commands can reach confirmation mode");
  resetScenario(); scenario.stateful = false;
  expectError(invoke(false, {"userdelete bob", "yes"}, true), "401", "stateful_web_session_required");
  check(scenario.executed.empty(), "stateless interactive rejected before execution");
  resetScenario(); scenario.steps = {{"Remote command sent: userdelete bob confirm"}};
  auto remote = decode(invoke(true, {"userdelete bob", "yes"}, true));
  check(scenario.executed.size() == 1 && scenario.executed[0].line == "remote:userdelete bob confirm",
        "bond confirmation translates to one remote one-shot");
  check(remote["count"] == 2 && remote["results"][0] == "OK: confirmed in the local browser",
        "bond preserves two-slot UI contract");
  check(remote["results"][1].as<std::string>().rfind("Pending:", 0) == 0,
        "bond ACK remains pending instead of claiming remote completion");
  resetScenario();
  const auto invalid = invoke(true, {"status", "yes"}, true);
  check(invalid.status.rfind("400", 0) == 0 && scenario.executed.empty(), "unsupported bond confirmation rejected");
}
static void testSessionAndShutdown() {
  for (bool bond : {false, true}) {
    resetScenario(); scenario.steps = {{"private prior reply"}, {"revoked reply", true, true, true}, {"never"}};
    expectError(invoke(bond, {"a", "revoke", "never"}), "401", "web_session_changed");
    check(scenario.executed.size() == 2, "revocation prevents later execution");
    resetScenario(); scenario.live = false;
    expectError(invoke(bond, {"never"}), "401", "web_session_changed");
    check(scenario.executed.empty(), "preexisting stale session executes nothing");
    resetScenario(); scenario.revokeOnSerialize = true;
    expectError(invoke(bond, {"private"}), "401", "web_session_changed");
    resetScenario(); scenario.steps = {{"stop OK", true, true, false, true}};
    const auto stopped = invoke(bond, {"stop", "never"});
    check(stopped.sends == 0 && scenario.executed.size() == 1, "server destruction prevents send and further execution");
  }
  resetScenario(); scenario.steps = {{"begin OK"}, {"revoked", true, true, true}};
  expectError(invoke(false, {"beginwrite", "revoke", "savesettings"}), "401", "web_session_changed");
  check(scenario.finalizers == 1, "revoked settings batch finalizes exactly once");
  resetScenario(); scenario.revokeOnFinalizer = true;
  scenario.steps = {{"begin OK"}, {"set OK"}, {"save uncertain", false, false}};
  expectError(invoke(false, {"beginwrite", "set 1", "savesettings"}), "401", "web_session_changed");
  check(scenario.finalizers == 1, "post-finalizer revocation is checked before serialization");
}
static void testResponseAllocationFailures() {
  for (bool bond : {false, true}) {
    const std::vector<std::string> commands = {"beginwrite", "a", "b", "savesettings"};
    const std::vector<Step> replies = {{std::string(220, 'a')}, {std::string(230, 'b')},
                                     {std::string(240, 'c')}, {"Settings saved"}};
    resetScenario(); scenario.steps = replies;
    const auto baseline = decode(invoke(bond, commands));
    check(baseline["ok"] == true, "allocation sweep baseline succeeds");
    const size_t allocationCount = responseAllocator.calls;
    check(allocationCount >= 4, "allocation sweep covers real owned reply allocations");
    for (size_t failure = 0; failure < allocationCount; ++failure) {
      resetScenario(); scenario.steps = replies; responseAllocator.failAt = failure;
      const auto response = invoke(bond, commands);
      check(responseAllocator.failed, "each planned response allocation was faulted");
      expectError(response, "500", "batch_response_oom");
      check(response.contentType == "application/json", "OOM sends literal JSON content type");
      check(scenario.executed.size() == commands.size(), "buffer OOM does not skip or replay commands");
      check(scenario.finalizers == 0, "buffer OOM preserves confirmed-save disarming");
    }
    resetScenario(); scenario.steps = {{"first"}, {"revoked", true, true, true}};
    responseAllocator.failAt = 0;
    expectError(invoke(bond, {"a", "revoke", "never"}), "401", "web_session_changed");
    check(scenario.executed.size() == 2, "session loss outranks response OOM");
    resetScenario(); scenario.steps = {{"stop OK", true, true, false, true}};
    responseAllocator.failAt = 0;
    check(invoke(bond, {"stop", "never"}).sends == 0, "dead server outranks response OOM");
  }
  resetScenario(); responseAllocator.failAt = 0;
  scenario.steps = {{"begin OK"}, {"set OK"}, {"failed", false, false}};
  expectError(invoke(false, {"beginwrite", "set 1", "savesettings"}), "500", "batch_response_oom");
  check(scenario.finalizers == 1 && scenario.executed.size() == 3,
        "response OOM still completes executor-affine settings cleanup");
  resetScenario(); responseAllocator.failAt = 0;
  scenario.steps = {{"begin OK"}, {"stopped", true, true, false, true}};
  const auto stopped = invoke(false, {"beginwrite", "stop", "savesettings"});
  check(stopped.sends == 0 && scenario.finalizers == 1,
        "server destruction plus OOM still finalizes without HTTP send");
}
static void testRepeatedRepliesAndSlotGrowth() {
  // At least 250 replies; the vendored 64-bit host default has 256 slots per
  // pool, so extend past that when needed. Do not change ArduinoJson's pool
  // settings merely to manufacture a growth allocation in this test.
  const size_t count = std::max<size_t>(250, ARDUINOJSON_POOL_CAPACITY + 8);
  const std::vector<std::string> commands(count, "repeat");
  const std::string repeated = "Identical owned reply: " + std::string(80, 'r');
  const std::vector<Step> steps(count, Step{repeated});
  for (bool bond : {false, true}) {
    resetScenario(); scenario.steps = steps;
    const auto baseline = decode(invoke(bond, commands));
    check(baseline["ok"] == true && baseline["count"] == count,
          "large repeated batch succeeds with original count");
    const auto replies = baseline["results"].as<JsonArrayConst>();
    check(replies.size() == count, "slot growth retains every repeated result");
    for (JsonVariantConst reply : replies)
      check(reply.as<std::string>() == repeated, "repeated result text is preserved");
    const size_t allocations = responseAllocator.calls;
    check(allocations >= 3, "slot-growth fixture allocates multiple pools plus text");
    check(allocations < count / 4,
          "identical owning Strings use JSON dedup rather than per-reply allocations");
    for (size_t failure = 0; failure < allocations; ++failure) {
      resetScenario(); scenario.steps = steps; responseAllocator.failAt = failure;
      expectError(invoke(bond, commands), "500", "batch_response_oom");
      check(responseAllocator.failed, "slot-growth allocation failure was injected");
      check(scenario.executed.size() == count,
            "late slot-pool OOM does not skip or replay later commands");
    }
  }
}
int main() {
  try {
    testNormalAndOwnership(); testSettingsAndTimeouts(); testInteractive();
    testSessionAndShutdown(); testResponseAllocationFailures();
    testRepeatedRepliesAndSlotGrowth();
    std::cout << "web batch extracted-handler tests passed (" << assertions
              << " assertions; real ArduinoJson, mocked platform boundaries)\n";
    return 0;
  } catch (const std::exception& failure) {
    std::cerr << "web batch extracted-handler failure: " << failure.what() << '\n';
    return 1;
  }
}
