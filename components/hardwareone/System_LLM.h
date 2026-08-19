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

#if ENABLE_LLM_SOURCE_ONBOARD

#include <Arduino.h>
#include <functional>

// Types and generation defaults shared with every other source and surface.
// They live outside this header because a CM5-only build has no engine but
// still needs to describe a generation. Do NOT redeclare them here.
#include "System_LLMTypes.h"   // also carries the surface-facing metadata /
                               // guided-menu API, which has no-op stubs when this
                               // engine is not in the build

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

// Minimum runtime context (TOKENS) considered usable for a real Q&A. Below this,
// PSRAM auto-fit has shrunk the KV window so far that a typical prompt fills it
// and generation stalls immediately (the "ctx=6/6, 0-1 tokens" failure). When
// the fitted context drops under this AND it was cut below what the caller asked
// for (i.e. PSRAM starvation, not an intentional small-model/llmMaxContext cap),
// llmContextDegraded() reports true so every surface can warn the user to reload
// the model or restart. Not a hard floor on loading — the model still loads.
#define LLM_MIN_USABLE_CONTEXT      24

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

// LLMConfig / LLMState / LLMStatus / LLMGenParams now live in
// System_LLMTypes.h (included above), so a build whose only source is the
// CM5 co-processor still has them without compiling the engine.

// LLM1 container magic ("LLM1"). Stays here rather than in the shared types:
// it describes THIS engine's on-disk checkpoint format and means nothing to a
// source that does not read local weights.
#define LLM1_MAGIC 0x4C4C4D31

// ---------------------------------------------------------------------------
// Model-declared capabilities — CAPS section (id 5) of the LLM1 info block.
//
//   u8  caps_version   (1)
//   u16 flags          (little-endian, same convention as the DOMAIN/ICON u16s)
//
// A model that omits the section, or declares a version this firmware does not
// know, reports NO capabilities. Fail-closed is the whole point: the only
// capability today decides whether the model's output may be offered to the
// user as a runnable device command.
//
// The section table is a TLV walk whose default arm skips unknown ids, so older
// firmware ignores this section harmlessly and a model carrying it stays
// loadable everywhere.
#define LLM_CAPS_VERSION 1

// The model was trained on THIS device's command vocabulary, so its answers may
// be offered as runnable CLI commands (the web page's Do: mode, which renders a
// RUN button). Absent this bit, a model is answer-only. A general-knowledge
// model that has never seen the command set cannot know one, so it would invent
// a plausible-looking command -- which is worse than refusing, because it looks
// real right up until it runs.
#define LLM_CAP_COMMAND_MODE 0x0001u

// Capabilities the currently loaded model declared. 0 when nothing is loaded.
uint16_t llmModelCaps();

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

// ============================================================================
// Command table + settings module are declared in System_LLMTypes.h (included
// above) — they are defined in System_LLMCommands.cpp under the FEATURE flag,
// and `const` needs that extern declaration in scope to get external linkage.

#endif // ENABLE_LLM_SOURCE_ONBOARD
#endif // SYSTEM_LLM_H
