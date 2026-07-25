/**
 * System_LLM.h - On-device LLM inference (LLM1 format)
 *
 * Tiny transformer inference for ESP32-S3 with PSRAM.
 * Model files (LLM1 binary format with embedded tokenizer)
 * are loaded from LittleFS.
 *
 * Forward pass based on Andrej Karpathy's llama2.c.
 * Binary format: esp32-llm-converter LLM1 (magic 0x4C4C4D31).
 */

#ifndef SYSTEM_LLM_H
#define SYSTEM_LLM_H

#include "System_BuildConfig.h"

#if ENABLE_ONDEVICE_LLM

#include <Arduino.h>
#include <functional>

// ============================================================================
// LLM Configuration
// ============================================================================

// Default path on LittleFS (tokenizer is embedded in model.bin)
#define LLM_DEFAULT_MODEL_PATH    "/system/llm/model.bin"

// Memory budget: minimum free PSRAM to allow LLM init (bytes)
// Keeps a reserve so the rest of the app doesn't starve
#define LLM_PSRAM_RESERVE_BYTES   (400 * 1024)

// Auto-fit: firmware automatically reduces context to fit available PSRAM.
// This is just the upper bound — the actual context used may be lower.
// NOTE: currently READ BY NOTHING. The comment on llmLoadModel below says
// "0 = use compile-time LLM_MAX_CONTEXT_LEN", but maxCtx==0 actually means
// "don't cap" — System_LLM_Model.cpp only applies requestedMaxCtx when it is
// > 0, so a model's own config.seq_len wins. Left in place rather than deleted
// because it is #ifndef-guarded for per-board override; treat the comment as
// aspirational, not as a description of current behaviour.
#ifndef LLM_MAX_CONTEXT_LEN
#define LLM_MAX_CONTEXT_LEN       1024
#endif

// Upper bound the llmmaxcontext SETTING accepts, in TOKENS. Not related to
// CMD_RESULT_MAX (4096 BYTES) — same number, unrelated concept, do not unify.
// One definition for the schema bound, the CLI usage text and the web loader's
// clamp; those were three hand-typed numbers that had already drifted (the web
// silently halved every load to 2048). The effective context is still
// min(model config.seq_len, this), then reduced further by PSRAM auto-fit —
// raising this cannot over-allocate, it only stops capping below the model.
#define LLM_SETTING_MAX_CONTEXT   4096

// Minimum runtime context (TOKENS) considered usable for a real Q&A. Below this,
// PSRAM auto-fit has shrunk the KV window so far that a typical prompt fills it
// and generation stalls immediately (the "ctx=6/6, 0-1 tokens" failure). When
// the fitted context drops under this AND it was cut below what the caller asked
// for (i.e. PSRAM starvation, not an intentional small-model/llmMaxContext cap),
// llmContextDegraded() reports true so every surface can warn the user to reload
// the model or restart. Not a hard floor on loading — the model still loads.
#define LLM_MIN_USABLE_CONTEXT      24

// Generation defaults
#define LLM_DEFAULT_MAX_TOKENS      256
#define LLM_DEFAULT_TEMPERATURE     0.5f
#define LLM_DEFAULT_TOPP            0.8f
#define LLM_DEFAULT_MINP            0.0f   // min-p relative floor (0 = off → use top-p)
#define LLM_DEFAULT_REP_PENALTY     1.3f   // repetition penalty divisor (1.0 = disabled)
#define LLM_DEFAULT_REP_WINDOW      32     // tokens looked back for rep penalty
#define LLM_DEFAULT_SENTENCE_LIMIT  2      // stop after N sentences (0 = disabled)
#define LLM_DEFAULT_HARD_CAP        80     // hard token cap regardless of sentences (0 = disabled)

// Inference task. This is a BYTE count (stack depth arg is StackType_t words,
// and sizeof(StackType_t)==1 on this Xtensa build, so words==bytes). The stack
// is one internal-DRAM block claimed at model load and reused for every
// generation (gLLMWorkerStack in System_LLM.cpp) — never re-acquired per
// generation, which is what the old dynamic 16 KB xTaskCreate did until it began
// failing once a model was loaded and the G2 page had fragmented internal DRAM
// down to a ~9 KB largest free block. Trimmed 16→12 KB:
// the measured gen-stack HWM is ~8-9 KB with everything on-stack, and we hoisted
// the ~4 KB emb diagnostic scratch off-stack (System_LLM.cpp), so real HWM is
// ~4-5 KB. 12 KB keeps the same absolute headroom the known-good 16 KB config
// had. Confirm on HW via the one-shot HWM log in llmWorkerTask, then trim further.
#define LLM_TASK_STACK_SIZE       (12 * 1024)
// Priority 2: still outranks loopTask/cmd_exec (prio 1) so generation makes
// progress, but the per-token and in-matmul vTaskDelay yields hand those tasks
// windows to answer BLE/serial polls promptly (see in-forward yields).
#define LLM_TASK_PRIORITY         2

// ============================================================================
// Model Binary Format (LLM1 — esp32-llm-converter)
// ============================================================================

// LLM1 header magic (big-endian in file: "LLM1")
#define LLM1_MAGIC 0x4C4C4D31

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
// Runtime State
// ============================================================================

enum class LLMState : uint8_t {
  UNLOADED = 0,     // No model loaded
  LOADING,          // Model load in progress
  READY,            // Model loaded, idle
  GENERATING,       // Inference running
  ERROR             // Load or runtime error
};

struct LLMStatus {
  LLMState state;
  char modelPath[64];
  LLMConfig config;
  size_t modelSizeBytes;      // Size of weight data in PSRAM
  size_t runtimeSizeBytes;    // KV cache + activations
  size_t totalPsramUsed;      // Total PSRAM consumed by LLM
  float lastTokensPerSec;     // Performance of last generation
  int lastTokenCount;         // Tokens generated in last run
  int lastContextUsed;        // Context positions used in last generation (prompt + generated)
  int lastContextMax;         // Total KV cache capacity (seq_ctx)
  float lastMeanLogprob;      // Phase 2 confidence: mean log-prob of generated tokens (0 = no signal; less negative = more confident)
  int lastConfTokens;         // Phase 2: # tokens that contributed to lastMeanLogprob
  char errorMsg[128];         // Last error message
};

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

// Token callback: called for each generated token string.
// Return false to stop generation early.
using LLMTokenCallback = std::function<bool(const char* token)>;

// ============================================================================
// Public API
// ============================================================================

// Initialize the LLM subsystem (does not load a model)
void llmInit();

// Load model from LittleFS or SD card (LLM1 format with embedded tokenizer).
// Path may start with /system/llm/ (LittleFS) or /sd/llm/ (SD card).
// maxCtx caps the KV cache context window; 0 = use compile-time LLM_MAX_CONTEXT_LEN.
// Reducing maxCtx is the primary lever for fitting larger models in PSRAM.
// Returns true if loaded successfully. Allocates PSRAM buffers.
bool llmLoadModel(const char* modelPath = LLM_DEFAULT_MODEL_PATH, int maxCtx = 0);

// Unload model and free all PSRAM buffers
void llmUnload();

// Check if model is loaded and ready for generation
bool llmIsReady();

// Get current status (thread-safe snapshot)
LLMStatus llmGetStatus();

// Optional per-model metadata from the LLM1 "info block" (a description + icon
// baked into the .bin by the converter), populated at load time. Zero-copy: the
// returned pointers reference fixed engine storage that stays valid until the
// next llmLoadModel/llmUnload. Kept out of LLMStatus so the by-value status
// snapshot (copied every OLED frame) does not grow.
const char* llmModelDescription();   // "" when the model carries no description
// Returns the loaded model's icon (1bpp, MSB-first, row-major) or false if none.
bool        llmModelIcon(const uint8_t** bits, uint8_t* width, uint8_t* height);

// Generate text from a prompt. Calls tokenCb for each output token.
// Runs synchronously on the calling task — caller is responsible for
// Wrap a bare question in the "Q: <question>\nA:" template the model is trained
// on, writing into caller-owned `out` and returning it. The trailing A: token is
// what asks the model to ANSWER; hand llmGenerate an unframed prompt and it
// simply continues the sentence instead ("Where is geodude" comes back as
// " are used to catch wild Pokemon.").
//
// Idempotent: anything already starting with "Q:" is returned unchanged, so
// callers that frame upstream (the web chat page) and callers that don't (CLI,
// OLED) can both route through it. A "do:" lead-in selects the "\nDo:" variant.
// Every interface that accepts free-text from a human should call this; the
// function is pure, so the caller owns the buffer and its serialization.
const char* llmFramePrompt(const char* userText, char* out, size_t outSize);

// running this on an appropriate task (not the httpd task).
// Returns number of tokens generated, or -1 on error.
//
// temperature: base sampling temperature
// topp:        nucleus sampling threshold
// suppressTokens/suppressTokenCount: token IDs from previous answers to penalize
//   throughout generation (retry mechanism — steers model away from prior answers)
int llmGenerate(const char* prompt, LLMTokenCallback tokenCb,
                int maxTokens = LLM_DEFAULT_MAX_TOKENS,
                float temperature = LLM_DEFAULT_TEMPERATURE,
                float topp = LLM_DEFAULT_TOPP,
                float repPenalty = LLM_DEFAULT_REP_PENALTY,
                int repWindow = LLM_DEFAULT_REP_WINDOW,
                int sentenceLimit = LLM_DEFAULT_SENTENCE_LIMIT,
                int hardCap = LLM_DEFAULT_HARD_CAP,
                const int* suppressTokens = nullptr,
                int suppressTokenCount = 0,
                float minP = LLM_DEFAULT_MINP);

// Request stop of in-progress generation (thread-safe)
void llmStop();

// ============================================================================
// Async generation (non-blocking — sensor-style architecture)
// ============================================================================

// Parameters for one generation run.  Caller fills this, passes to llmStartAsync.
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

// Start generation in a background FreeRTOS task (returns immediately).
// Returns new session ID (> 0) on success, 0 if model not ready or task
// could not be created.  Poll llmGetResultChunk / llmIsGenerationDone for output.
int llmStartAsync(const char* prompt, const LLMGenParams& params);

// Copy up to (maxLen-1) bytes starting at byte 'offset' into buf (NUL-terminated).
// Returns bytes copied.  0 = no new data at that offset or buffer not ready.
int llmGetResultChunk(int offset, char* buf, int maxLen);

// Total bytes appended to the result buffer so far (monotonically increasing).
int llmGetResultLen();

// True once the background generation task has finished (or been stopped).
bool llmIsGenerationDone();

// Monotonically increasing counter, bumped by each llmStartAsync call.
// Clients use this to detect stale polls from a previous generation.
int llmGetSessionId();

// Tokenize text into token IDs using the loaded model's tokenizer.
// Returns number of tokens written to outTokens. Returns 0 if no model loaded.
int llmTokenize(const char* text, int* outTokens, int maxTokens);

// List available model files from all storage locations.
// Returns JSON array: [{"name":"model.bin","size":1048576,"path":"/sd/llm/model.bin","storage":"sd"}, ...]
// storage field is "internal" (LittleFS /system/llm/) or "sd" (/sd/llm/)
String llmListModels();

// ============================================================================
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

// Command table (registered with CommandSystem)
struct CommandEntry;
extern const CommandEntry llmCommands[];
extern const size_t llmCommandsCount;

#endif // ENABLE_ONDEVICE_LLM
#endif // SYSTEM_LLM_H
