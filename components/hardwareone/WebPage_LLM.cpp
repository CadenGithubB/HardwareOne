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

#if ENABLE_LLM_BACKEND && ENABLE_HTTP_SERVER

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_http_server.h>

#if ENABLE_LLM_SOURCE_ONBOARD
  #include "System_LLM.h"   // engine-only; the shared vocabulary is in System_LLMTypes.h
#endif
#include "System_LLMBackend.h"   // registry + source dispatch
#if ENABLE_LLM_SOURCE_CM5
  #include "System_LLMCm5.h"      // cm5LlmSelectPendingMs + the select watchdog ceiling
  #include "System_Cm5Presence.h" // host mode: "starting" vs "gone"
#endif
#include "System_LLMChat.h"
#include "System_Debug.h"
#include "System_Settings.h"
#include "System_User.h"
#include "System_MemUtil.h"   // PSRAM_JSON_DOC
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

  LLMStatus st = llmBackendStatus();
  // Guided-menu presence + generation so the chat page can show/hide the guided
  // strip and refetch it when a model swap changes the menu (LLM_GUIDED_MENU_SPEC §7).
  uint8_t  menuGroups = llmMenuGroupCount();
  uint16_t menuGen    = llmMenuGeneration();
  char json[1024];  // headroom for the optional ctxWarning + host fragments + a long error
  const char* stateStr = "UNLOADED";
  switch (st.state) {
    case LLMState::LOADING:    stateStr = "LOADING"; break;
    case LLMState::READY:      stateStr = "READY"; break;
    case LLMState::GENERATING: stateStr = "GENERATING"; break;
    case LLMState::ERROR:      stateStr = "ERROR"; break;
    default: break;
  }

  // LLMConfig describes a LOCAL checkpoint and is ZEROED for a remote source,
  // where arch_type 0 / quant_type 0 would render as the plausible-looking lie
  // "Llama · FP32" for a model this device has never seen the weights of. Emit
  // empty strings instead — the page already skips falsy detail fields.
  const bool localModel = (llmBackendActiveKind() == LlmBackendKind::Onboard);
  const char* archStr  = !localModel ? ""
                       : ((st.config.arch_type == 1) ? "GPT-2" : "Llama");
  const char* quantStr = !localModel ? ""
                       : ((st.config.quant_type == 1) ? "INT8" : "FP32");

  // Degraded-context warning fragment (shared helper — same text as CLI/OLED/G2).
  // The message has no quotes/backslashes, so it needs no JSON escaping.
  char ctxWarnFrag[208];
  {
    char w[176];
    if (llmContextWarning(w, sizeof(w)) > 0)
      snprintf(ctxWarnFrag, sizeof(ctxWarnFrag), ",\"ctxWarn\":true,\"ctxWarning\":\"%s\"", w);
    else
      snprintf(ctxWarnFrag, sizeof(ctxWarnFrag), ",\"ctxWarn\":false");
  }

  // Remote-load progress. A CM5 select restarts llama-server on the host, so a
  // switch is a multi-GB disk read THERE — the link only ever carried the
  // model's name. Both numbers below are measured, not estimated: elapsed comes
  // from the select arm-point, and the ceiling is the same watchdog that will
  // abandon the select. hostMode separates "the host is starting" from "the
  // host is gone", which a single "not available right now" string used to
  // conflate — that ambiguity is the whole reason a first load looked broken.
  // Deliberately no percentage derived from model size: one timing sample is
  // not a calibration (see the 44-cps lesson in EVENAI_ASK_DISPLAY_DEBUG_PLAN).
  char hostFrag[160];   // +loadPct
  hostFrag[0] = '\0';
#if ENABLE_LLM_SOURCE_CM5
  {
    const Cm5PresenceSnapshot pres = cm5PresenceSnapshot();
    snprintf(hostFrag, sizeof(hostFrag),
             ",\"hostMode\":\"%s\",\"hostFresh\":%s,"
             "\"selectMs\":%lu,\"selectMaxMs\":%lu,\"loadPct\":%u",
             cm5PresenceModeName(pres.mode),
             (pres.fresh && pres.seenForSession) ? "true" : "false",
             (unsigned long)cm5LlmSelectPendingMs(),
             (unsigned long)CM5_LLM_SELECT_TIMEOUT_MS,
             (unsigned)cm5LlmLoadPercent());
  }
#endif

  snprintf(json, sizeof(json),
    "{\"state\":\"%s\",\"model\":\"%s\",\"params\":\"%dx%dx%d\","
    "\"psramKB\":%u,\"tokPerSec\":%.1f,\"lastTokens\":%d,\"error\":\"%s\","
    "\"dim\":%d,\"layers\":%d,\"heads\":%d,\"kvHeads\":%d,\"vocab\":%d,\"seqLen\":%d,"
    "\"ctxUsed\":%d,\"ctxMax\":%d,\"arch\":\"%s\",\"quant\":\"%s\","
    "\"menu\":{\"groups\":%u,\"gen\":%u},\"backend\":\"%s\",\"cmdMode\":%s%s%s}",
    stateStr, st.modelPath,
    st.config.dim, st.config.n_layers, st.config.n_heads,
    (unsigned)(st.totalPsramUsed / 1024),
    st.lastTokensPerSec, st.lastTokenCount, st.errorMsg,
    st.config.dim, st.config.n_layers, st.config.n_heads,
    st.config.n_kv_heads, st.config.vocab_size, st.config.seq_len,
    st.lastContextUsed, st.lastContextMax, archStr, quantStr,
    (unsigned)menuGroups, (unsigned)menuGen,
    llmBackendKindName(llmBackendActiveKind()),
    llmBackendSupportsCommandMode() ? "true" : "false", ctxWarnFrag, hostFrag);

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
// GET /api/llm/menu?kind=groups|tpl|ent&g=<g>&off=<off>
// Guided-input menu, served straight from the shared System_LLM_Menu API (NOT
// via /api/cli, so replies aren't clipped at the 4095 B CLI cap). JSON shapes
// mirror `llmmenu json` (LLM_GUIDED_MENU_SPEC §6/§7):
//   kind=groups → {"schema":1,"gen":G,"groups":[{"i":,"name":,"mode":,"templates":,"entities":},...]}
//   kind=tpl|ent → {"schema":1,"gen":G,"g":,"off":,"total":,"items":[...]}   (paged)
// All accessors copy out under sMenuLock; the page pages with `off` until
// off+items.length >= total.
// ============================================================================
static esp_err_t handleLLMMenu(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);

  size_t qlen = httpd_req_get_url_query_len(req);
  char   query[96] = {};
  if (qlen > 0 && qlen < sizeof(query)) httpd_req_get_url_query_str(req, query, sizeof(query));

  char kind[8] = {};
  char param[16];
  if (httpd_query_key_value(query, "kind", kind, sizeof(kind)) != ESP_OK)
    strlcpy(kind, "groups", sizeof(kind));
  int g = 0, off = 0;
  if (httpd_query_key_value(query, "g",   param, sizeof(param)) == ESP_OK) g   = atoi(param);
  if (httpd_query_key_value(query, "off", param, sizeof(param)) == ESP_OK) off = atoi(param);

  const uint16_t gen = llmMenuGeneration();
  const uint8_t  gc  = llmMenuGroupCount();

  PSRAM_JSON_DOC(doc);
  doc["schema"] = 1;
  doc["gen"]    = gen;

  if (strcmp(kind, "groups") == 0) {
    JsonArray groups = doc["groups"].to<JsonArray>();
    for (uint8_t i = 0; i < gc; i++) {
      LLMMenuGroupInfo gi;
      if (!llmMenuGroupInfo(i, &gi)) continue;
      JsonObject o = groups.add<JsonObject>();
      o["i"]         = i;
      o["name"]      = gi.name;                       // char[33] → copied into the doc pool
      o["mode"]      = (gi.flags & 0x01) ? "do" : "ask";
      o["templates"] = gi.tplCount;
      o["entities"]  = gi.entCount;
    }
  } else {
    const bool wantTpl = (strcmp(kind, "tpl") == 0);
    LLMMenuGroupInfo gi;
    if (g < 0 || off < 0 || gc == 0 || g >= (int)gc || !llmMenuGroupInfo((uint8_t)g, &gi))
      return sendJsonResponse(req, "{\"schema\":1,\"error\":\"bad group\"}");
    const int total = wantTpl ? (int)gi.tplCount : (int)gi.entCount;
    const int PAGE  = wantTpl ? 32 : 64;              // bigger than the CLI pages — no 4095 B cap here
    doc["g"]     = g;
    doc["off"]   = off;
    doc["total"] = total;
    JsonArray items = doc["items"].to<JsonArray>();
    char item[136];
    for (int i = off; i < total && i < off + PAGE; i++) {
      int n = wantTpl
                ? llmMenuTemplate((uint8_t)g, (uint16_t)i, item, sizeof(item), nullptr)
                : llmMenuEntity((uint8_t)g, (uint16_t)i, item, sizeof(item));
      if (n < 0) break;
      items.add(item);                                // char[] → copied into the doc pool
    }
  }

  String resp;
  serializeJson(doc, resp);
  return sendJsonResponse(req, resp.c_str());
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

  PSRAM_JSON_DOC(doc);
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
  // Was a hand-typed 2048 while the schema accepted 4096, so every web load
  // silently halved the configured context — and this is the ONLY surface that
  // reads llmMaxContext at all (cmd_llm_load takes no context argument).
  if (maxCtx > LLM_SETTING_MAX_CONTEXT) maxCtx = LLM_SETTING_MAX_CONTEXT;

  DEBUG_HTTPF("[LLM-API] POST /api/llm/load: model='%s' max_ctx=%d", modelName, maxCtx);

  // `model` is a registry id ("onboard:x.bin" / "cm5:y.gguf"); a bare filename
  // or absolute path still resolves, for hand-made requests. The path probing
  // and directory allowlist that used to live here now live once, in
  // llmResolveModelId — a remote model has no path to validate at all.
  char err[96] = {0};
  const bool ok = llmBackendSelect(modelName, err, sizeof(err), maxCtx);
  if (ok) {
    // A remote source is still switching when this returns, so report readiness
    // rather than implying the model is live.
    const bool ready = llmBackendIsReady();
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"ready\":%s}", ready ? "true" : "false");
    DEBUG_HTTPF("[LLM-API] Model selected: %s (ctx=%d ready=%d)", modelName, maxCtx, (int)ready);
    return sendJsonResponse(req, resp);
  }
  char resp[256];
  snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"%s\"}", err[0] ? err : "load failed");
  DEBUG_HTTPF("[LLM-API] Model select FAILED: %s error='%s'", modelName, err);
  return sendJsonResponse(req, resp);
}

// ============================================================================
// POST /api/llm/unload
// ============================================================================
static esp_err_t handleLLMUnload(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  DEBUG_HTTPF("[LLM-API] POST /api/llm/unload from user=%s", ctx.user.c_str());
  llmBackendUnload();
  DEBUG_HTTPF("[LLM-API] Model unloaded");
  return sendJsonResponse(req, "{\"ok\":true}");
}

// ============================================================================
// POST /api/llm/stop
// ============================================================================
static esp_err_t handleLLMStop(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  DEBUG_HTTPF("[LLM-API] POST /api/llm/stop from user=%s", ctx.user.c_str());
  llmBackendStop();
  return sendJsonResponse(req, "{\"ok\":true}");
}

// ============================================================================
// POST /api/llm/generate  { "prompt": "...", "max_tokens": 256, "temperature": 0.8 }
// Starts async background generation; returns {"ok":true,"session":N} immediately.
// Client polls GET /api/llm/result?session=N&offset=N for streamed output.
// ============================================================================

static esp_err_t handleLLMGenerate(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);

  if (!llmBackendIsReady()) {
    DEBUG_HTTPF("[LLM-API] POST /api/llm/generate: model not ready");
    return sendJsonResponse(req, "{\"ok\":false,\"error\":\"model not ready\"}");
  }

  char body[2048];
  if (!readPostBody(req, body, sizeof(body))) {
    DEBUG_HTTPF("[LLM-API] POST /api/llm/generate: body read failed (content_len=%d)", req->content_len);
    return sendJsonResponse(req, "{\"ok\":false,\"error\":\"bad request\"}");
  }

  PSRAM_JSON_DOC(doc);
  if (deserializeJson(doc, body)) {
    return sendJsonResponse(req, "{\"ok\":false,\"error\":\"invalid JSON\"}");
  }

  const char* prompt = doc["prompt"] | "";
  if (!prompt[0]) {
    DEBUG_HTTPF("[LLM-API] POST /api/llm/generate: empty prompt");
    return sendJsonResponse(req, "{\"ok\":false,\"error\":\"empty prompt\"}");
  }

  // Per-request param overrides — anything the client supplied wins, anything
  // it omits falls back to gSettings.llm* inside chatResolveParams. Shared with
  // the BLE `llmgenerate json` path via chatParamOverrideFromJson (System_LLMChat).
  ChatParamOverride ov;
  chatParamOverrideFromJson(doc.as<JsonObjectConst>(), ov);

  DEBUG_HTTPF("[LLM-API] POST /api/llm/generate: prompt='%.60s%s' max_tokens=%d temp=%.2f",
    prompt, strlen(prompt) > 60 ? "..." : "",
    (int)(ov.maxTokens   != INT32_MIN ? ov.maxTokens   : gSettings.llmMaxTokens),
    (double)(!isnan(ov.temperature)   ? ov.temperature : gSettings.llmTemperature));

  // Check before chatBeginTurn so the user gets the REASON. chatBeginTurn
  // returns a bare 0 for every refusal, which would surface here as "busy or
  // failed to start" — actively misleading for a request that will never work
  // on this backend no matter how long you wait.
  if (llmPromptIsCommandMode(prompt) && !llmBackendSupportsCommandMode()) {
    return sendJsonResponse(req,
        "{\"ok\":false,\"error\":\"Do: mode needs the on-device model — a remote "
        "model does not know this device's commands\"}");
  }

  int sessionId = chatBeginTurn(prompt, &ov);
  if (sessionId == 0) {
    return sendJsonResponse(req, "{\"ok\":false,\"error\":\"busy or failed to start\"}");
  }

  char json[48];
  snprintf(json, sizeof(json), "{\"ok\":true,\"session\":%d}", sessionId);
  return sendJsonResponse(req, json);
}

// ============================================================================
// GET /api/llm/result?session=N&offset=N
// Returns buffered tokens since byte offset as JSON:
//   {"text":"...","done":bool,"len":N,"next":N}
// `next` is the absolute byte cursor the device served up to — the client must
// use it rather than measuring the decoded text. `len` is the TOTAL buffered
// length and is NOT a cursor; never compare the two to decide completion.
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

  // Stale session → tell client to stop. Reading the chat-module's view of the
  // session means we get the same staleness semantics whether the user started
  // their last prompt from the web OR the OLED — the chat module owns the
  // canonical session ID.
  int curSession = chatGetSessionId();
  bool generating = chatIsGenerating();
  // Genuinely stale only if a DIFFERENT session is *actively streaming* (e.g. an
  // OLED-initiated generation took over). A finished/idle engine is NOT stale for
  // us — our result may have completed before this first poll (an instant gate
  // refusal returns in microseconds), so fall through and try to recover it.
  if (session != 0 && generating && session != curSession) {
    return sendJsonResponse(req, "{\"text\":\"\",\"done\":true,\"len\":0,\"stale\":true}");
  }

  // Pull bytes from the chat module's live streaming turn (drains the engine).
  // 512 keeps the chunk under ArduinoJson's PSRAM_JSON_DOC working set.
  char chunk[512];
  int n = chatReadStream(offset, chunk, sizeof(chunk));
  int totalLen = chatGetStreamLen();
  if (n == 0 && totalLen == 0) {
    // Nothing live for us — the generation may have finished (and its streaming
    // slot been finalized) before this poll. Recover it from the finished-turn
    // snapshot so instant results (gate refusals, one-liners) don't blank out.
    n        = chatReadFinished(session, offset, chunk, sizeof(chunk));
    totalLen = chatFinishedLen(session);
  }
  // Serving-edge hygiene, once, AFTER the finished-turn fallback has chosen
  // which reader filled `chunk`. The re-NUL must follow the trim: ArduinoJson
  // links the pointer and walks it with strlen, so trimming without it is a
  // silent no-op.
  n = utf8TrimPartialTail(chunk, n);
  chunk[n] = '\0';
  jsonSanitizeServedBytes(chunk, n);

  // `done` must NOT be gated on the client's cursor reaching `totalLen`. A turn
  // can legally end mid-character — the 2 KB cap drops a whole engine window and
  // freezes textLen wherever the previous one ended — so the trim may withhold
  // up to 3 bytes forever and `offset + n >= totalLen` would never be satisfied.
  // That is a fixed-offset livelock. Gate on `n == 0`, which is the terminal
  // condition the trim actually produces. DO NOT "improve" this to compare
  // against totalLen.
  //
  // The `n == 0` conjunct also closes a data-loss bug that predates the UTF-8
  // work: `!chatIsGenerating()` alone is cursor-blind, so an answer that was
  // already complete when the first poll landed got exactly ONE 511-byte window
  // with done:true and the page stopped there. Turn text runs to 2047 bytes, so
  // a fast answer — an instant gate refusal, a CM5 one-liner arriving in a UART
  // burst — silently lost everything past 511.
  bool done = !chatIsGenerating() && (n == 0);

  PSRAM_JSON_DOC(jdoc);
  jdoc["done"] = done;
  jdoc["len"]  = totalLen;
  jdoc["text"] = (n > 0) ? (const char*)chunk : "";
  // Absolute cursor, not a delta: the page's poll has a retry path, and an
  // absolute value is idempotent under a duplicated or reordered response. It is
  // authoritative because the sanitizer above guarantees strlen(chunk) == n,
  // whereas anything the client infers from the DECODED text is wrong for every
  // byte the trim deliberately passes through (U+FFFD re-encodes to 3 bytes).
  jdoc["next"] = offset + n;

  // A turn that ends because the SOURCE failed -- the host vanished mid-answer,
  // the stall watchdog fired, the host reported an error -- has a reason, and
  // the page has no other way to get it. finishGen refreshes status with
  // afterGen=true, which suppresses every announcement so a finished answer
  // does not re-print "Model loaded"; that same suppression swallowed the one
  // message explaining an EMPTY answer. Observed on hardware: stopping the CM5
  // daemon mid-generation left a blank reply and nothing else on screen.
  //
  // Only on the terminal poll, and only when there is something to say, so the
  // 150 ms hot path stays as lean as it was. Safe from staleness because
  // cm5LlmStartAsync clears the error string at the top of every generation, so
  // anything set here belongs to the turn that just ended.
  if (done) {
    const LLMStatus est = llmBackendStatus();
    if (est.errorMsg[0] != '\0') jdoc["error"] = (const char*)est.errorMsg;
  }

  String resp;
  serializeJson(jdoc, resp);
  return sendJsonResponse(req, resp.c_str());
}

// ============================================================================
// GET /api/llm/chat/turns — full conversation as JSON
// Used by the web UI on page load to render any prior turns (which now live
// in firmware, not browser JS). Each entry: {role, text, tokens, tokPerSec, streaming}.
// ============================================================================
static esp_err_t handleLLMChatTurns(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);

  int count = chatGetTurnCount();

  // Streaming write so a 16-turn history with 2 KB bodies doesn't allocate
  // 32+ KB of contiguous JSON in PSRAM all at once. Per-turn buffers are
  // small (textLen capped at LLM_CHAT_TURN_MAX_BYTES = 2 KB).
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send_chunk(req, "[", 1);

  char turnBuf[LLM_CHAT_TURN_MAX_BYTES + 1];
  for (int i = 0; i < count; i++) {
    ChatTurnInfo info;
    if (!chatGetTurnInfo(i, &info)) continue;

    int n = chatReadTurn(i, 0, turnBuf, sizeof(turnBuf));
    (void)n;  // turnBuf is NUL-terminated; ArduinoJson handles the rest.

    PSRAM_JSON_DOC(jdoc);
    jdoc["role"]       = (info.role == ChatTurnRole::USER) ? "user" : "assistant";
    jdoc["text"]       = (const char*)turnBuf;
    jdoc["tokens"]     = info.tokenCount;
    jdoc["tokPerSec"]  = info.tokensPerSecX10 / 10.0f;
    jdoc["streaming"]  = info.isStreaming;

    String entry;
    serializeJson(jdoc, entry);
    if (i > 0) httpd_resp_send_chunk(req, ",", 1);
    httpd_resp_send_chunk(req, entry.c_str(), entry.length());
  }

  httpd_resp_send_chunk(req, "]", 1);
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

// ============================================================================
// POST /api/llm/chat/retry — regenerate the last assistant turn
// Replaces the legacy "browser POSTs /api/llm/generate with suppress=[prior]"
// path. The firmware now owns retry semantics + suppress tokenization.
// ============================================================================
static esp_err_t handleLLMChatRetry(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);

  // Optional per-call param overrides (same shape as /api/llm/generate).
  // Empty body is fine — falls back to gSettings.
  ChatParamOverride ov;
  if (req->content_len > 0 && req->content_len < 2048) {
    char body[2048];
    if (readPostBody(req, body, sizeof(body))) {
      PSRAM_JSON_DOC(doc);
      if (!deserializeJson(doc, body)) {
        if (doc["max_tokens"].is<int>())     ov.maxTokens     = doc["max_tokens"].as<int>();
        if (doc["temperature"].is<float>())  ov.temperature   = doc["temperature"].as<float>();
        if (doc["top_p"].is<float>())        ov.topp          = doc["top_p"].as<float>();
        if (doc["hard_cap"].is<int>())       ov.hardCap       = doc["hard_cap"].as<int>();
      }
    }
  }

  int sessionId = chatRetryLast(&ov);
  if (sessionId == 0) {
    return sendJsonResponse(req, "{\"ok\":false,\"error\":\"no prior turn or busy\"}");
  }
  char json[48];
  snprintf(json, sizeof(json), "{\"ok\":true,\"session\":%d}", sessionId);
  return sendJsonResponse(req, json);
}

// ============================================================================
// POST /api/llm/chat/clear — wipe conversation history
// ============================================================================
static esp_err_t handleLLMChatClear(httpd_req_t* req) {
  WEB_AUTH_OR_RETURN(req, ctx);
  if (!chatClear()) {
    return sendJsonResponse(req, "{\"ok\":false,\"error\":\"busy — stop first\"}");
  }
  return sendJsonResponse(req, "{\"ok\":true}");
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
  static const httpd_uri_t llmPage = { .uri = "/llm", .method = HTTP_GET, .handler = handleLLMPage, .user_ctx = NULL };
  static const httpd_uri_t llmStatus = { .uri = "/api/llm/status", .method = HTTP_GET, .handler = handleLLMStatus, .user_ctx = NULL };
  static const httpd_uri_t llmModels = { .uri = "/api/llm/models", .method = HTTP_GET, .handler = handleLLMModels, .user_ctx = NULL };
  static const httpd_uri_t llmMenu = { .uri = "/api/llm/menu", .method = HTTP_GET, .handler = handleLLMMenu, .user_ctx = NULL };
  static const httpd_uri_t llmLoad = { .uri = "/api/llm/load", .method = HTTP_POST, .handler = handleLLMLoad, .user_ctx = NULL };
  static const httpd_uri_t llmUnload = { .uri = "/api/llm/unload", .method = HTTP_POST, .handler = handleLLMUnload, .user_ctx = NULL };
  static const httpd_uri_t llmGenerate = { .uri = "/api/llm/generate", .method = HTTP_POST, .handler = handleLLMGenerate, .user_ctx = NULL };
  static const httpd_uri_t llmStop    = { .uri = "/api/llm/stop",     .method = HTTP_POST, .handler = handleLLMStop,     .user_ctx = NULL };
  static const httpd_uri_t llmPoll    = { .uri = "/api/llm/result",   .method = HTTP_GET,  .handler = handleLLMPoll,     .user_ctx = NULL };
  static const httpd_uri_t llmChatTurns = { .uri = "/api/llm/chat/turns", .method = HTTP_GET,  .handler = handleLLMChatTurns, .user_ctx = NULL };
  static const httpd_uri_t llmChatRetry = { .uri = "/api/llm/chat/retry", .method = HTTP_POST, .handler = handleLLMChatRetry, .user_ctx = NULL };
  static const httpd_uri_t llmChatClear = { .uri = "/api/llm/chat/clear", .method = HTTP_POST, .handler = handleLLMChatClear, .user_ctx = NULL };

  httpd_register_uri_handler(server, &llmPage);
  httpd_register_uri_handler(server, &llmStatus);
  httpd_register_uri_handler(server, &llmModels);
  httpd_register_uri_handler(server, &llmMenu);
  httpd_register_uri_handler(server, &llmLoad);
  httpd_register_uri_handler(server, &llmUnload);
  httpd_register_uri_handler(server, &llmGenerate);
  httpd_register_uri_handler(server, &llmStop);
  httpd_register_uri_handler(server, &llmPoll);
  httpd_register_uri_handler(server, &llmChatTurns);
  httpd_register_uri_handler(server, &llmChatRetry);
  httpd_register_uri_handler(server, &llmChatClear);
}

#endif // ENABLE_LLM_BACKEND && ENABLE_HTTP_SERVER
