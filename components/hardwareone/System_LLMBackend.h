/**
 * System_LLMBackend.h — the model registry and source dispatch.
 *
 * The idea
 *   "Which model answers my question" is one choice, and the places an answer
 *   can come from are SOURCES. The on-device engine is one source; the CM5
 *   co-processor is another. They are symmetric — the local one has no special
 *   status beyond being local — and a third (someone else's HTTP endpoint, a
 *   second co-processor) should be a new file here plus a build flag, not an
 *   edit to four UI surfaces.
 *
 * What this replaces
 *   Four independent model enumerations that each scanned the filesystem
 *   themselves (web /api/llm/models, `llmmodels json`, the G2 lens picker, and
 *   the OLED file browser), and a conversation layer wired directly to the
 *   engine's globals. Surfaces now ask this module and never name a source.
 *
 * Contract shape
 *   The generation half deliberately mirrors the engine's existing async
 *   contract (start → poll len/chunk → done) rather than inventing a new one,
 *   so System_LLMChat's pull-on-read model works unchanged for every source.
 *   A push-fed source (CM5) buffers inbound text and answers the same polls.
 *
 * Threading
 *   Every function is safe from any task. The registry is read-mostly; the
 *   active-selection and per-source result state are each guarded internally.
 */

#ifndef SYSTEM_LLM_BACKEND_H
#define SYSTEM_LLM_BACKEND_H

#include "System_BuildConfig.h"

#if ENABLE_LLM_BACKEND

#include <Arduino.h>
#include "System_LLMTypes.h"

// ============================================================================
// Sources
// ============================================================================

enum class LlmBackendKind : uint8_t {
  None    = 0,
  Onboard = 1,   // on-device PSRAM engine (ENABLE_LLM_SOURCE_ONBOARD)
  Cm5     = 2,   // CM5 / Pi 5 co-processor over the UART host link (ENABLE_LLM_SOURCE_CM5)
};

const char* llmBackendKindName(LlmBackendKind k);   // "onboard" / "cm5" / "none"

// ============================================================================
// Model descriptor
// ============================================================================
// 156 bytes. Arrays of these must be EXT_RAM_BSS_ATTR statics owned by the
// surface, NEVER locals — several UI tasks run on ~7.5 KB stacks and an 8-row
// local would put 1.2 KB on one.

#define LLM_MODEL_ID_LEN    48
#define LLM_MODEL_NAME_LEN  32
#define LLM_MODEL_PATH_LEN  64

// Storage classes for the `storage` field.
#define LLM_STORAGE_INTERNAL 0
#define LLM_STORAGE_SD       1
#define LLM_STORAGE_REMOTE   2

// Suggested cap for a surface's own model array. At 156 B/row this is ~2.5 KB,
// which is why the header insists these live in EXT_RAM_BSS_ATTR statics.
#define LLM_REGISTRY_MAX_MODELS 16

struct LlmModelDesc {
  // Stable handle, "<source>:<name>", e.g. "onboard:model.bin" / "cm5:Qwen3-1.7B-Q4_0.gguf".
  // This is what every surface stores, what `llmload` accepts, and what the
  // llmDefaultModel setting persists. Contains no spaces by construction for
  // remote entries; a LOCAL filename containing a space is a pre-existing
  // hazard on the CLI path (arguments split on whitespace) and is unchanged here.
  char           id[LLM_MODEL_ID_LEN];
  char           name[LLM_MODEL_NAME_LEN];   // display text, may be truncated
  LlmBackendKind backend;
  char           path[LLM_MODEL_PATH_LEN];   // local only; "" for remote
  uint32_t       sizeKB;                     // 0 = unknown (remote often is)
  uint8_t        storage;                    // LLM_STORAGE_*
  bool           available;                  // false = listed but not selectable right now
};

// ============================================================================
// Registry
// ============================================================================

// Fill `out` with up to `cap` models across every compiled-in, runtime-enabled
// source. Returns the number written. Ordering is stable: sources in enum
// order, models within a source in discovery order.
//
// A source that is compiled in but unusable right now still contributes its
// rows with available=false (e.g. the CM5 catalog is known but the presence
// lease has gone stale), so a picker can show WHY a model is greyed out rather
// than silently losing it. gSettings.llmEnabled=0 suppresses every row.
size_t llmEnumerateModels(LlmModelDesc* out, size_t cap);

// Resolve one id back to its descriptor. THE single id→source resolver — every
// selection path must go through it rather than re-deriving a filesystem path.
// Also accepts a bare local filename ("model.bin") or absolute local path as a
// convenience for hand-typed CLI and for a llmDefaultModel saved before ids
// existed; those resolve to the Onboard source.
bool llmResolveModelId(const char* id, LlmModelDesc* out);

// ============================================================================
// Selection
// ============================================================================

// Select `id` as the active model. Blocking for a local model (weights read,
// seconds) and asynchronous for a remote one (the host restarts its server), so
// callers must not assume READY on return — poll llmBackendStatus().state.
// On failure returns false and writes a short reason into errOut when non-null.
// maxCtx caps the KV window and is honoured only by a LOCAL source (a remote
// host owns its own context window); 0 = the source's own default.
bool llmBackendSelect(const char* id, char* errOut, size_t errCap, int maxCtx = 0);

// Drop the active selection and release whatever it held.
void llmBackendUnload();

LlmBackendKind llmBackendActiveKind();
bool           llmBackendActiveModel(LlmModelDesc* out);   // false when none selected

// ============================================================================
// Generation — mirrors the engine's async contract for every source
// ============================================================================

bool     llmBackendIsReady();
LLMStatus llmBackendStatus();

// Start generation. Returns a session id (>0), or 0 if not ready / busy.
int      llmBackendStartAsync(const char* prompt, const LLMGenParams& params);
void     llmBackendStop();

int      llmBackendSessionId();                                  // bumped per start
int      llmBackendResultLen();                                  // bytes produced so far
int      llmBackendResultChunk(int offset, char* buf, int maxLen);
bool     llmBackendIsDone();

// Wrap a bare question in whatever framing the ACTIVE source needs. The onboard
// engine wants its trained "Q: …\nA:" template; a remote server applies its own
// chat template and system prompt host-side, so this is a pass-through there.
// Routing framing through the source is what stops the local template leaking
// into remote prompts. Returns a pointer into `out`, or `userText` unchanged.
const char* llmBackendFramePrompt(const char* userText, char* out, size_t outSize);

// True when `userText` opens with the "Do:" command-mode marker (leading
// whitespace tolerated). Shared so every surface agrees on what Do:-mode is.
bool llmPromptIsCommandMode(const char* userText);

// True only for the on-device engine. Do:-mode asks the model to emit a
// Hardware One CLI command into a box with a Run button; only the local model
// was trained on this device's command vocabulary, so a remote source would
// have to invent one. See the definition for why instructing it is not a fix.
bool llmBackendSupportsCommandMode();

// Tokenize with the ACTIVE source's tokenizer. Returns 0 for any source that
// has no local tokenizer — which is what makes the retry path's suppress-token
// list naturally empty for remote models instead of feeding it foreign ids.
int      llmBackendTokenize(const char* text, int* outTokens, int maxTokens);

// Housekeeping tick — lets a source time out a stalled remote generation.
// Called from the same place other subsystem ticks run. Cheap when idle.
void     llmBackendTick();

#endif // ENABLE_LLM_BACKEND
#endif // SYSTEM_LLM_BACKEND_H
