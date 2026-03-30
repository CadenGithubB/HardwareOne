/**
 * System_LLM.h - On-device LLM inference (llama2.c port)
 *
 * Tiny transformer inference for ESP32-S3 with PSRAM.
 * Model and tokenizer files are loaded from LittleFS.
 *
 * Based on Andrej Karpathy's llama2.c:
 *   https://github.com/karpathy/llama2.c
 * ESP32 adaptation inspired by DaveBben/esp32-llm.
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

// Default paths on LittleFS
#define LLM_DEFAULT_MODEL_PATH    "/system/llm/model.bin"
#define LLM_DEFAULT_TOKENIZER_PATH "/system/llm/tokenizer.bin"

// Memory budget: minimum free PSRAM to allow LLM init (bytes)
// Keeps a reserve so the rest of the app doesn't starve
#define LLM_PSRAM_RESERVE_BYTES   (256 * 1024)

// Generation defaults
#define LLM_DEFAULT_MAX_TOKENS    256
#define LLM_DEFAULT_TEMPERATURE   0.8f
#define LLM_DEFAULT_TOPP          0.9f

// Inference task
#define LLM_TASK_STACK_SIZE       (16 * 1024)
#define LLM_TASK_PRIORITY         3

// ============================================================================
// Model Binary Format (llama2.c compatible)
// ============================================================================

struct LLMConfig {
  int dim;          // transformer dimension
  int hidden_dim;   // ffn hidden dimension
  int n_layers;     // number of transformer layers
  int n_heads;      // number of query heads
  int n_kv_heads;   // number of key/value heads (can be < n_heads for GQA)
  int vocab_size;   // vocabulary size (usually 32000 for llama2)
  int seq_len;      // max sequence length
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

// Load model and tokenizer from LittleFS paths.
// Returns true if loaded successfully. Allocates PSRAM buffers.
bool llmLoadModel(const char* modelPath = LLM_DEFAULT_MODEL_PATH,
                  const char* tokenizerPath = LLM_DEFAULT_TOKENIZER_PATH);

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
int llmGenerate(const char* prompt, LLMTokenCallback tokenCb,
                int maxTokens = LLM_DEFAULT_MAX_TOKENS,
                float temperature = LLM_DEFAULT_TEMPERATURE,
                float topp = LLM_DEFAULT_TOPP);

// Request stop of in-progress generation (thread-safe)
void llmStop();

// List available model files in /system/llm/
// Returns JSON array string like [{"name":"model.bin","size":1048576}, ...]
String llmListModels();

#endif // ENABLE_ONDEVICE_LLM
#endif // SYSTEM_LLM_H
