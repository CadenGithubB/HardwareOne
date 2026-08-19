/**
 * System_LLMTypes.h — LLM types shared by every source and every surface.
 *
 * Why this file exists
 *   System_LLM.h is the ON-BOARD ENGINE's header and is compiled only when
 *   ENABLE_LLM_SOURCE_ONBOARD is set. But the conversation layer, the model
 *   registry, the settings schema and all four UI surfaces need to describe a
 *   generation and a status regardless of WHICH source answers — including a
 *   build whose only source is the CM5 co-processor and which therefore has no
 *   engine at all.
 *
 *   So the vocabulary lives here, gated on the FEATURE (ENABLE_LLM_BACKEND),
 *   and System_LLM.h includes it rather than redeclaring it.
 *
 * What belongs here
 *   Types and defaults that mean something to a caller who does not know which
 *   source is active. Engine-internal knobs (PSRAM reserve, worker stack size,
 *   the on-disk model path) deliberately stay in System_LLM.h.
 */

#ifndef SYSTEM_LLM_TYPES_H
#define SYSTEM_LLM_TYPES_H

#include "System_BuildConfig.h"

#if ENABLE_LLM_BACKEND

#include <Arduino.h>

// ============================================================================
// Generation defaults — the values the settings schema seeds from
// ============================================================================

#define LLM_DEFAULT_MAX_TOKENS      256
#define LLM_DEFAULT_TEMPERATURE     0.5f
#define LLM_DEFAULT_TOPP            0.8f
#define LLM_DEFAULT_MINP            0.0f   // min-p relative floor (0 = off → use top-p)
#define LLM_DEFAULT_REP_PENALTY     1.3f   // repetition penalty divisor (1.0 = disabled)
#define LLM_DEFAULT_REP_WINDOW      32     // tokens looked back for rep penalty
#define LLM_DEFAULT_SENTENCE_LIMIT  2      // stop after N sentences (0 = disabled)
#define LLM_DEFAULT_HARD_CAP        80     // hard token cap regardless of sentences (0 = disabled)

// Upper bound the llmmaxcontext SETTING accepts, in TOKENS. Not related to
// CMD_RESULT_MAX (4096 BYTES) — same number, unrelated concept, do not unify.
// One definition for the schema bound, the CLI usage text and the web loader's
// clamp; those were three hand-typed numbers that had already drifted.
// Meaningless for a remote source (the host owns its own context window), but
// the setting is still accepted and simply ignored there.
#define LLM_SETTING_MAX_CONTEXT   4096

// ============================================================================
// Model shape — describes a LOCAL checkpoint
// ============================================================================
// Every field here is a property of the on-device LLM1 file format. A remote
// source leaves the whole struct zeroed; surfaces must branch on the backend
// kind rather than rendering these unconditionally.

struct LLMConfig {
  int dim;            // transformer dimension
  int hidden_dim;     // ffn hidden dimension
  int n_layers;       // number of transformer layers
  int n_heads;        // number of query heads
  int n_kv_heads;     // number of key/value heads (can be < n_heads for GQA)
  int vocab_size;     // vocabulary size
  int seq_len;        // max sequence length
  uint8_t quant_type; // 0=FP32, 1=INT8, 2=INT4_MIXED
  uint16_t group_size;// quantization group size
  uint8_t arch_type;  // 0=Llama, 1=GPT-2
  uint8_t n_q8_start; // (quant_type==2) INT8 layers at front
  uint8_t n_q8_end;   // (quant_type==2) INT8 layers at back
  uint8_t file_version; // LLM1 file version (2 or 3)
};

// ============================================================================
// Runtime state
// ============================================================================

enum class LLMState : uint8_t {
  UNLOADED = 0,     // No model selected
  LOADING,          // Selection in progress (local: reading weights; remote: host switching)
  READY,            // Selected and idle
  GENERATING,       // Inference running
  ERROR             // Load or runtime error
};

struct LLMStatus {
  LLMState state;
  char modelPath[64];
  LLMConfig config;           // zeroed for a remote source
  size_t modelSizeBytes;      // Size of weight data in PSRAM (0 remote)
  size_t runtimeSizeBytes;    // KV cache + activations (0 remote)
  size_t totalPsramUsed;      // Total PSRAM consumed locally (0 remote)
  float lastTokensPerSec;     // Performance of last generation
  int lastTokenCount;         // Tokens generated in last run
  int lastContextUsed;        // Context positions used (0 remote)
  int lastContextMax;         // Total KV cache capacity (0 remote)
  float lastMeanLogprob;      // Confidence: mean log-prob (0 = no signal; remote: always 0)
  int lastConfTokens;         // # tokens contributing to lastMeanLogprob
  char errorMsg[128];         // Last error message
};

// ============================================================================
// Generation parameters
// ============================================================================
// Filled by chatResolveParams() from gSettings + per-call overrides, then handed
// to whichever source is active. A remote source honours the subset its server
// implements and ignores the rest — notably suppressTokens, which are token IDs
// from THIS device's tokenizer and are meaningless anywhere else.

struct LLMGenParams {
  int   maxTokens;
  float temperature;
  float topp;
  float minP;
  float repPenalty;
  int   repWindow;
  int   sentenceLimit;
  int   hardCap;
  int   suppressTokens[128];
  int   suppressCount;
};


// Command table + settings module, defined in System_LLMCommands.cpp. These
// declarations must be visible in that TU: `const` at namespace scope has
// INTERNAL linkage in C++ unless a prior extern declaration says otherwise, so
// without this the definitions silently become file-static and every registration
// site fails to link.
struct CommandEntry;
struct SettingsModule;
extern const CommandEntry llmCommands[];
extern const size_t llmCommandsCount;
extern const SettingsModule llmSettingsModule;

// JSON array of every model the registry can see, across all sources. Declared
// for the FEATURE because the web picker needs it in a build with no engine.
// (Implementation lives in System_LLMCommands.cpp — it is a projection of
// llmEnumerateModels(), not a filesystem scan.)
String llmListModels();

// ============================================================================
// Surface-facing model metadata + guided-menu API
// ============================================================================
// Declared for the FEATURE, not the engine. Every one of these describes
// something only a LOCAL checkpoint has — an info block, an embedded question
// menu, a PSRAM-fitted context window — so with no on-board source they have
// no-op stubs (System_LLMBackend.cpp) that report "absent" rather than being
// absent themselves.
//
// That is deliberate: the alternative is ~30 `#if ENABLE_LLM_SOURCE_ONBOARD`
// blocks scattered through the web, OLED and G2 surfaces. A guided menu with
// zero groups and an icon that returns false are exactly what those surfaces
// already handle for a model that ships neither, so they hide the guided UI on
// a remote model with no extra branching.

// Degraded-context detection — single source of truth for every LLM surface
// (CLI, web page + /api/llm, OLED, G2). Returns true when a model is loaded but
// PSRAM auto-fit shrank its context below LLM_MIN_USABLE_CONTEXT AND below what
// was requested (so an intentionally small model or a deliberate llmMaxContext
// cap does NOT trip it). outCtx/outModelSeq, when non-null, receive the active
// context and the model's native seq_len even when not degraded.
bool llmContextDegraded(int* outCtx = nullptr, int* outModelSeq = nullptr);

// Formats the one-line, human-readable degraded-context warning into buf (always
// NUL-terminates). Returns the string length, or 0 when not degraded (buf set to
// ""). Same wording on every surface — the caller supplies the buffer so it is
// task-safe (no shared state). Recommend cap >= 160.
size_t llmContextWarning(char* buf, size_t cap);


// Optional per-model metadata from the LLM1 "info block" (a description + icon
// baked into the .bin by the converter), populated at load time. Zero-copy: the
// returned pointers reference fixed engine storage that stays valid until the
// next llmLoadModel/llmUnload. Kept out of LLMStatus so the by-value status
// snapshot (copied every OLED frame) does not grow.
const char* llmModelDescription();   // "" when the model carries no description
// Returns the loaded model's icon (1bpp, MSB-first, row-major) or false if none.
bool        llmModelIcon(const uint8_t** bits, uint8_t* width, uint8_t* height);


// Guided-input menu API (System_LLM_Menu.cpp)
// ============================================================================
// A model's info block may carry a MENU section: curated, corpus-exact question
// templates ("What type is {}?") + entity rosters ("Bulbasaur", ...). Surfaces
// (web/OLED/G2/CLI) present pickers and submit integer indices; composition
// happens once, on-device, and the composed question feeds the existing chat
// pipeline unchanged. All accessors are thread-safe (internal sMenuLock) and
// COPY OUT — no pointer into the menu blob ever escapes. See
// LLM_GUIDED_MENU_SPEC §5. ChatParamOverride is from System_LLMChat.h.
struct ChatParamOverride;

// Menu generation counter — bumped on every model load and unload. A surface
// caches it, and treats any change as "the menu may have changed, refetch".
uint16_t llmMenuGeneration(void);

// Number of guided-input groups (0 = this model has no menu; hide guided UI).
uint8_t  llmMenuGroupCount(void);

// Copied-out snapshot of one group's header (name + counts + flags).
struct LLMMenuGroupInfo {
  char     name[33];   // NUL-terminated (<=32 chars)
  uint8_t  flags;      // bit0 = Do-mode
  uint16_t tplCount;
  uint16_t entCount;
};
bool llmMenuGroupInfo(uint8_t g, LLMMenuGroupInfo* out);

// Copy the display form of template t in group g into buf (NUL-terminated). The
// slot marker (0x1F) is rendered as "{}". *hasSlot (if non-null) reports whether
// the template has an entity slot. Returns bytes written, or -1 on bad index.
int  llmMenuTemplate(uint8_t g, uint16_t t, char* buf, size_t cap, bool* hasSlot);

// Copy entity e of group g into buf (NUL-terminated). Returns bytes written, -1 bad index.
int  llmMenuEntity(uint8_t g, uint16_t e, char* buf, size_t cap);

// Compose the final question for (group g, template t, entity e) into buf. Pass
// e = -1 for a slotless template. Returns composed length, or <0 on error
// (-2 bad index, -3 no menu).
int  llmMenuCompose(uint8_t g, uint16_t t, int e, char* buf, size_t cap);

// Compose and submit a guided question to the chat pipeline. `gen` must match the
// current menuGeneration (guards against composing against a swapped model).
// Do-mode groups get the "do: " scaffold + hardCap=4/sentenceLimit=0 (suggestion
// only — never auto-executed). Returns:
//   >0 = chat session id;  0 = busy (generation in flight);
//   -1 = stale generation;  -2 = bad index;  -3 = no menu.
int  llmMenuAsk(uint16_t gen, uint8_t g, uint16_t t, int e, const ChatParamOverride* ov);

// Re-ask the last guided question PLAIN (a fresh generation with NO suppress list
// — chatRetryLast would ban the memorized-correct answer, which is wrong for a
// guided ask). Same return codes as llmMenuAsk.
int  llmMenuRepeatLast(void);

// True iff a guided last-ask exists; *sessionOut (if non-null) receives its chat
// session id. Retry branch (spec §5): guided-last -> llmMenuRepeatLast(), else ->
// chatRetryLast(). Do NOT gate on chatGetSessionId() — it reads 0 the instant a
// turn finishes, so post-completion the comparison always fails and wrongly falls
// through to chatRetryLast (which bans the memorized-correct answer). A surface
// that also accepts free-text tracks "was my last turn guided" locally and matches
// this session id (OLED); a guided-only surface (G2 lens) branches on existence.
bool llmMenuLastAskInfo(int* sessionOut);


#endif // ENABLE_LLM_BACKEND
#endif // SYSTEM_LLM_TYPES_H
