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
#ifndef LLM_MAX_CONTEXT_LEN
#define LLM_MAX_CONTEXT_LEN       1024
#endif

// Generation defaults
#define LLM_DEFAULT_MAX_TOKENS      256
#define LLM_DEFAULT_TEMPERATURE     0.5f
#define LLM_DEFAULT_TOPP            0.8f
#define LLM_DEFAULT_MIROSTAT_TAU    5.0f   // target surprise in bits (Mirostat 2)
#define LLM_DEFAULT_MIROSTAT_ETA    0.1f   // Mirostat learning rate
#define LLM_DEFAULT_REP_PENALTY     1.3f   // repetition penalty divisor (1.0 = disabled)
#define LLM_DEFAULT_REP_WINDOW      32     // tokens looked back for rep penalty
#define LLM_DEFAULT_SENTENCE_LIMIT  2      // stop after N sentences (0 = disabled)
#define LLM_DEFAULT_HARD_CAP        80     // hard token cap regardless of sentences (0 = disabled)

// Inference task
#define LLM_TASK_STACK_SIZE       (16 * 1024)
#define LLM_TASK_PRIORITY         3

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
  char errorMsg[128];         // Last error message
};

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
// running this on an appropriate task (not the httpd task).
// Returns number of tokens generated, or -1 on error.
//
// temperature: base sampling temperature (dynamically scaled per step from logit distribution)
// topp:        nucleus sampling threshold (used when useMirostat2=false)
// useMirostat2: enable Mirostat 2 sampling (adaptive surprise targeting)
// mirostatTau: target surprise in bits (typical: 3–7, higher = more diverse)
// mirostatEta: learning rate for Mirostat mu update (typical: 0.05–0.2)
// suppressTokens/suppressTokenCount: token IDs from previous answers to penalize
//   throughout generation (retry mechanism — steers model away from prior answers)
int llmGenerate(const char* prompt, LLMTokenCallback tokenCb,
                int maxTokens = LLM_DEFAULT_MAX_TOKENS,
                float temperature = LLM_DEFAULT_TEMPERATURE,
                float topp = LLM_DEFAULT_TOPP,
                bool useMirostat2 = false,
                float mirostatTau = LLM_DEFAULT_MIROSTAT_TAU,
                float mirostatEta = LLM_DEFAULT_MIROSTAT_ETA,
                float repPenalty = LLM_DEFAULT_REP_PENALTY,
                int repWindow = LLM_DEFAULT_REP_WINDOW,
                int sentenceLimit = LLM_DEFAULT_SENTENCE_LIMIT,
                int hardCap = LLM_DEFAULT_HARD_CAP,
                bool dynTemp = false,
                const int* suppressTokens = nullptr,
                int suppressTokenCount = 0);

// Request stop of in-progress generation (thread-safe)
void llmStop();

// Tokenize text into token IDs using the loaded model's tokenizer.
// Returns number of tokens written to outTokens. Returns 0 if no model loaded.
int llmTokenize(const char* text, int* outTokens, int maxTokens);

// List available model files from all storage locations.
// Returns JSON array: [{"name":"model.bin","size":1048576,"path":"/sd/llm/model.bin","storage":"sd"}, ...]
// storage field is "internal" (LittleFS /system/llm/) or "sd" (/sd/llm/)
String llmListModels();

// Command table (registered with CommandSystem)
struct CommandEntry;
extern const CommandEntry llmCommands[];
extern const size_t llmCommandsCount;

#endif // ENABLE_ONDEVICE_LLM
#endif // SYSTEM_LLM_H
