#include "System_LLMHttp.h"
#ifdef ESP_PLATFORM
#include "System_Events.h"  // systemEventPost: typed system-event ring (host builds have no ring)
#endif
#if ENABLE_LLM_BACKEND && ENABLE_LLM_SOURCE_HTTP

#include "System_LLMHttpCore.h"
#include "System_LLMHttpTransport.h"
#include "System_MemUtil.h"
#include "System_TaskUtils.h"  // SERVICE_TASK_IDLE_RETIRE_MS: the Terminal worker's idle exit
#include <atomic>
#include <new>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_random.h>
#include <mbedtls/platform_util.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace {
using namespace hw1http;
constexpr uint32_t kIoTimeoutMs = 2000;
constexpr uint32_t kStallMs = 60000;
constexpr uint32_t kTotalMs = 300000;
constexpr size_t kBodyCap = 14336;
struct Config {
  bool enabled = false, plaintext = false, valid = false;
  char url[kUrlMax + 24]{};
  char model[kModelMax + 1]{};
  char token[kTokenMax + 1]{};
  char ca[kCaMax + 1]{};
};
EXT_RAM_BSS_ATTR Config sConfig;
EXT_RAM_BSS_ATTR char sResult[kResultCap];
size_t sResultLen = 0;
char sError[128]{};
char sConfigError[96]{};
char sConversation[48]{};
bool sSelected = false, sWorker = false, sDone = true;
LlmHttpIo* sIo = nullptr;
uint32_t sStartedMs = 0;
uint32_t sRevision = 1;
int sSession = 0, sNextSession = 1;
std::atomic<bool> sCancel{false};
// A settings publication failure must never silently leave the previous target
// usable. This gate can close even when allocation or the source mutex fails.
std::atomic<bool> sConfigAdmitted{false};
std::atomic<bool> sConfigFailurePending{false};
// A chat worker is in flight. The tick reads it before taking the source lock;
// a start that lands after a false read is watched on the next lap.
std::atomic<bool> sHttpTickWatch{false};
std::atomic<SemaphoreHandle_t> sMutex{nullptr};
// Whether the published URL is HTTPS (the phone relay carries HTTPS only), for
// lock-free path checks from the backend tick.
std::atomic<bool> sSecure{true};
constexpr const char* kConfigPublicationError = "Agent configuration could not be applied; request stopped";

struct Lock {
  SemaphoreHandle_t mutex = nullptr;
  explicit Lock(bool wait = false) {
    auto m = sMutex.load(std::memory_order_acquire);
    if (!m) {
      auto candidate = xSemaphoreCreateMutex();
      if (!candidate) return;
      if (!sMutex.compare_exchange_strong(m, candidate, std::memory_order_acq_rel,
                                         std::memory_order_acquire)) vSemaphoreDelete(candidate);
      else m = candidate;
    }
    if (xSemaphoreTake(m, wait ? portMAX_DELAY : pdMS_TO_TICKS(200)) == pdTRUE) mutex = m;
  }
  ~Lock() { if (mutex) xSemaphoreGive(mutex); }
  explicit operator bool() const { return mutex != nullptr; }
};

void rotateLocked() {
  uint32_t nonce[4]; esp_fill_random(nonce, sizeof(nonce));
  snprintf(sConversation, sizeof(sConversation), "hw1:%08lx%08lx%08lx%08lx",
           (unsigned long)nonce[0], (unsigned long)nonce[1],
           (unsigned long)nonce[2], (unsigned long)nonce[3]);
}

void interruptSocketLocked() {
  // Published before connect; cancellation never frees worker-owned I/O.
  llmHttpIoCancel(sIo);
}

#if (ENABLE_G2_GLASSES && ENABLE_LOCAL_STT) || ENABLE_BLE_PHONE_PIPE
bool sTerminalWorker = false;
LlmHttpIo* sTerminalIo = nullptr;
uint32_t sTerminalToken = 0, sTerminalNextToken = 0;
std::atomic<bool> sTerminalCancel{false};
EXT_RAM_BSS_ATTR HttpTerminalResult sTerminalResult;
EXT_RAM_BSS_ATTR char sTerminalResponse[HTTP_TERMINAL_JSON_CAP + 1];
// The kept Terminal worker (System_LLMHttpTerminal.inc), under Lock; sTerminalKept
// mirrors sTerminalTask != nullptr for the start's check before it takes the lock.
TaskHandle_t sTerminalTask = nullptr;
std::atomic<bool> sTerminalKept{false};
bool sTerminalRetire = false;
void interruptTerminalLocked() {
  sTerminalCancel.store(true, std::memory_order_release);
  llmHttpIoCancel(sTerminalIo);
  // A parked worker wakes to see a withdrawn or changed configuration and leave.
  if (sTerminalTask) xTaskNotifyGive(sTerminalTask);
}
#else
void interruptTerminalLocked() {}
#endif

void descriptor(LlmModelDesc* out, bool available) {
  memset(out, 0, sizeof(*out));
  strlcpy(out->id, "http:openclaw", sizeof(out->id));
  strlcpy(out->name, "openclaw", sizeof(out->name));
  out->backend = LlmBackendKind::Http;
  out->storage = LLM_STORAGE_REMOTE;
  out->available = available;
}

// Bound JSON allocation as well as the wire frame size. An 8KB event with many
// tiny members can otherwise expand into far more heap than its input size.
class JsonBudget : public ArduinoJson::Allocator {
  struct alignas(std::max_align_t) Header { size_t size; };
  size_t used_ = 0;
  static constexpr size_t limit_ = 24576;
 public:
  void* allocate(size_t size) override {
    if (size > limit_ - used_) return nullptr;
    auto* p = static_cast<Header*>(ps_alloc(sizeof(Header) + size, AllocPref::PreferPsram, "llm.http.json"));
    if (!p) return nullptr;
    p->size = size; used_ += size; return p + 1;
  }
  void deallocate(void* ptr) override {
    if (!ptr) return;
    auto* p = static_cast<Header*>(ptr) - 1; used_ -= p->size; ps_free(p);
  }
  void* reallocate(void* ptr, size_t size) override {
    if (!ptr) return allocate(size);
    auto* p = static_cast<Header*>(ptr) - 1;
    if (size > limit_ - (used_ - p->size)) return nullptr;
    const size_t old = p->size;
    auto* next = static_cast<Header*>(ps_realloc(p, sizeof(Header) + size, AllocPref::PreferPsram, "llm.http.json"));
    if (!next) return nullptr;
    next->size = size; used_ = used_ - old + size; return next + 1;
  }
};

struct Run {
  Config config;
  int session;
  uint32_t revision;
  char conversation[48];
  char prompt[kPromptMax + 1];
  bool contentType = false;
};

esp_err_t httpEvent(esp_http_client_event_t* event) {
  if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value &&
      !strcasecmp(event->header_key, "Content-Type")) {
    auto* run = static_cast<Run*>(event->user_data);
    const char* v = event->header_value;
    run->contentType = !strncasecmp(v, "text/event-stream", 17) &&
                       (!v[17] || v[17] == ';' || v[17] == ' ');
  }
  return ESP_OK;
}

void publish(const Run& run, const char* text, size_t size, const char* error, bool terminal) {
  Lock lock(true);
  if (!lock) return;
  if (sSession != run.session || sRevision != run.revision) return;
  // Stop owns the terminal message once cancelled. No late network bytes leak.
  if (!sCancel.load(std::memory_order_acquire) && sConfigAdmitted.load(std::memory_order_acquire)) {
    memcpy(sResult, text, size + 1); sResultLen = size;
    if (error) strlcpy(sError, error, sizeof(sError));
  }
  if (terminal) {
    sDone = true;
#ifdef ESP_PLATFORM
    if (!sCancel.load(std::memory_order_acquire)) {
      char det[24];
      snprintf(det, sizeof(det), "%u chars", (unsigned)sResultLen);
      systemEventPost(SYSEVT_LLM_GEN_DONE, error ? "error" : "eos", det);
    }
#endif
  }
}

void appendTerminalLocked(const char* message) {
  // All published text reserves 96 bytes for truncation/terminal notices.
  if (sResultLen >= sizeof(sResult) - 1) return;
  int n = snprintf(sResult + sResultLen, sizeof(sResult) - sResultLen,
                   "\n[%.60s]", message);
  if (n > 0) sResultLen += static_cast<size_t>(n) < sizeof(sResult) - sResultLen ?
      static_cast<size_t>(n) : sizeof(sResult) - sResultLen - 1;
}

void failConfigurationPublication() {
  sConfigAdmitted.store(false, std::memory_order_release);
  sConfigFailurePending.store(true, std::memory_order_release);
  sCancel.store(true, std::memory_order_release);
#if (ENABLE_G2_GLASSES && ENABLE_LOCAL_STT) || ENABLE_BLE_PHONE_PIPE
  sTerminalCancel.store(true, std::memory_order_release);
#endif
}

void applyConfigurationFailureLocked() {
  if (!sConfigFailurePending.exchange(false, std::memory_order_acq_rel)) return;
  ++sRevision;
  rotateLocked();
  strlcpy(sError, kConfigPublicationError, sizeof(sError));
  if (sWorker) appendTerminalLocked(sError);
  interruptSocketLocked();
  interruptTerminalLocked();
}

// The worker exclusively owns client and transport. Stop cancels the published
// I/O handle; only this worker closes/frees it after unpublishing under Lock.
void worker(void* context) {
  const uint32_t start = millis();
  // ESP-IDF's HTTP_CLIENT debug logs print the complete outbound header block
  // (including Authorization) and inbound header values. Suppress that tag
  // before initialization/header setup and keep it suppressed after cleanup.
  // Do not restore a previous debug level while another request can start.
  // Only lowered: a quieter saved `esplog HTTP_CLIENT` level stays.
  if (esp_log_level_get("HTTP_CLIENT") > ESP_LOG_WARN) esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN);
  auto* run = static_cast<Run*>(context);
  // Some IDF profiles compile dynamic log-level control out, making set_level
  // a no-op. Fail before constructing a credential-bearing HTTP client if its
  // effective level still permits header dumps.
  const char* error = esp_log_level_get("HTTP_CLIENT") >= ESP_LOG_DEBUG ?
      "HTTP debug logging exposes credentials; lower log verbosity" : nullptr;
  char errorBuffer[112]{};
  JsonBudget allocator;
  auto* parserMemory = ps_alloc(sizeof(hw1http::Stream), AllocPref::PreferPsram, "llm.http.stream");
  auto* stream = parserMemory ? new (parserMemory) hw1http::Stream(&allocator) : nullptr;
  auto* body = static_cast<char*>(ps_alloc(kBodyCap, AllocPref::PreferPsram, "llm.http.body"));
  esp_http_client_handle_t client = nullptr;
  LlmHttpIo* io = nullptr;
  size_t bodyLen = 0;
  if (!stream || !body) error = "Not enough memory for HTTP agent";
  if (!error) {
    JsonDocument doc(&allocator);
    doc["model"] = run->config.model;
    doc["user"] = run->conversation;
    doc["stream"] = true;
    JsonObject message = doc["messages"].to<JsonArray>().add<JsonObject>();
    message["role"] = "user";
    message["content"] = run->prompt;
    if (doc.overflowed() || measureJson(doc) >= kBodyCap) error = "Agent request exceeds memory limit";
    else bodyLen = serializeJson(doc, body, kBodyCap);
  }
  if (!error && !sCancel.load(std::memory_order_acquire)) {
    esp_http_client_config_t cfg{};
    cfg.url = run->config.url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = kIoTimeoutMs;
    cfg.disable_auto_redirect = true;
    cfg.max_authorization_retries = -1;
    cfg.buffer_size = 1024;
    cfg.buffer_size_tx = 1536;
    cfg.event_handler = httpEvent;
    cfg.user_data = run;
    cfg.keep_alive_enable = false;
    io = llmHttpIoCreate(run->config.ca, !strncasecmp(run->config.url, "https://", 8),
                         sCancel, start, kTotalMs, run->revision);
    if (io) { Lock lock(true); if (lock) sIo = io; }
    if (llmHttpIoConfigure(io, cfg)) client = esp_http_client_init(&cfg);
    if (!client) error = "Could not initialize HTTP agent";
  }
  if (!error && client) {
    // Header never enters diagnostics. URL validation forbids alternate auth
    // syntax and redirects are not followed, so this secret has one recipient.
    char authorization[kTokenMax + 8];
    snprintf(authorization, sizeof(authorization), "Bearer %s", run->config.token);
    if (esp_http_client_set_header(client, "Authorization", authorization) != ESP_OK ||
        esp_http_client_set_header(client, "Content-Type", "application/json") != ESP_OK ||
        esp_http_client_set_header(client, "Accept", "text/event-stream") != ESP_OK)
      error = "Could not set agent request headers";
    mbedtls_platform_zeroize(authorization, sizeof(authorization));
  }
  uint32_t lastData = start;
  if (!error && client && !sCancel.load(std::memory_order_acquire)) {
    if (esp_http_client_open(client, static_cast<int>(bodyLen)) != ESP_OK)
      error = "Agent connection failed; request was not retried";
    if (error && llmHttpIoRetireWaitFailed(io)) {
      // The path is up; the phone kept the previous stream: the path text
      // would name a working path as the reason.
      error = "Not sent: the phone did not release the previous request in time";
    } else if (error && llmHttpIoPathWaitFailed(io)) {
      // The bounded wait for a path ended (or the phone link stopped coming
      // back): no HTTP byte went out on any path.
      char path[96];
      llmHttpPathText(!strncasecmp(run->config.url, "https://", 8), path, sizeof(path));
      snprintf(errorBuffer, sizeof(errorBuffer), "Not sent: %s", path);
      error = errorBuffer;
    } else if (error && llmHttpIoRetriedSamePath(io)) {
      error = "Agent connection failed after one retry; request was not sent";
    }
    if (!error) {
      Lock lock(true);
      if (lock) {
        llmHttpIoConnected(io, client);
        if (sCancel.load(std::memory_order_acquire)) interruptSocketLocked();
      }
    }
    // Partial writes continue the SAME request, never replay a POST.
    size_t sent = 0;
    while (!error && sent < bodyLen && !sCancel.load(std::memory_order_acquire)) {
      if (uint32_t(millis() - start) >= kTotalMs) { error = "Agent request time limit reached; outcome unknown"; break; }
      int n = esp_http_client_write(client, body + sent, static_cast<int>(bodyLen - sent));
      if (n <= 0) error = "Agent request write failed; outcome unknown";
      else sent += static_cast<size_t>(n);
    }
    if (!error && !sCancel.load(std::memory_order_acquire)) {
      int64_t headers;
      do {
        headers = esp_http_client_fetch_headers(client);
        if (headers == -ESP_ERR_HTTP_EAGAIN) vTaskDelay(pdMS_TO_TICKS(10));
      } while (headers == -ESP_ERR_HTTP_EAGAIN && !sCancel.load(std::memory_order_acquire) &&
               static_cast<uint32_t>(millis() - start) < kStallMs);
      if (headers < 0) error = "Agent response headers timed out or failed; outcome unknown";
      else {
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
          snprintf(errorBuffer, sizeof(errorBuffer), "Agent HTTP %d; request was not retried", status);
          error = errorBuffer;
        } else if (!run->contentType) error = "Agent did not return text/event-stream";
      }
    }
    // read() may loop internally until its requested length is filled. A
    // one-byte read makes even a slow-drip stream return to our time/cancel
    // gates; TLS retains its own receive buffer. Publish only at event edges.
    char chunk[1];
    size_t published = 0;
    unsigned serviced = 0;
    lastData = millis();
    while (!error && !sCancel.load(std::memory_order_acquire) && !stream->done) {
      if (static_cast<uint32_t>(millis() - start) >= kTotalMs) { error = "Agent request time limit reached; outcome unknown"; break; }
      if (static_cast<uint32_t>(millis() - lastData) >= kStallMs) { error = "Agent stream stalled; outcome unknown"; break; }
      const int n = esp_http_client_read(client, chunk, sizeof(chunk));
      if (n == -ESP_ERR_HTTP_EAGAIN) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
      if (n < 0) { error = "Agent stream disconnected; outcome unknown"; break; }
      if (!n) { stream->eof(); error = stream->error; break; }
      lastData = millis();
      stream->feed(chunk, static_cast<size_t>(n));
      error = stream->error;
      if (stream->text.size != published) {
        publish(*run, stream->text.bytes, stream->text.size, nullptr, false);
        published = stream->text.size;
      }
      if (++serviced == 256) { serviced = 0; vTaskDelay(1); }
    }
  }
  {
    Lock lock(true);
    if (lock) { applyConfigurationFailureLocked(); sIo = nullptr; }
  }
  llmHttpIoFinish(io, !error && !sCancel.load(std::memory_order_acquire));
  if (client) { esp_http_client_close(client); esp_http_client_cleanup(client); }
  llmHttpIoDestroy(io);
  if (stream) {
    if (error) {
      stream->text.finish(false);
      // Wide enough for a whole path reason ("Not sent: phone not heard ...").
      char notice[108]; snprintf(notice, sizeof(notice), "\n[%.100s]", error);
      stream->text.literal(notice, strlen(notice));
    }
    publish(*run, stream->text.bytes, stream->text.size, error, true);
    stream->hw1http::Stream::~Stream(); ps_free(stream);
  } else {
    // The allocation-failure path must not place a full reply buffer on this
    // worker's 8 KiB stack. Publish the fixed notice directly instead.
    publish(*run, kInterrupted, strlen(kInterrupted), error, true);
  }
  if (body) { mbedtls_platform_zeroize(body, kBodyCap); ps_free(body); }
  llmHttpRequestRelease(LlmHttpRequestKind::Chat);
  {
    Lock lock(true);
    if (lock) {
      sDone = true;
      sWorker = false;
      sHttpTickWatch.store(false, std::memory_order_release);
    }
  }
  run->~Run(); mbedtls_platform_zeroize(run, sizeof(*run)); ps_free(run);
  // FreeRTOS self-delete does not unwind this stack; all owning objects above
  // have explicitly released their storage before this point.
  vTaskDelete(nullptr);
}
} // namespace

bool httpLlmValidateBaseUrl(const char* baseUrl) {
  char normalized[kUrlMax + 24];
  // Structural validation is independent of the plaintext opt-in, so settings
  // can be provisioned in either order. Configure still applies the policy.
  return endpoint(baseUrl, true, normalized, sizeof(normalized));
}

bool httpLlmConfigure(bool enabled, const char* baseUrl, const char* model,
                      const char* token, bool allowPlaintext, const char* caPem) {
  // Heap rather than a 6KB config on the caller's small command/UI stack.
  auto* next = static_cast<Config*>(ps_calloc(1, sizeof(Config), AllocPref::PreferPsram, "llm.http.config"));
  if (!next) {
    failConfigurationPublication();
    Lock lock;
    if (lock) applyConfigurationFailureLocked();
    return false;
  }
  // Start the object's lifetime while retaining calloc's zeroed padding for
  // the bytewise comparison with the current configuration below.
  new (next) Config;
  next->enabled = enabled; next->plaintext = allowPlaintext;
  const char* reason = nullptr;
  if (!endpoint(baseUrl, allowPlaintext, next->url, sizeof(next->url)))
    reason = "Set a valid HTTPS base URL ending /v1 (plaintext requires opt-in)";
  else if (!headerSafe(model) || strlen(model) > kModelMax) reason = "Set a valid agent model (127 bytes max)";
  else if (!headerSafe(token) || strlen(token) > kTokenMax) reason = "Set an agent token (1024 bytes max)";
  else if (caPem && strlen(caPem) > kCaMax) reason = "Agent CA certificate exceeds 4096 bytes";
  else {
    strlcpy(next->model, model, sizeof(next->model));
    strlcpy(next->token, token, sizeof(next->token));
    strlcpy(next->ca, caPem ? caPem : "", sizeof(next->ca));
#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (!strncmp(next->url, "https://", 8) && !next->ca[0])
      reason = "Set an agent CA certificate; this build has no CA bundle";
#endif
  }
  next->valid = !reason;
  bool valid = next->valid;
  {
    Lock lock;
    if (!lock) {
      failConfigurationPublication();
      next->~Config(); mbedtls_platform_zeroize(next, sizeof(*next)); ps_free(next); return false;
    }
    const bool recoveringPublication = !sConfigAdmitted.load(std::memory_order_acquire);
    applyConfigurationFailureLocked();
    if (memcmp(&sConfig, next, sizeof(*next))) {
      sCancel.store(true, std::memory_order_release);
      interruptSocketLocked();
      interruptTerminalLocked();
      ++sRevision;
      sConfig = *next;
      rotateLocked();
      if (sWorker) {
        strlcpy(sError, "Agent configuration changed; request stopped locally", sizeof(sError));
        appendTerminalLocked(sError);
      }
      else sError[0] = 0;
    }
    strlcpy(sConfigError, reason ? reason : "", sizeof(sConfigError));
    sSecure.store(!strncasecmp(sConfig.url, "https://", 8), std::memory_order_release);
    if (recoveringPublication && !sWorker) sError[0] = 0;
    sConfigAdmitted.store(true, std::memory_order_release);
  }
  next->~Config(); mbedtls_platform_zeroize(next, sizeof(*next)); ps_free(next);
  return valid;
}

namespace {
bool secureLocked() { return !strncasecmp(sConfig.url, "https://", 8); }
// The path is up, or the phone link is coming back by itself (a chat waits
// for it in the transport). Wi-Fi recovery is not modelled.
bool pathUsableLocked() {
  const bool secure = secureLocked();
  return llmHttpPathAvailable(secure) || llmHttpPathRecoverable(secure);
}
// Configuration reasons only; the path never makes the source unavailable.
const char* unavailableLocked() {
  return !sConfigAdmitted.load(std::memory_order_acquire) ? kConfigPublicationError :
         !sConfig.enabled ? "HTTP agent disabled (llmhttpenabled)" :
         !sConfig.valid ? sConfigError : nullptr;
}
}  // namespace

size_t httpLlmEnumerate(LlmModelDesc* out, size_t cap) {
  if (!out || !cap) return 0;
  Lock lock;
  if (!lock || !sConfig.enabled) return 0;
  // Availability is configuration only: a phone link that is down or coming
  // back never hides the source or refuses its selection.
  descriptor(out, sConfigAdmitted.load(std::memory_order_acquire) && sConfig.valid); return 1;
}
bool httpLlmFindByName(const char* name, LlmModelDesc* out) {
  return name && !strcasecmp(name, "openclaw") && httpLlmEnumerate(out, 1) == 1;
}
bool httpLlmSelectByName(const char* name, char* error, size_t cap) {
  Lock lock;
  const char* reason = !lock ? "HTTP agent busy" :
      !name || strcasecmp(name, "openclaw") ? "Unknown HTTP agent" :
      !sConfigAdmitted.load(std::memory_order_acquire) ? kConfigPublicationError :
      !sConfig.enabled ? "HTTP agent is disabled" :
      !sConfig.valid ? sConfigError : sWorker ? "HTTP agent request still active" : nullptr;
  if (reason) { if (error && cap) strlcpy(error, reason, cap); return false; }
  sSelected = true; sError[0] = 0;
  if (!sConversation[0]) rotateLocked();
  return true;
}
size_t httpLlmUnavailableReason(char* out, size_t cap) {
  if (!out || !cap) return 0;
  out[0] = 0;
  Lock lock;
  const char* reason = !lock ? "HTTP agent busy" : unavailableLocked();
  if (!reason) return 0;
  const size_t n = strlcpy(out, reason, cap);
  return n < cap ? n : cap - 1;
}
bool httpLlmPathUp() { return llmHttpPathAvailable(sSecure.load(std::memory_order_acquire)); }
size_t httpLlmPathText(char* out, size_t cap) {
  return llmHttpPathText(sSecure.load(std::memory_order_acquire), out, cap);
}
void httpLlmUnload() {
  httpLlmStop(); Lock lock;
  if (lock) { sSelected = false; rotateLocked(); }
}
bool httpLlmIsReady() {
  Lock lock;
  return lock && sConfigAdmitted.load(std::memory_order_acquire) && sSelected &&
         sConfig.enabled && sConfig.valid && !sWorker && pathUsableLocked();
}
LlmStatus httpLlmStatus() {
  LlmStatus st{}; Lock lock;
  if (!lock) { st.state = LlmState::ERROR; strlcpy(st.errorMsg, "HTTP agent busy", sizeof(st.errorMsg)); return st; }
  strlcpy(st.modelPath, "http:openclaw", sizeof(st.modelPath));
  if (!sConfigAdmitted.load(std::memory_order_acquire)) {
    st.state = LlmState::ERROR;
    strlcpy(st.errorMsg, kConfigPublicationError, sizeof(st.errorMsg)); return st;
  }
  const bool secure = secureLocked();
  const bool up = llmHttpPathAvailable(secure);
  const bool usable = up || llmHttpPathRecoverable(secure);
  // A failed request no longer makes the source ERROR: the next one may work.
  // ERROR is for configuration, or a path that will not come back by itself.
  st.state = !sSelected ? LlmState::UNLOADED : sWorker ? LlmState::GENERATING :
             !sConfig.valid || !sConfig.enabled || !usable ? LlmState::ERROR : LlmState::READY;
  if (const char* reason = unavailableLocked()) strlcpy(st.errorMsg, reason, sizeof(st.errorMsg));
  else if (!up) llmHttpPathText(secure, st.errorMsg, sizeof(st.errorMsg));
  else if (sError[0]) snprintf(st.errorMsg, sizeof(st.errorMsg), "last request: %s", sError);
  return st;
}
int httpLlmStartAsync(const char* prompt, const LlmGenParams& /*params*/) {
  if (!prompt || !*prompt || strlen(prompt) > kPromptMax) return 0;
  auto* run = static_cast<Run*>(ps_calloc(1, sizeof(Run), AllocPref::PreferPsram, "llm.http.run"));
  if (!run) return 0;
  new (run) Run;
  Lock lock;
  // The transport admits a chat while the phone link is coming back.
  if (!lock || !sConfigAdmitted.load(std::memory_order_acquire) || !sSelected ||
      !sConfig.enabled || !sConfig.valid || sWorker) {
    run->~Run(); ps_free(run); return 0;
  }
  if(llmHttpRequestAdmit(LlmHttpRequestKind::Chat,!strncasecmp(sConfig.url,"https://",8))!=LlmHttpAdmission::Accepted) {
    run->~Run();ps_free(run);return 0;
  }
  run->config = sConfig; run->revision = sRevision;
  if (!sConversation[0]) rotateLocked();
  strlcpy(run->conversation, sConversation, sizeof(run->conversation));
  strlcpy(run->prompt, prompt, sizeof(run->prompt));
  sSession = sNextSession;
  sNextSession = sNextSession == INT32_MAX ? 1 : sNextSession + 1;
  run->session = sSession;
  sResultLen = 0; sResult[0] = 0; sError[0] = 0;
  sCancel.store(false, std::memory_order_release); sWorker = true; sDone = false;
  sHttpTickWatch.store(true, std::memory_order_release);
  sStartedMs = millis();
  if (xTaskCreate(worker, "llm_http", 8192, run, 1, nullptr) != pdPASS) {
    llmHttpRequestRelease(LlmHttpRequestKind::Chat);
    sWorker = false; sDone = true;
    sHttpTickWatch.store(false, std::memory_order_release);
    strlcpy(sError, "Could not start HTTP agent worker", sizeof(sError));
    run->~Run(); mbedtls_platform_zeroize(run, sizeof(*run)); ps_free(run); return 0;
  }
  return sSession;
}
void httpLlmStop() {
  Lock lock;
  if (lock && sWorker) {
    if (sCancel.load(std::memory_order_acquire)) return;
    sCancel.store(true, std::memory_order_release);
    interruptSocketLocked();
    strlcpy(sError, "Stopped locally; host actions may continue", sizeof(sError));
    appendTerminalLocked(sError);
  }
}
int httpLlmSessionId() { Lock lock; return lock ? sSession : 0; }
int httpLlmResultLen() { Lock lock; return lock ? static_cast<int>(sResultLen) : 0; }
int httpLlmResultChunk(int offset, char* out, int cap) {
  if (!out || cap < 1) return 0;
  out[0] = 0; Lock lock;
  if (!lock || offset < 0 || static_cast<size_t>(offset) >= sResultLen) return 0;
  size_t n = sResultLen - static_cast<size_t>(offset);
  if (n > static_cast<size_t>(cap - 1)) n = cap - 1;
  memcpy(out, sResult + offset, n); out[n] = 0; return static_cast<int>(n);
}
bool httpLlmIsDone() { Lock lock; return lock && sDone; }
void httpLlmTick() {
  if (!sConfigFailurePending.load(std::memory_order_acquire) &&
      !sHttpTickWatch.load(std::memory_order_acquire)) return;
  Lock lock;
  if (lock) applyConfigurationFailureLocked();
  if (lock && sWorker && !sCancel.load(std::memory_order_acquire) &&
      static_cast<uint32_t>(millis() - sStartedMs) >= kTotalMs) {
    sCancel.store(true, std::memory_order_release);
    strlcpy(sError, "Agent request time limit reached; outcome unknown", sizeof(sError));
    appendTerminalLocked(sError);
    interruptSocketLocked();
  }
}
bool httpLlmResetConversation() {
  Lock lock; if (!lock || sWorker) return false;
  rotateLocked(); return true;
}
#include "System_LLMHttpTerminal.inc"
#endif
