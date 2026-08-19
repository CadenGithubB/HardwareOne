// System_LLMCommands.cpp — the LLM command surface, settings table and registry rows.
//
// Split out of System_LLM.cpp so it compiles under ENABLE_LLM_BACKEND (the
// FEATURE) rather than under the engine's own flag. That file holds the
// on-device inference engine and is compiled only when the onboard source is
// selected; these commands must exist in a build whose only answer source is
// the CM5 co-processor.
//
// Every handler here is source-agnostic: it talks to System_LLMBackend or
// System_LLMChat, never to the engine. The three that genuinely need local
// weights (guided menus, the corruption self-test) keep their registry row
// unconditionally — a row must never be #if-wrapped, because a missing id reads
// to the app as "assume present" — and report the limitation from inside the
// handler instead.

#include "System_BuildConfig.h"

#if ENABLE_LLM_BACKEND

#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>

#include "System_Command.h"
#include "System_Debug.h"
#include "System_LLMBackend.h"
#include "System_LLMChat.h"
#include "System_LLMTypes.h"
#include "System_MemUtil.h"
#include "System_Settings.h"
#include "System_Utils.h"
#include "System_VFS.h"

#if ENABLE_LLM_SOURCE_ONBOARD
  #include "System_LLM.h"
  #include "System_LLM_Internal.h"   // cmd_llm_menu / cmd_llm_ask live in System_LLM_Menu.cpp
#endif

// Shared reply scratch. Command handlers are serialized by cmd_exec_task, so a
// single buffer is safe; PSRAM keeps it off the tight internal heap.
EXT_RAM_BSS_ATTR static char llmCmdBuf[1024];

// ---------------------------------------------------------------------------
// Model listing — a projection of the registry, usable with no engine present
// ---------------------------------------------------------------------------

String llmListModels() {
  // Was a private filesystem scan. Now a projection of the shared registry, so
  // the web picker sees every source — including remote ones that have no file
  // on this device — and cannot drift from what `llmmodels` reports.
  EXT_RAM_BSS_ATTR static LlmModelDesc rows[LLM_REGISTRY_MAX_MODELS];
  const size_t n = llmEnumerateModels(rows, LLM_REGISTRY_MAX_MODELS);

  String json = "[";
  for (size_t k = 0; k < n; k++) {
    if (k) json += ",";
    json += "{\"id\":\"";      json += rows[k].id;
    json += "\",\"name\":\""; json += rows[k].name;
    json += "\",\"backend\":\""; json += llmBackendKindName(rows[k].backend);
    json += "\",\"size\":";   json += (unsigned long)rows[k].sizeKB * 1024UL;
    json += ",\"path\":\"";   json += rows[k].path;
    json += "\",\"storage\":\"";
    json += (rows[k].storage == LLM_STORAGE_SD)     ? "sd"
          : (rows[k].storage == LLM_STORAGE_REMOTE) ? "remote" : "internal";
    json += "\",\"available\":";
    json += rows[k].available ? "true" : "false";
    json += "}";
  }
  json += "]";
  return json;
}

// ---------------------------------------------------------------------------
// Engine-only rows — present in every build, honest when the engine is absent
// ---------------------------------------------------------------------------

// Defined further down in this file; the wrappers below sit above it so they
// can be named in llmCommands[] without reordering the moved block.
static const char* cmd_llm_corrupt_test(const String& args);

static const char* cmdLlmMenuRow(const String& a) {
#if ENABLE_LLM_SOURCE_ONBOARD
  return cmd_llm_menu(a);
#else
  (void)a;
  return "Error: guided question menus come from a local model file - this build answers from a remote source";
#endif
}

static const char* cmdLlmAskRow(const String& a) {
#if ENABLE_LLM_SOURCE_ONBOARD
  return cmd_llm_ask(a);
#else
  (void)a;
  return "Error: guided questions need a local model - use llmgenerate instead";
#endif
}

static const char* cmdLlmCorruptRow(const String& a) {
#if ENABLE_LLM_SOURCE_ONBOARD
  return cmd_llm_corrupt_test(a);
#else
  (void)a;
  return "Error: no on-board engine in this build";
#endif
}

// Debug: arm a one-shot RunState corruption to verify the forward() guard +
// llmBindRunState rebind + retry recovery path. The next generation nulls s->x;
// the guard catches it, rebinds from the intact base blocks, and continues — so
// the answer still completes. Lets the corruption-recovery path be proven on demand.
static const char* cmd_llm_corrupt_test(const String& /*args*/) {
  if (!llmBackendIsReady()) return "Error: LLM not loaded — load a model first";
#if ENABLE_LLM_SOURCE_ONBOARD
  gLLM.injectCorruptOnce = true;
#else
  return "Error: no on-board engine in this build";
#endif
  return "Armed: next generation injects one RunState corruption. Run a prompt, then look for "
         "'[LLM] TEST: injected' followed by 'rebound RunState, retry 1/2' — the answer should still complete.";
}

static const char* cmd_llm_status(const String& argsInput) {
  LLMStatus st = llmBackendStatus();
  const char* stateStr = "UNLOADED";
  switch (st.state) {
    case LLMState::LOADING:    stateStr = "LOADING"; break;
    case LLMState::READY:      stateStr = "READY"; break;
    case LLMState::GENERATING: stateStr = "GENERATING"; break;
    case LLMState::ERROR:      stateStr = "ERROR"; break;
    default: break;
  }

  // Structured path: one verbatim JSON blob via a PSRAM buffer (no
  // broadcastOutput). This is what the app's Chat page polls for
  // state / model / tok-s. Schema:
  //   {"v":1,"state","model","tokPerSec","error","psramKB",
  //    "contextUsed","contextMax","tokens"}
  if (argWantsJson(argsInput)) {
    const char* model = st.modelPath;
    const char* slash = strrchr(st.modelPath, '/');
    if (slash) model = slash + 1;            // filename only, per contract
    PSRAM_JSON_DOC(doc);
    // Degraded-context warning (shared helper — same text every surface uses).
    // char[] so ArduinoJson copies it into the doc pool (safe past this scope).
    char ctxWarnBuf[192];
#if ENABLE_LLM_SOURCE_ONBOARD
    // PSRAM context auto-fit is an on-device concept: a remote host owns its
    // own window, so there is nothing here to warn about.
    const bool ctxDeg = llmContextWarning(ctxWarnBuf, sizeof(ctxWarnBuf)) > 0;
#else
    ctxWarnBuf[0] = '\0';
    const bool ctxDeg = false;
#endif
    doc["schema"]           = 1;
    doc["state"]       = stateStr;
    doc["model"]       = model;
    doc["tokPerSec"]   = st.lastTokensPerSec;
    doc["error"]       = st.errorMsg;        // "" when no error
    doc["psramKB"]     = (unsigned)(st.totalPsramUsed / 1024);
    doc["contextUsed"] = st.lastContextUsed;
    doc["contextMax"]  = st.lastContextMax;
    doc["tokens"]      = st.lastTokenCount;
    doc["meanLogprob"] = st.lastMeanLogprob;   // Phase 2 confidence (0 = no signal)
    doc["confTokens"]  = st.lastConfTokens;
    doc["ctxWarn"]     = ctxDeg;               // true → context auto-shrunk unusably low
    if (ctxDeg) doc["ctxWarning"] = ctxWarnBuf;
    // Guided-input menu: presence + generation so a surface can show/hide the
    // guided picker and detect a model swap (refetch on gen change).
    JsonObject menu = doc["menu"].to<JsonObject>();
#if ENABLE_LLM_SOURCE_ONBOARD
    menu["groups"] = llmMenuGroupCount();
    menu["gen"]    = llmMenuGeneration();
#else
    menu["groups"] = 0;   // guided menus are baked into a local .bin
    menu["gen"]    = 0;
#endif
    static char* jbuf = nullptr;
    if (!jbuf) jbuf = (char*)ps_alloc(640, AllocPref::PreferPSRAM, "llmstatus.json");
    if (!jbuf) return "{\"error\":\"oom\"}";
    serializeJson(doc, jbuf, 640);
    return jbuf;
  }

  char ctxWarnLine[224];
  {
    ctxWarnLine[0] = '\0';
#if ENABLE_LLM_SOURCE_ONBOARD
    char w[192];
    if (llmContextWarning(w, sizeof(w)) > 0)
      snprintf(ctxWarnLine, sizeof(ctxWarnLine), "WARNING: %s\n", w);
#endif
  }
  snprintf(llmCmdBuf, sizeof(llmCmdBuf),
    "LLM State: %s\n"
    "Model: %s\n"
    "Config: dim=%d layers=%d heads=%d vocab=%d seq=%d ctx=%d\n"
    "PSRAM: %uKB (weights=%uKB runtime=%uKB)\n"
    "Last: %d tokens @ %.1f tok/s\n"
    "%s%s%s",
    stateStr, st.modelPath,
    st.config.dim, st.config.n_layers, st.config.n_heads,
    st.config.vocab_size, st.config.seq_len, st.lastContextMax,
    (unsigned)(st.totalPsramUsed / 1024),
    (unsigned)(st.modelSizeBytes / 1024),
    (unsigned)(st.runtimeSizeBytes / 1024),
    st.lastTokenCount, st.lastTokensPerSec,
    ctxWarnLine,
    st.errorMsg[0] ? "Error: " : "",
    st.errorMsg[0] ? st.errorMsg : "");
  return llmCmdBuf;
}

static const char* cmd_llm_load(const String& args) {
  if (!gSettings.llmEnabled) {
    return "{\"schema\":1,\"ok\":false,\"error\":\"LLM disabled - run llmenabled 1 first\"}";
  }
  CommandArgs ca(args);
  String a = ca.arg(0);   // optional model id / filename

  // Selection now goes through the registry, which owns every id→source rule:
  // "onboard:model.bin", "cm5:Qwen3-1.7B-Q4_0.gguf", a bare filename, or an
  // absolute path. The old SD-then-internal probe that used to live here is
  // inside llmResolveModelId, so the CLI, the web, the autostart and the
  // pickers cannot disagree about what a name means.
  const char* want = a.length() > 0 ? a.c_str() : gSettings.llmDefaultModel.c_str();

  char err[96] = {0};
  const bool ok = llmBackendSelect(want, err, sizeof(err));

  // JSON reply mirrors POST /api/llm/load ({"ok":true} / {"ok":false,"error"}).
  // The app sends `llmload <name>` (no `json` token) and parses the reply like
  // the web, so this must be a JSON object, not the old human "Model loaded:".
  if (ok) {
    // A remote source returns here with the host still switching models, so
    // report accepted-not-ready rather than implying the model is live.
    const bool ready = llmBackendIsReady();
    snprintf(llmCmdBuf, sizeof(llmCmdBuf),
             "{\"schema\":1,\"ok\":true,\"ready\":%s%s}",
             ready ? "true" : "false",
             ready ? "" : ",\"hint\":\"the source is still loading - poll 'llmstatus json'\"");
  } else {
    // err is firmware-controlled (short, no quotes/backslashes) — safe to inline.
    snprintf(llmCmdBuf, sizeof(llmCmdBuf),
             "{\"schema\":1,\"ok\":false,\"error\":\"%s\"}", err[0] ? err : "load failed");
  }
  return llmCmdBuf;
}

static const char* cmd_llm_unload(const String&) {
  // llmUnload() already stops a live generation and waits for it before freeing
  // the weights, so there is nothing to add here. The generation worker stays
  // parked on its notify holding its stack — see gLLMWorkerStack.
  llmBackendUnload();
  return "{\"schema\":1,\"ok\":true}";   // mirror POST /api/llm/unload
}

static const char* cmd_llm_models(const String& argsInput) {
  // SCHEMA 2 — rows are objects carrying an `id`, not bare filenames.
  // A model is no longer necessarily a file on this device, so a name alone is
  // not a handle any more; `id` is what llmload/llmdefaultmodel accept. Apps
  // built against schema 1 (which read models[] as strings) must be updated.
  if (argWantsJson(argsInput)) {
    EXT_RAM_BSS_ATTR static LlmModelDesc rows[LLM_REGISTRY_MAX_MODELS];
    const size_t n = llmEnumerateModels(rows, LLM_REGISTRY_MAX_MODELS);
    PSRAM_JSON_DOC(out);
    out["schema"] = 2;
    JsonArray arr = out["models"].to<JsonArray>();
    for (size_t k = 0; k < n; k++) {
      JsonObject o = arr.add<JsonObject>();
      o["id"]        = rows[k].id;         // char[] → copied into the doc pool
      o["name"]      = rows[k].name;
      o["backend"]   = llmBackendKindName(rows[k].backend);
      o["sizeKB"]    = rows[k].sizeKB;
      o["storage"]   = (rows[k].storage == LLM_STORAGE_SD)     ? "sd"
                     : (rows[k].storage == LLM_STORAGE_REMOTE) ? "remote" : "internal";
      o["available"] = rows[k].available;
    }
    static char* jbuf = nullptr;
    if (!jbuf) jbuf = (char*)ps_alloc(1536, AllocPref::PreferPSRAM, "llmmodels.json");
    if (!jbuf) return "{\"error\":\"oom\"}";
    serializeJson(out, jbuf, 1536);
    return jbuf;
  }

  EXT_RAM_BSS_ATTR static LlmModelDesc rows[LLM_REGISTRY_MAX_MODELS];
  const size_t n = llmEnumerateModels(rows, LLM_REGISTRY_MAX_MODELS);
  int w = snprintf(llmCmdBuf, sizeof(llmCmdBuf), "Models (%u):", (unsigned)n);
  for (size_t k = 0; k < n && w > 0 && w < (int)sizeof(llmCmdBuf); k++) {
    w += snprintf(llmCmdBuf + w, sizeof(llmCmdBuf) - w, "\n  %-28s %s%s",
                  rows[k].id,
                  llmBackendKindName(rows[k].backend),
                  rows[k].available ? "" : "  (unavailable)");
  }
  if (n == 0) {
    snprintf(llmCmdBuf, sizeof(llmCmdBuf),
             "No models. %s",
             gSettings.llmEnabled ? "Add a .bin under /system/llm, or connect the CM5."
                                  : "The LLM is disabled - run llmenabled 1 first.");
  }
  return llmCmdBuf;
}

static const char* cmd_llm_generate(const String& args) {
  // Structured (async, non-blocking) path. Contract: `llmgenerate json <prompt>`
  // — the leading `json` token is the mode flag, everything after it is the
  // prompt (which may contain spaces, and may itself mention the word "json").
  // So detect the LEADING token here rather than via argWantsJson(), which
  // scans the whole string and would false-trigger on a prompt that merely
  // says "json". Kicks generation off via the shared chat layer and returns
  // {session} immediately — it must NOT block until generation finishes (that
  // is the whole reason this path exists; the blocking human path below would
  // tie up the BLE channel for the entire run and trip the app's watchdog).
  {
    String a = args; a.trim();
    if (argLeadingTokenIsJson(a)) {
      if (!llmBackendIsReady()) return "{\"schema\":1,\"ok\":false,\"error\":\"model not ready\"}";
      String payload = a.startsWith("json ") ? a.substring(5) : String();
      payload.trim();
      if (payload.length() == 0) return "{\"schema\":1,\"ok\":false,\"error\":\"empty prompt\"}";

      // Two accepted shapes:
      //   json <raw prompt text>                    → no overrides
      //   json {"prompt":"...","params":{...}}       → per-request overrides
      // The object form lets the app tune a single reply (e.g. hard_cap +
      // sentence_limit for a Do:-style short answer) without mutating settings.
      // See docs/LLM_BLE_GENERATE_OVERRIDES.md for the wire contract.
      String prompt;
      ChatParamOverride ov;
      bool haveOverrides = false;
      if (payload[0] == '{') {
        PSRAM_JSON_DOC(body);   // parse pool in PSRAM, not the tight internal DRAM
        if (deserializeJson(body, payload) != DeserializationError::Ok)
          return "{\"schema\":1,\"ok\":false,\"error\":\"invalid JSON payload\"}";
        prompt = (const char*)(body["prompt"] | "");
        prompt.trim();
        if (prompt.length() == 0) return "{\"schema\":1,\"ok\":false,\"error\":\"empty prompt\"}";
        JsonObjectConst params = body["params"].as<JsonObjectConst>();
        if (!params.isNull()) { chatParamOverrideFromJson(params, ov); haveOverrides = true; }
      } else {
        prompt = payload;  // raw text form
      }

      int session = chatBeginTurn(prompt.c_str(), haveOverrides ? &ov : nullptr);
      if (session <= 0) return "{\"schema\":1,\"ok\":false,\"error\":\"busy or failed to start\"}";
      // Mirror the web's {"ok":true,"session":N} (POST /api/llm/generate &
      // /chat/retry). The app validates the start by `ok`, so omitting it reads
      // as "command not recognized" → its "streaming not supported" fallback.
      // Generation runs async: this only starts it. The reply streams in
      // separately, so point the caller at the poll command (the `hint` key
      // mirrors the human-path cliHint — same text either way).
      snprintf(llmCmdBuf, sizeof(llmCmdBuf),
               "{\"schema\":1,\"ok\":true,\"session\":%d,"
               "\"hint\":\"the reply streams in asynchronously - read it with 'llmresult json 0'\"}",
               session);
      return llmCmdBuf;
    }
  }

  if (!llmBackendIsReady()) return "Error: no model loaded";

  CommandArgs ca(args);
  if (ca.count() == 0) return "Error: invalid arguments — Usage: llm generate <prompt>";
  String a = ca.raw();  // full prompt text

  // This path drives the engine directly instead of going through chatBeginTurn,
  // so it has to frame the prompt itself — otherwise `llmgenerate <question>`
  // would keep completing prose while `llmgenerate json <question>` answered
  // properly. Static rather than a stack buffer: llmGenerate runs on this same
  // task and already wants ~5 KB of its stack, and llmGenerate's
  // READY->GENERATING gate is what stops two synchronous generations — hence two
  // users of this buffer — from overlapping.
  EXT_RAM_BSS_ATTR static char framedBuf[1024];
  const char* framed = llmBackendFramePrompt(a.c_str(), framedBuf, sizeof(framedBuf));

  // Build output into buffer
  String output;
  output.reserve(1024);

#if ENABLE_LLM_SOURCE_ONBOARD
  int result = llmGenerate(framed, [&output](const char* token) -> bool {
    output += token;
    return (output.length() < 2000);  // safety limit for CLI
  }, 128, LLM_DEFAULT_TEMPERATURE);
#else
  // The blocking human path exists only for the local engine: a remote answer
  // arrives asynchronously as inbound pushes, and blocking this task waiting
  // for them would stall the very UART drain that delivers them.
  (void)framed;
  int result = -1;
  return "Error: this build answers from a remote source - use 'llmgenerate json <text>' "
         "then 'llmresult json 0'";
#endif

  if (result < 0) {
    snprintf(llmCmdBuf, sizeof(llmCmdBuf), "Generation error");
  } else {
    // Truncate if needed
    if (output.length() >= sizeof(llmCmdBuf) - 32) {
      output = output.substring(0, sizeof(llmCmdBuf) - 32);
      output += "\n[truncated]";
    }
    snprintf(llmCmdBuf, sizeof(llmCmdBuf), "%s\n(%d tokens)", output.c_str(), result);
  }
  return llmCmdBuf;
}

static const char* cmd_llm_result(const String& args) {
  // Poll for streamed tokens. Contract: `llmresult json <offset>` →
  //   {"v":1,"text":"<bytes since offset>","done":<bool>,"len":<total so far>}
  // Mirrors GET /api/llm/result. We read straight from the engine's result
  // buffer (llmGetResult*) rather than the chat layer's streaming cursor:
  // the engine buffer persists after generation ends, so the final chunk and
  // the total length stay readable on the very poll where done flips true —
  // whereas chatReadStream()/chatGetStreamLen() return 0 the instant the
  // streaming turn is finalized, which would silently drop the tail.
  String a = args; a.trim();
  if (!argLeadingTokenIsJson(a))
    return "Error: invalid arguments — Usage: llmresult json <offset>";

  int offset = 0;
  if (a.startsWith("json ")) {
    String rest = a.substring(5); rest.trim();
    offset = rest.toInt();
    if (offset < 0) offset = 0;
  }

  // 512-byte read window mirrors the web poller. The app polls ~every 350 ms
  // and advances offset = len.
  //
  // CORRECTION to what this comment used to claim: data IS lost whenever the
  // backlog exceeds the window. Advancing to `len` skips everything between
  // offset+n and len, and the engine buffer is not re-read for the skipped
  // span. The `next` cursor below is the fix on this side; whether the app
  // adopts it is the app's call, and until it does this path stays
  // lossy-but-no-worse.
  char chunk[512];
  int  n     = llmBackendResultChunk(offset, chunk, sizeof(chunk));
  int  total = llmBackendResultLen();
  bool done  = llmBackendIsDone();

  // Same serving-edge hygiene as /api/llm/result: never end a chunk inside a
  // multi-byte UTF-8 sequence, and never hand ArduinoJson a raw control byte
  // (which would emit invalid JSON, or stop the linked-string serializer early
  // on a NUL while the byte count carried on). Re-NUL between the two.
  n = utf8TrimPartialTail(chunk, n);
  chunk[n] = '\0';
  jsonSanitizeServedBytes(chunk, n);

  PSRAM_JSON_DOC(doc);
  doc["schema"]    = 1;
  doc["text"] = (n > 0) ? (const char*)chunk : "";   // linked; chunk outlives serialize
  doc["done"] = done;
  doc["len"]  = total;
  // Absolute byte cursor actually served. Additive: a client that ignores it
  // behaves exactly as before. `done`/`len` semantics are deliberately NOT
  // changed here — different backend, different consumer, not testable from
  // this repo.
  doc["next"] = offset + n;
  // Sized for the worst case: a full 512-byte window where every byte needs
  // \uXXXX escaping (512×6) + envelope, still inside the 4 KB command-return
  // cap. Real LLM text escapes to ~1.1× so the typical blob is ~550 B.
  static char* jbuf = nullptr;
  if (!jbuf) jbuf = (char*)ps_alloc(3200, AllocPref::PreferPSRAM, "llmresult.json");
  if (!jbuf) return "{\"error\":\"oom\"}";
  serializeJson(doc, jbuf, 3200);
  return jbuf;
}

static const char* cmd_llm_stop(const String&) {
  llmBackendStop();
  return "{\"schema\":1,\"ok\":true}";   // mirror POST /api/llm/stop
}

static const char* cmd_llm_clear(const String&) {
  // Reset the shared conversation. Thin wrapper over chatClear() — the same
  // call POST /api/llm/chat/clear makes. Without this, a BLE app's "New chat"
  // only wipes its own bubbles while the device keeps accumulating turns
  // (the model still "remembers" cleared messages and contextUsed creeps up).
  // chatClear() refuses mid-generation, so surface that as a stop-first hint.
  // JSON mirrors POST /api/llm/chat/clear so the app parses it like the web.
  if (!chatClear()) return "{\"schema\":1,\"ok\":false,\"error\":\"busy — stop first\"}";
  return "{\"schema\":1,\"ok\":true}";
}

static const char* cmd_llm_retry(const String&) {
  // Regenerate the last assistant reply. Thin wrapper over chatRetryLast()
  // (mirror of POST /api/llm/chat/retry): it drops the last reply, steers the
  // model away from repeating it, and kicks off a fresh async generation.
  // Returns a session exactly like `llmgenerate json` — the app then polls
  // `llmresult json <offset>` to stream the new reply.
  if (!llmBackendIsReady()) return "{\"schema\":1,\"ok\":false,\"error\":\"model not ready\"}";
  int session = chatRetryLast(nullptr);
  if (session <= 0) return "{\"schema\":1,\"ok\":false,\"error\":\"no prior turn or busy\"}";
  // Like llmgenerate, this only kicks off the regeneration; the new reply
  // streams in separately via the result poller.
  snprintf(llmCmdBuf, sizeof(llmCmdBuf),
           "{\"schema\":1,\"session\":%d,"
           "\"hint\":\"the new reply streams in asynchronously - read it with 'llmresult json 0'\"}",
           session);
  return llmCmdBuf;
}

static const char* cmd_llm_turns(const String& args) {
  // Resync the conversation after a reconnect. Wraps the same turn-reader
  // functions the web's GET /api/llm/chat/turns uses (chatGetTurnCount /
  // chatGetTurnInfo / chatReadTurn — web-only until now).
  //
  // The web endpoint streams an unbounded array; a BLE command reply is capped
  // at 4 KB and can't chunk arbitrarily. So this is paginated ONE TURN PER
  // CALL by index — a single turn (<=2 KB body) always fits. The app reads
  // `count` from any response, then fetches index 0..count-1. Schema:
  //   {"v":1,"count":N,"index":I,"role":"user|assistant","text":"…",
  //    "tokens":T,"tokPerSec":F,"streaming":bool}
  //   out-of-range → {"v":1,"count":N,"index":I,"end":true}
  String a = args; a.trim();
  if (!argLeadingTokenIsJson(a))
    return "Error: invalid arguments — Usage: llmturns json <index>";

  int index = 0;
  if (a.startsWith("json ")) {
    String rest = a.substring(5); rest.trim();
    index = rest.toInt();
    if (index < 0) index = 0;
  }

  int count = chatGetTurnCount();
  PSRAM_JSON_DOC(doc);
  doc["schema"]     = 1;
  doc["count"] = count;
  doc["index"] = index;

  ChatTurnInfo info;
  if (index >= count || !chatGetTurnInfo(index, &info)) {
    doc["end"] = true;
    static char* ebuf = nullptr;
    if (!ebuf) ebuf = (char*)ps_alloc(96, AllocPref::PreferPSRAM, "llmturns_end.json");
    if (!ebuf) return "{\"error\":\"oom\"}";
    serializeJson(doc, ebuf, 96);
    return ebuf;
  }

  // Read the turn body into a reusable PSRAM scratch buffer. Assigned to the
  // doc as a (non-const) char* so ArduinoJson COPIES it into the doc pool —
  // the scratch buffer can then be reused safely on the next call.
  static char* turnBuf = nullptr;
  if (!turnBuf) turnBuf = (char*)ps_alloc(LLM_CHAT_TURN_MAX_BYTES + 1, AllocPref::PreferPSRAM, "llmturn.txt");
  if (!turnBuf) return "{\"error\":\"oom\"}";
  chatReadTurn(index, 0, turnBuf, LLM_CHAT_TURN_MAX_BYTES + 1);

  doc["role"]      = (info.role == ChatTurnRole::USER) ? "user" : "assistant";
  doc["text"]      = turnBuf;                       // char* → copied into doc
  doc["tokens"]    = info.tokenCount;
  doc["tokPerSec"] = info.tokensPerSecX10 / 10.0f;
  doc["streaming"] = info.isStreaming;

  // One turn body is <=2 KB; realistic text escapes to ~1.1×, so the blob sits
  // well under the 4 KB command-return cap.
  static char* jbuf = nullptr;
  if (!jbuf) jbuf = (char*)ps_alloc(4096, AllocPref::PreferPSRAM, "llmturns.json");
  if (!jbuf) return "{\"error\":\"oom\"}";
  serializeJson(doc, jbuf, 4096);
  return jbuf;
}

// ============================================================================
// LLM Settings Module (generation defaults, persisted)
// ============================================================================

// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options, isSecret, group, cmdKey
static const SettingEntry llmSettingEntries[] = {
  { "llmEnabled", SETTING_BOOL, &gSettings.llmEnabled, 1, 0, nullptr, 0, 1, "Enabled", nullptr, false, nullptr, "llmenabled" },
  { "autoStart",     SETTING_BOOL,   &gSettings.llmAutoStart,     0, 0,    nullptr,    0,    1, "Auto-start at boot",   nullptr, false, nullptr, "llmautostart" },  // idx 1
  { "temperature",   SETTING_FLOAT,  &gSettings.llmTemperature,  0,   0.5f, nullptr,    0,    0, "Temperature",          nullptr, false, nullptr, "llmtemperature"   },
  { "topP",          SETTING_FLOAT,  &gSettings.llmTopP,         0,   0.8f, nullptr,    0,    1, "Top-P",                nullptr, false, nullptr, "llmtopp"          },
  { "maxTokens",     SETTING_INT,    &gSettings.llmMaxTokens,    256, 0,    nullptr,    1,  512, "Max Tokens",           nullptr, false, nullptr, "llmmaxtokens"     },
  { "sentenceLimit", SETTING_INT,    &gSettings.llmSentenceLimit,  2, 0,    nullptr,    0,   20, "Sentence Limit",       nullptr, false, nullptr, "llmsentencelimit" },
  { "hardCap",       SETTING_INT,    &gSettings.llmHardCap,       80, 0,    nullptr,    0,  512, "Hard Cap",             nullptr, false, nullptr, "llmhardcap"       },
  { "repPenalty",    SETTING_FLOAT,  &gSettings.llmRepPenalty,    0,  1.3f, nullptr,    1,    3, "Rep Penalty",          nullptr, false, nullptr, "llmreppenalty"    },
  { "repWindow",     SETTING_INT,    &gSettings.llmRepWindow,     32, 0,    nullptr,    1, LLM_DEFAULT_REP_WINDOW, "Rep Window", nullptr, false, nullptr, "llmrepwindow" },  // max = ring size; higher values were silently truncated
  { "maxContext",    SETTING_INT,    &gSettings.llmMaxContext,     0, 0,    nullptr,    0, LLM_SETTING_MAX_CONTEXT, "Max Context (0=auto)", nullptr, false, nullptr, "llmmaxcontext"    },
  { "defaultModel",  SETTING_STRING, &gSettings.llmDefaultModel,  0, 0,    "model.bin",0,    0, "Default Model",        nullptr, false, nullptr, "llmdefaultmodel"  },
  // Row order is free. LLM_SETTING_CMD resolves by cmdKey, not by position, so
  // rows may be inserted, reordered or removed anywhere without touching the
  // command handlers. (This was NOT always true: the macro used to index this
  // array directly, and adding llmEnabled/autoStart at the top silently shifted
  // every setter by two — llmdefaultmodel wrote a string into repWindow — for as
  // long as the whole file stayed compiled out. Do not reintroduce index-based
  // dispatch. Mirostat + dynTemp rows were removed 2026-07-10.)
  { "minP",          SETTING_FLOAT,  &gSettings.llmMinP,          0, 0.0f, nullptr,    0,    1, "Min-P (0=off)",        nullptr, false, nullptr, "llmminp"   },  // idx 11
  { "kvPrecision",   SETTING_INT,    &gSettings.llmKvPrecision,   1, 0,    nullptr,    0,    2, "KV Cache (0=FP32,1=FP16,2=INT8, reload to apply)", "0:FP32,1:FP16,2:INT8", false, nullptr, "llmkvprec" },  // idx 12 (default FP16: 2x ctx, ~lossless)
  { "noRepeatNgram", SETTING_INT,    &gSettings.llmNoRepeatNgram, 0, 0,    nullptr,    0,    8, "No-repeat n-gram (0=off)", nullptr, false, nullptr, "llmnorepeatngram" },  // idx 13 (shipped off per user; 3 typical)
  { "confThreshold", SETTING_FLOAT,  &gSettings.llmConfThreshold, 0, -1.0f, nullptr,  -8,    0, "Confidence gate mean-logprob (0=off)", nullptr, false, nullptr, "llmconfthreshold" },  // idx 14
  { "contentBoost",  SETTING_FLOAT,  &gSettings.llmContentBoost,  0, 1.5f, nullptr,    0,    4, "Content boost (0=off)", nullptr, false, nullptr, "llmcontentboost" },  // idx 15 (on-topic logit bonus; co-tune w/ repPenalty)
  { "domainGate",    SETTING_BOOL,   &gSettings.llmDomainGate,    1, 0,    nullptr,    0,    1, "Domain gate (refuse off-topic)", nullptr, false, nullptr, "llmdomaingate" },  // idx 16 (refuse prompts outside the .bin's embedded domain vocab)
  { "profile",       SETTING_BOOL,   &gSettings.llmProfile,       0, 0,    nullptr,    0,    1, "Profiler (per-section fwd timing)", nullptr, false, nullptr, "llmprofile" },  // idx 17 (diagnostic; dumps a breakdown after each generation)
};

extern const SettingsModule llmSettingsModule = {
  "llm", "apps.llm", llmSettingEntries,
  sizeof(llmSettingEntries) / sizeof(llmSettingEntries[0]),
  nullptr,
  "On-device LLM generation defaults"
};

// Setting command handlers — one per entry, resolved by cmdKey.
//
// Each handler looks its row up by the same cmdKey string the CommandEntry below
// registers, so the command name and the setting it writes are stated once and
// cannot drift apart. The previous form indexed llmSettingEntries[] by position,
// which made row order load-bearing across ~60 lines of unrelated table; two rows
// added at the top left all 17 setters writing their neighbour's field.
//
// Searches this module's own table rather than findSettingByCmdKey() (which walks
// every registered module): it is smaller, has no dependency on settings-module
// registration order, and keeps the LLM surface self-contained. A cmdKey that
// matches no row returns a clear error instead of silently writing another field.
static const SettingEntry* llmSettingByCmdKey(const char* cmdKey) {
  for (const SettingEntry& e : llmSettingEntries) {
    if (e.cmdKey && strcasecmp(e.cmdKey, cmdKey) == 0) return &e;
  }
  return nullptr;
}

#define LLM_SETTING_CMD(funcName, cmdKeyLit) \
  static const char* funcName(const String& a) { \
    const SettingEntry* e = llmSettingByCmdKey(cmdKeyLit); \
    if (!e) return "Error: no LLM setting registered for this command"; \
    return handleSettingCommand(e, a); \
  }

LLM_SETTING_CMD(cmd_llm_temperature,   "llmtemperature")
LLM_SETTING_CMD(cmd_llm_topp,          "llmtopp")
LLM_SETTING_CMD(cmd_llm_maxtokens,     "llmmaxtokens")
LLM_SETTING_CMD(cmd_llm_sentencelimit, "llmsentencelimit")
LLM_SETTING_CMD(cmd_llm_hardcap,       "llmhardcap")
LLM_SETTING_CMD(cmd_llm_reppenalty,    "llmreppenalty")
LLM_SETTING_CMD(cmd_llm_repwindow,     "llmrepwindow")
LLM_SETTING_CMD(cmd_llm_maxcontext,    "llmmaxcontext")
LLM_SETTING_CMD(cmd_llm_defaultmodel,  "llmdefaultmodel")
LLM_SETTING_CMD(cmd_llm_minp,          "llmminp")
LLM_SETTING_CMD(cmd_llm_kvprec,        "llmkvprec")
LLM_SETTING_CMD(cmd_llm_autostart,     "llmautostart")
LLM_SETTING_CMD(cmd_llm_norepeatngram, "llmnorepeatngram")
LLM_SETTING_CMD(cmd_llm_confthreshold, "llmconfthreshold")
LLM_SETTING_CMD(cmd_llm_contentboost,  "llmcontentboost")
LLM_SETTING_CMD(cmd_llm_domaingate,    "llmdomaingate")
LLM_SETTING_CMD(cmd_llm_profile,       "llmprofile")

const CommandEntry llmCommands[] = {
  { "llmstatus",        "Show LLM engine status (add 'json' for JSON output)",               false, cmd_llm_status },
  { "llmload",          "Load model [model.bin]",               true,  cmd_llm_load,         "Usage: llmload [filename.bin]" },
  { "llmunload",        "Unload model and free PSRAM",          true,  cmd_llm_unload },
  { "llmautostart",     "Auto-load default model at boot (0|1)", true,  cmd_llm_autostart,    "Usage: llmautostart <0|1>" },
  { "llmmodels",        "List available model files (add 'json' for JSON output)",           false, cmd_llm_models },
  { "llmgenerate",      "Ask the loaded model a question",      false, cmd_llm_generate,     "Usage: llmgenerate <question>  |  llmgenerate do: <intent>\nThe question is wrapped in the model's Q:/A: format for you, same as the web chat. Prefix 'do:' to ask for a command instead of an answer. A prompt that already starts with 'Q:' is sent as-is." },
  { "llmresult",        "Poll streamed generation (JSON)",      false, cmd_llm_result,       "Usage: llmresult json <offset>" },
  { "llmstop",          "Stop in-progress generation",          false, cmd_llm_stop },
  { "llmcorrupttest",   "Debug: force corruption-recovery test", true, cmdLlmCorruptRow },
  { "llmclear",         "Reset the LLM conversation",           false, cmd_llm_clear },
  { "llmretry",         "Regenerate the last reply (JSON)",     false, cmd_llm_retry },
  { "llmturns",         "Read a conversation turn (JSON)",      false, cmd_llm_turns,        "Usage: llmturns json <index>" },
  { "llmmenu",          "Guided-input menu status/listing (add 'json')",  false, cmdLlmMenuRow, "Usage: llmmenu  |  llmmenu json  |  llmmenu json tpl <g> <off>  |  llmmenu json ent <g> <off>\nLists this model's guided question templates + entity rosters (empty if the model ships none)." },
  { "llmask",           "Ask a guided question by index",       false, cmdLlmAskRow,          "Usage: llmask <group> <template> [entity]  |  llmask json <gen> <g> <t> [e]  |  llmask repeat\nComposes the question on-device from 'llmmenu' indices and streams the answer (read it with 'llmresult json 0')." },
  { "llmtemperature",   "Set default sampling temperature",     true,  cmd_llm_temperature,  "Usage: llmtemperature <0.0-2.0>" },
  { "llmtopp",          "Set default Top-P threshold",          true,  cmd_llm_topp,         "Usage: llmtopp <0.0-1.0>" },
  { "llmmaxtokens",     "Set default max tokens per reply",     true,  cmd_llm_maxtokens,    "Usage: llmmaxtokens <1-512>" },
  { "llmsentencelimit", "Set default sentence stop limit",      true,  cmd_llm_sentencelimit,"Usage: llmsentencelimit <0-20>" },
  { "llmhardcap",       "Set default hard token cap",           true,  cmd_llm_hardcap,      "Usage: llmhardcap <0-512>" },
  { "llmreppenalty",    "Set default repetition penalty",       true,  cmd_llm_reppenalty,   "Usage: llmreppenalty <1.0-3.0>" },
  { "llmrepwindow",     "Set default rep-penalty look-back",    true,  cmd_llm_repwindow,    "Usage: llmrepwindow <1-32>" },
  { "llmmaxcontext",    "Set KV cache context window (0=auto)", true,  cmd_llm_maxcontext,   "Usage: llmmaxcontext <0-4096>" },
  { "llmdefaultmodel",  "Set default model filename",           true,  cmd_llm_defaultmodel, "Usage: llmdefaultmodel <filename.bin>" },
  { "llmminp",          "Set min-p sampling floor (0=off)",     true,  cmd_llm_minp,         "Usage: llmminp <0.0-1.0>" },
  { "llmkvprec",        "KV cache precision (0=FP32,1=FP16,2=INT8)", true,  cmd_llm_kvprec,       "Usage: llmkvprec <0..2>  (0=FP32,1=FP16,2=INT8; reload model to apply)" },
  { "llmnorepeatngram", "Ban repeating generated n-grams (0=off)", true, cmd_llm_norepeatngram, "Usage: llmnorepeatngram <0-8>  (default 0=off; 3 breaks verbatim phrase loops)" },
  { "llmconfthreshold", "Low-confidence hedge threshold (0=off)",  true, cmd_llm_confthreshold, "Usage: llmconfthreshold <-8.0-0>  (mean logprob; default -1.0, 0 disables)" },
  { "llmcontentboost",  "On-topic logit bonus (0=off)",         true,  cmd_llm_contentboost, "Usage: llmcontentboost <0.0-4.0>  (default 1.5; higher = stickier to prompt words)" },
  { "llmdomaingate",    "Refuse prompts outside the model's domain (0|1)", true, cmd_llm_domaingate, "Usage: llmdomaingate <0|1>  (only enforced when the model .bin carries a domain vocab)" },
  { "llmprofile",       "Per-section forward-pass timing breakdown (0|1)",  true, cmd_llm_profile,    "Usage: llmprofile <0|1>  (diagnostic; splits qkv/attn/ffn/cls after each generation. Turn OFF other debugllm* flags for clean numbers)" },
};
const size_t llmCommandsCount = sizeof(llmCommands) / sizeof(llmCommands[0]);


#endif // ENABLE_LLM_BACKEND
