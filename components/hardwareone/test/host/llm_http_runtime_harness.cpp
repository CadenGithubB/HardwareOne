#include "System_LLMHttpCore.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <atomic>
#include <new>

// Arduino declares this global type; the HTTP SSE parser must stay qualified.
class Stream {};

#define ENABLE_LLM_BACKEND 1
#define ENABLE_LLM_SOURCE_HTTP 1
#define CONFIG_MBEDTLS_CERTIFICATE_BUNDLE 0
#define EXT_RAM_BSS_ATTR
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 0xffffffff
#define pdMS_TO_TICKS(x) (x)
using SemaphoreHandle_t = std::mutex*;
static SemaphoreHandle_t xSemaphoreCreateMutex() { return new std::mutex; }
static bool failNextLock = false;
static int xSemaphoreTake(SemaphoreHandle_t p, unsigned wait) {
  if (failNextLock && wait != portMAX_DELAY) { failNextLock = false; return 0; }
  p->lock(); return 1;
}
static void xSemaphoreGive(SemaphoreHandle_t p) { p->unlock(); }
static void vSemaphoreDelete(SemaphoreHandle_t p) { delete p; }
static uint32_t nowMs = 10;
static uint32_t millis() { return nowMs; }
static void vTaskDelay(unsigned n) { nowMs += n; }
static void (*pendingTask)(void*) = nullptr;
static void* pendingArg = nullptr;
static bool failTask = false;
static int xTaskCreate(void (*fn)(void*), const char*, int, void* ctx, int, void*) {
  if (failTask) { failTask = false; return 0; }
  assert(!pendingTask); pendingTask = fn; pendingArg = ctx; return 1;
}
static void vTaskDelete(void*) {}
enum class AllocPref { PreferPSRAM };
static size_t allocations = 0;
static bool failConfigAllocation = false;
static void* ps_alloc(size_t n, AllocPref, const char*) { ++allocations; return malloc(n); }
static void* ps_calloc(size_t n, size_t s, AllocPref, const char* tag) {
  if (failConfigAllocation && !strcmp(tag, "llm.http.config")) {
    failConfigAllocation = false; return nullptr;
  }
  ++allocations; return calloc(n, s);
}
static void* ps_realloc(void* p, size_t n, AllocPref, const char*) { return realloc(p, n); }
static void ps_free(void* p) { if (p) { --allocations; free(p); } }
static void esp_fill_random(void* p, size_t n) {
  static uint8_t x = 0; auto* b = static_cast<uint8_t*>(p);
  while (n--) *b++ = ++x;
}
struct Wifi { bool connected = true; bool isConnected() { return connected; } } WiFi;
enum class LlmBackendKind { None, Onboard, Cm5, Http };
constexpr unsigned LLM_STORAGE_REMOTE = 2;
struct LlmModelDesc { char id[48], name[32]; LlmBackendKind backend; char path[64]; uint32_t sizeKB; uint8_t storage; bool available; };
enum class LLMState { UNLOADED, LOADING, READY, GENERATING, ERROR };
struct LLMStatus { LLMState state; char modelPath[64]; char errorMsg[128]; };
struct LLMGenParams {};
using esp_err_t = int;
constexpr int ESP_LOG_WARN = 2, ESP_LOG_DEBUG = 4;
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
static std::function<void()> readHook;
static esp_http_client_handle_t esp_http_client_init(esp_http_client_config_t* cfg) {
  assert(httpLogLevel <= ESP_LOG_WARN);
  assert(cfg->disable_auto_redirect && cfg->max_authorization_retries == -1);
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
      eventType ? "text/event-stream; charset=utf-8" : "text/html", c->cfg.user_data};
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
}
static void drainWorker() {
  assert(pendingTask); auto task = pendingTask; void* arg = pendingArg;
  pendingTask = nullptr; pendingArg = nullptr; task(arg);
  assert(httpLlmIsDone()); assert(allocations == 0);
  assert(logSuppressions == 1);
  assert(ignoreLogSuppression || httpLogLevel == ESP_LOG_WARN);
}
static std::string response() {
  char out[2048]; httpLlmResultChunk(0, out, sizeof(out)); return out;
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
  // Snapshot allocation failure closes admission immediately, even with a
  // previously working target. A later successful publication re-enables it.
  resetServer(); failConfigAllocation = true;
  assert(!httpLlmConfigure(true, "http://new/v1", "openclaw/default", "new-token", true, ""));
  assert(!httpLlmIsReady() && !httpLlmStartAsync("must not reach old target", {}));
  LlmModelDesc unavailable{}; assert(httpLlmEnumerate(&unavailable, 1) == 1 && !unavailable.available);
  assert(httpLlmConfigure(true, "http://other/v1", "openclaw/default", "different", true, ""));
  assert(httpLlmIsReady() && httpLlmStatus().state == LLMState::READY);
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
  std::cout << "HTTP worker lifecycle tests passed\n";
}
