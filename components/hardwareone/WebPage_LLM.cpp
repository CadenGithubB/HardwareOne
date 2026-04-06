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
#include "System_Settings.h"
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

  int maxCtx = doc["max_ctx"] | gSettings.llmMaxContext;  // 0 = use compile-time default
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
// Starts async background generation; returns {"ok":true,"session":N} immediately.
// Client polls GET /api/llm/result?session=N&offset=N for streamed output.
// ============================================================================

static esp_err_t handleLLMGenerate(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);

  if (!llmIsReady()) {
    DEBUG_HTTPF("[LLM-API] POST /api/llm/generate: model not ready");
    return sendJsonResponse(req, "{\"ok\":false,\"error\":\"model not ready\"}");
  }

  char body[2048];
  if (!readPostBody(req, body, sizeof(body))) {
    DEBUG_HTTPF("[LLM-API] POST /api/llm/generate: body read failed (content_len=%d)", req->content_len);
    return sendJsonResponse(req, "{\"ok\":false,\"error\":\"bad request\"}");
  }

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    return sendJsonResponse(req, "{\"ok\":false,\"error\":\"invalid JSON\"}");
  }

  const char* prompt = doc["prompt"] | "";
  if (!prompt[0]) {
    DEBUG_HTTPF("[LLM-API] POST /api/llm/generate: empty prompt");
    return sendJsonResponse(req, "{\"ok\":false,\"error\":\"empty prompt\"}");
  }

  LLMGenParams params;
  params.maxTokens    = doc["max_tokens"]     | gSettings.llmMaxTokens;
  params.temperature  = doc["temperature"]    | gSettings.llmTemperature;
  params.topp         = doc["top_p"]          | gSettings.llmTopP;
  params.useMirostat2 = doc["mirostat2"]      | (bool)gSettings.llmUseMirostat2;
  params.mirostatTau  = doc["mirostat_tau"]   | gSettings.llmMirostatTau;
  params.mirostatEta  = doc["mirostat_eta"]   | gSettings.llmMirostatEta;
  params.repPenalty   = doc["rep_penalty"]    | gSettings.llmRepPenalty;
  params.repWindow    = doc["rep_window"]     | gSettings.llmRepWindow;
  params.sentenceLimit= doc["sentence_limit"] | gSettings.llmSentenceLimit;
  params.hardCap      = doc["hard_cap"]       | gSettings.llmHardCap;
  params.dynTemp      = doc["dyn_temp"]       | (bool)gSettings.llmDynTemp;

  // Clamp
  if (params.maxTokens    <   1) params.maxTokens    = 1;
  if (params.maxTokens    > 512) params.maxTokens    = 512;
  if (params.temperature  < 0.0f)  params.temperature  = 0.0f;
  if (params.temperature  > 2.0f)  params.temperature  = 2.0f;
  if (params.topp         < 0.01f) params.topp         = 0.01f;
  if (params.topp         > 1.0f)  params.topp         = 1.0f;
  if (params.mirostatTau  < 0.5f)  params.mirostatTau  = 0.5f;
  if (params.mirostatTau  > 20.0f) params.mirostatTau  = 20.0f;
  if (params.mirostatEta  < 0.01f) params.mirostatEta  = 0.01f;
  if (params.mirostatEta  > 1.0f)  params.mirostatEta  = 1.0f;
  if (params.repPenalty   < 1.0f)  params.repPenalty   = 1.0f;
  if (params.repPenalty   > 5.0f)  params.repPenalty   = 5.0f;
  if (params.repWindow    <   0)   params.repWindow    = 0;
  if (params.repWindow    > 128)   params.repWindow    = 128;
  if (params.sentenceLimit<   0)   params.sentenceLimit= 0;
  if (params.sentenceLimit>  20)   params.sentenceLimit= 20;
  if (params.hardCap      <   0)   params.hardCap      = 0;
  if (params.hardCap      > 512)   params.hardCap      = 512;

  // Tokenize suppress texts (previous answers to avoid on retry)
  params.suppressCount = 0;
  JsonArray suppressArr = doc["suppress"].as<JsonArray>();
  if (suppressArr) {
    int tmpBuf[64];
    for (JsonVariant v : suppressArr) {
      const char* text = v.as<const char*>();
      if (!text || !text[0]) continue;
      int n = llmTokenize(text, tmpBuf, 64);
      for (int i = 0; i < n && params.suppressCount < 128; i++) {
        int tok = tmpBuf[i];
        bool dup = false;
        for (int j = 0; j < params.suppressCount; j++) {
          if (params.suppressTokens[j] == tok) { dup = true; break; }
        }
        if (!dup) params.suppressTokens[params.suppressCount++] = tok;
      }
    }
  }

  DEBUG_HTTPF("[LLM-API] POST /api/llm/generate (async): prompt='%.60s%s' max_tokens=%d temp=%.2f",
    prompt, strlen(prompt) > 60 ? "..." : "", params.maxTokens, params.temperature);

  int sessionId = llmStartAsync(prompt, params);
  if (sessionId == 0) {
    return sendJsonResponse(req, "{\"ok\":false,\"error\":\"failed to start generation\"}");
  }

  char json[48];
  snprintf(json, sizeof(json), "{\"ok\":true,\"session\":%d}", sessionId);
  return sendJsonResponse(req, json);
}

// ============================================================================
// GET /api/llm/result?session=N&offset=N
// Returns buffered tokens since byte offset as JSON: {"text":"...","done":bool,"len":N}
// ============================================================================
static esp_err_t handleLLMPoll(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);

  // Parse query string: ?session=N&offset=N
  size_t qlen = httpd_req_get_url_query_len(req);
  char   query[64] = {};
  if (qlen > 0 && qlen < sizeof(query)) {
    httpd_req_get_url_query_str(req, query, sizeof(query));
  }

  int offset = 0, session = 0;
  char param[16];
  if (httpd_query_key_value(query, "offset",  param, sizeof(param)) == ESP_OK) offset  = atoi(param);
  if (httpd_query_key_value(query, "session", param, sizeof(param)) == ESP_OK) session = atoi(param);

  // Stale session → tell client to stop
  if (session != llmGetSessionId()) {
    return sendJsonResponse(req, "{\"text\":\"\",\"done\":true,\"len\":0,\"stale\":true}");
  }

  // Read up to 512 bytes of new tokens starting at offset
  char chunk[512];
  int n = llmGetResultChunk(offset, chunk, sizeof(chunk));
  bool done = llmIsGenerationDone();

  // Serialize with ArduinoJson so token text is properly escaped
  JsonDocument jdoc;
  jdoc["done"] = done;
  jdoc["len"]  = llmGetResultLen();
  jdoc["text"] = (n > 0) ? (const char*)chunk : "";

  String resp;
  serializeJson(jdoc, resp);
  return sendJsonResponse(req, resp.c_str());
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
  static httpd_uri_t llmStop    = { .uri = "/api/llm/stop",     .method = HTTP_POST, .handler = handleLLMStop,     .user_ctx = NULL };
  static httpd_uri_t llmPoll    = { .uri = "/api/llm/result",   .method = HTTP_GET,  .handler = handleLLMPoll,     .user_ctx = NULL };

  httpd_register_uri_handler(server, &llmPage);
  httpd_register_uri_handler(server, &llmStatus);
  httpd_register_uri_handler(server, &llmModels);
  httpd_register_uri_handler(server, &llmLoad);
  httpd_register_uri_handler(server, &llmUnload);
  httpd_register_uri_handler(server, &llmGenerate);
  httpd_register_uri_handler(server, &llmStop);
  httpd_register_uri_handler(server, &llmPoll);
}

#endif // ENABLE_ONDEVICE_LLM && ENABLE_HTTP_SERVER
