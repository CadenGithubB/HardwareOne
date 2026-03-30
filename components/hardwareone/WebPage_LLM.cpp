/**
 * WebPage_LLM.cpp - HTTP API handlers for on-device LLM
 *
 * Provides REST endpoints consumed by WebPage_LLM.h chat UI:
 *   GET  /api/llm/status   - model state, config, performance
 *   GET  /api/llm/models   - list available .bin files
 *   POST /api/llm/load     - load a model by name
 *   POST /api/llm/unload   - free model memory
 *   POST /api/llm/generate - streamed text generation (chunked)
 *   POST /api/llm/stop     - abort in-progress generation
 *   GET  /llm              - chat page
 */

#include "System_BuildConfig.h"

#if ENABLE_ONDEVICE_LLM && ENABLE_HTTP_SERVER

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_http_server.h>

#include "System_LLM.h"
#include "System_Debug.h"
#include "WebServer_Utils.h"
#include "WebPage_LLM.h"

// External helpers from WebServer_Server.cpp
extern void streamPageWithContent(httpd_req_t* req, const String& activePage, const String& username, void (*contentStreamer)(httpd_req_t*, const String&));

// ============================================================================
// Helper: read POST body into a stack buffer
// ============================================================================
static bool readPostBody(httpd_req_t* req, char* buf, size_t bufSize) {
  size_t contentLen = req->content_len;
  if (contentLen == 0 || contentLen >= bufSize) return false;
  size_t received = 0;
  while (received < contentLen) {
    int ret = httpd_req_recv(req, buf + received, contentLen - received);
    if (ret <= 0) {
      if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
      return false;
    }
    received += ret;
  }
  buf[received] = '\0';
  return true;
}

// ============================================================================
// GET /api/llm/status
// ============================================================================
static esp_err_t handleLLMStatus(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);

  LLMStatus st = llmGetStatus();
  char json[384];
  const char* stateStr = "UNLOADED";
  switch (st.state) {
    case LLMState::LOADING:    stateStr = "LOADING"; break;
    case LLMState::READY:      stateStr = "READY"; break;
    case LLMState::GENERATING: stateStr = "GENERATING"; break;
    case LLMState::ERROR:      stateStr = "ERROR"; break;
    default: break;
  }

  snprintf(json, sizeof(json),
    "{\"state\":\"%s\",\"model\":\"%s\",\"params\":\"%dx%dx%d\","
    "\"psramKB\":%u,\"tokPerSec\":%.1f,\"lastTokens\":%d,\"error\":\"%s\","
    "\"dim\":%d,\"layers\":%d,\"heads\":%d,\"vocab\":%d,\"seqLen\":%d}",
    stateStr, st.modelPath,
    st.config.dim, st.config.n_layers, st.config.n_heads,
    (unsigned)(st.totalPsramUsed / 1024),
    st.lastTokensPerSec, st.lastTokenCount, st.errorMsg,
    st.config.dim, st.config.n_layers, st.config.n_heads,
    st.config.vocab_size, st.config.seq_len);

  return sendJsonResponse(req, json);
}

// ============================================================================
// GET /api/llm/models
// ============================================================================
static esp_err_t handleLLMModels(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);

  String models = llmListModels();
  return sendJsonResponse(req, models.c_str());
}

// ============================================================================
// POST /api/llm/load  { "model": "model.bin" }
// ============================================================================
static esp_err_t handleLLMLoad(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);

  char body[256];
  if (!readPostBody(req, body, sizeof(body))) {
    return sendJsonResponse(req, "{\"ok\":false,\"error\":\"Bad request\"}");
  }

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    return sendJsonResponse(req, "{\"ok\":false,\"error\":\"Invalid JSON\"}");
  }

  const char* modelName = doc["model"] | "";
  if (!modelName[0]) {
    return sendJsonResponse(req, "{\"ok\":false,\"error\":\"No model specified\"}");
  }

  // Build full path
  char modelPath[96];
  snprintf(modelPath, sizeof(modelPath), "/system/llm/%s", modelName);

  // Tokenizer: look for tokenizer.bin in same directory
  const char* tokenizerPath = LLM_DEFAULT_TOKENIZER_PATH;

  bool ok = llmLoadModel(modelPath, tokenizerPath);
  if (ok) {
    return sendJsonResponse(req, "{\"ok\":true}");
  } else {
    LLMStatus st = llmGetStatus();
    char resp[256];
    snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"%s\"}", st.errorMsg);
    return sendJsonResponse(req, resp);
  }
}

// ============================================================================
// POST /api/llm/unload
// ============================================================================
static esp_err_t handleLLMUnload(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  llmUnload();
  return sendJsonResponse(req, "{\"ok\":true}");
}

// ============================================================================
// POST /api/llm/stop
// ============================================================================
static esp_err_t handleLLMStop(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  llmStop();
  return sendJsonResponse(req, "{\"ok\":true}");
}

// ============================================================================
// POST /api/llm/generate  { "prompt": "...", "max_tokens": 256, "temperature": 0.8 }
// Streams response as chunked text/plain
// ============================================================================

// Context passed to the generation task
struct LLMGenCtx {
  httpd_req_t* req;
  char prompt[512];
  int maxTokens;
  float temperature;
  bool finished;
  bool error;
};

static esp_err_t handleLLMGenerate(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);

  if (!llmIsReady()) {
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "[error: model not ready]", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  char body[1024];
  if (!readPostBody(req, body, sizeof(body))) {
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "[error: bad request]", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "[error: invalid JSON]", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  const char* prompt = doc["prompt"] | "";
  int maxTokens = doc["max_tokens"] | LLM_DEFAULT_MAX_TOKENS;
  float temperature = doc["temperature"] | LLM_DEFAULT_TEMPERATURE;

  if (!prompt[0]) {
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "[error: empty prompt]", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Clamp
  if (maxTokens < 1) maxTokens = 1;
  if (maxTokens > 512) maxTokens = 512;
  if (temperature < 0.0f) temperature = 0.0f;
  if (temperature > 2.0f) temperature = 2.0f;

  // Start chunked response as text/plain for streaming
  httpd_resp_set_type(req, "text/plain");

  // Generate synchronously on this httpd worker thread.
  // The httpd task stack is ~11KB which is tight, but the heavy lifting
  // (matmul, KV cache) is in heap/PSRAM. The llmGenerate function yields
  // via vTaskDelay so the watchdog stays happy.
  int result = llmGenerate(prompt, [req](const char* token) -> bool {
    esp_err_t err = httpd_resp_send_chunk(req, token, strlen(token));
    return (err == ESP_OK);
  }, maxTokens, temperature);

  // End chunked response
  httpd_resp_send_chunk(req, NULL, 0);

  if (result < 0) {
    DEBUG_HTTPF("[LLM] Generation failed for prompt: %.32s...", prompt);
  }

  return ESP_OK;
}

// ============================================================================
// GET /llm - Chat page
// ============================================================================
static void streamLLMContent(httpd_req_t* req, const String& username) {
  streamLLMInner(req, username);
}

static esp_err_t handleLLMPage(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  streamPageWithContent(req, "llm", ctx.user, streamLLMContent);
  return ESP_OK;
}

// ============================================================================
// Handler Registration
// ============================================================================
void registerLLMHandlers(httpd_handle_t server) {
  static httpd_uri_t llmPage = { .uri = "/llm", .method = HTTP_GET, .handler = handleLLMPage, .user_ctx = NULL };
  static httpd_uri_t llmStatus = { .uri = "/api/llm/status", .method = HTTP_GET, .handler = handleLLMStatus, .user_ctx = NULL };
  static httpd_uri_t llmModels = { .uri = "/api/llm/models", .method = HTTP_GET, .handler = handleLLMModels, .user_ctx = NULL };
  static httpd_uri_t llmLoad = { .uri = "/api/llm/load", .method = HTTP_POST, .handler = handleLLMLoad, .user_ctx = NULL };
  static httpd_uri_t llmUnload = { .uri = "/api/llm/unload", .method = HTTP_POST, .handler = handleLLMUnload, .user_ctx = NULL };
  static httpd_uri_t llmGenerate = { .uri = "/api/llm/generate", .method = HTTP_POST, .handler = handleLLMGenerate, .user_ctx = NULL };
  static httpd_uri_t llmStop = { .uri = "/api/llm/stop", .method = HTTP_POST, .handler = handleLLMStop, .user_ctx = NULL };

  httpd_register_uri_handler(server, &llmPage);
  httpd_register_uri_handler(server, &llmStatus);
  httpd_register_uri_handler(server, &llmModels);
  httpd_register_uri_handler(server, &llmLoad);
  httpd_register_uri_handler(server, &llmUnload);
  httpd_register_uri_handler(server, &llmGenerate);
  httpd_register_uri_handler(server, &llmStop);
}

#endif // ENABLE_ONDEVICE_LLM && ENABLE_HTTP_SERVER
