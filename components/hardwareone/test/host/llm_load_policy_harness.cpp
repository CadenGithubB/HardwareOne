// Model load/unload policy: the shipping web handlers, the llmload/llmunload
// command handlers, CommandArgs and the backend's select/unload are inserted
// at the markers. The command executor is a double that applies the admin
// gate the registry rows carry (the runner pins those rows' requiresAdmin
// flag) and then dispatches to the real command handlers. The engine
// (llmLoadModel/llmUnload) and model resolution are doubles that record calls.
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ArduinoJson.h>

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
  char operator[](size_t at) const { return at < value_.size() ? value_[at] : '\0'; }
  String substring(size_t start, size_t end) const { return value_.substr(start, end - start); }
  bool operator==(const char* other) const { return value_ == (other ? other : ""); }
  bool startsWith(const char* prefix) const { return value_.rfind(prefix, 0) == 0; }
  void trim() {
    const auto first = value_.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { value_.clear(); return; }
    value_ = value_.substr(first, value_.find_last_not_of(" \t\r\n") - first + 1);
  }
  long toInt() const { return std::strtol(value_.c_str(), nullptr, 10); }
 private:
  std::string value_;
};

// ---- platform and settings doubles -----------------------------------------
typedef int esp_err_t;
#define ESP_OK 0
#define ENABLE_LLM_SOURCE_ONBOARD 1
#define ENABLE_LLM_SOURCE_CM5 0
#define DEBUG_HTTPF(...) do { } while (false)
#define PSRAM_JSON_DOC(name) JsonDocument name

struct Settings {
  bool llmEnabled = true;
  int llmMaxContext = 0;
  String llmDefaultModel = "onboard:tiny.bin";
};
static Settings gSettings;

struct AuthContext {
  String user;
  bool admin = false;
};

struct httpd_req_t {
  std::string body;
  AuthContext who;
  std::string reply;
};

static AuthContext makeWebAuthCtx(httpd_req_t* req) { return req->who; }
// Any logged-in non-guest web user passes these two, which is the point:
// the role check for load/unload belongs to the command executor.
static bool tgRequireAuth(AuthContext& ctx) { return ctx.user.length() > 0; }
static bool webGuestAccessAllowed(httpd_req_t*, AuthContext&) { return true; }

static esp_err_t sendJsonResponse(httpd_req_t* req, const char* body) {
  req->reply = body ? body : "";
  return ESP_OK;
}

static bool readPostBody(httpd_req_t* req, char* buf, size_t bufSize) {
  if (req->body.empty() || req->body.size() >= bufSize) return false;
  std::memcpy(buf, req->body.c_str(), req->body.size() + 1);
  return true;
}

// INSERT_TYPES

// ---- engine and registry doubles -------------------------------------------
static bool gInExecutor = false;      // true only while the executor double runs a command
static bool gDirectSelectTest = false; // the backend's own unit cases call it directly
static std::vector<int> gLoadCtx;      // maxCtx each llmLoadModel received
static unsigned gUnloads = 0;
static bool gReady = false;

static bool llmLoadModel(const char*, int maxCtx) {
  CHECK(gInExecutor || gDirectSelectTest);   // never straight from a surface
  gLoadCtx.push_back(maxCtx);
  gReady = true;
  return true;
}
static void llmUnload() {
  CHECK(gInExecutor || gDirectSelectTest);
  ++gUnloads;
  gReady = false;
}
static bool llmBackendIsReady() { return gReady; }

static bool llmResolveModelId(const char* id, LlmModelDesc* out) {
  if (!id || (std::strcmp(id, "onboard:tiny.bin") != 0 && std::strcmp(id, "tiny.bin") != 0))
    return false;
  std::memset(out, 0, sizeof(*out));
  std::snprintf(out->id, sizeof(out->id), "onboard:tiny.bin");
  std::snprintf(out->name, sizeof(out->name), "tiny.bin");
  std::snprintf(out->path, sizeof(out->path), "/system/llm/tiny.bin");
  out->backend = LlmBackendKind::Onboard;
  out->available = true;
  return true;
}

static LlmBackendKind sActiveKind = LlmBackendKind::None;
static LlmModelDesc sActiveDesc;
static char llmCmdBuf[1024];

// INSERT_BACKEND

// INSERT_COMMANDS

// ---- command executor double -----------------------------------------------
static std::vector<std::string> gSubmitted;

bool executeUnifiedWebCommand(httpd_req_t*, AuthContext& ctx, const String& cmd, String& out) {
  const std::string line = cmd.c_str();
  gSubmitted.push_back(line);
  const size_t space = line.find(' ');
  const std::string name = line.substr(0, space);
  const std::string args = space == std::string::npos ? "" : line.substr(space + 1);
  const bool adminRow = (name == "llmload" || name == "llmunload");
  if (adminRow && !ctx.admin) {
    out = String("Error: Admin access required for command '" + name +
                 "'. Contact an administrator.");
    return false;
  }
  const char* result = "Error: Unknown command";
  gInExecutor = true;
  if (name == "llmload") result = cmd_llm_load(String(args));
  else if (name == "llmunload") result = cmd_llm_unload(String(args));
  gInExecutor = false;
  out = String(result);
  return std::strncmp(result, "Error", 5) != 0;
}

// INSERT_WEB

// ---- cases -----------------------------------------------------------------
static void reset() {
  gSettings = Settings();
  gLoadCtx.clear();
  gSubmitted.clear();
  gUnloads = 0;
  gReady = false;
  gDirectSelectTest = false;
  sActiveKind = LlmBackendKind::None;
}

static httpd_req_t request(const char* user, bool admin, const char* body) {
  httpd_req_t req;
  req.who.user = user;
  req.who.admin = admin;
  req.body = body ? body : "";
  return req;
}

static bool contains(const std::string& text, const char* part) {
  return text.find(part) != std::string::npos;
}

// A user-role web session cannot load: the request is submitted as the
// llmload command carrying the model id, the executor refuses it, and the
// engine is never touched.
static void caseUserLoadRefused() {
  reset();
  httpd_req_t req = request("bob", false, "{\"model\":\"onboard:tiny.bin\"}");
  CHECK(handleLLMLoad(&req) == ESP_OK);
  CHECK(gLoadCtx.empty());
  CHECK(gSubmitted.size() == 1);
  CHECK(gSubmitted[0] == "llmload \"onboard:tiny.bin\"");
  JsonDocument doc;
  CHECK(!deserializeJson(doc, req.reply));
  CHECK(doc["ok"] == false);
  CHECK(contains(doc["error"] | "", "Admin access required"));
}

static void caseUserUnloadRefused() {
  reset();
  sActiveKind = LlmBackendKind::Onboard;
  gReady = true;
  httpd_req_t req = request("bob", false, "{}");
  CHECK(handleLLMUnload(&req) == ESP_OK);
  CHECK(gUnloads == 0);
  CHECK(sActiveKind == LlmBackendKind::Onboard);
  CHECK(gSubmitted.size() == 1 && gSubmitted[0] == "llmunload");
  JsonDocument doc;
  CHECK(!deserializeJson(doc, req.reply));
  CHECK(doc["ok"] == false);
  CHECK(contains(doc["error"] | "", "Admin access required"));
}

// An admin load runs the real llmload handler; the context the engine gets
// is llmMaxContext, and the body's max_ctx can only lower it.
static void caseAdminLoadThroughCommand() {
  reset();
  gSettings.llmMaxContext = 1024;
  httpd_req_t req = request("root", true, "{\"model\":\"onboard:tiny.bin\"}");
  CHECK(handleLLMLoad(&req) == ESP_OK);
  CHECK(gSubmitted.size() == 1 && gSubmitted[0] == "llmload \"onboard:tiny.bin\"");
  CHECK(gLoadCtx.size() == 1 && gLoadCtx[0] == 1024);
  CHECK(contains(req.reply, "\"ok\":true"));

  req = request("root", true, "{\"model\":\"onboard:tiny.bin\",\"max_ctx\":4096}");
  CHECK(handleLLMLoad(&req) == ESP_OK);
  CHECK(gSubmitted.back() == "llmload \"onboard:tiny.bin\" 4096");
  CHECK(gLoadCtx.back() == 1024);          // clamped to the setting

  req = request("root", true, "{\"model\":\"onboard:tiny.bin\",\"max_ctx\":256}");
  CHECK(handleLLMLoad(&req) == ESP_OK);
  CHECK(gLoadCtx.back() == 256);           // lowering is allowed

  req = request("root", true, "{\"model\":\"onboard:missing.bin\"}");
  CHECK(handleLLMLoad(&req) == ESP_OK);
  JsonDocument doc;
  CHECK(!deserializeJson(doc, req.reply));
  CHECK(doc["ok"] == false);
  CHECK(contains(doc["error"] | "", "no such model"));
}

static void caseAdminUnloadThroughCommand() {
  reset();
  sActiveKind = LlmBackendKind::Onboard;
  gReady = true;
  httpd_req_t req = request("root", true, "{}");
  CHECK(handleLLMUnload(&req) == ESP_OK);
  CHECK(gSubmitted.size() == 1 && gSubmitted[0] == "llmunload");
  CHECK(gUnloads == 1);
  CHECK(sActiveKind == LlmBackendKind::None);
  CHECK(contains(req.reply, "\"ok\":true"));
}

// An id that cannot travel as one quoted command token is refused before
// anything is submitted.
static void caseBadIdRejected() {
  reset();
  httpd_req_t req = request("root", true, "{\"model\":\"onboard:a\\\" b\"}");
  CHECK(handleLLMLoad(&req) == ESP_OK);
  CHECK(gSubmitted.empty());
  CHECK(gLoadCtx.empty());
  CHECK(contains(req.reply, "Invalid model id"));
}

// The backend owns the context policy for every caller (CLI, BLE, G2, OLED,
// boot autostart pass 0).
static void caseSelectAppliesSetting() {
  reset();
  gDirectSelectTest = true;
  char err[96];
  struct { int setting, requested, expected; } rows[] = {
    {256, 0, 256},       // 0 picks up llmMaxContext
    {256, 4096, 256},    // a larger value is clamped
    {256, 128, 128},     // a smaller one lowers it
    {256, -5, 256},
    {0, 0, 0},           // no cap set: the model's own window
    {0, 512, 512},
    {0, 9000, 4096},     // schema range
    {9000, 0, 4096},
  };
  for (const auto& row : rows) {
    gSettings.llmMaxContext = row.setting;
    CHECK(llmBackendSelect("onboard:tiny.bin", err, sizeof(err), row.requested));
    CHECK(gLoadCtx.back() == row.expected);
  }
  gSettings.llmMaxContext = 768;
  CHECK(llmBackendSelect("onboard:tiny.bin", err, sizeof(err)));
  CHECK(gLoadCtx.back() == 768);
}

// llmload carries the optional ceiling through to the backend.
static void caseCommandContextArgument() {
  reset();
  gSettings.llmMaxContext = 1024;
  gInExecutor = true;
  CHECK(contains(cmd_llm_load(String("\"onboard:tiny.bin\" 300")), "\"ok\":true"));
  CHECK(gLoadCtx.back() == 300);
  CHECK(contains(cmd_llm_load(String("onboard:tiny.bin")), "\"ok\":true"));
  CHECK(gLoadCtx.back() == 1024);
  CHECK(contains(cmd_llm_load(String("")), "\"ok\":true"));   // default model, boot autostart
  CHECK(gLoadCtx.back() == 1024);
  gInExecutor = false;
}

static void caseBuildLoadCommand() {
#if HAVE_BUILD_LOAD_COMMAND
  char out[64];
  CHECK(llmBuildLoadCommand("onboard:tiny.bin", 0, out, sizeof(out)));
  CHECK(std::strcmp(out, "llmload \"onboard:tiny.bin\"") == 0);
  CHECK(llmBuildLoadCommand("cm5:My Model.gguf", 512, out, sizeof(out)));
  CHECK(std::strcmp(out, "llmload \"cm5:My Model.gguf\" 512") == 0);
  CHECK(!llmBuildLoadCommand("a\"b", 0, out, sizeof(out)) && out[0] == '\0');
  CHECK(!llmBuildLoadCommand("a\nllmunload", 0, out, sizeof(out)));
  CHECK(!llmBuildLoadCommand("", 0, out, sizeof(out)));
  CHECK(!llmBuildLoadCommand("onboard:a-name-far-too-long-for-this-buffer.bin", 0, out, 24));
#else
  CHECK(!"llmBuildLoadCommand is missing from System_LLMBackend.cpp");
#endif
}

int main(int argc, char** argv) {
  struct { const char* name; void (*run)(); } cases[] = {
    {"user_load_refused", caseUserLoadRefused},
    {"user_unload_refused", caseUserUnloadRefused},
    {"admin_load_through_command", caseAdminLoadThroughCommand},
    {"admin_unload_through_command", caseAdminUnloadThroughCommand},
    {"bad_id_rejected", caseBadIdRejected},
    {"select_applies_setting", caseSelectAppliesSetting},
    {"command_context_argument", caseCommandContextArgument},
    {"build_load_command", caseBuildLoadCommand},
  };
  unsigned ran = 0;
  for (const auto& c : cases) {
    if (argc > 1 && std::strcmp(argv[1], c.name) != 0) continue;
    c.run();
    ++ran;
    std::printf("PASS %s\n", c.name);
  }
  if (ran == 0) {
    std::fprintf(stderr, "no case named %s\n", argc > 1 ? argv[1] : "");
    return 2;
  }
  std::printf("llm_load_policy: %u checks\n", checks);
  return 0;
}
