// System_LLMBackend.cpp — model registry and source dispatch.
// Design rationale lives in System_LLMBackend.h.

#include "System_LLMBackend.h"

#if ENABLE_LLM_BACKEND

#include <string.h>
#include <strings.h>            // strncasecmp

#include "System_Debug.h"
#include "System_Filesystem.h"
#include "System_Mutex.h"        // FsLockGuard
#include "System_Settings.h"
#include "System_VFS.h"
#include <esp_attr.h>  // EXT_RAM_BSS_ATTR

#if ENABLE_LLM_SOURCE_ONBOARD
  #include "System_LLM.h"
#endif
#if ENABLE_LLM_SOURCE_CM5
  #include "System_LLMCm5.h"
#endif

// ============================================================================
// Active selection
// ============================================================================
// Which source owns the current model. Written only by select/unload, read on
// every poll; a plain enum store is atomic on this target and the sources keep
// their own internal locks, so no extra mutex is warranted here.

static LlmBackendKind sActiveKind = LlmBackendKind::None;
EXT_RAM_BSS_ATTR static LlmModelDesc sActiveDesc;  // PSRAM: task-context select/unload/read only (initializer dropped: .ext_ram.bss is zeroed)

const char* llmBackendKindName(LlmBackendKind k) {
  switch (k) {
    case LlmBackendKind::Onboard: return "onboard";
    case LlmBackendKind::Cm5:     return "cm5";
    case LlmBackendKind::None:    break;
  }
  return "none";
}

// ============================================================================
// Onboard enumeration
// ============================================================================
// A struct-shaped rescan rather than a call into llmListModels(), which builds
// an Arduino String of JSON. Surfaces want rows, and two of them (G2, OLED)
// cannot afford the allocation on their tap paths — which is exactly why they
// each grew a private duplicate of this scan in the first place.

#if ENABLE_LLM_SOURCE_ONBOARD
static size_t onboardEnumerate(LlmModelDesc* out, size_t cap) {
  if (!out || cap == 0) return 0;
  FsLockGuard fsGuard("llm.enumerate");
  size_t w = 0;

  auto scanDir = [&](const char* dirPath, uint8_t storage) {
    if (w >= cap) return;
    File dir = VFS::openGuarded(dirPath, FILE_READ, VFS::systemAuth("llm.enumerate"));
    if (!dir || !dir.isDirectory()) return;
    File entry;
    while (w < cap && (entry = dir.openNextFile())) {
      const char* raw = entry.name();
      if (!raw || !raw[0]) { entry.close(); continue; }
      const char* base = strrchr(raw, '/');
      base = base ? base + 1 : raw;
      const size_t len = strlen(base);
      if (len >= 4 && strcasecmp(base + len - 4, ".bin") == 0) {
        LlmModelDesc& d = out[w];
        memset(&d, 0, sizeof(d));
        snprintf(d.id, sizeof(d.id), "onboard:%s", base);
        strlcpy(d.name, base, sizeof(d.name));
        d.backend = LlmBackendKind::Onboard;
        snprintf(d.path, sizeof(d.path), "%s/%s", dirPath, base);
        d.sizeKB  = (uint32_t)(entry.size() / 1024);
        d.storage = storage;
        d.available = true;    // a file on disk is always selectable
        w++;
      }
      entry.close();
    }
    dir.close();
  };

  scanDir("/system/llm", LLM_STORAGE_INTERNAL);
  if (VFS::isSDAvailable()) scanDir("/sd/llm", LLM_STORAGE_SD);
  return w;
}
#endif  // ENABLE_LLM_SOURCE_ONBOARD

// ============================================================================
// Registry
// ============================================================================

size_t llmEnumerateModels(LlmModelDesc* out, size_t cap) {
  if (!out || cap == 0) return 0;
  // Runtime master switch. Off means every picker is empty rather than every
  // picker being full of models that would refuse to load.
  if (!gSettings.llmEnabled) return 0;

  size_t w = 0;
#if ENABLE_LLM_SOURCE_ONBOARD
  w += onboardEnumerate(out + w, cap - w);
#endif
#if ENABLE_LLM_SOURCE_CM5
  if (w < cap) w += cm5LlmEnumerate(out + w, cap - w);
#endif
  return w;
}

#if ENABLE_LLM_SOURCE_ONBOARD
// Targeted lookup of one filename in the two model directories. Deliberately
// NOT "enumerate everything then filter": a full scan would need a
// 156 B/row array, and this runs on the cmd_exec and httpd stacks.
static bool onboardFindByName(const char* name, LlmModelDesc* out) {
  if (!name || !*name) return false;
  FsLockGuard fsGuard("llm.resolve");
  const char* dirs[2] = { "/system/llm", nullptr };
  uint8_t     stor[2] = { LLM_STORAGE_INTERNAL, LLM_STORAGE_SD };
  if (VFS::isSDAvailable()) dirs[1] = "/sd/llm";

  for (int i = 0; i < 2; i++) {
    if (!dirs[i]) continue;
    char full[LLM_MODEL_PATH_LEN];
    snprintf(full, sizeof(full), "%s/%s", dirs[i], name);
    File f = VFS::openGuarded(full, FILE_READ, VFS::systemAuth("llm.resolve"));
    if (!f || f.isDirectory()) { if (f) f.close(); continue; }
    memset(out, 0, sizeof(*out));
    snprintf(out->id, sizeof(out->id), "onboard:%s", name);
    strlcpy(out->name, name, sizeof(out->name));
    out->backend   = LlmBackendKind::Onboard;
    strlcpy(out->path, full, sizeof(out->path));
    out->sizeKB    = (uint32_t)(f.size() / 1024);
    out->storage   = stor[i];
    out->available = true;
    f.close();
    return true;
  }
  return false;
}
#endif

bool llmResolveModelId(const char* id, LlmModelDesc* out) {
  if (!id || !*id || !out) return false;
  if (!gSettings.llmEnabled) return false;

  const char* colon = strchr(id, ':');
  if (colon) {
    const size_t pfx = (size_t)(colon - id);
    const char* name = colon + 1;
    if (!*name) return false;
#if ENABLE_LLM_SOURCE_ONBOARD
    if (pfx == 7 && strncasecmp(id, "onboard", 7) == 0)
      return onboardFindByName(name, out);
#endif
#if ENABLE_LLM_SOURCE_CM5
    if (pfx == 3 && strncasecmp(id, "cm5", 3) == 0)
      return cm5LlmFindByName(name, out);
#endif
    return false;   // a prefix this build does not have
  }

  // No prefix: a bare filename as typed on the CLI, or an absolute path as
  // stored by a llmDefaultModel that predates ids. Both mean the onboard
  // source — remote models are only ever addressed by their full id.
#if ENABLE_LLM_SOURCE_ONBOARD
  {
    const char* base = strrchr(id, '/');
    base = base ? base + 1 : id;
    return onboardFindByName(base, out);
  }
#else
  return false;
#endif
}

// ============================================================================
// Selection
// ============================================================================

bool llmBackendSelect(const char* id, char* errOut, size_t errCap, int maxCtx) {
  auto bail = [&](const char* m) {
    if (errOut && errCap) strlcpy(errOut, m, errCap);
    return false;
  };
  if (!id || !*id) return bail("no model given");
  if (!gSettings.llmEnabled) return bail("the LLM is disabled (llmenabled 0)");

  LlmModelDesc d;
  if (!llmResolveModelId(id, &d)) return bail("no such model");
  if (!d.available)               return bail("that model is not available right now");

  switch (d.backend) {
#if ENABLE_LLM_SOURCE_ONBOARD
    case LlmBackendKind::Onboard: {
      // Blocking: reads weights into PSRAM, seconds. Callers are warned in the
      // header not to assume READY on return, which stays true for the remote
      // case below even though this one happens to be synchronous.
      if (!llmLoadModel(d.path, maxCtx)) return bail("model failed to load");
      // Descriptor BEFORE kind. Readers dispatch on sActiveKind and then read
      // sActiveDesc, so publishing the kind first opens a window where the new
      // kind is paired with the previous descriptor. This narrows that window
      // to nothing a single reader can observe in practice; it does not close
      // it, because neither store is atomic and there is no barrier here. A
      // real fix needs the pair under one lock.
      sActiveDesc = d;
      sActiveKind = LlmBackendKind::Onboard;
      return true;
    }
#endif
#if ENABLE_LLM_SOURCE_CM5
    case LlmBackendKind::Cm5: {
      char err[64] = {0};
      if (!cm5LlmSelectByName(d.name, err, sizeof(err)))
        return bail(err[0] ? err : "the CM5 refused the model");
      sActiveDesc = d;                      // descriptor before kind — see above
      sActiveKind = LlmBackendKind::Cm5;
      return true;
    }
#endif
    default: break;
  }
  return bail("that source is not compiled into this build");
}

void llmBackendUnload() {
  switch (sActiveKind) {
#if ENABLE_LLM_SOURCE_ONBOARD
    case LlmBackendKind::Onboard: llmUnload();    break;
#endif
#if ENABLE_LLM_SOURCE_CM5
    case LlmBackendKind::Cm5:     cm5LlmUnload(); break;
#endif
    default: break;
  }
  sActiveKind = LlmBackendKind::None;
  memset(&sActiveDesc, 0, sizeof(sActiveDesc));
}

LlmBackendKind llmBackendActiveKind() { return sActiveKind; }

bool llmBackendActiveModel(LlmModelDesc* out) {
  if (!out || sActiveKind == LlmBackendKind::None) return false;
  *out = sActiveDesc;

#if ENABLE_LLM_SOURCE_CM5
  // A remote host is AUTHORITATIVE about what it is actually serving. If it
  // answered a select for "ghost" by loading something else and saying so via
  // `cm5 llm ready <gen> <name>`, the cached descriptor here is the model the
  // USER asked for, not the one that exists — so re-derive from the source.
  //
  // Without this the picker would snap back correctly (every surface reads
  // llmBackendStatus() -> cm5LlmStatus(), which the host updates) while this
  // accessor kept reporting the ghost forever. Nothing consumes it yet, so it
  // was a trap set for whichever surface used it first rather than a live bug.
  if (sActiveKind == LlmBackendKind::Cm5) {
    const LLMStatus st = cm5LlmStatus();
    const char* colon = strchr(st.modelPath, ':');
    if (colon && colon[1]) {
      strlcpy(out->id, st.modelPath, sizeof(out->id));
      strlcpy(out->name, colon + 1, sizeof(out->name));
    }
  }
#endif
  return true;
}

// ============================================================================
// Generation dispatch
// ============================================================================
// Every one of these was previously a direct call to the engine from the chat
// layer. Routing them through the active source is what lets a turn be answered
// by something that is not the on-device engine.

#if ENABLE_LLM_SOURCE_ONBOARD
  #define LLM_ONBOARD_CASE(expr) case LlmBackendKind::Onboard: return (expr);
  #define LLM_ONBOARD_VOID(expr) case LlmBackendKind::Onboard: (expr); return;
#else
  #define LLM_ONBOARD_CASE(expr)
  #define LLM_ONBOARD_VOID(expr)
#endif
#if ENABLE_LLM_SOURCE_CM5
  #define LLM_CM5_CASE(expr)     case LlmBackendKind::Cm5: return (expr);
  #define LLM_CM5_VOID(expr)     case LlmBackendKind::Cm5: (expr); return;
#else
  #define LLM_CM5_CASE(expr)
  #define LLM_CM5_VOID(expr)
#endif

bool llmBackendIsReady() {
  if (!gSettings.llmEnabled) return false;
  switch (sActiveKind) {
    LLM_ONBOARD_CASE(llmIsReady())
    LLM_CM5_CASE(cm5LlmIsReady())
    default: return false;
  }
}

LLMStatus llmBackendStatus() {
  switch (sActiveKind) {
    LLM_ONBOARD_CASE(llmGetStatus())
    LLM_CM5_CASE(cm5LlmStatus())
    default: break;
  }
  LLMStatus st;
  memset(&st, 0, sizeof(st));
  st.state = LLMState::UNLOADED;
  return st;
}

int llmBackendStartAsync(const char* prompt, const LLMGenParams& params) {
  switch (sActiveKind) {
    LLM_ONBOARD_CASE(llmStartAsync(prompt, params))
    LLM_CM5_CASE(cm5LlmStartAsync(prompt, params))
    default: return 0;
  }
}

void llmBackendStop() {
  switch (sActiveKind) {
    LLM_ONBOARD_VOID(llmStop())
    LLM_CM5_VOID(cm5LlmStop())
    default: return;
  }
}

int llmBackendSessionId() {
  switch (sActiveKind) {
    LLM_ONBOARD_CASE(llmGetSessionId())
    LLM_CM5_CASE(cm5LlmSessionId())
    default: return 0;
  }
}

int llmBackendResultLen() {
  switch (sActiveKind) {
    LLM_ONBOARD_CASE(llmGetResultLen())
    LLM_CM5_CASE(cm5LlmResultLen())
    default: return 0;
  }
}

int llmBackendResultChunk(int offset, char* buf, int maxLen) {
  switch (sActiveKind) {
    LLM_ONBOARD_CASE(llmGetResultChunk(offset, buf, maxLen))
    LLM_CM5_CASE(cm5LlmResultChunk(offset, buf, maxLen))
    default: return 0;
  }
}

bool llmBackendIsDone() {
  switch (sActiveKind) {
    LLM_ONBOARD_CASE(llmIsGenerationDone())
    LLM_CM5_CASE(cm5LlmIsDone())
    // Nothing selected means nothing is running. Reporting "done" here rather
    // than "running" matters: the chat drain finalizes on this, and the
    // opposite answer would strand a streaming turn forever.
    default: return true;
  }
}

const char* llmBackendFramePrompt(const char* userText, char* out, size_t outSize) {
  switch (sActiveKind) {
#if ENABLE_LLM_SOURCE_ONBOARD
    case LlmBackendKind::Onboard:
      return llmFramePrompt(userText, out, outSize);
#endif
    default:
      // Remote servers apply their own chat template and system prompt, so the
      // local "Q: ...\nA:" scaffolding is deliberately NOT added — it would show
      // up verbatim in the answer. Nothing is added here at all.
      //
      // Do:-mode prompts never reach this point for a remote source: they are
      // refused upstream by llmBackendSupportsCommandMode(). Rewriting one into
      // an instruction was tried and is wrong — see that function.
      return userText;
  }
}

bool llmPromptIsCommandMode(const char* userText) {
  const char* p = userText ? userText : "";
  while (*p == ' ' || *p == '\t') ++p;
  return strncasecmp(p, "do:", 3) == 0;
}

bool llmBackendSupportsCommandMode() {
  // Do:-mode asks the model to emit a Hardware One CLI command, and then the UI
  // puts that answer in a box with a Run button next to it. That only works
  // because the on-device model was TRAINED on this device's command
  // vocabulary — the "Q: <intent>\nDo:" scaffolding is a cue into a corpus it
  // has actually seen.
  //
  // A general instruction-tuned model on the CM5 has never seen any of it. It
  // cannot know the command set, so it does the thing such models always do
  // with an unanswerable request: it invents something plausible. A fabricated
  // command rendered next to a Run button is worse than no feature, so remote
  // sources refuse Do:-mode rather than approximate it.
  //
  // Instructing the remote to "reply with only the Hardware One command" was
  // tried first and is a trap: it improves the FORMAT of the answer without
  // giving the model any way to know the content, which makes a hallucination
  // look more like a real command, not less.
  // The rule is a property of the MODEL, not of the source. A future assistant
  // model running on the CM5 should qualify; the Pokemon model running onboard
  // should not. Source alone answers neither question.
  //
  // The model declares it, in the CAPS section of its own file, and a model that
  // says nothing gets nothing. Note the natural-looking proxies are all wrong:
  // the shipped help agent carries NO guided MENU while the Pokemon and Pop
  // Culture models do, so keying on menu presence would have disabled Do: for
  // the real assistant and handed it to a Pokemon model.
#if ENABLE_LLM_SOURCE_ONBOARD
  if (sActiveKind == LlmBackendKind::Onboard)
    return (llmModelCaps() & LLM_CAP_COMMAND_MODE) != 0;
#endif
  // Remote sources cannot declare this yet: the catalog line carries no
  // capability field. When a CM5-hosted assistant model exists, the host needs a
  // way to advertise it -- an additive trailing field on `cm5 llm models`, or a
  // flag on `ready` -- and this is the one place that has to learn about it.
  return false;
}

int llmBackendTokenize(const char* text, int* outTokens, int maxTokens) {
  switch (sActiveKind) {
    LLM_ONBOARD_CASE(llmTokenize(text, outTokens, maxTokens))
    // No local tokenizer ⇒ 0, which leaves the retry path's suppress list empty
    // instead of feeding a remote model token ids from a foreign vocabulary.
    default: return 0;
  }
}

void llmBackendTick() {
#if ENABLE_LLM_SOURCE_CM5
  cm5LlmTick();

  // Deferred autostart for a REMOTE default model.
  //
  // Boot autostart runs `llmload <llmDefaultModel>` from HardwareOne.cpp long
  // before the CM5 has checked in, so a remote default always failed there with
  // "CM5 service has not checked in" and then never retried — the setting would
  // look broken. A local default is unaffected: the filesystem is up by then, it
  // loads at boot, and this never fires because something is already selected.
  //
  // Latched to ONE attempt per boot: a retry loop would re-select (and restart
  // the host's llama-server) every tick while a model is genuinely unavailable.
  static bool sRemoteAutostartTried = false;
  if (!sRemoteAutostartTried &&
      sActiveKind == LlmBackendKind::None &&
      gSettings.llmEnabled && gSettings.llmAutoStart &&
      gSettings.llmDefaultModel.startsWith("cm5:")) {
    LlmModelDesc d;
    if (llmResolveModelId(gSettings.llmDefaultModel.c_str(), &d) && d.available) {
      sRemoteAutostartTried = true;
      char err[64] = {0};
      const bool ok = llmBackendSelect(gSettings.llmDefaultModel.c_str(), err, sizeof(err));
      DEBUG_UART_CONTROLF("[LLM] deferred remote autostart %s: %s",
                          gSettings.llmDefaultModel.c_str(), ok ? "accepted" : err);
    }
  }
#endif
}


// ============================================================================
// No-op stubs for the local-model API when there is no on-board engine
// ============================================================================
// Declared unconditionally in System_LLMTypes.h so the web / OLED / G2 surfaces
// can call them without knowing which source is active. Each answers "this
// model has none of that", which is the same answer a local .bin without an
// info block or a MENU section already gives — so the surfaces' existing
// "hide the guided UI when groupCount == 0" logic covers the remote case with
// no new branching.

#if !ENABLE_LLM_SOURCE_ONBOARD

bool llmContextDegraded(int* outCtx, int* outModelSeq) {
  if (outCtx) *outCtx = 0;
  if (outModelSeq) *outModelSeq = 0;
  return false;   // a remote host owns its own context window
}

size_t llmContextWarning(char* buf, size_t cap) {
  if (buf && cap) buf[0] = '\0';
  return 0;
}

const char* llmModelDescription() { return ""; }

bool llmModelIcon(const uint8_t** bits, uint8_t* width, uint8_t* height) {
  if (bits) *bits = nullptr;
  if (width) *width = 0;
  if (height) *height = 0;
  return false;   // icons are baked into a local .bin's info block
}

uint16_t llmMenuGeneration(void) { return 0; }
uint8_t  llmMenuGroupCount(void) { return 0; }   // 0 ⇒ every surface hides guided UI
bool     llmMenuGroupInfo(uint8_t, LLMMenuGroupInfo*) { return false; }
int      llmMenuTemplate(uint8_t, uint16_t, char* buf, size_t cap, bool* hasSlot) {
  if (buf && cap) buf[0] = '\0';
  if (hasSlot) *hasSlot = false;
  return -1;
}
int      llmMenuEntity(uint8_t, uint16_t, char* buf, size_t cap) {
  if (buf && cap) buf[0] = '\0';
  return -1;
}
int      llmMenuCompose(uint8_t, uint16_t, int, char* buf, size_t cap) {
  if (buf && cap) buf[0] = '\0';
  return -3;   // "no menu"
}
int      llmMenuAsk(uint16_t, uint8_t, uint16_t, int, const ChatParamOverride*) { return -3; }
int      llmMenuRepeatLast(void) { return -3; }
bool     llmMenuLastAskInfo(int* sessionOut) {
  if (sessionOut) *sessionOut = 0;
  return false;
}

#endif // !ENABLE_LLM_SOURCE_ONBOARD

#endif // ENABLE_LLM_BACKEND
