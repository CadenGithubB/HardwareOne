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
#include "System_User.h"
#include "WebServer_Server.h"
#include "WebServer_Utils.h"
#include "WebPage_LLM.h"

// External helpers from WebServer_Server.cpp
extern void streamPageWithContent(httpd_req_t* req, const String& activePage, const String& username, void (*contentStreamer)(httpd_req_t*, const String&));
extern void streamBeginHtml(httpd_req_t* req, const char* title, bool isPublic, const String& username, const String& activePage);
extern void streamEndHtml(httpd_req_t* req);

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
  DEBUG_HTTPF("[LLM-API] GET /api/llm/status from user=%s", ctx.user.c_str());

  LLMStatus st = llmGetStatus();
  char json[448];
  const char* stateStr = "UNLOADED";
  switch (st.state) {
    case LLMState::LOADING:    stateStr = "LOADING"; break;
    case LLMState::READY:      stateStr = "READY"; break;
    case LLMState::GENERATING: stateStr = "GENERATING"; break;
    case LLMState::ERROR:      stateStr = "ERROR"; break;
    default: break;
  }

  const char* archStr = (st.config.arch_type == 1) ? "GPT-2" : "Llama";
  const char* quantStr = (st.config.quant_type == 1) ? "INT8" : "FP32";

  snprintf(json, sizeof(json),
    "{\"state\":\"%s\",\"model\":\"%s\",\"params\":\"%dx%dx%d\","
    "\"psramKB\":%u,\"tokPerSec\":%.1f,\"lastTokens\":%d,\"error\":\"%s\","
    "\"dim\":%d,\"layers\":%d,\"heads\":%d,\"kvHeads\":%d,\"vocab\":%d,\"seqLen\":%d,"
    "\"ctxUsed\":%d,\"ctxMax\":%d,\"arch\":\"%s\",\"quant\":\"%s\"}",
    stateStr, st.modelPath,
    st.config.dim, st.config.n_layers, st.config.n_heads,
    (unsigned)(st.totalPsramUsed / 1024),
    st.lastTokensPerSec, st.lastTokenCount, st.errorMsg,
    st.config.dim, st.config.n_layers, st.config.n_heads,
    st.config.n_kv_heads, st.config.vocab_size, st.config.seq_len,
    st.lastContextUsed, st.lastContextMax, archStr, quantStr);

  return sendJsonResponse(req, json);
}

// ============================================================================
// GET /api/llm/models
// ============================================================================
static esp_err_t handleLLMModels(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  DEBUG_HTTPF("[LLM-API] GET /api/llm/models from user=%s", ctx.user.c_str());

  String models = llmListModels();
  DEBUG_HTTPF("[LLM-API] models response: %s", models.c_str());
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
    DEBUG_HTTPF("[LLM-API] POST /api/llm/load: no model specified");
    return sendJsonResponse(req, "{\"ok\":false,\"error\":\"No model specified\"}");
  }

  int maxCtx = doc["max_ctx"] | 0;  // 0 = use firmware default
  if (maxCtx < 0) maxCtx = 0;
  if (maxCtx > 2048) maxCtx = 2048;

  DEBUG_HTTPF("[LLM-API] POST /api/llm/load: model='%s' max_ctx=%d", modelName, maxCtx);

  // Accept either a full path (/sd/llm/... or /system/llm/...) or a bare filename
  char modelPath[128];
  if (modelName[0] == '/') {
    // Full path — validate it's under an allowed LLM directory
    if (strncasecmp(modelName, "/system/llm/", 12) != 0 &&
        strncasecmp(modelName, "/sd/llm/", 8) != 0) {
      DEBUG_HTTPF("[LLM-API] Rejected invalid model path: %s", modelName);
      return sendJsonResponse(req, "{\"ok\":false,\"error\":\"Invalid model path\"}");
    }
    strlcpy(modelPath, modelName, sizeof(modelPath));
  } else {
    // Bare filename — default to internal storage
    snprintf(modelPath, sizeof(modelPath), "/system/llm/%s", modelName);
  }

  DEBUG_HTTPF("[LLM-API] Resolved model path: %s", modelPath);
  bool ok = llmLoadModel(modelPath, maxCtx);
  if (ok) {
    DEBUG_HTTPF("[LLM-API] Model loaded successfully: %s (ctx=%d)", modelPath, maxCtx);
    return sendJsonResponse(req, "{\"ok\":true}");
  } else {
    LLMStatus st = llmGetStatus();
    char resp[256];
    snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"%s\"}", st.errorMsg);
    DEBUG_HTTPF("[LLM-API] Model load FAILED: %s error='%s'", modelPath, st.errorMsg);
    return sendJsonResponse(req, resp);
  }
}

// ============================================================================
// POST /api/llm/unload
// ============================================================================
static esp_err_t handleLLMUnload(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  DEBUG_HTTPF("[LLM-API] POST /api/llm/unload from user=%s", ctx.user.c_str());
  llmUnload();
  DEBUG_HTTPF("[LLM-API] Model unloaded");
  return sendJsonResponse(req, "{\"ok\":true}");
}

// ============================================================================
// POST /api/llm/stop
// ============================================================================
static esp_err_t handleLLMStop(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  DEBUG_HTTPF("[LLM-API] POST /api/llm/stop from user=%s", ctx.user.c_str());
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
    DEBUG_HTTPF("[LLM-API] POST /api/llm/generate: model not ready");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "[error: model not ready]", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  char body[2048];
  if (!readPostBody(req, body, sizeof(body))) {
    DEBUG_HTTPF("[LLM-API] POST /api/llm/generate: bad request (body read failed, content_len=%d)", req->content_len);
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
  float topp = doc["top_p"] | LLM_DEFAULT_TOPP;
  bool useMirostat2 = doc["mirostat2"] | false;
  float mirostatTau = doc["mirostat_tau"] | LLM_DEFAULT_MIROSTAT_TAU;
  float mirostatEta = doc["mirostat_eta"] | LLM_DEFAULT_MIROSTAT_ETA;
  float repPenalty = doc["rep_penalty"] | LLM_DEFAULT_REP_PENALTY;
  int repWindow = doc["rep_window"] | LLM_DEFAULT_REP_WINDOW;
  int sentenceLimit = doc["sentence_limit"] | LLM_DEFAULT_SENTENCE_LIMIT;
  int hardCap = doc["hard_cap"] | LLM_DEFAULT_HARD_CAP;
  bool dynTemp = doc["dyn_temp"] | false;

  // Tokenize suppress texts (previous answers to avoid on retry)
  static constexpr int MAX_SUPPRESS_TOKENS = 128;
  int suppressBuf[MAX_SUPPRESS_TOKENS];
  int suppressCount = 0;
  JsonArray suppressArr = doc["suppress"].as<JsonArray>();
  if (suppressArr) {
    int tmpBuf[64];  // per-string tokenize buffer
    for (JsonVariant v : suppressArr) {
      const char* text = v.as<const char*>();
      if (!text || !text[0]) continue;
      int n = llmTokenize(text, tmpBuf, 64);
      for (int i = 0; i < n && suppressCount < MAX_SUPPRESS_TOKENS; i++) {
        int tok = tmpBuf[i];
        // Deduplicate
        bool dup = false;
        for (int j = 0; j < suppressCount; j++) {
          if (suppressBuf[j] == tok) { dup = true; break; }
        }
        if (!dup) suppressBuf[suppressCount++] = tok;
      }
    }
    if (suppressCount > 0) {
      DEBUG_HTTPF("[LLM-API] suppress: %d unique tokens from %d previous answers",
                  suppressCount, suppressArr.size());
    }
  }

  if (!prompt[0]) {
    DEBUG_HTTPF("[LLM-API] POST /api/llm/generate: empty prompt");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "[error: empty prompt]", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  DEBUG_HTTPF("[LLM-API] POST /api/llm/generate: prompt='%.80s%s' max_tokens=%d temp=%.2f topp=%.2f",
    prompt, strlen(prompt) > 80 ? "..." : "", maxTokens, temperature, topp);
  DEBUG_HTTPF("[LLM-API]   mirostat2=%d tau=%.1f eta=%.2f rep_pen=%.2f rep_win=%d sent_lim=%d hard_cap=%d dynTemp=%d suppress=%d",
    useMirostat2 ? 1 : 0, mirostatTau, mirostatEta, repPenalty, repWindow, sentenceLimit, hardCap, dynTemp ? 1 : 0, suppressCount);

  // Clamp
  if (maxTokens < 1) maxTokens = 1;
  if (maxTokens > 512) maxTokens = 512;
  if (temperature < 0.0f) temperature = 0.0f;
  if (temperature > 2.0f) temperature = 2.0f;
  if (topp < 0.01f) topp = 0.01f;
  if (topp > 1.0f) topp = 1.0f;
  if (mirostatTau < 0.5f) mirostatTau = 0.5f;
  if (mirostatTau > 20.0f) mirostatTau = 20.0f;
  if (mirostatEta < 0.01f) mirostatEta = 0.01f;
  if (mirostatEta > 1.0f) mirostatEta = 1.0f;
  if (repPenalty < 1.0f) repPenalty = 1.0f;
  if (repPenalty > 5.0f) repPenalty = 5.0f;
  if (repWindow < 0) repWindow = 0;
  if (repWindow > 128) repWindow = 128;
  if (sentenceLimit < 0) sentenceLimit = 0;
  if (sentenceLimit > 20) sentenceLimit = 20;
  if (hardCap < 0) hardCap = 0;
  if (hardCap > 512) hardCap = 512;

  // Start chunked response as text/plain for streaming
  httpd_resp_set_type(req, "text/plain");
  DEBUG_HTTPF("[LLM-API] Starting chunked generation (clamped: max_tokens=%d temp=%.2f topp=%.2f)", maxTokens, temperature, topp);
  unsigned long genStartMs = millis();

  // Generate synchronously on this httpd worker thread.
  // The httpd task stack is ~11KB which is tight, but the heavy lifting
  // (matmul, KV cache) is in heap/PSRAM. The llmGenerate function yields
  // via vTaskDelay so the watchdog stays happy.
  int result = llmGenerate(prompt, [req](const char* token) -> bool {
    esp_err_t err = httpd_resp_send_chunk(req, token, strlen(token));
    return (err == ESP_OK);
  }, maxTokens, temperature, topp, useMirostat2, mirostatTau, mirostatEta,
     repPenalty, repWindow, sentenceLimit, hardCap, dynTemp,
     suppressCount > 0 ? suppressBuf : nullptr, suppressCount);

  // End chunked response
  httpd_resp_send_chunk(req, NULL, 0);

  unsigned long genElapsedMs = millis() - genStartMs;
  if (result < 0) {
    DEBUG_HTTPF("[LLM-API] Generation FAILED (%d) in %lums for prompt: %.64s...", result, genElapsedMs, prompt);
    DEBUG_LLM_GENERATEF("[LLM] Generation failed for prompt: %.32s...", prompt);
  } else {
    DEBUG_HTTPF("[LLM-API] Generation complete: %d tokens in %lums (%.1f tok/s)",
      result, genElapsedMs, genElapsedMs > 0 ? (result * 1000.0f / genElapsedMs) : 0.0f);
  }

  return ESP_OK;
}

// ============================================================================
// GET /llm - Chat page
// ============================================================================
static void streamLLMContent(httpd_req_t* req, const String& username) {
  streamBeginHtml(req, "LLM", false, username, "llm");
  httpd_resp_send_chunk(req, "<div class='card'>", HTTPD_RESP_USE_STRLEN);
  streamLLMInner(req, username);
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
  streamEndHtml(req);
}

static esp_err_t handleLLMPage(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  DEBUG_HTTPF("[LLM-API] GET /llm page for user=%s", ctx.user.c_str());
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
