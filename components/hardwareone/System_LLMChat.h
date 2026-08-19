// System_LLMChat.h — shared LLM conversation state
//
// Why this module exists
//   System_LLM is a pure inference engine: prompt in, tokens out, one-shot.
//   It has no concept of "a conversation" — that lived in browser JS
//   (prevAnswers[]) for the web surface, and in a private 32-line ring in
//   OLED_Mode_LLM for the OLED surface. Two parallel implementations of the
//   same idea, neither aware of the other.
//
//   This module centralizes the conversation: one turn list, one currently-
//   streaming assistant turn, one source of truth for generation parameters
//   (resolved from gSettings.llm* + per-call overrides + clamps). Both the
//   HTTP handlers and the OLED renderer call into this module; the engine
//   only sees one generation in flight at a time, gated by the chat mutex.
//
// Threading
//   All public functions are safe to call from any task. Internal state is
//   protected by sChatMutex. Reader functions (chatGetTurnInfo / chatReadTurn /
//   chatGetStreamLen / chatReadStream) opportunistically drain new bytes from
//   the engine into the current assistant turn before returning — pull-on-read
//   means no separate poll task is required (saves DRAM).
//
// Memory
//   Turn metadata lives in DRAM (~24 B × 16 = 384 B static). Turn text bodies
//   are allocated in PSRAM via ps_alloc, sized 256 B initially and grown by
//   realloc on overflow up to LLM_CHAT_TURN_MAX_BYTES. When the ring is full,
//   the oldest turn's text is freed.

#ifndef SYSTEM_LLM_CHAT_H
#define SYSTEM_LLM_CHAT_H

#include "System_BuildConfig.h"

#if ENABLE_LLM_BACKEND

#include <Arduino.h>
#include <ArduinoJson.h>
#include "System_LLMBackend.h"   // dispatch to whichever source owns the turn
#include "System_LLMTypes.h"

// Ring depth — older turns roll off when this is exceeded.
#define LLM_CHAT_MAX_TURNS 16

// Hard cap per turn text body. Assistant output past this is truncated
// (the engine's maxTokens almost always trips first).
#define LLM_CHAT_TURN_MAX_BYTES 2048

enum class ChatTurnRole : uint8_t {
  USER = 0,
  ASSISTANT = 1,
};

// Lightweight snapshot of a single turn's metadata. The actual text is
// fetched via chatReadTurn() (offset-based copy) so callers don't hold
// borrowed pointers across ring evictions.
struct ChatTurnInfo {
  ChatTurnRole role;
  uint32_t     textLen;
  uint32_t     tokenCount;       // ASSISTANT only — number of tokens produced
  uint16_t     tokensPerSecX10;  // ASSISTANT only — perf × 10 at turn completion
  bool         isStreaming;      // true ONLY for the live assistant turn during generation
};

// Optional per-call parameter override. Unset fields fall back to
// gSettings.llm*. Sentinel values (NAN / INT32_MIN / -1) mark "unset" so
// the struct is trivially zero-initializable and the resolver can use a
// single ternary per field. Keep this in sync with chatResolveParams().
struct ChatParamOverride {
  int   maxTokens     = INT32_MIN;
  float temperature   = NAN;
  float topp          = NAN;
  float minP          = NAN;
  float repPenalty    = NAN;
  int   repWindow     = INT32_MIN;
  int   sentenceLimit = INT32_MIN;
  int   hardCap       = INT32_MIN;
};

// ============================================================================
// Lifecycle
// ============================================================================

// Idempotent — creates the chat mutex on first call. Safe to call multiple
// times. The first surface to touch the module triggers init.
void chatInit();

// ============================================================================
// Mutating operations
// ============================================================================

// Append a USER turn with userPrompt, then start an ASSISTANT turn whose
// text will be filled by the engine asynchronously. Returns the new engine
// session ID (>0) on success; 0 if the model isn't ready or another
// generation is already in flight. opt may be nullptr → use settings defaults.
// Fill a ChatParamOverride from a JSON object's per-request keys (max_tokens,
// temperature, top_p, min_p, rep_penalty, rep_window, sentence_limit, hard_cap).
// Absent keys keep their unset sentinel → resolver falls back to gSettings.
// Shared by the web POST body and the BLE `llmgenerate json` path so both
// speak the identical override contract (one source of truth).
void chatParamOverrideFromJson(JsonObjectConst src, ChatParamOverride& out);

int  chatBeginTurn(const char* userPrompt, const ChatParamOverride* opt = nullptr);

// Re-run the most recent USER turn. Removes the last ASSISTANT turn (if any)
// and tokenizes its text as suppress tokens to steer the model away from
// repeating itself. Returns session ID (>0) on success; 0 if no prior turn,
// model not ready, or generation already in flight.
int  chatRetryLast(const ChatParamOverride* opt = nullptr);

// Request the engine stop. Does not remove the partial assistant turn —
// the user can see what was generated before the stop.
void chatStop();

// Clear all turns and free PSRAM buffers. Refused while a generation is in
// flight (returns false). Stop first, then clear.
bool chatClear();

// ============================================================================
// Reader API
// ============================================================================

// Number of completed + currently-streaming turns visible to the user.
int  chatGetTurnCount();

// Get metadata for turn at logical index [0 .. chatGetTurnCount()-1].
// Returns false if index is out of range. Pass index = -1 (or
// chatGetTurnCount()-1) for the most recent turn.
bool chatGetTurnInfo(int index, ChatTurnInfo* out);

#include "System_LLMUtf8.h"   // utf8TrimPartialTail / jsonSanitizeServedBytes

// Copy up to (maxLen-1) bytes of turn text starting at byte `offset` into buf
// (NUL-terminated). Returns bytes copied (0 = no data at that offset).
// For the live streaming turn, this pulls fresh tokens from the engine before
// returning, so two consecutive calls with the same offset can return more
// data on the second call.
int  chatReadTurn(int index, int offset, char* buf, int maxLen);

// Convenience: identical to chatReadTurn(streamingIndex, …) where the
// streaming index is the live assistant turn. Returns 0 if nothing is
// streaming. Most useful for displays that want a fixed cursor into the
// current generation without computing the index.
int  chatGetStreamLen();
int  chatReadStream(int offset, char* buf, int maxLen);
// Recover the most recently finished assistant turn for a session — for a result
// that completed before the client's first poll (e.g. an instant gate refusal).
int  chatReadFinished(int session, int offset, char* buf, int maxLen);
int  chatFinishedLen(int session);

// True iff there's a live assistant turn currently being filled.
bool chatIsGenerating();

// Engine session ID of the live turn (or 0). Surfaces use this to detect
// "I was looking at the previous generation; this is a new one — reset my
// local offset cursor."
int  chatGetSessionId();

// ============================================================================
// Parameter resolution (public so the web JSON path can use it too)
// ============================================================================

// Fill `out` from gSettings.llm* with `override` taking precedence where set.
// All values are clamped to valid ranges. Suppress tokens are NOT touched
// here — those come from the retry path. After clamping, `out` is ready to
// hand to llmStartAsync().
void chatResolveParams(const ChatParamOverride& override, LLMGenParams* out);

#endif // ENABLE_LLM_BACKEND
#endif // SYSTEM_LLM_CHAT_H
