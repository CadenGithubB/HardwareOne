// System_LLMChat.cpp — chat conversation owner + engine demuxer
//
// See System_LLMChat.h for the why and the threading model. This file is
// the only place the conversation actually lives — both web and OLED render
// from here.

#include "System_LLMChat.h"

#if ENABLE_ONDEVICE_LLM

#include <Arduino.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "System_Debug.h"
#include "System_MemUtil.h"   // ps_alloc / ps_free
#include "System_Settings.h"

// ============================================================================
// Tunables
// ============================================================================
#define LLM_CHAT_TURN_INIT_BYTES 256   // Initial PSRAM allocation per turn
                                       // (grows by 2× on overflow up to MAX_BYTES)

// Suppress-token tokenizer scratch — tokens harvested from the prior assistant
// turn for chatRetryLast. 64 is enough to cover one assistant response.
#define LLM_CHAT_SUPPRESS_SCRATCH 64

// ============================================================================
// Internal state
// ============================================================================

struct InternalTurn {
  ChatTurnRole role;
  uint32_t     textLen;
  uint32_t     textCap;
  char*        text;              // ps_alloc'd; nullptr when slot is empty
  uint32_t     tokenCount;        // ASSISTANT only
  uint16_t     tokensPerSecX10;   // ASSISTANT only
};

static InternalTurn sTurns[LLM_CHAT_MAX_TURNS];
static uint8_t sTurnHead  = 0;    // index of OLDEST valid turn (ring start)
static uint8_t sTurnCount = 0;    // 0..LLM_CHAT_MAX_TURNS

// Live streaming state — set by chatBeginTurn, cleared when engine reports
// done. Streaming turn is always the newest assistant turn.
static int  sStreamingTurnSlot = -1;  // -1 = nothing streaming
static int  sStreamingSessionId = 0;
static int  sEngineOffsetDrained = 0; // bytes copied from engine into the turn

// Remember the most recently FINISHED assistant turn so a client whose first
// poll arrives after the turn was already drained + finalized can still fetch it.
// This is what makes an instant-completing generation (e.g. a gate refusal that
// returns in microseconds, before the browser's first poll) actually reach the
// web/OLED live view instead of blanking. Invalidated when a new turn begins.
static int  sLastFinishedSession = 0;
static int  sLastFinishedSlot    = -1;

static SemaphoreHandle_t sChatMutex = nullptr;

// ============================================================================
// Mutex helpers
// ============================================================================

namespace {

struct ChatLock {
  bool held;
  ChatLock() : held(false) {
    if (sChatMutex) {
      held = (xSemaphoreTake(sChatMutex, pdMS_TO_TICKS(200)) == pdTRUE);
    }
  }
  ~ChatLock() { if (held) xSemaphoreGive(sChatMutex); }
};

} // namespace

// ============================================================================
// Ring buffer helpers (assume lock held)
// ============================================================================

static inline int slotForIdx(int logicalIdx) {
  return (sTurnHead + logicalIdx) % LLM_CHAT_MAX_TURNS;
}

static void freeTurnSlot(int slot) {
  InternalTurn& t = sTurns[slot];
  if (t.text) {
    free(t.text);
    t.text = nullptr;
  }
  t.textLen = 0;
  t.textCap = 0;
  t.tokenCount = 0;
  t.tokensPerSecX10 = 0;
}

// Allocate or grow a turn's text buffer so it can hold `needed` bytes + NUL.
// Returns true on success, false if allocation failed OR cap reached.
static bool ensureTurnCapacity(InternalTurn& t, uint32_t needed) {
  if (needed + 1 <= t.textCap) return true;
  if (needed + 1 > LLM_CHAT_TURN_MAX_BYTES) {
    // Reached hard cap; pre-allocate to cap if not already there.
    if (t.textCap >= LLM_CHAT_TURN_MAX_BYTES) return false;
    uint32_t newCap = LLM_CHAT_TURN_MAX_BYTES;
    char* newBuf = (char*)ps_realloc(t.text, newCap,
                                     AllocPref::PreferPSRAM);
    if (!newBuf) return false;
    t.text = newBuf;
    t.textCap = newCap;
    return needed + 1 <= newCap;
  }
  uint32_t newCap = t.textCap > 0 ? t.textCap : LLM_CHAT_TURN_INIT_BYTES;
  while (newCap < needed + 1) newCap *= 2;
  if (newCap > LLM_CHAT_TURN_MAX_BYTES) newCap = LLM_CHAT_TURN_MAX_BYTES;
  char* newBuf = (char*)ps_realloc(t.text, newCap, AllocPref::PreferPSRAM);
  if (!newBuf) return false;
  t.text = newBuf;
  t.textCap = newCap;
  return needed + 1 <= newCap;
}

// Append a turn, evicting the oldest if the ring is full. Returns the slot
// index of the newly-appended turn. Allocates an initial PSRAM buffer for
// the text body.
static int appendTurnLocked(ChatTurnRole role, const char* text, uint32_t textLen) {
  if (sTurnCount == LLM_CHAT_MAX_TURNS) {
    // Evict oldest. If it's the streaming turn (shouldn't be — streaming is
    // always newest), clear the streaming pointer too.
    int oldSlot = sTurnHead;
    if (sStreamingTurnSlot == oldSlot) sStreamingTurnSlot = -1;
    freeTurnSlot(oldSlot);
    sTurnHead = (sTurnHead + 1) % LLM_CHAT_MAX_TURNS;
    sTurnCount--;
  }
  int newSlot = (sTurnHead + sTurnCount) % LLM_CHAT_MAX_TURNS;
  InternalTurn& t = sTurns[newSlot];
  // Reset (in case slot was previously used)
  freeTurnSlot(newSlot);
  t.role = role;
  // Pre-allocate the initial buffer regardless of textLen so streaming
  // assistant turns (textLen=0 at start) get their buffer up front.
  uint32_t initial = textLen > 0 ? textLen : LLM_CHAT_TURN_INIT_BYTES - 1;
  if (ensureTurnCapacity(t, initial) && text && textLen > 0) {
    memcpy(t.text, text, textLen);
    t.textLen = textLen;
    t.text[textLen] = '\0';
  } else if (t.text) {
    t.text[0] = '\0';
  }
  sTurnCount++;
  return newSlot;
}

// Remove and free the newest turn. Used by retry to discard the previous
// assistant answer before regenerating.
static void popLastTurnLocked() {
  if (sTurnCount == 0) return;
  int lastSlot = (sTurnHead + sTurnCount - 1) % LLM_CHAT_MAX_TURNS;
  if (sStreamingTurnSlot == lastSlot) sStreamingTurnSlot = -1;
  freeTurnSlot(lastSlot);
  sTurnCount--;
}

// ============================================================================
// Engine-to-turn drain (assume lock held)
// ============================================================================

// Pull any new bytes from the engine's result buffer into the current
// streaming turn. Called by all reader functions before they return so
// surfaces see fresh data without a separate poll task.
static void drainEngineLocked() {
  if (sStreamingTurnSlot < 0) return;

  // Detect session drift — if the engine moved on to a different session
  // (or started a brand-new one), the streaming turn is stale.
  if (llmGetSessionId() != sStreamingSessionId) {
    sStreamingTurnSlot = -1;
    sStreamingSessionId = 0;
    sEngineOffsetDrained = 0;
    return;
  }

  InternalTurn& t = sTurns[sStreamingTurnSlot];

  // Bulk-copy any new bytes
  while (true) {
    int engineLen = llmGetResultLen();
    if (engineLen <= sEngineOffsetDrained) break;

    char chunk[256];
    int got = llmGetResultChunk(sEngineOffsetDrained, chunk, sizeof(chunk));
    if (got <= 0) break;

    if (!ensureTurnCapacity(t, t.textLen + got)) {
      // Hit cap; consume from engine to keep the offset advancing but
      // drop bytes that don't fit. Surfaces will show the truncated text.
      sEngineOffsetDrained += got;
      continue;
    }
    memcpy(t.text + t.textLen, chunk, got);
    t.textLen += got;
    t.text[t.textLen] = '\0';
    sEngineOffsetDrained += got;
  }

  // If engine reports done, finalize the turn's metrics.
  if (llmIsGenerationDone()) {
    LLMStatus st = llmGetStatus();
    t.tokenCount      = st.lastTokenCount;
    t.tokensPerSecX10 = (uint16_t)(st.lastTokensPerSec * 10.0f);
    // Snapshot the finished turn so a client that hasn't polled yet can still
    // fetch it (chatReadFinished), even though we clear the streaming slot here.
    sLastFinishedSession = sStreamingSessionId;
    sLastFinishedSlot    = sStreamingTurnSlot;
    sStreamingTurnSlot = -1;
    sStreamingSessionId = 0;
    sEngineOffsetDrained = 0;
  }
}

// ============================================================================
// Lifecycle
// ============================================================================

void chatInit() {
  if (sChatMutex) return;
  sChatMutex = xSemaphoreCreateMutex();
  if (!sChatMutex) {
    ERROR_SYSTEMF("[LLMChat] Failed to create mutex");
    return;
  }
  // Zero-init turn slots
  for (int i = 0; i < LLM_CHAT_MAX_TURNS; i++) {
    sTurns[i] = InternalTurn{};
  }
  sTurnHead = 0;
  sTurnCount = 0;
  sStreamingTurnSlot = -1;
  sStreamingSessionId = 0;
  sEngineOffsetDrained = 0;
}

// ============================================================================
// Parameter resolution
// ============================================================================

void chatParamOverrideFromJson(JsonObjectConst d, ChatParamOverride& ov) {
  if (d["max_tokens"].is<int>())     ov.maxTokens     = d["max_tokens"].as<int>();
  if (d["temperature"].is<float>())  ov.temperature   = d["temperature"].as<float>();
  if (d["top_p"].is<float>())        ov.topp          = d["top_p"].as<float>();
  if (d["min_p"].is<float>())        ov.minP          = d["min_p"].as<float>();
  if (d["rep_penalty"].is<float>())  ov.repPenalty    = d["rep_penalty"].as<float>();
  if (d["rep_window"].is<int>())     ov.repWindow     = d["rep_window"].as<int>();
  if (d["sentence_limit"].is<int>()) ov.sentenceLimit = d["sentence_limit"].as<int>();
  if (d["hard_cap"].is<int>())       ov.hardCap       = d["hard_cap"].as<int>();
}

void chatResolveParams(const ChatParamOverride& o, LLMGenParams* out) {
  if (!out) return;

  // Resolve from override → settings → engine default
  out->maxTokens    = (o.maxTokens     != INT32_MIN) ? o.maxTokens     : gSettings.llmMaxTokens;
  out->temperature  = !isnan(o.temperature)          ? o.temperature   : gSettings.llmTemperature;
  out->topp         = !isnan(o.topp)                 ? o.topp          : gSettings.llmTopP;
  out->minP         = !isnan(o.minP)                 ? o.minP          : gSettings.llmMinP;  // 0 = off → use top-p
  out->repPenalty   = !isnan(o.repPenalty)           ? o.repPenalty    : gSettings.llmRepPenalty;
  out->repWindow    = (o.repWindow     != INT32_MIN) ? o.repWindow     : gSettings.llmRepWindow;
  out->sentenceLimit= (o.sentenceLimit != INT32_MIN) ? o.sentenceLimit : gSettings.llmSentenceLimit;
  out->hardCap      = (o.hardCap       != INT32_MIN) ? o.hardCap       : gSettings.llmHardCap;

  // Clamps (same as the prior WebPage_LLM logic — single source of truth now)
  if (out->maxTokens    <   1) out->maxTokens    = 1;
  if (out->maxTokens    > 512) out->maxTokens    = 512;
  if (out->temperature  < 0.0f)  out->temperature  = 0.0f;
  if (out->temperature  > 2.0f)  out->temperature  = 2.0f;
  if (out->topp         < 0.01f) out->topp         = 0.01f;
  if (out->topp         > 1.0f)  out->topp         = 1.0f;
  if (out->minP         < 0.0f)  out->minP         = 0.0f;
  if (out->minP         > 1.0f)  out->minP         = 1.0f;
  if (out->repPenalty   < 1.0f)  out->repPenalty   = 1.0f;
  if (out->repPenalty   > 5.0f)  out->repPenalty   = 5.0f;
  if (out->repWindow    <   0)   out->repWindow    = 0;
  if (out->repWindow    >  32)   out->repWindow    = 32;  // ring buffer is LLM_DEFAULT_REP_WINDOW; larger was silently truncated
  if (out->sentenceLimit<   0)   out->sentenceLimit= 0;
  if (out->sentenceLimit>  20)   out->sentenceLimit= 20;
  if (out->hardCap      <   0)   out->hardCap      = 0;
  if (out->hardCap      > 512)   out->hardCap      = 512;

  out->suppressCount = 0;  // Retry path fills this; default path leaves empty.
}

// ============================================================================
// Mutating operations
// ============================================================================

int chatBeginTurn(const char* userPrompt, const ChatParamOverride* opt) {
  chatInit();
  if (!userPrompt || !*userPrompt) return 0;
  if (!llmIsReady()) return 0;

  ChatLock lk;
  if (!lk.held) return 0;

  // Refuse to start if another generation is in flight.
  if (sStreamingTurnSlot >= 0) {
    // Drain first to check if it's actually still running.
    drainEngineLocked();
    if (sStreamingTurnSlot >= 0) return 0;
  }

  // Append USER turn (full prompt copied into the slot's buffer)
  uint32_t promptLen = strlen(userPrompt);
  appendTurnLocked(ChatTurnRole::USER, userPrompt, promptLen);

  // Append empty ASSISTANT turn — its buffer will be filled as the engine streams.
  int assistantSlot = appendTurnLocked(ChatTurnRole::ASSISTANT, nullptr, 0);

  // Resolve params
  LLMGenParams params;
  chatResolveParams(opt ? *opt : ChatParamOverride{}, &params);

  // Hand off to the engine
  int sid = llmStartAsync(userPrompt, params);
  if (sid <= 0) {
    // Engine refused — roll back the empty assistant turn so the UI doesn't
    // show a stale "..." that never resolves.
    popLastTurnLocked();
    return 0;
  }

  sStreamingTurnSlot   = assistantSlot;
  sStreamingSessionId  = sid;
  sEngineOffsetDrained = 0;
  sLastFinishedSlot    = -1;   // a new turn supersedes any finished-turn snapshot
  sLastFinishedSession = 0;
  return sid;
}

int chatRetryLast(const ChatParamOverride* opt) {
  chatInit();
  if (!llmIsReady()) return 0;

  ChatLock lk;
  if (!lk.held) return 0;

  // Don't allow retry mid-generation — surface should call chatStop first.
  drainEngineLocked();
  if (sStreamingTurnSlot >= 0) return 0;
  if (sTurnCount < 1) return 0;

  // Find the most recent USER turn (walk back from the newest).
  int lastUserSlot = -1;
  int lastAssistantSlot = -1;
  for (int i = sTurnCount - 1; i >= 0; i--) {
    int s = slotForIdx(i);
    if (sTurns[s].role == ChatTurnRole::ASSISTANT && lastAssistantSlot < 0) {
      lastAssistantSlot = s;
    } else if (sTurns[s].role == ChatTurnRole::USER) {
      lastUserSlot = s;
      break;
    }
  }
  if (lastUserSlot < 0) return 0;

  // Snapshot the prompt before mutations so the pointer stays valid. PSRAM-resident
  // (the snapshot is required for pointer stability across the turn-ring pop/append
  // below; only its placement moved off scarce internal DRAM).
  EXT_RAM_BSS_ATTR static char promptBuf[1024];
  uint32_t plen = sTurns[lastUserSlot].textLen;
  if (plen >= sizeof(promptBuf)) plen = sizeof(promptBuf) - 1;
  memcpy(promptBuf, sTurns[lastUserSlot].text, plen);
  promptBuf[plen] = '\0';

  // Resolve params, then tokenize the prior assistant answer for suppress.
  LLMGenParams params;
  chatResolveParams(opt ? *opt : ChatParamOverride{}, &params);

  if (lastAssistantSlot >= 0 && sTurns[lastAssistantSlot].text &&
      sTurns[lastAssistantSlot].textLen > 0) {
    int scratch[LLM_CHAT_SUPPRESS_SCRATCH];
    int n = llmTokenize(sTurns[lastAssistantSlot].text, scratch,
                        LLM_CHAT_SUPPRESS_SCRATCH);
    for (int i = 0; i < n && params.suppressCount < 128; i++) {
      int tok = scratch[i];
      // Dedupe — repeated tokens in the answer would otherwise eat slots.
      bool dup = false;
      for (int j = 0; j < params.suppressCount; j++) {
        if (params.suppressTokens[j] == tok) { dup = true; break; }
      }
      if (!dup) params.suppressTokens[params.suppressCount++] = tok;
    }
  }

  // Remove the previous assistant turn (we're regenerating it).
  if (lastAssistantSlot >= 0) {
    // Only pop if it was the LAST turn — if there's content after it (shouldn't
    // be possible, but defensively), don't disturb the order.
    int lastSlot = (sTurnHead + sTurnCount - 1) % LLM_CHAT_MAX_TURNS;
    if (lastAssistantSlot == lastSlot) popLastTurnLocked();
  }

  // New assistant turn for this retry attempt.
  int newSlot = appendTurnLocked(ChatTurnRole::ASSISTANT, nullptr, 0);

  int sid = llmStartAsync(promptBuf, params);
  if (sid <= 0) {
    popLastTurnLocked();
    return 0;
  }

  sStreamingTurnSlot   = newSlot;
  sStreamingSessionId  = sid;
  sEngineOffsetDrained = 0;
  return sid;
}

void chatStop() {
  llmStop();
  // The drain on next read will see llmIsGenerationDone() == true and
  // finalize the current assistant turn (keeping whatever was generated).
}

bool chatClear() {
  ChatLock lk;
  if (!lk.held) return false;
  // Refuse mid-generation — engine state could get out of sync.
  drainEngineLocked();
  if (sStreamingTurnSlot >= 0) return false;

  for (int i = 0; i < LLM_CHAT_MAX_TURNS; i++) freeTurnSlot(i);
  sTurnHead = 0;
  sTurnCount = 0;
  sStreamingTurnSlot = -1;
  sStreamingSessionId = 0;
  sEngineOffsetDrained = 0;
  return true;
}

// ============================================================================
// Reader API
// ============================================================================

int chatGetTurnCount() {
  ChatLock lk;
  if (!lk.held) return 0;
  drainEngineLocked();
  return sTurnCount;
}

bool chatGetTurnInfo(int index, ChatTurnInfo* out) {
  if (!out) return false;
  ChatLock lk;
  if (!lk.held) return false;
  drainEngineLocked();
  if (index < 0) index = sTurnCount - 1;
  if (index < 0 || index >= sTurnCount) return false;
  int slot = slotForIdx(index);
  const InternalTurn& t = sTurns[slot];
  out->role            = t.role;
  out->textLen         = t.textLen;
  out->tokenCount      = t.tokenCount;
  out->tokensPerSecX10 = t.tokensPerSecX10;
  out->isStreaming     = (slot == sStreamingTurnSlot);
  return true;
}

int chatReadTurn(int index, int offset, char* buf, int maxLen) {
  if (!buf || maxLen <= 0) return 0;
  buf[0] = '\0';
  ChatLock lk;
  if (!lk.held) return 0;
  drainEngineLocked();
  if (index < 0) index = sTurnCount - 1;
  if (index < 0 || index >= sTurnCount) return 0;
  int slot = slotForIdx(index);
  const InternalTurn& t = sTurns[slot];
  if (!t.text || offset < 0 || (uint32_t)offset >= t.textLen) return 0;
  int avail = (int)t.textLen - offset;
  int copy = avail < (maxLen - 1) ? avail : (maxLen - 1);
  memcpy(buf, t.text + offset, copy);
  buf[copy] = '\0';
  return copy;
}

int chatGetStreamLen() {
  ChatLock lk;
  if (!lk.held) return 0;
  drainEngineLocked();
  if (sStreamingTurnSlot < 0) return 0;
  return (int)sTurns[sStreamingTurnSlot].textLen;
}

// Read the most recently FINISHED assistant turn for `session`. Recovers a result
// that completed (and was finalized) before the client's first poll — e.g. an
// instant gate refusal, or a turn another surface's poll already drained.
// Returns 0 unless `session` is that last-finished turn.
int chatReadFinished(int session, int offset, char* buf, int maxLen) {
  if (!buf || maxLen <= 0) return 0;
  buf[0] = '\0';
  ChatLock lk;
  if (!lk.held) return 0;
  if (session == 0 || session != sLastFinishedSession || sLastFinishedSlot < 0) return 0;
  const InternalTurn& t = sTurns[sLastFinishedSlot];
  if (!t.text || offset < 0 || (uint32_t)offset >= t.textLen) return 0;
  int avail = (int)t.textLen - offset;
  int copy = avail < (maxLen - 1) ? avail : (maxLen - 1);
  memcpy(buf, t.text + offset, copy);
  buf[copy] = '\0';
  return copy;
}

int chatFinishedLen(int session) {
  ChatLock lk;
  if (!lk.held) return 0;
  if (session == 0 || session != sLastFinishedSession || sLastFinishedSlot < 0) return 0;
  return (int)sTurns[sLastFinishedSlot].textLen;
}

int chatReadStream(int offset, char* buf, int maxLen) {
  if (!buf || maxLen <= 0) return 0;
  buf[0] = '\0';
  ChatLock lk;
  if (!lk.held) return 0;
  drainEngineLocked();
  if (sStreamingTurnSlot < 0) return 0;
  const InternalTurn& t = sTurns[sStreamingTurnSlot];
  if (!t.text || offset < 0 || (uint32_t)offset >= t.textLen) return 0;
  int avail = (int)t.textLen - offset;
  int copy = avail < (maxLen - 1) ? avail : (maxLen - 1);
  memcpy(buf, t.text + offset, copy);
  buf[copy] = '\0';
  return copy;
}

bool chatIsGenerating() {
  ChatLock lk;
  if (!lk.held) return false;
  drainEngineLocked();
  return sStreamingTurnSlot >= 0;
}

int chatGetSessionId() {
  ChatLock lk;
  if (!lk.held) return 0;
  return sStreamingSessionId;
}

#endif // ENABLE_ONDEVICE_LLM
