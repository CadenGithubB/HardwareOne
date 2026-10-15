// Private implementation included by System_LLMHttp.cpp. Configuration and
// credential publication remain with that owner; Terminal results are separate
// from normal chat. All network work runs on this worker, never the G2 worker.
#if (ENABLE_G2_GLASSES && ENABLE_LOCAL_STT) || ENABLE_BLE_PHONE_PIPE
namespace {
constexpr uint32_t kTerminalTotalMs = 15000;
struct TerminalRun {
  Config config;
  uint32_t token = 0, revision = 0;
  size_t bodyLen = 0;
  bool contentType = false;
  char body[HTTP_TERMINAL_JSON_CAP + 1]{};
  char response[HTTP_TERMINAL_JSON_CAP + 1]{};
};

esp_err_t terminalHttpEvent(esp_http_client_event_t* event) {
  if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value &&
      !strcasecmp(event->header_key, "Content-Type")) {
    auto* run = static_cast<TerminalRun*>(event->user_data);
    const char* value = event->header_value;
    run->contentType = !strncasecmp(value, "application/json", 16) &&
        (!value[16] || value[16] == ';' || value[16] == ' ');
  }
  return ESP_OK;
}

bool terminalCancelled() {
  return sTerminalCancel.load(std::memory_order_acquire) ||
      !sConfigAdmitted.load(std::memory_order_acquire);
}

void terminalWorker(void* context) {
  auto* run = static_cast<TerminalRun*>(context);
  esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN);
  const char* error = esp_log_level_get("HTTP_CLIENT") >= ESP_LOG_DEBUG ?
      "HTTP debug logging exposes credentials; lower log verbosity" : nullptr;
  int status = 0;
  size_t received = 0;
  esp_http_client_handle_t client = nullptr;
  LlmHttpIo* io = nullptr;
  const uint32_t started = millis();
  if (!error && !terminalCancelled()) {
    esp_http_client_config_t cfg{};
    cfg.url = run->config.url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = kIoTimeoutMs;
    cfg.disable_auto_redirect = true;
    cfg.max_authorization_retries = -1;
    cfg.buffer_size = 1024;
    cfg.buffer_size_tx = 1536;
    cfg.event_handler = terminalHttpEvent;
    cfg.user_data = run;
    cfg.keep_alive_enable = false;
    io = llmHttpIoCreate(run->config.ca, !strncasecmp(run->config.url, "https://", 8),
                         sTerminalCancel, started, kTerminalTotalMs, run->revision);
    if (io) { Lock lock(true); if (lock) sTerminalIo = io; }
    if (llmHttpIoConfigure(io, cfg)) client = esp_http_client_init(&cfg);
    if (!client) error = "Could not initialize Terminal connection";
  }
  if (!error && client) {
    char authorization[kTokenMax + 8];
    snprintf(authorization, sizeof(authorization), "Bearer %s", run->config.token);
    if (esp_http_client_set_header(client, "Authorization", authorization) != ESP_OK ||
        esp_http_client_set_header(client, "Content-Type", "application/json") != ESP_OK ||
        esp_http_client_set_header(client, "Accept", "application/json") != ESP_OK)
      error = "Could not set Terminal request headers";
    mbedtls_platform_zeroize(authorization, sizeof(authorization));
  }
  if (!error && client && !terminalCancelled()) {
    if (esp_http_client_open(client, static_cast<int>(run->bodyLen)) != ESP_OK)
      error = "Terminal connection failed; no retry";
    if (!error) {
      Lock lock(true);
      if (lock) {
        llmHttpIoConnected(io, client);
        if (terminalCancelled()) interruptTerminalLocked();
      }
    }
    size_t sent = 0;
    while (!error && sent < run->bodyLen && !terminalCancelled()) {
      if (uint32_t(millis() - started) >= kTerminalTotalMs) {
        error = "Terminal request deadline; outcome unknown"; break;
      }
      const int n = esp_http_client_write(client, run->body + sent, int(run->bodyLen - sent));
      if (n <= 0) error = "Terminal write failed; outcome unknown";
      else sent += size_t(n);
    }
    if (!error && !terminalCancelled()) {
      int64_t length;
      do {
        length = esp_http_client_fetch_headers(client);
        if (length == -ESP_ERR_HTTP_EAGAIN) vTaskDelay(pdMS_TO_TICKS(10));
      } while (length == -ESP_ERR_HTTP_EAGAIN && !terminalCancelled() &&
               uint32_t(millis() - started) < kTerminalTotalMs);
      status = esp_http_client_get_status_code(client);
      if (length < 0) error = "Terminal headers failed; outcome unknown";
      else if (length > int64_t(HTTP_TERMINAL_JSON_CAP)) error = "Terminal response too large";
      else if (!run->contentType) error = "Terminal host did not return JSON";
      else if (status < 200 || status >= 300) error = "Terminal host refused request";
    }
    while (!error && !terminalCancelled()) {
      if (uint32_t(millis() - started) >= kTerminalTotalMs) {
        error = "Terminal response deadline; outcome unknown"; break;
      }
      char chunk[128];
      const int n = esp_http_client_read(client, chunk, sizeof(chunk));
      if (n == -ESP_ERR_HTTP_EAGAIN) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
      if (n < 0) { error = "Terminal disconnected; outcome unknown"; break; }
      if (!n) {
        if (!esp_http_client_is_complete_data_received(client))
          error = "Terminal response incomplete; outcome unknown";
        break;
      }
      if (size_t(n) > HTTP_TERMINAL_JSON_CAP - received) {
        error = "Terminal response too large"; break;
      }
      memcpy(run->response + received, chunk, size_t(n));
      received += size_t(n);
    }
  }
  {
    Lock lock(true);
    if (lock) sTerminalIo = nullptr;
  }
  if (client) { esp_http_client_close(client); esp_http_client_cleanup(client); }
  llmHttpIoDestroy(io);
  {
    Lock lock(true);
    if (lock && sTerminalToken == run->token) {
      if (terminalCancelled() || sRevision != run->revision)
        error = "Terminal stopped locally; host outcome may be unknown";
      sTerminalResult = {};
      sTerminalResult.done = true;
      sTerminalResult.httpStatus = status;
      sTerminalResult.configRevision = run->revision;
      memset(sTerminalResponse, 0, sizeof(sTerminalResponse));
      if (error) strlcpy(sTerminalResult.error, error, sizeof(sTerminalResult.error));
      else {
        memcpy(sTerminalResponse, run->response, received);
        sTerminalResult.length = received;
      }
      sTerminalWorker = false;
    }
  }
  run->~TerminalRun();
  mbedtls_platform_zeroize(run, sizeof(*run));
  ps_free(run);
  vTaskDelete(nullptr);
}
} // namespace

uint32_t httpTerminalConfigRevision() {
  Lock lock;
  return lock && sConfigAdmitted.load(std::memory_order_acquire) &&
      sConfig.enabled && sConfig.valid ? sRevision : 0;
}

bool httpTerminalStart(const char* json, size_t length, HttpTerminalToken* token) {
  if (token) *token = 0;
  if (!token || !json || !length || length > HTTP_TERMINAL_JSON_CAP ||
      memchr(json, 0, length)) return false;
  auto* memory = ps_calloc(1, sizeof(TerminalRun), AllocPref::PreferPSRAM, "llm.terminal.run");
  if (!memory) return false;
  auto* run = new (memory) TerminalRun;
  Lock lock;
  if (!lock || !sConfigAdmitted.load(std::memory_order_acquire) ||
      !sConfig.enabled || !sConfig.valid || sTerminalWorker || !llmHttpPathAvailable(!strncasecmp(sConfig.url, "https://", 8))) {
    run->~TerminalRun(); ps_free(run); return false;
  }
  run->config = sConfig;
  const char suffix[] = "/chat/completions";
  const size_t urlLen = strlen(run->config.url), suffixLen = sizeof(suffix) - 1;
  if (urlLen < suffixLen || strcmp(run->config.url + urlLen - suffixLen, suffix)) {
    run->~TerminalRun(); mbedtls_platform_zeroize(run, sizeof(*run)); ps_free(run); return false;
  }
  strlcpy(run->config.url + urlLen - suffixLen, "/terminal",
          sizeof(run->config.url) - (urlLen - suffixLen));
  run->revision = sRevision;
  if (++sTerminalNextToken == 0) ++sTerminalNextToken;
  run->token = sTerminalToken = sTerminalNextToken;
  run->bodyLen = length;
  memcpy(run->body, json, length);
  sTerminalResult = {};
  sTerminalResult.configRevision = sRevision;
  memset(sTerminalResponse, 0, sizeof(sTerminalResponse));
  sTerminalCancel.store(false, std::memory_order_release);
  sTerminalWorker = true;
  if (xTaskCreate(terminalWorker, "g2_terminal_http", 8192, run, 1, nullptr) != pdPASS) {
    sTerminalWorker = false;
    sTerminalResult.done = true;
    strlcpy(sTerminalResult.error, "Could not start Terminal worker", sizeof(sTerminalResult.error));
    run->~TerminalRun(); mbedtls_platform_zeroize(run, sizeof(*run)); ps_free(run); return false;
  }
  *token = run->token;
  return true;
}

bool httpTerminalRead(HttpTerminalToken token, char* out, size_t cap, HttpTerminalResult* result) {
  if (out && cap) out[0] = 0;
  if (result) *result = {};
  if (!token || !out || !cap || !result) return false;
  Lock lock;
  if (!lock || sTerminalToken != token) return false;
  *result = sTerminalResult;
  if (!result->done) return true;
  if (!sConfigAdmitted.load(std::memory_order_acquire) || result->configRevision != sRevision) {
    result->length = 0;
    strlcpy(result->error, "Terminal configuration changed", sizeof(result->error));
    return true;
  }
  if (cap <= result->length) return false;
  memcpy(out, sTerminalResponse, result->length + 1);
  return true;
}

void httpTerminalCancel(HttpTerminalToken token) {
  Lock lock;
  if (!lock || !token || token != sTerminalToken) return;
  interruptTerminalLocked();
  memset(sTerminalResponse, 0, sizeof(sTerminalResponse));
  sTerminalResult.length = 0;
  strlcpy(sTerminalResult.error, "Stopped locally; host actions may continue", sizeof(sTerminalResult.error));
}
#endif
