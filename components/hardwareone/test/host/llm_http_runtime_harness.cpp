#include "System_LLMHttpCore.h"
#include "System_LLMHttpTransportCore.h"
#include "BLE_PhonePipePath.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <atomic>
#include <condition_variable>
#include <new>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#define ESP_LOGW(...) ((void)0)

// Arduino declares this global type; the HTTP SSE parser must stay qualified.
class Stream {};

#define ENABLE_LLM_BACKEND 1
#define ENABLE_LLM_SOURCE_HTTP 1
#define ENABLE_G2_GLASSES 1
#define ENABLE_LOCAL_STT 1
#define ENABLE_BLE_PHONE_PIPE 0
#define CONFIG_MBEDTLS_CERTIFICATE_BUNDLE 0
#define EXT_RAM_BSS_ATTR
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 0xffffffff
#define pdMS_TO_TICKS(x) (x)
using SemaphoreHandle_t = std::mutex*;
static SemaphoreHandle_t xSemaphoreCreateMutex() { return new std::mutex; }
static bool failNextLock = false;
// Runs on main just before the n-th later bounded lock take (not yet held).
static std::function<void()> lockHook;
static int lockHookTake = 0;
static int xSemaphoreTake(SemaphoreHandle_t p, unsigned wait) {
  if (lockHook && wait != portMAX_DELAY && !--lockHookTake) { auto hook = lockHook; lockHook = {}; hook(); }
  if (failNextLock && wait != portMAX_DELAY) { failNextLock = false; return 0; }
  p->lock(); return 1;
}
static void xSemaphoreGive(SemaphoreHandle_t p) { p->unlock(); }
static void vSemaphoreDelete(SemaphoreHandle_t p) { delete p; }
static uint32_t nowMs = 10;
static uint32_t millis() { return nowMs; }
// Runs on main inside vTaskDelay (not on the worker): lets a test give the
// parked worker its turn while a caller waits for it.
static void (*delayHook)() = nullptr;
static void vTaskDelay(unsigned n);
static void (*pendingTask)(void*) = nullptr;
static void* pendingArg = nullptr;
static bool failTask = false;
// The chat worker ("llm_http") runs to its end inside drainWorker(). The kept
// Terminal worker ("g2_terminal_http") is a real thread that takes turns with
// main: it runs only inside drainTerminal()/idleTerminal() and hands the turn
// back when it parks in ulTaskNotifyTake or leaves through vTaskDelete. The
// turn hand-off orders every access to the shared fakes.
using TaskHandle_t = void*;
using BaseType_t = int;
static std::mutex turnMutex;
static std::condition_variable turnChanged;
static bool workerTurn = false;
enum class TerminalTask { None, Alive, Exited };
static TerminalTask terminalTask = TerminalTask::None;
static std::thread terminalThread;
static thread_local bool onTerminalThread = false;
static int terminalTaskHandle = 0;  // its address is the handle
static int terminalCreates = 0, terminalDeletes = 0;
static uint32_t terminalNotifications = 0;
static bool wakeIsTimeout = false, terminalStarted = false;
static unsigned lastParkTicks = 0;
static std::function<void()> parkHook;  // on the worker, at its wait, no lock held
static void handTurn(bool toWorker) {
  std::unique_lock<std::mutex> lock(turnMutex);
  workerTurn = toWorker; turnChanged.notify_all();
  turnChanged.wait(lock, [&] { return workerTurn != toWorker; });
}
static void terminalThreadMain(void (*fn)(void*), void* arg) {
  onTerminalThread = true;
  { std::unique_lock<std::mutex> lock(turnMutex); turnChanged.wait(lock, [] { return workerTurn; }); }
  terminalStarted = true;
  fn(arg);
  std::lock_guard<std::mutex> lock(turnMutex);
  terminalTask = TerminalTask::Exited; workerTurn = false; turnChanged.notify_all();
}
static int xTaskCreate(void (*fn)(void*), const char* name, int stack, void* ctx, int priority,
                       TaskHandle_t* created) {
  if (failTask) { failTask = false; return 0; }
  if (!strcmp(name, "g2_terminal_http")) {
    assert(stack == 8192 && priority == 1 && created && !onTerminalThread);
    assert(terminalTask != TerminalTask::Alive);
    if (terminalThread.joinable()) terminalThread.join();
    terminalTask = TerminalTask::Alive; terminalNotifications = 0; wakeIsTimeout = terminalStarted = false;
    ++terminalCreates; *created = &terminalTaskHandle;
    terminalThread = std::thread(terminalThreadMain, fn, ctx);
    return 1;
  }
  assert(!pendingTask); pendingTask = fn; pendingArg = ctx; return 1;
}
static void vTaskDelete(void* task) { if (onTerminalThread) { assert(!task); ++terminalDeletes; } }
[[maybe_unused]] static void xTaskNotifyGive(TaskHandle_t task) {
  assert(task == &terminalTaskHandle && terminalTask == TerminalTask::Alive);
  ++terminalNotifications;
}
[[maybe_unused]] static uint32_t ulTaskNotifyTake(BaseType_t clear, unsigned ticks) {
  assert(onTerminalThread && clear == 1);
  if (parkHook) { auto hook = parkHook; parkHook = {}; hook(); }
  if (!terminalNotifications) {
    lastParkTicks = ticks;
    handTurn(false);  // parked: main runs until it hands the turn back
    if (wakeIsTimeout) { wakeIsTimeout = false; nowMs += ticks; return 0; }
    assert(terminalNotifications);  // a parked task runs again only when woken
  }
  const uint32_t count = terminalNotifications; terminalNotifications = 0; return count;
}
enum class AllocPref { PreferPsram };
static size_t allocations = 0;
static bool failConfigAllocation = false, failTerminalRunAllocation = false;
static int terminalRunAllocations = 0;
static std::unordered_set<void*> terminalRuns;
static std::unordered_map<void*, size_t> sensitiveAllocations;
static size_t wipedAllocations = 0;
static void mbedtls_platform_zeroize(void* p, size_t n) {
  auto* bytes = static_cast<volatile unsigned char*>(p);
  while (n--) *bytes++ = 0;
}
static void* ps_alloc(size_t n, AllocPref, const char* tag) {
  ++allocations;
  void* p = malloc(n);
  if (p && !strcmp(tag, "llm.http.body")) sensitiveAllocations[p] = n;
  return p;
}
static void* ps_calloc(size_t n, size_t s, AllocPref, const char* tag) {
  if (failConfigAllocation && !strcmp(tag, "llm.http.config")) {
    failConfigAllocation = false; return nullptr;
  }
  if (!strcmp(tag, "llm.terminal.run")) {
    if (failTerminalRunAllocation) { failTerminalRunAllocation = false; return nullptr; }
    ++terminalRunAllocations;
  }
  ++allocations;
  void* p = calloc(n, s);
  if (p && !strcmp(tag, "llm.terminal.run")) terminalRuns.insert(p);
  if (p && (!strcmp(tag, "llm.http.config") || !strcmp(tag, "llm.http.run") ||
            !strcmp(tag, "llm.terminal.run")))
    sensitiveAllocations[p] = n * s;
  return p;
}
static void* ps_realloc(void* p, size_t n, AllocPref, const char*) { return realloc(p, n); }
static void ps_free(void* p) {
  if (!p) return;
  const auto found = sensitiveAllocations.find(p);
  if (found != sensitiveAllocations.end()) {
    const auto* bytes = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < found->second; ++i) assert(bytes[i] == 0);
    sensitiveAllocations.erase(found);
    ++wipedAllocations;
  }
  terminalRuns.erase(p);
  --allocations; free(p);
}
static void esp_fill_random(void* p, size_t n) {
  static uint8_t x = 0; auto* b = static_cast<uint8_t*>(p);
  while (n--) *b++ = ++x;
}
struct Wifi { bool connected = true; bool isConnected() { return connected; } } WiFi;
static bool netUp() { return WiFi.isConnected(); }
enum class LlmBackendKind { None, Onboard, Cm5, Http };
constexpr unsigned LLM_STORAGE_REMOTE = 2;
struct LlmModelDesc { char id[48], name[32]; LlmBackendKind backend; char path[64]; uint32_t sizeKb; uint8_t storage; bool available; };
enum class LlmState { UNLOADED, LOADING, READY, GENERATING, ERROR };
struct LlmStatus { LlmState state; char modelPath[64]; char errorMsg[128]; };
struct LlmGenParams {};
using esp_err_t = int;
constexpr int ESP_LOG_ERROR = 1, ESP_LOG_WARN = 2, ESP_LOG_DEBUG = 4;
static int httpLogLevel = ESP_LOG_DEBUG;
static int logSuppressions = 0;
static bool ignoreLogSuppression = false;
static void esp_log_level_set(const char* tag, int level) {
  assert(!strcmp(tag, "HTTP_CLIENT"));
  assert(level == ESP_LOG_WARN);
  if (!ignoreLogSuppression) httpLogLevel = level;
  ++logSuppressions;
}
static int esp_log_level_get(const char* tag) { assert(!strcmp(tag, "HTTP_CLIENT")); return httpLogLevel; }
constexpr int ESP_OK = 0, ESP_ERR_HTTP_EAGAIN = 0x7007, HTTP_METHOD_POST = 2, HTTP_EVENT_ON_HEADER = 1;
struct esp_http_client_event_t { int event_id; const char *header_key, *header_value; void* user_data; };
struct esp_http_client_config_t {
  const char* url = nullptr; int method, timeout_ms; bool disable_auto_redirect;
  int max_authorization_retries, buffer_size, buffer_size_tx;
  esp_err_t (*event_handler)(esp_http_client_event_t*) = nullptr;
  void* user_data; bool keep_alive_enable; const char* cert_pem = nullptr;
};
struct Client { esp_http_client_config_t cfg; };
using esp_http_client_handle_t = Client*;
static std::string wire, request, auth;
static size_t readOffset;
static int opens = 0, cleanups = 0, shuts = 0, status = 200;
static bool failWrite = false, failOpen = false, eventType = true;
static bool terminalJson = false, completeResponse = true;
static std::string requestUrl;
static std::function<void()> readHook;
static esp_http_client_handle_t esp_http_client_init(esp_http_client_config_t* cfg) {
  assert(httpLogLevel <= ESP_LOG_WARN);
  assert(cfg->disable_auto_redirect && cfg->max_authorization_retries == -1);
  requestUrl = cfg->url;
  assert(cfg->timeout_ms <= 2000); return new Client{*cfg};
}
static int esp_http_client_set_header(Client*, const char* key, const char* value) {
  assert(httpLogLevel <= ESP_LOG_WARN);
  if (!strcmp(key, "Authorization")) auth = value; return 0;
}
static int esp_http_client_open(Client*, int) {
  assert(httpLogLevel <= ESP_LOG_WARN); ++opens; return failOpen ? -1 : 0;
}
static int esp_http_client_get_socket(Client*) { return 12345; }
static int fakeShutdown(int fd, int) { assert(fd == 12345); ++shuts; return 0; }
#define shutdown fakeShutdown
#define SHUT_RDWR 2
static int esp_http_client_write(Client*, const char* p, int n) {
  if (failWrite) return -1;
  request.append(p, static_cast<size_t>(n)); return n;
}
static int64_t esp_http_client_fetch_headers(Client* c) {
  esp_http_client_event_t event{HTTP_EVENT_ON_HEADER, "Content-Type",
      terminalJson ? "application/json" : eventType ? "text/event-stream; charset=utf-8" : "text/html", c->cfg.user_data};
  c->cfg.event_handler(&event); return 0;
}
static int esp_http_client_get_status_code(Client*) { return status; }
static int esp_http_client_read(Client*, char* p, int n) {
  ++nowMs;
  if (readHook) readHook();
  if (shuts) return -1;
  if (readOffset == wire.size()) return 0;
  size_t count = std::min(static_cast<size_t>(n), wire.size() - readOffset);
  memcpy(p, wire.data() + readOffset, count); readOffset += count; return static_cast<int>(count);
}
static void esp_http_client_close(Client*) {}
static void esp_http_client_cleanup(Client* c) { ++cleanups; delete c; }
static bool esp_http_client_is_complete_data_received(Client*) { return completeResponse; }

// A READY phone link with its relay on (RELAY_ON): the only phone state relay
// users route on (phone link v2).
static bool phoneRelay = false;
static bool phonePipeRelayAvailable() { return phoneRelay; }
// The phone path view (BLE_PhonePipePath.h): by default Up with the relay on,
// otherwise Off. A test sets pathView for a link that is coming back or held.
static bool pathViewSet = false;
static PhonePathView pathView;
static unsigned nudges = 0;
PhonePathView phonePipePathView() {
  if (pathViewSet) return pathView;
  PhonePathView v; v.state = phoneRelay ? PhonePathState::Up : PhonePathState::Off; return v;
}
bool phonePipeNudgeReconnect() { ++nudges; return phonePathRecoverable(phonePipePathView()); }
static void phoneView(PhonePathState state, PhonePathHint hint = PhonePathHint::None, uint32_t inMs = 0) {
  pathViewSet = true; pathView = {}; pathView.state = state; pathView.hint = hint; pathView.retryInMs = inMs;
}
// INSERT_TASK_UTILS
// INSERT_IO
// INSERT_INTERFACE
// INSERT_IMPLEMENTATION

static const char* validWire = "data: {\"choices\":[{\"delta\":{\"content\":\"answer\"}}]}\n\n"
                               "data: [DONE]\n\n";
static void resetServer() {
  // Reproduce a user enabling broad debug between requests. Every worker must
  // suppress the sensitive tag again before it can touch HTTP client headers.
  httpLogLevel = ESP_LOG_DEBUG; logSuppressions = 0;
  ignoreLogSuppression = false;
  wire = validWire; request.clear(); auth.clear(); readOffset = 0;
  opens = cleanups = shuts = 0; status = 200;
  failWrite = failOpen = false; eventType = true; readHook = {};
  terminalJson = false; completeResponse = true; requestUrl.clear();
}
static void drainWorker() {
  assert(pendingTask); auto task = pendingTask; void* arg = pendingArg;
  pendingTask = nullptr; pendingArg = nullptr; task(arg);
  assert(httpLlmIsDone()); assert(allocations == 0);
  assert(logSuppressions == 1);
  assert(ignoreLogSuppression || httpLogLevel == ESP_LOG_WARN);
}
// Runs the kept Terminal worker until it parks again or leaves.
static void drainTerminal() {
  assert(terminalTask == TerminalTask::Alive && (!terminalStarted || terminalNotifications || wakeIsTimeout));
  handTurn(true);
  if (terminalTask == TerminalTask::Exited) terminalThread.join();
  assert(logSuppressions >= 1);
  assert(ignoreLogSuppression || httpLogLevel == ESP_LOG_WARN);
}
// The parked worker's wait runs out with no wake.
static void idleTerminal() {
  assert(!terminalNotifications); wakeIsTimeout = true; drainTerminal();
}
static bool terminalKept() { return terminalTask == TerminalTask::Alive; }
static void vTaskDelay(unsigned n) {
  if (delayHook && !onTerminalThread) delayHook();
  nowMs += n;
}
static void drainIfKept() { if (terminalKept()) drainTerminal(); }
// Every byte of the kept run is zero between requests: no credential, body or
// response survives into the next request.
static void assertRunsWiped() {
  for (void* run : terminalRuns) {
    const auto* bytes = static_cast<const unsigned char*>(run);
    for (size_t i = 0; i < sensitiveAllocations[run]; ++i) assert(bytes[i] == 0);
  }
}
static bool runHolds(const char* text) {
  for (void* run : terminalRuns)
    if (memmem(run, sensitiveAllocations[run], text, strlen(text))) return true;
  return false;
}
static void assertTerminalAdmissionFree() {
  assert(llmHttpRequestAdmit(LlmHttpRequestKind::Terminal, true) == LlmHttpAdmission::Accepted);
  llmHttpRequestRelease(LlmHttpRequestKind::Terminal);
}
static std::string response() {
  char chunk[512];
  std::string out;
  int n;
  while ((n = httpLlmResultChunk(out.size(), chunk, sizeof(chunk))) > 0) out.append(chunk, n);
  return out;
}
static std::string user() {
  JsonDocument doc; assert(!deserializeJson(doc, request));
  assert(doc["model"] == "openclaw/default");
  assert(doc["messages"].size() == 1);
  assert(doc["max_tokens"].isNull() && doc["tools"].isNull());
  return doc["user"].as<std::string>();
}
int main() {
  assert(httpLlmValidateBaseUrl("http://agent:18789/v1"));
  assert(httpLlmValidateBaseUrl("https://agent:18789/v1/"));
  assert(!httpLlmValidateBaseUrl("https://secret@agent/v1"));
  assert(!httpLlmValidateBaseUrl("https://agent/v1?token=secret"));
  assert(!httpLlmValidateBaseUrl("https://agent/v1#fragment"));
  assert(!httpLlmConfigure(true, "https://agent/v1", "openclaw/default", "secret", false, ""));
  char error[128]; assert(!httpLlmSelectByName("openclaw", error, sizeof(error)));
  assert(httpLlmConfigure(true, "http://agent/v1", "openclaw/default", "secret", true, ""));
  assert(httpLlmSelectByName("openclaw", error, sizeof(error)));
  resetServer(); int first = httpLlmStartAsync("hello", {}); assert(first > 0);
  assert(!httpLlmResetConversation()); assert(!httpLlmStartAsync("duplicate", {}));
  drainWorker(); assert(response() == "answer"); assert(opens == 1 && cleanups == 1);
  assert(auth == "Bearer secret"); auto conversation = user();
  resetServer(); assert(httpLlmStartAsync("follow up", {}) != first); drainWorker(); assert(user() == conversation);
  assert(httpLlmResetConversation());
  resetServer(); assert(httpLlmStartAsync("new chat", {})); drainWorker(); assert(user() != conversation);
  resetServer(); wire.clear();
  for (int i = 0; i < 3; ++i)
    wire += "data: {\"choices\":[{\"delta\":{\"content\":\"" + std::string(4000, 'r') + "\"}}]}\n\n";
  wire += "data: [DONE]\n\n";
  assert(httpLlmStartAsync("long reply", {})); drainWorker();
  assert(response() == std::string(12000, 'r') && httpLlmResultLen() == 12000);
  for (int code : {301, 401, 500}) {
    resetServer(); status = code; assert(httpLlmStartAsync("once", {})); drainWorker();
    assert(opens == 1 && response().find("HTTP " + std::to_string(code)) != std::string::npos);
  }
  resetServer(); failWrite = true; assert(httpLlmStartAsync("once", {})); drainWorker();
  assert(opens == 1 && response().find("outcome unknown") != std::string::npos);
  resetServer(); wire.resize(wire.find("data: [DONE]"));
  assert(httpLlmStartAsync("partial", {})); drainWorker();
  assert(response().find("answer\n[HTTP stream ended before [DONE]]") == 0);
  resetServer(); readHook = [] { if (readOffset == 65) httpLlmStop(); };
  assert(httpLlmStartAsync("cancel", {})); drainWorker();
  assert(shuts == 1 && response().find("Stopped locally") != std::string::npos);
  assert(!strstr(httpLlmStatus().errorMsg, "secret"));
  resetServer(); assert(httpLlmStartAsync("cancel before connect", {})); httpLlmStop(); drainWorker();
  assert(opens == 0 && response().find("Stopped locally") != std::string::npos);
  resetServer(); readHook = [] { if (readOffset == 12) {
    assert(httpLlmConfigure(true, "http://other/v1", "openclaw/default", "different", true, ""));
  } };
  assert(httpLlmStartAsync("change target", {})); drainWorker();
  assert(shuts == 1 && response().find("configuration changed") != std::string::npos);
  resetServer(); failTask = true; assert(!httpLlmStartAsync("no task", {})); assert(httpLlmIsDone());
  assert(allocations == 0);
  resetServer(); ignoreLogSuppression = true;
  assert(httpLlmStartAsync("unsafe debug profile", {})); drainWorker();
  assert(opens == 0 && auth.empty());
  assert(response().find("HTTP debug logging exposes credentials") != std::string::npos);
  // A quieter saved level (esplog HTTP_CLIENT error) stays: the worker only
  // lowers a level above WARN.
  resetServer(); httpLogLevel = ESP_LOG_ERROR;
  assert(httpLlmStartAsync("quieter level kept", {}));
  { auto task = pendingTask; void* arg = pendingArg; pendingTask = nullptr; pendingArg = nullptr; task(arg); }
  assert(httpLlmIsDone() && allocations == 0 && opens == 1);
  assert(logSuppressions == 0 && httpLogLevel == ESP_LOG_ERROR);
  // Snapshot allocation failure closes admission immediately, even with a
  // previously working target. A later successful publication re-enables it.
  resetServer(); failConfigAllocation = true;
  assert(!httpLlmConfigure(true, "http://new/v1", "openclaw/default", "new-token", true, ""));
  assert(!httpLlmIsReady() && !httpLlmStartAsync("must not reach old target", {}));
  LlmModelDesc unavailable{}; assert(httpLlmEnumerate(&unavailable, 1) == 1 && !unavailable.available);
  assert(httpLlmConfigure(true, "http://other/v1", "openclaw/default", "different", true, ""));
  assert(httpLlmIsReady() && httpLlmStatus().state == LlmState::READY);
  // A successful identical publication during a request does not cancel it or
  // rotate the conversation, including the common settings-reload path.
  resetServer(); readHook = [] { if (readOffset == 12) {
    assert(httpLlmConfigure(true, "http://other/v1", "openclaw/default", "different", true, ""));
  } };
  assert(httpLlmStartAsync("same settings", {})); drainWorker();
  assert(shuts == 0 && response() == "answer");
  conversation = user();
  resetServer(); assert(httpLlmStartAsync("same conversation", {})); drainWorker(); assert(user() == conversation);
  // Losing the config lock still closes the atomic admission gate. The worker
  // reconciles its pending visible error before releasing the old transport.
  resetServer(); readHook = [] { if (readOffset == 12) {
    failNextLock = true;
    assert(!httpLlmConfigure(true, "http://new/v1", "openclaw/default", "new-token", true, ""));
    assert(!httpLlmIsReady());
  } };
  assert(httpLlmStartAsync("lock publication failure", {})); drainWorker();
  assert(shuts == 1 && response().find("configuration could not be applied") != std::string::npos);
  assert(!httpLlmStartAsync("old config remains barred", {}));
  assert(httpLlmConfigure(true, "http://other/v1", "openclaw/default", "different", true, ""));
  resetServer(); readHook = [] { if (readOffset == 12) {
    failConfigAllocation = true;
    assert(!httpLlmConfigure(true, "http://new/v1", "openclaw/default", "new-token", true, ""));
  } };
  assert(httpLlmStartAsync("allocation publication failure", {})); drainWorker();
  assert(shuts == 1 && response().find("configuration could not be applied") != std::string::npos);
  assert(!httpLlmIsReady() && !httpLlmStartAsync("old config remains barred", {}));
  // Phone lifecycle: the source is a choice that never depends on the path.
  // Listed, selectable and ready while the phone link is coming back; a chat
  // is admitted then (the transport waits for the link). Held or relay off is
  // not ready, with the reason in the status.
  assert(httpLlmConfigure(true, "https://relay/v1", "openclaw/default", "device-token", false, "test-ca"));
  WiFi.connected = false; phoneRelay = false;
  phoneView(PhonePathState::Retrying, PhonePathHint::NotHeard, 12000);
  { LlmModelDesc row{}; assert(httpLlmEnumerate(&row, 1) == 1 && row.available); }
  { char reason[96]; assert(httpLlmUnavailableReason(reason, sizeof(reason)) == 0 && !reason[0]); }
  httpLlmUnload(); assert(httpLlmSelectByName("openclaw", error, sizeof(error)));
  assert(httpLlmIsReady() && !httpLlmPathUp());
  { const LlmStatus st = httpLlmStatus();
    assert(st.state == LlmState::READY);
    assert(!strcmp(st.errorMsg, "phone not heard (out of range, or phone Bluetooth/app off?), retrying in 12 s")); }
  { char path[160]; assert(httpLlmPathText(path, sizeof(path)) == strlen(path) && !strcmp(path, httpLlmStatus().errorMsg)); }
  resetServer(); assert(httpLlmStartAsync("while the phone comes back", {}) > 0); drainWorker();
  assert(opens == 1 && response() == "answer");
  phoneView(PhonePathState::Connecting);
  assert(httpLlmIsReady() && !strcmp(httpLlmStatus().errorMsg, "connecting to phone"));
  struct HeldCase { PhonePathState state; PhonePathHint hint; const char* text; };
  for (const HeldCase& c : {HeldCase{PhonePathState::Held, PhonePathHint::ClosedByUser, "phone link closed (openphone to reconnect)"},
                            HeldCase{PhonePathState::RelayOff, PhonePathHint::None, "relay off in the app"},
                            HeldCase{PhonePathState::NotPaired, PhonePathHint::None, "no phone paired (phone pair new)"},
                            HeldCase{PhonePathState::Off, PhonePathHint::None, "no network: Wi-Fi off and no phone link"}}) {
    phoneView(c.state, c.hint);
    assert(!httpLlmIsReady() && !httpLlmStartAsync("refused", {}));
    const LlmStatus st = httpLlmStatus(); assert(st.state == LlmState::ERROR && !strcmp(st.errorMsg, c.text));
    // Still listed and selectable: the phone being down never refuses the source.
    LlmModelDesc row{}; assert(httpLlmEnumerate(&row, 1) == 1 && row.available);
    httpLlmUnload(); assert(httpLlmSelectByName("openclaw", error, sizeof(error)));
  }
  // A failed request no longer turns the source to ERROR: it stays READY and
  // the status carries the last request's error until the next one.
  WiFi.connected = true; pathViewSet = false;
  assert(httpLlmPathUp());
  { char path[32]; httpLlmPathText(path, sizeof(path)); assert(!strcmp(path, "Wi-Fi")); }
  resetServer(); status = 500; assert(httpLlmStartAsync("fails once", {})); drainWorker();
  { const LlmStatus st = httpLlmStatus();
    assert(st.state == LlmState::READY && httpLlmIsReady());
    assert(!strcmp(st.errorMsg, "last request: Agent HTTP 500; request was not retried")); }
  resetServer(); assert(httpLlmStartAsync("works again", {})); drainWorker();
  assert(httpLlmStatus().state == LlmState::READY && !httpLlmStatus().errorMsg[0]);
  // Configuration still makes it unavailable, with the reason.
  assert(httpLlmConfigure(false, "https://relay/v1", "openclaw/default", "device-token", false, "test-ca"));
  { char reason[96]; assert(httpLlmUnavailableReason(reason, sizeof(reason)) && !strcmp(reason, "HTTP agent disabled (llmhttpenabled)"));
    LlmModelDesc row{}; assert(httpLlmEnumerate(&row, 1) == 0); }
  assert(!httpLlmSelectByName("openclaw", error, sizeof(error)));
  assert(httpLlmConfigure(true, "https://relay/v1", "openclaw/default", "device-token", false, "test-ca"));
  assert(httpLlmSelectByName("openclaw", error, sizeof(error)));
  // Terminal runs the actual bounded JSON worker using the same immutable
  // credentials, without exposing its JSON in the shared chat result.
  assert(httpLlmConfigure(true, "https://relay/v1", "openclaw/default", "device-token", false, "test-ca"));
  const std::string priorChat = response();
  const char* operation = "{\"schema\":1,\"op\":\"list\"}";
  HttpTerminalToken token = 0, refused = 0;
  HttpTerminalResult terminalResult{};
  char terminalOutput[HTTP_TERMINAL_JSON_CAP + 1];
  resetServer(); terminalJson = true; wire = "{\"schema\":1,\"ok\":true}";
  WiFi.connected=false;
  assert(!httpTerminalPathAvailable());
  assert(httpTerminalStart(operation, strlen(operation), &token)==HttpTerminalStartResult::Unavailable && !token);
  assert(opens==0);
  // Without Wi-Fi the phone is the path only while its relay is on; a READY
  // link with relay off carries the console only.
  phoneRelay=true;assert(httpTerminalPathAvailable());phoneRelay=false;assert(!httpTerminalPathAvailable());
  WiFi.connected=true;
  assert(llmHttpRequestAdmit(LlmHttpRequestKind::Terminal,true)==LlmHttpAdmission::Accepted);
  assert(httpTerminalStart(operation, strlen(operation), &token)==HttpTerminalStartResult::Busy && !token);
  assert(opens==0);llmHttpRequestRelease(LlmHttpRequestKind::Terminal);
  const uint32_t revision = httpTerminalConfigRevision();
  // One worker task and one run serve the whole sequence below, until the
  // idle exit. (The refused start above made and freed one run.)
  const int runBase = terminalRunAllocations;
  assert(revision && httpTerminalStart(operation, strlen(operation), &token) == HttpTerminalStartResult::Accepted);
  assert(httpTerminalStart(operation, strlen(operation), &refused) == HttpTerminalStartResult::Busy && !refused);
  assert(httpTerminalRead(token, terminalOutput, sizeof(terminalOutput), &terminalResult) && !terminalResult.done);
  drainTerminal(); assert(terminalKept() && terminalCreates == 1 && allocations == 1); assertRunsWiped();
  assert(opens == 1 && cleanups == 1 && requestUrl == "https://relay/v1/terminal");
  assert(request == operation && auth == "Bearer device-token");
  assert(httpTerminalRead(token, terminalOutput, sizeof(terminalOutput), &terminalResult));
  assert(terminalResult.done && !terminalResult.error[0] && terminalOutput == wire);
  assert(terminalResult.configRevision == revision && response() == priorChat);
  assert(!httpTerminalRead(token, terminalOutput, 2, &terminalResult));
  assertTerminalAdmissionFree();
  resetServer();status=503;wire="busy";terminalJson=false;
  assert(httpTerminalStart(operation, strlen(operation), &token)==HttpTerminalStartResult::Accepted);
  drainTerminal();assert(httpTerminalRead(token,terminalOutput,sizeof(terminalOutput),&terminalResult));
  assert(terminalResult.httpStatus==503 && !strcmp(terminalResult.error,"relay busy with an earlier request (outcome unknown)"));
  assert(opens==1 && !terminalOutput[0]); assertRunsWiped();
  const HttpTerminalToken previousToken = token;
  resetServer(); terminalJson = true; wire = "{}";
  assert(httpTerminalStart(operation, strlen(operation), &token) == HttpTerminalStartResult::Accepted);
  assert(!httpTerminalRead(previousToken, terminalOutput, sizeof(terminalOutput), &terminalResult));
  httpTerminalCancel(previousToken); // A stale close cannot cancel the new view.
  drainTerminal(); assert(!shuts);
  for (int fault = 0; fault < 4; ++fault) {
    resetServer(); terminalJson = true; wire = "{}";
    if (fault == 0) wire.assign(HTTP_TERMINAL_JSON_CAP + 1, 'x');
    if (fault == 1) completeResponse = false;
    if (fault == 2) status = 302;
    if (fault == 3) failWrite = true;
    assert(httpTerminalStart(operation, strlen(operation), &token) == HttpTerminalStartResult::Accepted);
    drainTerminal();
    assert(httpTerminalRead(token, terminalOutput, sizeof(terminalOutput), &terminalResult));
    assert(terminalResult.done && terminalResult.error[0] && !terminalOutput[0] && opens == 1);
    assertRunsWiped(); assertTerminalAdmissionFree();
  }
  resetServer(); terminalJson = true; wire = "{}";
  assert(httpTerminalStart(operation, strlen(operation), &token) == HttpTerminalStartResult::Accepted);
  httpTerminalCancel(token); drainTerminal(); assert(opens == 0);
  assert(httpTerminalRead(token, terminalOutput, sizeof(terminalOutput), &terminalResult));
  assert(terminalResult.done && !strcmp(terminalResult.error, "Terminal stopped locally; host outcome may be unknown"));
  // No stale data: a long body and response, then a short one. The host sees
  // exactly the new body, the reader exactly the new response.
  const std::string longBody = "{\"schema\":1,\"op\":\"prompt\",\"text\":\"" + std::string(3000, 'b') + "\"}";
  resetServer(); terminalJson = true; wire = "{\"text\":\"" + std::string(3500, 'L') + "\"}";
  assert(httpTerminalStart(longBody.c_str(), longBody.size(), &token) == HttpTerminalStartResult::Accepted);
  drainTerminal(); assert(request == longBody);
  assert(httpTerminalRead(token, terminalOutput, sizeof(terminalOutput), &terminalResult) && terminalOutput == wire);
  assertRunsWiped();
  resetServer(); terminalJson = true; wire = "{\"ok\":1}";
  assert(httpTerminalStart(operation, strlen(operation), &token) == HttpTerminalStartResult::Accepted);
  drainTerminal(); assert(request == operation);
  assert(httpTerminalRead(token, terminalOutput, sizeof(terminalOutput), &terminalResult));
  assert(terminalResult.length == wire.size() && terminalOutput == wire);
  assert(!memchr(sTerminalResponse + wire.size(), 'L', sizeof(sTerminalResponse) - wire.size()));
  // A start while the worker is between its last request and its wait (no lock
  // held): the request is staged, the wait returns at once and serves it.
  resetServer(); terminalJson = true; wire = "{\"first\":1}";
  HttpTerminalToken second = 0;
  parkHook = [&] {
    resetServer(); terminalJson = true; wire = "{\"second\":2}";
    assert(httpTerminalStart(operation, strlen(operation), &second) == HttpTerminalStartResult::Accepted);
  };
  assert(httpTerminalStart(operation, strlen(operation), &token) == HttpTerminalStartResult::Accepted);
  drainTerminal(); assert(second && second != token && !parkHook && opens == 1);
  assert(!httpTerminalRead(token, terminalOutput, sizeof(terminalOutput), &terminalResult));
  assert(httpTerminalRead(second, terminalOutput, sizeof(terminalOutput), &terminalResult));
  assert(terminalResult.done && !strcmp(terminalOutput, "{\"second\":2}"));
  // A wake with nothing staged (a cancel after completion) parks it again for
  // the rest of the idle time only.
  nowMs += 20000; httpTerminalCancel(second); drainTerminal();
  assert(terminalKept() && lastParkTicks == SERVICE_TASK_IDLE_RETIRE_MS - 20000);
  assert(terminalCreates == 1 && terminalDeletes == 0 && terminalRunAllocations == runBase + 1 && allocations == 1);
  // After the idle time it leaves: the run is freed wiped (ps_free checks), the
  // task deleted once; the next start makes a new one.
  idleTerminal(); assert(!terminalKept() && terminalDeletes == 1 && allocations == 0 && terminalRuns.empty());
  resetServer(); terminalJson = true; wire = "{}";
  assert(httpTerminalStart(operation, strlen(operation), &token) == HttpTerminalStartResult::Accepted);
  drainTerminal(); assert(terminalCreates == 2 && terminalKept() && terminalRunAllocations == runBase + 2);
  // Released while parked (view closed): it leaves at once.
  httpTerminalReleaseWorker(); drainTerminal();
  assert(!terminalKept() && terminalDeletes == 2 && allocations == 0);
  httpTerminalReleaseWorker();  // nothing kept: nothing to wake
  assert(httpTerminalRead(token, terminalOutput, sizeof(terminalOutput), &terminalResult) && !strcmp(terminalOutput, "{}"));
  // Released while a request is in flight (view closed mid-request): the
  // request ends as before, then the worker leaves.
  resetServer(); terminalJson = true; wire = "{}";
  assert(httpTerminalStart(operation, strlen(operation), &token) == HttpTerminalStartResult::Accepted);
  httpTerminalCancel(token); httpTerminalReleaseWorker(); drainTerminal();
  assert(opens == 0 && !terminalKept() && terminalCreates == 3 && terminalDeletes == 3 && allocations == 0);
  assert(httpTerminalRead(token, terminalOutput, sizeof(terminalOutput), &terminalResult) && terminalResult.done &&
         !strcmp(terminalResult.error, "Terminal stopped locally; host outcome may be unknown"));
  assertTerminalAdmissionFree();
  // Released, then a new view starts before the worker woke: it is reused.
  resetServer(); terminalJson = true; wire = "{}";
  assert(httpTerminalStart(operation, strlen(operation), &token) == HttpTerminalStartResult::Accepted);
  drainTerminal(); assert(terminalCreates == 4);
  httpTerminalReleaseWorker();
  assert(httpTerminalStart(operation, strlen(operation), &token) == HttpTerminalStartResult::Accepted);
  drainTerminal(); assert(terminalKept() && terminalCreates == 4 && opens == 2);
  // The configuration withdrawn while parked (a failed publication, applied by
  // the tick): the wake makes it leave.
  failNextLock = true;
  assert(!httpLlmConfigure(true, "https://relay/v1", "openclaw/default", "device-token", false, "test-ca"));
  httpLlmTick(); drainTerminal(); assert(!terminalKept() && terminalDeletes == 4 && allocations == 0);
  assert(!httpTerminalConfigRevision());
  assert(httpTerminalStart(operation, strlen(operation), &token) == HttpTerminalStartResult::Unavailable && !token);
  assert(httpLlmConfigure(true, "https://relay/v1", "openclaw/default", "device-token", false, "test-ca"));
  // A changed configuration while parked: it leaves.
  resetServer(); terminalJson = true; wire = "{}";
  assert(httpTerminalStart(operation, strlen(operation), &token) == HttpTerminalStartResult::Accepted);
  drainTerminal(); assert(terminalCreates == 5);
  assert(httpLlmConfigure(true, "https://relay/v1", "openclaw/default", "rotated-token", false, "test-ca"));
  drainTerminal(); assert(!terminalKept() && terminalDeletes == 5 && allocations == 0);
  resetServer(); terminalJson = true; wire = "{}";
  assert(httpTerminalStart(operation, strlen(operation), &token) == HttpTerminalStartResult::Accepted);
  drainTerminal(); assert(terminalCreates == 6 && auth == "Bearer rotated-token");
  // Busy and Unavailable while a request is staged on the kept worker make
  // nothing and leave the staged run as it is.
  resetServer(); terminalJson = true; wire = "{}";
  assert(httpTerminalStart(operation, strlen(operation), &token) == HttpTerminalStartResult::Accepted);
  assert(httpTerminalStart(operation, strlen(operation), &refused) == HttpTerminalStartResult::Busy && !refused);
  WiFi.connected = false;
  assert(httpTerminalStart(operation, strlen(operation), &refused) == HttpTerminalStartResult::Unavailable && !refused);
  WiFi.connected = true;
  assert(runHolds(operation) && terminalNotifications == 1);
  drainTerminal(); assert(request == operation && auth == "Bearer rotated-token" && terminalRunAllocations == runBase + 7);  // six workers, one refused start
  assert(httpTerminalRead(token, terminalOutput, sizeof(terminalOutput), &terminalResult) && !strcmp(terminalOutput, "{}"));
  // Admission refused with the worker parked: Busy, the run wiped again, no wake.
  assert(llmHttpRequestAdmit(LlmHttpRequestKind::Terminal, true) == LlmHttpAdmission::Accepted);
  assert(httpTerminalStart(operation, strlen(operation), &refused) == HttpTerminalStartResult::Busy && !refused);
  llmHttpRequestRelease(LlmHttpRequestKind::Terminal);
  assert(terminalKept() && terminalNotifications == 0 && allocations == 1); assertRunsWiped();
  // The admission gate closed while a request is staged: the next start is
  // Unavailable and leaves that run alone; the request stops locally and the
  // worker then leaves.
  resetServer(); terminalJson = true; wire = "{}";
  assert(httpTerminalStart(operation, strlen(operation), &token) == HttpTerminalStartResult::Accepted);
  failNextLock = true;
  assert(!httpLlmConfigure(true, "https://relay/v1", "openclaw/default", "rotated-token", false, "test-ca"));
  assert(httpTerminalStart(operation, strlen(operation), &refused) == HttpTerminalStartResult::Unavailable && !refused);
  assert(runHolds(operation));
  drainTerminal(); assert(opens == 0 && !terminalKept() && allocations == 0);
  assert(httpTerminalRead(token, terminalOutput, sizeof(terminalOutput), &terminalResult) && terminalResult.done &&
         !strcmp(terminalResult.error, "Terminal configuration changed"));
  assert(!strcmp(sTerminalResult.error, "Terminal stopped locally; host outcome may be unknown"));
  assertTerminalAdmissionFree();
  assert(httpLlmConfigure(true, "https://relay/v1", "openclaw/default", "rotated-token", false, "test-ca"));
  // The kept worker leaves between the start's check and its lock (the start's
  // second bounded take, after httpTerminalPathAvailable): the start makes a run
  // and a new worker, as with none kept.
  resetServer(); terminalJson = true; wire = "{}";
  assert(httpTerminalStart(operation, strlen(operation), &token) == HttpTerminalStartResult::Accepted);
  drainTerminal(); assert(terminalKept()); request.clear();
  int createsNow = terminalCreates, runsNow = terminalRunAllocations;
  lockHookTake = 2; lockHook = [] { idleTerminal(); assert(!terminalKept() && allocations == 0); };
  assert(httpTerminalStart(operation, strlen(operation), &token) == HttpTerminalStartResult::Accepted);
  assert(!lockHook && terminalCreates == createsNow + 1 && terminalRunAllocations == runsNow + 1);
  drainTerminal(); assert(terminalKept() && allocations == 1 && request == operation);
  httpTerminalReleaseWorker(); drainTerminal(); assert(!terminalKept() && allocations == 0);
  // Another start keeps a worker between this start's allocation and its lock:
  // this start uses the kept worker and frees the run it made.
  resetServer(); terminalJson = true; wire = "{}";
  createsNow = terminalCreates; runsNow = terminalRunAllocations;
  HttpTerminalToken other = 0;
  lockHookTake = 2; lockHook = [&] {
    assert(httpTerminalStart(operation, strlen(operation), &other) == HttpTerminalStartResult::Accepted);
    drainTerminal(); assert(terminalKept() && allocations == 2);
    resetServer(); terminalJson = true; wire = "{\"outer\":1}";
  };
  assert(httpTerminalStart(operation, strlen(operation), &token) == HttpTerminalStartResult::Accepted);
  assert(other && token != other && terminalCreates == createsNow + 1 && terminalRunAllocations == runsNow + 2);
  assert(allocations == 1 && terminalNotifications == 1);
  drainTerminal(); assert(httpTerminalRead(token, terminalOutput, sizeof(terminalOutput), &terminalResult) &&
                          terminalResult.done && !strcmp(terminalOutput, "{\"outer\":1}") && opens == 1);
  httpTerminalReleaseWorker(); drainTerminal(); assert(!terminalKept() && allocations == 0);
  // No memory for the first run: Busy, as before; a failed task start: Busy
  // with the old error, nothing kept, nothing left allocated.
  failTerminalRunAllocation = true;
  assert(httpTerminalStart(operation, strlen(operation), &token) == HttpTerminalStartResult::Busy && !token);
  const int createsBefore = terminalCreates;
  failTask = true;
  assert(httpTerminalStart(operation, strlen(operation), &token) == HttpTerminalStartResult::Busy && !token);
  assert(terminalCreates == createsBefore && !terminalKept() && allocations == 0 && terminalRuns.empty());
  assert(!strcmp(sTerminalResult.error, "Could not start Terminal worker") && sTerminalResult.done);
  assertTerminalAdmissionFree();
  // The configuration changes mid-request: the request ends as before and the
  // worker then leaves.
  resetServer(); terminalJson = true; wire = "{}";
  readHook = [] { if (!readOffset) assert(httpLlmConfigure(true, "https://replacement/v1",
      "openclaw/default", "replacement-token", false, "test-ca")); };
  assert(httpTerminalStart(operation, strlen(operation), &token) == HttpTerminalStartResult::Accepted);
  drainTerminal();
  assert(httpTerminalConfigRevision() != revision && shuts == 1);
  assert(httpTerminalRead(token, terminalOutput, sizeof(terminalOutput), &terminalResult));
  assert(terminalResult.error[0] && !terminalOutput[0] && response() == priorChat);
  assert(!terminalKept() && terminalCreates == terminalDeletes);
  assert(wipedAllocations > 0 && sensitiveAllocations.empty() && allocations == 0);
  // Released for speech while parked: the call waits until the worker has
  // left and its stack had two ticks to be freed, then answers true.
  resetServer(); terminalJson = true; wire = "{}";
  assert(httpTerminalStart(operation, strlen(operation), &token) == HttpTerminalStartResult::Accepted);
  drainTerminal(); assert(terminalKept());
  {
    const int deletes = terminalDeletes; const uint32_t before = nowMs;
    delayHook = drainIfKept;
    assert(httpTerminalReleaseWorkerAndWait(50));
    delayHook = nullptr;
    assert(!terminalKept() && terminalDeletes == deletes + 1 && allocations == 0 && terminalRuns.empty());
    assert(nowMs - before == 3);  // one tick until it left, two for the stack
  }
  // Nothing kept: true at once, no wait.
  { const uint32_t before = nowMs; assert(httpTerminalReleaseWorkerAndWait(50) && nowMs == before); }
  // A request in flight is not waited for: false at once, and the worker
  // leaves after that request, as httpTerminalReleaseWorker asks.
  resetServer(); terminalJson = true; wire = "{}";
  assert(httpTerminalStart(operation, strlen(operation), &token) == HttpTerminalStartResult::Accepted);
  {
    const uint32_t before = nowMs;
    assert(!httpTerminalReleaseWorkerAndWait(50) && nowMs == before && terminalKept());
    drainTerminal(); assert(!terminalKept() && allocations == 0);
  }
  // A worker that does not get to run in time: false after waitMs.
  resetServer(); terminalJson = true; wire = "{}";
  assert(httpTerminalStart(operation, strlen(operation), &token) == HttpTerminalStartResult::Accepted);
  drainTerminal(); assert(terminalKept());
  {
    const uint32_t before = nowMs;
    assert(!httpTerminalReleaseWorkerAndWait(50) && nowMs - before == 50 && terminalKept());
    drainTerminal(); assert(!terminalKept() && allocations == 0);
  }
  assert(terminalCreates == terminalDeletes);
  std::cout << "HTTP worker lifecycle tests passed\n";
}
