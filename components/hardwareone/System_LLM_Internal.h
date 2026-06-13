/**
 * System_LLM_Internal.h - Private engine types shared across the LLM TUs.
 *
 * These were file-local in System_LLM.cpp. They are lifted here so the engine
 * can be split into cooperating translation units (kernels, sampler, tokenizer,
 * model loader) that all operate on the single `gLLM` runtime singleton — which
 * matches the codebase's "module = free functions over a file-static singleton"
 * idiom, just spread across files now.
 *
 * NOT a public header — only the System_LLM*.cpp TUs include it. Public API and
 * the LLMConfig/LLMState/LLMGenParams types live in System_LLM.h.
 */
#pragma once

#include "System_LLM.h"
#include "System_LLM_Kernels.h"   // QuantTensor
#include <cstdint>
#include <cstddef>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// ============================================================================
// Transformer Weights — pointers into a single PSRAM block
// ============================================================================

struct TransformerWeights {
  // FP32 weights (always used in quant_type==0; for quant_type==1 these are null
  // for quantized tensors — norms + pos embedding + GPT-2 w1 dummy stay FP32)
  float* token_embedding_table;  // (vocab_size, dim) — null in INT8 mode
  float* rms_att_weight;         // (layer, dim) — always FP32
  float* rms_ffn_weight;         // (layer, dim) — always FP32
  float* wq;                     // (layer, dim, dim) — null in INT8 mode
  float* wk;                     // (layer, dim, kv_dim) — null in INT8 mode
  float* wv;                     // (layer, dim, kv_dim) — null in INT8 mode
  float* wo;                     // (layer, dim, dim) — null in INT8 mode
  float* w1;                     // (layer, hidden_dim, dim) — null in INT8/Llama; FP32 dummy for GPT-2
  float* w2;                     // (layer, dim, hidden_dim) — null in INT8 mode
  float* w3;                     // (layer, hidden_dim, dim) — null in INT8 mode
  float* rms_final_weight;       // (dim,) — always FP32
  float* rms_att_bias;           // (layer, dim) — GPT-2 v2 only, always FP32; NULL otherwise
  float* rms_ffn_bias;           // (layer, dim) — GPT-2 v2 only, always FP32; NULL otherwise
  float* rms_final_bias;         // (dim,) — GPT-2 v2 only, always FP32; NULL otherwise
  float* wcls;                   // (vocab_size, dim) — null in INT8 mode; may alias embedding in FP32 mode
  float* pos_embedding_table;    // (seq_len, dim) — GPT-2 only, always FP32

  // INT8 weights + per-group scales (used when quant_type==1 or Q8 layers in quant_type==2)
  // All quantized tensors: scales first (float[n_groups]), then data (int8[n_elements])
  int8_t* emb_i8;  float* emb_sc;    // token embedding
  int8_t* wq_i8;   float* wq_sc;
  int8_t* wk_i8;   float* wk_sc;
  int8_t* wv_i8;   float* wv_sc;
  int8_t* wo_i8;   float* wo_sc;
  int8_t* w1_i8;   float* w1_sc;     // Llama gate only; GPT-2 w1 stays FP32 dummy
  int8_t* w2_i8;   float* w2_sc;
  int8_t* w3_i8;   float* w3_sc;
  int8_t* wcls_i8; float* wcls_sc;   // null if tied (points to emb_i8/emb_sc when tied)

  // ── INT4 mixed mode (quant_type==2) ──
  // Per-layer quant: 1=INT8, 2=INT4 (length n_layers; null if not mixed)
  uint8_t* layer_quant;

  // Contiguous Q4 packed data block (nibble-packed, all Q4 layers)
  uint8_t* q4_data;
  size_t   q4_data_size;

  // Contiguous Q4 scales block (FP32, all Q4 layers)
  float*   q4_scales;
  size_t   q4_scales_size;

  // Per-layer byte offsets into q4_data / q4_scales for each tensor type.
  // Only valid for layers where layer_quant[l] == 2.
  // Attention tensors: wq, wk, wv, wo — offsets into q4_data (packed bytes)
  //                    and into q4_scales (float offsets, not bytes)
  struct Q4LayerOffsets {
    size_t wq_data;  size_t wq_sc;
    size_t wk_data;  size_t wk_sc;
    size_t wv_data;  size_t wv_sc;
    size_t wo_data;  size_t wo_sc;
    size_t w1_data;  size_t w1_sc;  // gate (Llama only)
    size_t w2_data;  size_t w2_sc;  // down
    size_t w3_data;  size_t w3_sc;  // up
  };
  Q4LayerOffsets* q4_offsets;  // array of n_layers (null entries for INT8 layers)

  // ── Prepackaged weights for linear() ──
  // Per-layer attention + FFN matrices with offsets resolved at load time, so
  // forward() indexes instead of recomputing pointers every token. Built by
  // buildLayerTensors() after all the raw pointers above are set.
  struct LayerTensors { QuantTensor wq, wk, wv, wo, w1, w2, w3; };
  LayerTensors* layerT;   // array[n_layers]
  QuantTensor   clsT;     // classifier (wcls)
};

// ============================================================================
// Run State — activation buffers and KV cache
// ============================================================================

struct RunState {
  float* x;          // activation at current position (dim,)
  float* xb;         // same, inside a residual branch (dim,)
  float* xb2;        // additional buffer (dim,)
  float* hb;         // buffer for hidden dimension in ffn (hidden_dim,)
  float* hb2;        // buffer for hidden dimension in ffn (hidden_dim,)
  float* q;          // query (dim,)
  float* key_cache;  // (layer, seq_len, kv_dim)
  float* value_cache;// (layer, seq_len, kv_dim)
  float* att;        // attention scores (n_heads, seq_len)
  float* logits;     // output logits (vocab_size,)
  // RoPE scratch (Llama). rope_inv: per-dim inverse frequencies, constant —
  // filled once at load. rope_cos/rope_sin: per-token rotation, computed once
  // per forward() and reused across all layers (was recomputed per layer).
  float* rope_inv;   // (head_size,)
  float* rope_cos;   // (dim/2,)
  float* rope_sin;   // (dim/2,)
};

// ============================================================================
// Tokenizer (merge-based BPE, embedded in LLM1 model file)
// ============================================================================

struct MergeEntry {
  uint32_t left_id;
  uint32_t right_id;
  uint32_t merged_id;
};

// Merge lookup: keyed by (left_id << 16) | right_id -> {merged_id, priority}
struct MergeLookup {
  uint32_t key;       // (left_id << 16) | right_id
  uint32_t merged_id;
  int priority;       // lower = higher precedence (index in merge list)
};

// Pre-split token: a multi-byte vocab entry that must be matched as a whole
// string before BPE runs (like HuggingFace's added_tokens / special tokens).
struct PreSplitToken {
  const char* str;  // points into vocab string pool (not separately allocated)
  int id;
  int len;          // strlen(str) — cached to avoid recomputing
};

struct TokenizerState {
  char** vocab;
  int vocab_size;
  MergeEntry* merges;
  int merge_count;
  // Fast lookup for encoding
  MergeLookup* merge_map;
  int merge_map_capacity;
  int byte_to_token[256]; // single-byte char -> token id (-1 if not found)
  // Special / added tokens that must be matched before BPE
  PreSplitToken* presplit;
  int presplit_count;
};

// ============================================================================
// Module State — the single engine runtime singleton (defined in System_LLM.cpp)
// ============================================================================

struct LLMRuntime {
  LLMConfig config;
  TransformerWeights weights;
  RunState state;
  TokenizerState tokenizer;

  // Raw allocated blocks (for free)
  float*   weightsData;    // FP32 weights (quant_type==0) or norms+scales (quant_type==1)
  int8_t*  weightsQ8Data;  // INT8 weight data block; null for FP32 models
  float*   stateData;      // Cold state: KV cache + att + logits (PSRAM)
  float*   stateHotData;   // Hot activations: x/xb/xb2/q/hb/hb2 (internal RAM)
  char*    tokenizerData;  // string pool for vocab entries

  size_t weightsSize;
  size_t weightsQ8Size;
  size_t stateSize;
  size_t stateHotSize;

  LLMState runState;
  volatile bool stopRequested;
  float lastTokPerSec;
  int lastTokCount;
  int lastContextUsed;   // prompt + generated tokens in last run
  int lastContextMax;    // KV cache capacity for last run
  char modelPath[64];
  char errorMsg[128];

  // Effective context for KV cache (<= config.seq_len); may be capped by requestedMaxCtx
  int seq_ctx;
  int requestedMaxCtx;   // set by llmLoadModel before loadWeights is called

  // Repetition penalty ring buffer (allocated at model load, reused per generation)
  int* repBuf;
  int repBufSize;  // = LLM_DEFAULT_REP_WINDOW (capped 1-256)

  // Top-p sampling index buffer (allocated once at model load, reused per token)
  int* sampleIndices;
  int sampleIndicesSize;  // = vocab_size

  SemaphoreHandle_t mutex;
};

extern LLMRuntime gLLM;

// ============================================================================
// Shared PSRAM allocation helpers (defined in System_LLM.cpp)
// ============================================================================

void* llmPsramAlloc(size_t size, const char* tag);
void  llmPsramFree(void** ptr);

// Set gLLM.errorMsg (printf-style). Does NOT change runState — callers that
// also transition to ERROR keep doing that explicitly.
void  setLlmError(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
