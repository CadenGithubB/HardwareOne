/**
 * System_LLM.cpp - On-device LLM inference engine
 *
 * Tiny transformer inference for ESP32-S3 with PSRAM.
 * Forward pass based on Andrej Karpathy's llama2.c.
 * Supports INT8 and INT4/INT8 mixed quantization (weights stay quantized in PSRAM).
 * Binary format: LLM1 (esp32-llm-converter) with embedded BPE tokenizer.
 * Dual architecture: GPT-2 (LayerNorm + GELU) and Llama (RMSNorm + SwiGLU).
 *
 * Key adaptations:
 *   - All weight/activation buffers allocated in PSRAM via heap_caps
 *   - Fused INT8/INT4 dequantize-matmul (no full FP32 expansion)
 *   - esp-dsp dot product for S3 SIMD acceleration
 *   - LittleFS/SD model loading (no mmap)
 *   - Pre-split tokenizer for added/special tokens unreachable by BPE
 *   - Thread-safe generation with stop flag and token callback
 */

/*
 * DEBUG NOTE (prediction engine diagnostics)
 * ----------------------------------------
 * Prediction-engine debug calls are ENABLED (via DEBUG_LLM_FORWARD / DEBUG_LLM_GENERATE):
 *   - forward() logit distribution stats + top-5 candidates at each position
 *   - Prompt prediction tracking (PROMPT_PRED per token, PROMPT_TRACK at boundary)
 *   - sample() pre-logit range & entropy
 *   - sample_topp nucleus stats, per-candidate detail, sampled rank
 *   - sample_mirostat2 mu/threshold detail + chosen token surprise
 *   - Rep penalty per-step log
 *   - Suppress penalty per-step log
 *   - Content logit boost per-step log
 *   - Dynamic temperature per-token log
 *   - Per-generated-token sample line (pos/sampled/top/eff_temp/mu)
 *
 * Still commented out (low-level transformer internals):
 *   - Per-prompt-token position tracking
 *   - Generation boundary embedding similarity pairs
 *   - KV cache health check at generation start
 *   - Prompt token logit rank check at generation start
 *   - Pre-generation embedding norm / pairwise similarity / CONFUSER analysis
 *   - Content token listing
 *   - forward() per-layer QKV matmul stats
 *   - forward() per-layer attention pattern stats (per-head weights)
 *   - forward() post-attention residual stats
 *   - forward() pre/post-GELU FFN stats
 *   - forward() post-FFN residual stats
 *   - forward() embedding + position encoding stats
 */

#include "System_BuildConfig.h"

#if ENABLE_ONDEVICE_LLM

#include "System_LLM.h"
#include "System_Debug.h"
#include "System_MemUtil.h"
#include "System_Filesystem.h"
#include "System_VFS.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <climits>
#include <algorithm>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"

// esp-dsp for accelerated dot product on S3
#include "dsps_dotprod.h"

extern bool filesystemReady;

// ============================================================================
// Table of Contents
// ============================================================================
//
//  1. Constants
//  2. Data Structures (TransformerWeights, RunState, Tokenizer types)
//  3. Module State (gLLM)
//  4. PSRAM Allocation Helpers
//  5. Math Primitives (rmsnorm, layernorm, softmax, matmul variants)
//  6. Forward Pass Debug Utilities (VecStats, FORWARD_DBG_POS)
//  7. Forward Pass
//  8. Sampling (argmax, top-p, mirostat2, dispatcher)
//  9. Tokenizer (hash map, load, encode, decode)
// 10. Model Loading (validation, read helpers, weight loading, spot checks)
// 11. Public API (init, load, unload, generate, stop, list)
// 12. CLI Commands
//

// ============================================================================
// 1. Constants
// ============================================================================

// GELU tanh approximation coefficients (matches HuggingFace gelu_new)
static constexpr float GELU_COEFF_A = 0.7978845608f;  // sqrt(2/pi)
static constexpr float GELU_COEFF_B = 0.044715f;

// Logit clamp bounds — prevents ±Inf from INT8 accumulation errors
static constexpr float LOGIT_CLAMP_MAX =  50.0f;
static constexpr float LOGIT_CLAMP_MIN = -50.0f;

// File I/O chunk size for reading tensors from flash
static constexpr size_t READ_CHUNK_SIZE = 4096;

// EOS token IDs by architecture
static constexpr int EOS_TOKEN_LLAMA = 2;   // Llama/SentencePiece
static constexpr int EOS_TOKEN_GPT2  = 0;   // GPT-2 (<|endoftext|>)

// Generation loop intervals
static constexpr int YIELD_INTERVAL      = 4;   // vTaskDelay every N generated tokens
static constexpr int HEALTH_LOG_INTERVAL = 16;   // log generation health every N tokens

// ============================================================================
// 2. Data Structures — Transformer Weights, Run State, Tokenizer
// ============================================================================

// Transformer Weights — pointers into a single PSRAM block

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
};

// Run State — activation buffers and KV cache

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
};

// Tokenizer (merge-based BPE, embedded in LLM1 model file)

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
// 3. Module State
// ============================================================================

static struct {
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

  SemaphoreHandle_t mutex;
} gLLM = {};

// ============================================================================
// 4. PSRAM Allocation Helpers
// ============================================================================

static void* llmPsramAlloc(size_t size, const char* tag) {
  void* p = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM);
  if (!p) {
    ERROR_LLMF("PSRAM alloc failed: %s (%u bytes)", tag, (unsigned)size);
  }
  return p;
}

static void llmPsramFree(void** ptr) {
  if (ptr && *ptr) {
    heap_caps_free(*ptr);
    *ptr = nullptr;
  }
}

// ============================================================================
// 5. Math Primitives
// ============================================================================

static void rmsnorm(float* o, const float* x, const float* weight, int size) {
  float ss = 0.0f;
  for (int j = 0; j < size; j++) ss += x[j] * x[j];
  ss /= size;
  ss += 1e-5f;
  ss = 1.0f / sqrtf(ss);
  for (int j = 0; j < size; j++) o[j] = weight[j] * (ss * x[j]);
}

// LayerNorm with optional bias (bias may be NULL for v1 models)
static void layernorm(float* o, const float* x, const float* weight, const float* bias, int size) {
  float mean = 0.0f;
  for (int j = 0; j < size; j++) mean += x[j];
  mean /= size;
  float var = 0.0f;
  for (int j = 0; j < size; j++) { float d = x[j] - mean; var += d * d; }
  var /= size;
  float s = 1.0f / sqrtf(var + 1e-5f);
  if (bias) {
    for (int j = 0; j < size; j++) o[j] = weight[j] * ((x[j] - mean) * s) + bias[j];
  } else {
    for (int j = 0; j < size; j++) o[j] = weight[j] * ((x[j] - mean) * s);
  }
}

static void softmax(float* x, int size) {
  float max_val = x[0];
  for (int i = 1; i < size; i++) {
    if (x[i] > max_val) max_val = x[i];
  }
  float sum = 0.0f;
  for (int i = 0; i < size; i++) {
    x[i] = expf(x[i] - max_val);
    sum += x[i];
  }
  for (int i = 0; i < size; i++) x[i] /= sum;
}

static void matmul(float* xout, const float* x, const float* w, int n, int d) {
  // w(d,n) @ x(n,) -> xout(d,)
  // Use esp-dsp dot product for SIMD acceleration on S3
  for (int i = 0; i < d; i++) {
    float val = 0.0f;
    dsps_dotprod_f32(x, w + i * n, &val, n);
    xout[i] = val;
  }
}

// Number of scale groups for n_elements with given group_size.
static inline size_t scaleCount(size_t n_elements, int group_size) {
  return ((size_t)n_elements + group_size - 1) / group_size;
}

// Fused INT8 dequantize + matmul: w(d,n) @ x(n,) -> xout(d,)
// Scales are stored in flat (row-major) quantization order: the scale for
// element at flat index k is scales[k / group_size].
//
// Fast path (common case, n % group_size == 0): precomputes per-row scale
// pointer and iterates over groups with a scalar multiply pulled out of the
// inner loop, avoiding an integer division per element.
static void matmul_q8(float* xout, const float* x, const int8_t* w,
                      const float* scales, int group_size, int n, int d) {
  const int n_groups = (n + group_size - 1) / group_size;
  const bool aligned = (n_groups * group_size == n);  // n % group_size == 0

  if (aligned) {
    // Fast path: group boundaries fall on exact element boundaries.
    // The scale for row i, group g is at scales[i * n_groups + g].
    for (int i = 0; i < d; i++) {
      const int8_t* row      = w      + (size_t)i * n;
      const float*  row_sc   = scales + (size_t)i * n_groups;
      float val = 0.0f;
      for (int g = 0; g < n_groups; g++) {
        const float sc      = row_sc[g];
        const int8_t* rg    = row + g * group_size;
        const float*  xg    = x   + g * group_size;
        for (int j = 0; j < group_size; j++) {
          val += (float)rg[j] * xg[j] * sc;
        }
      }
      xout[i] = val;
    }
  } else {
    // General fallback (unusual: n not a multiple of group_size)
    for (int i = 0; i < d; i++) {
      const int8_t* row      = w + (size_t)i * n;
      const size_t  row_base = (size_t)i * n;
      float val = 0.0f;
      for (int j = 0; j < n; j++) {
        val += ((float)row[j] * scales[(row_base + j) / group_size]) * x[j];
      }
      xout[i] = val;
    }
  }
}

// Fused INT4 dequantize + matmul: w_packed(d, ceil(n/2) bytes) @ x(n,) -> xout(d,)
// Nibble packing: low nibble (bits 3:0) = even index, high nibble (bits 7:4) = odd index.
// Signed 4-bit range [-8, 7].  Scales are identical layout to INT8 (per-group FP32).
static void matmul_q4(float* xout, const float* x, const uint8_t* w_packed,
                      const float* scales, int group_size, int n, int d) {
  const int n_groups        = (n + group_size - 1) / group_size;
  const int row_packed_bytes = (n + 1) / 2;

  for (int i = 0; i < d; i++) {
    const uint8_t* row    = w_packed + (size_t)i * row_packed_bytes;
    const float*   row_sc = scales   + (size_t)i * n_groups;
    float val = 0.0f;

    for (int g = 0; g < n_groups; g++) {
      const float  sc    = row_sc[g];
      const int    start = g * group_size;
      const int    end   = (start + group_size > n) ? n : start + group_size;
      for (int j = start; j < end; j++) {
        uint8_t byte = row[j >> 1];
        int8_t  w_val = (j & 1) ? (int8_t)(byte) >> 4         // high nibble (odd)
                                : (int8_t)((byte) << 4) >> 4;  // low nibble (even)
        val += (float)w_val * x[j] * sc;
      }
    }
    xout[i] = val;
  }
}

// Dispatch: calls matmul (FP32), matmul_q4 (INT4 packed), or matmul_q8 (INT8).
static inline void wmatmul(float* xout, const float* x,
                            const float* fp, const int8_t* i8, const float* sc,
                            const uint8_t* q4, const float* q4_sc,
                            int gs, int n, int d) {
  if (fp)       matmul(xout, x, fp, n, d);
  else if (q4)  matmul_q4(xout, x, q4, q4_sc, gs, n, d);
  else          matmul_q8(xout, x, i8, sc, gs, n, d);
}

// ============================================================================
// 6. Forward Pass Debug Utilities
// ============================================================================

struct VecStats { float vmin, vmax, mean, l2; int nans, infs; };

static VecStats vecstats(const float* v, int n) {
  VecStats s = {v[0], v[0], 0.f, 0.f, 0, 0};
  for (int i = 0; i < n; i++) {
    float x = v[i];
    if (isnan(x)) { s.nans++; continue; }
    if (isinf(x)) { s.infs++; continue; }
    if (x < s.vmin) s.vmin = x;
    if (x > s.vmax) s.vmax = x;
    s.mean += x;
    s.l2 += x * x;
  }
  int valid = n - s.nans - s.infs;
  if (valid > 0) { s.mean /= valid; s.l2 = sqrtf(s.l2 / valid); }
  return s;
}

// Position gating: log pos 0, 1, then every 8th. All layers, full volume.
#define FORWARD_DBG_POS(pos) ((pos) <= 1 || ((pos) % 8 == 0))

// ── Generation-time prompt diagnostics ──────────────────────────────────────
// Tracks which prompt tokens the model attends to during generation.
// Populated during llmGenerate(), read by forward() for detailed attention logging.
struct PromptDiagnostics {
  int*   tokens;            // prompt token IDs (points into prompt_tokens array)
  int    count;             // number of prompt tokens
  int    num_prompt_tokens; // same as count (for clarity in forward)
  bool   active;            // set during generation only

  // Embedding similarity matrix (populated during prompt processing)
  // Stores cosine similarity between adjacent prompt token embeddings
  // and between content tokens and each other
  float  emb_norms[32];    // L2 norms of prompt token embeddings (capped at 32)
  float  emb_dots[32];     // dot products between consecutive prompt token embeddings
};

static PromptDiagnostics gPromptDiag = {};

// ============================================================================
// 7. Forward Pass
// ============================================================================

static float* forward(int token, int pos) {
  LLMConfig* p = &gLLM.config;
  TransformerWeights* w = &gLLM.weights;
  RunState* s = &gLLM.state;
  const int S = gLLM.seq_ctx;
  const bool isGPT2 = (p->arch_type == 1);

  int dim = p->dim;
  int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
  int kv_mul = p->n_heads / p->n_kv_heads;
  int hidden_dim = p->hidden_dim;
  int head_size = dim / p->n_heads;
  const int gs = p->group_size;

  if (pos == 0) {
    DEBUG_LLM_FORWARDF("[LLM] forward pos=0 token=%d arch=%s dim=%d gs=%d",
                       token, isGPT2 ? "GPT2" : "Llama", dim, gs);
  }

  // Copy (or dequantize) token embedding row into activation
  if (w->token_embedding_table) {
    memcpy(s->x, w->token_embedding_table + token * dim, dim * sizeof(float));
  } else {
    // INT8 mode: dequantize single row on the fly.
    // Use flat index (token*dim + i) to find the correct scale group,
    // which handles group_size > dim (e.g. gs=128 with dim=64).
    const int8_t* row      = w->emb_i8 + (size_t)token * dim;
    const size_t  flat_base = (size_t)token * dim;
    for (int i = 0; i < dim; i++) s->x[i] = (float)row[i] * w->emb_sc[(flat_base + i) / gs];

    if (pos == 0) {
      // Dump scale groups used for the first token's embedding row, and
      // the first few dequantized values so scale correctness is visible.
      size_t sc_idx_first = flat_base / gs;
      size_t sc_idx_last  = (flat_base + dim - 1) / gs;
      DEBUG_LLM_FORWARDF("[LLM] emb token=%d flat_base=%u sc_groups=[%u..%u] "
                         "scales=[%.4f..%.4f] x[0..3]=[%.3f,%.3f,%.3f,%.3f]",
                         token, (unsigned)flat_base,
                         (unsigned)sc_idx_first, (unsigned)sc_idx_last,
                         w->emb_sc[sc_idx_first], w->emb_sc[sc_idx_last],
                         dim > 0 ? s->x[0] : 0.f, dim > 1 ? s->x[1] : 0.f,
                         dim > 2 ? s->x[2] : 0.f, dim > 3 ? s->x[3] : 0.f);
    }
  }

  // Capture raw embedding (before pos encoding) for prompt diagnostics
  // We compute the L2 norm and dot product with previous token's embedding
  // to check whether the model can distinguish different prompt tokens.
  if (gPromptDiag.active && pos < gPromptDiag.num_prompt_tokens && pos < 32) {
    float norm = 0.f;
    for (int i = 0; i < dim; i++) norm += s->x[i] * s->x[i];
    gPromptDiag.emb_norms[pos] = sqrtf(norm);

    // Dot product with previous token's embedding (stored in xb2 as temp)
    if (pos > 0 && pos < 32) {
      float dot = 0.f;
      for (int i = 0; i < dim; i++) dot += s->x[i] * s->xb2[i];
      gPromptDiag.emb_dots[pos] = dot;

      // Cosine similarity
      float prev_norm = gPromptDiag.emb_norms[pos - 1];
      float cur_norm  = gPromptDiag.emb_norms[pos];
      float cosim = (prev_norm > 1e-8f && cur_norm > 1e-8f) ?
                    dot / (prev_norm * cur_norm) : 0.f;
      //DEBUG_LLM_FORWARDF("[LLM] emb_sim: tok[%d]=%d vs tok[%d]=%d  cosine=%.4f  dot=%.3f  norms=[%.3f,%.3f]",
      //                   pos - 1, gPromptDiag.tokens[pos - 1],
      //                   pos, token,
      //                   cosim, dot, prev_norm, cur_norm);
    }
    // Save current raw embedding into xb2 for next token's comparison
    memcpy(s->xb2, s->x, dim * sizeof(float));
  }

  // GPT-2: add learned positional embedding
  if (isGPT2 && w->pos_embedding_table) {
    float* pe = w->pos_embedding_table + pos * dim;
    for (int i = 0; i < dim; i++) s->x[i] += pe[i];
  }

  // Debug: post-embedding activation health
  if (FORWARD_DBG_POS(pos)) {
    VecStats es = vecstats(s->x, dim);
    //DEBUG_LLM_FORWARDF("[LLM] pos=%d emb+pe: min=%.3f max=%.3f mean=%.3f L2=%.3f nan=%d inf=%d",
    //                   pos, es.vmin, es.vmax, es.mean, es.l2, es.nans, es.infs);
    if (es.nans || es.infs) {
      DEBUG_LLM_FORWARDF("[LLM] CRITICAL: NaN/Inf in embedding at pos=%d token=%d!", pos, token);
    }
  }

  // Forward through all layers
  int fwd_q8_li = 0;  // Q8 layer index for mixed mode (= l for pure INT8)

  for (int l = 0; l < p->n_layers; l++) {
    const bool isQ4Layer = (w->layer_quant && w->layer_quant[l] == 2);

    // Attention norm (LayerNorm for GPT-2, RMSNorm for Llama)
    if (isGPT2) {
      layernorm(s->xb, s->x, w->rms_att_weight + l * dim, w->rms_att_bias ? w->rms_att_bias + l * dim : nullptr, dim);
    } else {
      rmsnorm(s->xb, s->x, w->rms_att_weight + l * dim, dim);
    }

    // Key and value point to the KV cache
    int loff = l * S * kv_dim;
    float* key_cache_row = s->key_cache + loff + pos * kv_dim;
    float* value_cache_row = s->value_cache + loff + pos * kv_dim;

    // FP32 offsets (used when qt==0; FP32 ptrs are null otherwise)
    size_t fp_lD2  = (size_t)l * dim * dim;
    size_t fp_lDkv = (size_t)l * dim * kv_dim;
    size_t fp_lDH  = (size_t)l * (size_t)dim * hidden_dim;

    // Q8 offsets: indexed by fwd_q8_li (= l for pure INT8; sparse for mixed mode)
    size_t q8_lD2   = (size_t)fwd_q8_li * dim * dim;
    size_t q8_lDkv  = (size_t)fwd_q8_li * dim * kv_dim;
    size_t q8_lDH   = (size_t)fwd_q8_li * (size_t)dim * hidden_dim;
    size_t q8_scWQ  = gs ? q8_lD2  / gs : 0;
    size_t q8_scWKV = gs ? q8_lDkv / gs : 0;
    size_t q8_scWO  = q8_scWQ;
    size_t q8_scWDH = gs ? q8_lDH  / gs : 0;

    // Q4 pointers for this layer (null unless this is a Q4 layer)
    const uint8_t* q4wq = nullptr; const float* q4wq_s = nullptr;
    const uint8_t* q4wk = nullptr; const float* q4wk_s = nullptr;
    const uint8_t* q4wv = nullptr; const float* q4wv_s = nullptr;
    const uint8_t* q4wo = nullptr; const float* q4wo_s = nullptr;
    const uint8_t* q4w1 = nullptr; const float* q4w1_s = nullptr;
    const uint8_t* q4w2 = nullptr; const float* q4w2_s = nullptr;
    const uint8_t* q4w3 = nullptr; const float* q4w3_s = nullptr;

    if (isQ4Layer) {
      const auto& off = w->q4_offsets[l];
      q4wq = w->q4_data + off.wq_data;  q4wq_s = w->q4_scales + off.wq_sc;
      q4wk = w->q4_data + off.wk_data;  q4wk_s = w->q4_scales + off.wk_sc;
      q4wv = w->q4_data + off.wv_data;  q4wv_s = w->q4_scales + off.wv_sc;
      q4wo = w->q4_data + off.wo_data;  q4wo_s = w->q4_scales + off.wo_sc;
      q4w1 = w->q4_data + off.w1_data;  q4w1_s = w->q4_scales + off.w1_sc;
      q4w2 = w->q4_data + off.w2_data;  q4w2_s = w->q4_scales + off.w2_sc;
      q4w3 = w->q4_data + off.w3_data;  q4w3_s = w->q4_scales + off.w3_sc;
    }

    // Q8 pointers (null if this is a Q4 layer)
    const int8_t* i8_wq  = (!isQ4Layer && w->wq_i8)  ? w->wq_i8  + q8_lD2   : nullptr;
    const float*  sc_wq  = (!isQ4Layer && w->wq_sc)   ? w->wq_sc  + q8_scWQ   : nullptr;
    const int8_t* i8_wk  = (!isQ4Layer && w->wk_i8)   ? w->wk_i8  + q8_lDkv  : nullptr;
    const float*  sc_wk  = (!isQ4Layer && w->wk_sc)    ? w->wk_sc  + q8_scWKV  : nullptr;
    const int8_t* i8_wv  = (!isQ4Layer && w->wv_i8)   ? w->wv_i8  + q8_lDkv  : nullptr;
    const float*  sc_wv  = (!isQ4Layer && w->wv_sc)    ? w->wv_sc  + q8_scWKV  : nullptr;
    const int8_t* i8_wo  = (!isQ4Layer && w->wo_i8)   ? w->wo_i8  + q8_lD2   : nullptr;
    const float*  sc_wo  = (!isQ4Layer && w->wo_sc)    ? w->wo_sc  + q8_scWO   : nullptr;

    // QKV matmuls
    wmatmul(s->q,          s->xb,
            w->wq  ? w->wq  + fp_lD2  : nullptr, i8_wq, sc_wq,
            q4wq, q4wq_s, gs, dim, dim);
    wmatmul(key_cache_row, s->xb,
            w->wk  ? w->wk  + fp_lDkv : nullptr, i8_wk, sc_wk,
            q4wk, q4wk_s, gs, dim, kv_dim);
    wmatmul(value_cache_row, s->xb,
            w->wv  ? w->wv  + fp_lDkv : nullptr, i8_wv, sc_wv,
            q4wv, q4wv_s, gs, dim, kv_dim);

    // Debug: Q/K/V matmul outputs
    if (FORWARD_DBG_POS(pos)) {
      VecStats qs = vecstats(s->q, dim);
      VecStats ks = vecstats(key_cache_row, kv_dim);
      VecStats vs = vecstats(value_cache_row, kv_dim);
      //DEBUG_LLM_FORWARDF("[LLM] L%d pos=%d Q: [%.3f,%.3f] L2=%.3f  K: [%.3f,%.3f] L2=%.3f  V: [%.3f,%.3f] L2=%.3f",
      //                   l, pos, qs.vmin, qs.vmax, qs.l2, ks.vmin, ks.vmax, ks.l2, vs.vmin, vs.vmax, vs.l2);
      if (qs.nans || qs.infs || ks.nans || ks.infs || vs.nans || vs.infs) {
        DEBUG_LLM_FORWARDF("[LLM] CRITICAL: NaN/Inf in QKV at L%d pos=%d! Q:%d/%d K:%d/%d V:%d/%d",
                           l, pos, qs.nans, qs.infs, ks.nans, ks.infs, vs.nans, vs.infs);
      }
    }

    // RoPE relative positional encoding (Llama only — GPT-2 uses absolute pos embeddings)
    if (!isGPT2) {
      for (int i = 0; i < dim; i += 2) {
        int head_dim = i % head_size;
        float freq = 1.0f / powf(10000.0f, head_dim / (float)head_size);
        float val = pos * freq;
        float fcr = cosf(val);
        float fci = sinf(val);
        int rotn = (i < kv_dim) ? 2 : 1;  // rotate q and k
        for (int v = 0; v < rotn; v++) {
          float* vec = (v == 0) ? s->q : key_cache_row;
          float v0 = vec[i];
          float v1 = vec[i + 1];
          vec[i]     = v0 * fcr - v1 * fci;
          vec[i + 1] = v0 * fci + v1 * fcr;
        }
      }
    }

    // Multi-head attention
    for (int h = 0; h < p->n_heads; h++) {
      float* q_h = s->q + h * head_size;
      float* att_h = s->att + h * S;
      // Iterate over all timesteps including current
      for (int t = 0; t <= pos; t++) {
        float* k = s->key_cache + loff + t * kv_dim + (h / kv_mul) * head_size;
        float score = 0.0f;
        dsps_dotprod_f32(q_h, k, &score, head_size);
        score /= sqrtf((float)head_size);
        att_h[t] = score;
      }
      // Softmax over attention scores
      softmax(att_h, pos + 1);
      // Weighted sum of values
      float* xb_h = s->xb + h * head_size;
      memset(xb_h, 0, head_size * sizeof(float));
      for (int t = 0; t <= pos; t++) {
        float* v = s->value_cache + loff + t * kv_dim + (h / kv_mul) * head_size;
        float a = att_h[t];
        for (int i = 0; i < head_size; i++) {
          xb_h[i] += a * v[i];
        }
      }
    }

    // Debug: attention pattern diagnostics
    if (FORWARD_DBG_POS(pos)) {
      float worst_entropy = 999.f;
      int worst_head = 0;
      float max_attn_weight = 0.f;
      int max_attn_head = 0;
      for (int h = 0; h < p->n_heads; h++) {
        float* att_h = s->att + h * S;
        float h_entropy = 0.f, h_max = 0.f;
        for (int t = 0; t <= pos; t++) {
          float a = att_h[t];
          if (a > h_max) h_max = a;
          if (a > 1e-8f) h_entropy -= a * log2f(a);
        }
        if (h_entropy < worst_entropy) { worst_entropy = h_entropy; worst_head = h; }
        if (h_max > max_attn_weight) { max_attn_weight = h_max; max_attn_head = h; }
      }
      //DEBUG_LLM_FORWARDF("[LLM] L%d pos=%d attn: worst_entropy=%.2f(h%d) max_w=%.3f(h%d) ctx=%d",
      //                   l, pos, worst_entropy, worst_head, max_attn_weight, max_attn_head, pos + 1);
      (void)worst_entropy; (void)worst_head; (void)max_attn_weight; (void)max_attn_head;

      // ── DEEP ATTENTION HEATMAP ──────────────────────────────────────────────
      // At the first generation position (pos == num_prompt_tokens - 1) and
      // every 8th generated position, dump full per-head attention weights to
      // each prompt token. This reveals whether the model actually attends to
      // content tokens (e.g. "wifi", "off") or ignores them.
      if (gPromptDiag.active && pos >= gPromptDiag.num_prompt_tokens - 1) {
        int npt = gPromptDiag.num_prompt_tokens;
        for (int h = 0; h < p->n_heads; h++) {
          float* att_h = s->att + h * S;

          // Sum attention on prompt region vs generated region
          float prompt_attn = 0.f, gen_attn = 0.f;
          for (int t = 0; t <= pos; t++) {
            if (t < npt) prompt_attn += att_h[t];
            else         gen_attn    += att_h[t];
          }

          // Find top-2 attended prompt positions
          int top1_pos = 0, top2_pos = 0;
          float top1_w = -1.f, top2_w = -1.f;
          for (int t = 0; t < npt && t <= pos; t++) {
            if (att_h[t] > top1_w) {
              top2_pos = top1_pos; top2_w = top1_w;
              top1_pos = t;        top1_w = att_h[t];
            } else if (att_h[t] > top2_w) {
              top2_pos = t;        top2_w = att_h[t];
            }
          }

          // Get token names for readability
          const char* top1_name = (gPromptDiag.tokens && top1_pos < npt &&
            gPromptDiag.tokens[top1_pos] >= 0 &&
            gPromptDiag.tokens[top1_pos] < gLLM.tokenizer.vocab_size) ?
            gLLM.tokenizer.vocab[gPromptDiag.tokens[top1_pos]] : "?";
          const char* top2_name = (gPromptDiag.tokens && top2_pos < npt &&
            gPromptDiag.tokens[top2_pos] >= 0 &&
            gPromptDiag.tokens[top2_pos] < gLLM.tokenizer.vocab_size) ?
            gLLM.tokenizer.vocab[gPromptDiag.tokens[top2_pos]] : "?";

          //DEBUG_LLM_FORWARDF("[LLM] L%d pos=%d h%d attn_to_prompt=%.3f attn_to_gen=%.3f "
          //                   "top1=pos%d'%s'(%.3f) top2=pos%d'%s'(%.3f)",
          //                   l, pos, h, prompt_attn, gen_attn,
          //                   top1_pos, top1_name, top1_w,
          //                   top2_pos, top2_name, top2_w);
          (void)prompt_attn; (void)gen_attn; (void)top1_pos; (void)top2_pos;
          (void)top1_w; (void)top2_w; (void)top1_name; (void)top2_name;

          // For the first generation position only, dump attention to EVERY prompt token
          // This is the most critical diagnostic — shows exactly what the model "sees"
          if (pos == npt - 1 && l % 5 == 0) { // every 5th layer to keep output manageable
            char attn_buf[256];
            int bpos = 0;
            bpos += snprintf(attn_buf + bpos, sizeof(attn_buf) - bpos,
                             "[LLM] L%d h%d HEATMAP:", l, h);
            for (int t = 0; t < npt && t <= pos; t++) {
              const char* tname = (gPromptDiag.tokens[t] >= 0 &&
                gPromptDiag.tokens[t] < gLLM.tokenizer.vocab_size) ?
                gLLM.tokenizer.vocab[gPromptDiag.tokens[t]] : "?";
              // Truncate token name for display
              char short_name[8];
              snprintf(short_name, sizeof(short_name), "%s", tname);
              bpos += snprintf(attn_buf + bpos, sizeof(attn_buf) - bpos,
                               " %s=%.2f", short_name, att_h[t]);
              if (bpos >= (int)sizeof(attn_buf) - 20) break;
            }
            //DEBUG_LLM_FORWARDF("%s", attn_buf);
          }
        }
      }
    }

    // Output projection
    wmatmul(s->xb2, s->xb,
            w->wo  ? w->wo  + fp_lD2 : nullptr, i8_wo, sc_wo,
            q4wo, q4wo_s, gs, dim, dim);

    // Residual connection
    for (int i = 0; i < dim; i++) s->x[i] += s->xb2[i];

    // Debug: post-attention residual
    if (FORWARD_DBG_POS(pos)) {
      VecStats rs = vecstats(s->x, dim);
      //DEBUG_LLM_FORWARDF("[LLM] L%d pos=%d post_attn_res: [%.3f,%.3f] mean=%.3f L2=%.3f nan=%d",
      //                   l, pos, rs.vmin, rs.vmax, rs.mean, rs.l2, rs.nans + rs.infs);
      (void)rs;
    }

    // FFN norm (LayerNorm for GPT-2, RMSNorm for Llama)
    if (isGPT2) {
      layernorm(s->xb, s->x, w->rms_ffn_weight + l * dim, w->rms_ffn_bias ? w->rms_ffn_bias + l * dim : nullptr, dim);
    } else {
      rmsnorm(s->xb, s->x, w->rms_ffn_weight + l * dim, dim);
    }

    // FFN Q8 pointers for w1/w2/w3 (null if Q4 layer)
    const int8_t* i8_w1 = (!isQ4Layer && w->w1_i8) ? w->w1_i8 + q8_lDH : nullptr;
    const float*  sc_w1 = (!isQ4Layer && w->w1_sc)  ? w->w1_sc + q8_scWDH : nullptr;
    const int8_t* i8_w2 = (!isQ4Layer && w->w2_i8) ? w->w2_i8 + q8_lDH : nullptr;
    const float*  sc_w2 = (!isQ4Layer && w->w2_sc)  ? w->w2_sc + q8_scWDH : nullptr;
    const int8_t* i8_w3 = (!isQ4Layer && w->w3_i8) ? w->w3_i8 + q8_lDH : nullptr;
    const float*  sc_w3 = (!isQ4Layer && w->w3_sc)  ? w->w3_sc + q8_scWDH : nullptr;

    if (isGPT2) {
      // GPT-2 FFN: up projection → GELU → down projection (no gate)
      wmatmul(s->hb, s->xb,
              w->w3  ? w->w3  + fp_lDH : nullptr, i8_w3, sc_w3,
              q4w3, q4w3_s, gs, dim, hidden_dim);

      // Debug: pre-GELU activation range (dead neurons = all near-zero or clamped)
      if (FORWARD_DBG_POS(pos)) {
        VecStats pg = vecstats(s->hb, hidden_dim);
        int dead = 0;
        for (int i = 0; i < hidden_dim; i++) if (fabsf(s->hb[i]) < 1e-6f) dead++;
        //DEBUG_LLM_FORWARDF("[LLM] L%d pos=%d preGELU: [%.3f,%.3f] L2=%.3f dead=%d/%d nan=%d",
        //                   l, pos, pg.vmin, pg.vmax, pg.l2, dead, hidden_dim, pg.nans + pg.infs);
        (void)pg; (void)dead;
      }

      // GELU (tanh approximation — matches HF gelu_new)
      for (int i = 0; i < hidden_dim; i++) {
        float x = s->hb[i];
        s->hb[i] = 0.5f * x * (1.0f + tanhf(GELU_COEFF_A * (x + GELU_COEFF_B * x * x * x)));
      }

      // Debug: post-GELU
      if (FORWARD_DBG_POS(pos)) {
        VecStats ag = vecstats(s->hb, hidden_dim);
        //DEBUG_LLM_FORWARDF("[LLM] L%d pos=%d postGELU: [%.3f,%.3f] L2=%.3f",
        //                   l, pos, ag.vmin, ag.vmax, ag.l2);
        (void)ag;
      }

      wmatmul(s->xb, s->hb,
              w->w2  ? w->w2  + fp_lDH : nullptr, i8_w2, sc_w2,
              q4w2, q4w2_s, gs, hidden_dim, dim);
    } else {
      // Llama FFN: SwiGLU (gate * silu(up)) → down
      wmatmul(s->hb,  s->xb,
              w->w1  ? w->w1  + fp_lDH : nullptr, i8_w1, sc_w1,
              q4w1, q4w1_s, gs, dim, hidden_dim);
      wmatmul(s->hb2, s->xb,
              w->w3  ? w->w3  + fp_lDH : nullptr, i8_w3, sc_w3,
              q4w3, q4w3_s, gs, dim, hidden_dim);
      for (int i = 0; i < hidden_dim; i++) {
        float val = s->hb[i];
        val *= (1.0f / (1.0f + expf(-val)));
        val *= s->hb2[i];
        s->hb[i] = val;
      }
      wmatmul(s->xb,  s->hb,
              w->w2  ? w->w2  + fp_lDH : nullptr, i8_w2, sc_w2,
              q4w2, q4w2_s, gs, hidden_dim, dim);
    }

    // Residual
    for (int i = 0; i < dim; i++) s->x[i] += s->xb[i];

    // Debug: post-FFN residual stream health — track growth across layers.
    // Healthy: L2 grows slowly. Exploding: L2 doubles+ per layer. Vanishing: L2 → 0.
    if (FORWARD_DBG_POS(pos)) {
      VecStats fs = vecstats(s->x, dim);
      //DEBUG_LLM_FORWARDF("[LLM] L%d pos=%d post_ffn_res: [%.3f,%.3f] mean=%.3f L2=%.3f nan=%d",
      //                   l, pos, fs.vmin, fs.vmax, fs.mean, fs.l2, fs.nans + fs.infs);
      if (fs.nans || fs.infs) {
        DEBUG_LLM_FORWARDF("[LLM] CRITICAL: NaN/Inf in residual at L%d pos=%d!", l, pos);
      }
    }

    if (!isQ4Layer) fwd_q8_li++;
  }

  // Final norm (LayerNorm for GPT-2, RMSNorm for Llama)
  if (isGPT2) {
    layernorm(s->x, s->x, w->rms_final_weight, w->rms_final_bias, dim);
  } else {
    rmsnorm(s->x, s->x, w->rms_final_weight, dim);
  }

  // Debug: post-final-norm activation
  if (FORWARD_DBG_POS(pos)) {
    VecStats fn = vecstats(s->x, dim);
    //DEBUG_LLM_FORWARDF("[LLM] pos=%d final_norm: [%.3f,%.3f] mean=%.3f L2=%.3f nan=%d bias=%s",
    //                   pos, fn.vmin, fn.vmax, fn.mean, fn.l2, fn.nans + fn.infs,
    //                   (isGPT2 && w->rms_final_bias) ? "yes" : "no");
    (void)fn;
  }

  // Classifier into logits (always FP32 or Q8, never Q4)
  wmatmul(s->logits, s->x,
          w->wcls, w->wcls_i8, w->wcls_sc,
          nullptr, nullptr, gs, dim, p->vocab_size);

  // Dump logit distribution stats — healthy model has wide spread and clear top candidates.
  // Flat/uniform logits = broken weights. NaN = numerical explosion.
  if (FORWARD_DBG_POS(pos)) {
    float lmin = s->logits[0], lmax = s->logits[0], lsum = 0.f;
    int top_id = 0, nan_count = 0;
    for (int i = 0; i < p->vocab_size; i++) {
      float v = s->logits[i];
      if (isnan(v) || isinf(v)) { nan_count++; continue; }
      if (v < lmin) lmin = v;
      if (v > lmax) { lmax = v; top_id = i; }
      lsum += v;
    }
    // Also find top-5 for visibility into what the model is actually predicting
    int top5[5] = {0,0,0,0,0};
    float top5v[5] = {-1e30f,-1e30f,-1e30f,-1e30f,-1e30f};
    for (int i = 0; i < p->vocab_size; i++) {
      float v = s->logits[i];
      if (isnan(v) || isinf(v)) continue;
      for (int k = 0; k < 5; k++) {
        if (v > top5v[k]) {
          for (int m = 4; m > k; m--) { top5[m] = top5[m-1]; top5v[m] = top5v[m-1]; }
          top5[k] = i; top5v[k] = v;
          break;
        }
      }
    }
    DEBUG_LLM_FORWARDF("[LLM] pos=%d logits: [%.2f,%.2f] mean=%.2f spread=%.2f nan=%d top=%d(%.2f)",
                       pos, lmin, lmax, lsum / (p->vocab_size - nan_count),
                       lmax - lmin, nan_count, top_id, lmax);
    DEBUG_LLM_FORWARDF("[LLM] pos=%d top5: %d(%.2f) %d(%.2f) %d(%.2f) %d(%.2f) %d(%.2f)",
                       pos, top5[0], top5v[0], top5[1], top5v[1], top5[2], top5v[2],
                       top5[3], top5v[3], top5[4], top5v[4]);
  }

  return s->logits;
}

// ============================================================================
// 8. Sampling
// ============================================================================

static int sample_argmax(const float* probabilities, int n) {
  int max_i = 0;
  float max_p = probabilities[0];
  for (int i = 1; i < n; i++) {
    if (probabilities[i] > max_p) {
      max_i = i;
      max_p = probabilities[i];
    }
  }
  return max_i;
}

static int sample_topp(float* probabilities, int n, float topp) {
  // Top-p (nucleus) sampling: only consider the smallest set of tokens whose
  // cumulative probability exceeds topp.  This prunes the long tail of low-
  // probability tokens that cause garbled / random output.
  //
  // We need to track (prob, original_index) pairs.  Allocate a small index
  // array on the heap — for vocab 8192 this is 32KB, well within budget since
  // we free it immediately.  We do a partial selection sort, accumulating mass
  // until cumsum >= topp, then sample from the nucleus only.

  // Build index array alongside probabilities
  int* indices = (int*)malloc(n * sizeof(int));
  if (!indices) {
    DEBUG_LLM_GENERATEF("[LLM] sample_topp: OOM allocating %d indices, falling back to categorical", n);
    // OOM fallback: plain categorical
    float r = (float)esp_random() / (float)UINT32_MAX;
    float cdf = 0.0f;
    for (int i = 0; i < n; i++) {
      cdf += probabilities[i];
      if (cdf > r) return i;
    }
    return n - 1;
  }
  for (int i = 0; i < n; i++) indices[i] = i;

  float cumsum = 0.0f;
  int nucleus_n = 0;

  // Partial selection sort by descending probability.  Pull the largest
  // probability to position [i], stop once cumulative mass >= topp.
  // For a typical peaked distribution this is ~50–200 iterations, not 8192.
  for (int i = 0; i < n && cumsum < topp; i++) {
    // Find the max in the unsorted tail [i..n)
    int max_idx = i;
    float max_val = probabilities[i];
    for (int j = i + 1; j < n; j++) {
      if (probabilities[j] > max_val) {
        max_idx = j;
        max_val = probabilities[j];
      }
    }
    // Swap both probabilities and indices
    if (max_idx != i) {
      float tmp_p = probabilities[i];
      probabilities[i] = probabilities[max_idx];
      probabilities[max_idx] = tmp_p;
      int tmp_i = indices[i];
      indices[i] = indices[max_idx];
      indices[max_idx] = tmp_i;
    }
    cumsum += probabilities[i];
    nucleus_n = i + 1;
  }

  // Debug: log nucleus stats and top candidates
  DEBUG_LLM_GENERATEF("[LLM] top-p: nucleus=%d/%d tokens, cumsum=%.4f (target=%.2f)",
                      nucleus_n, n, cumsum, topp);
  // Log top 5 candidates in the nucleus
  int dbg_n = (nucleus_n < 5) ? nucleus_n : 5;
  for (int di = 0; di < dbg_n; di++) {
    DEBUG_LLM_GENERATEF("[LLM]   nucleus[%d]: tok=%d prob=%.4f (%.1f%%)",
                        di, indices[di], probabilities[di], probabilities[di] * 100.0f);
  }

  // Sample from the nucleus only (re-normalised by cumsum)
  float r = (float)esp_random() / (float)UINT32_MAX * cumsum;
  float cdf = 0.0f;
  int result = indices[nucleus_n - 1]; // fallback to last in nucleus
  int result_rank = nucleus_n - 1;
  for (int i = 0; i < nucleus_n; i++) {
    cdf += probabilities[i];
    if (cdf > r) { result = indices[i]; result_rank = i; break; }
  }

  DEBUG_LLM_GENERATEF("[LLM]   sampled tok=%d at rank=%d/%d (r=%.4f)",
                      result, result_rank, nucleus_n, r / cumsum);

  free(indices);
  return result;
}

static int sample(float* logits, int vocab_size, float temperature, float topp) {
  if (temperature == 0.0f) {
    int tok = sample_argmax(logits, vocab_size);
    DEBUG_LLM_GENERATEF("[LLM] sample: greedy (temp=0) -> tok=%d logit=%.2f", tok, logits[tok]);
    return tok;
  }

  // Clamp logits before temperature scaling to prevent saturation from INT8
  // accumulation errors or extreme activations compounding into ±Inf after division
  for (int q = 0; q < vocab_size; q++) {
    if (logits[q] > LOGIT_CLAMP_MAX) logits[q] = LOGIT_CLAMP_MAX;
    if (logits[q] < LOGIT_CLAMP_MIN) logits[q] = LOGIT_CLAMP_MIN;
  }

  // Apply temperature
  for (int q = 0; q < vocab_size; q++) {
    logits[q] /= temperature;
  }

  // Compute pre-softmax stats for debug
  float pre_max = logits[0], pre_min = logits[0];
  for (int q = 1; q < vocab_size; q++) {
    if (logits[q] > pre_max) pre_max = logits[q];
    if (logits[q] < pre_min) pre_min = logits[q];
  }

  // Softmax
  softmax(logits, vocab_size);

  // Post-softmax: find max prob and compute entropy estimate
  float max_prob = 0.0f;
  int max_prob_id = 0;
  float entropy = 0.0f;
  for (int q = 0; q < vocab_size; q++) {
    if (logits[q] > max_prob) { max_prob = logits[q]; max_prob_id = q; }
    if (logits[q] > 1e-8f) entropy -= logits[q] * log2f(logits[q]);
  }
  DEBUG_LLM_GENERATEF("[LLM] sample: temp=%.2f topp=%.2f pre_logit=[%.1f,%.1f] top_prob=%.3f(tok=%d) entropy=%.1f bits",
                      temperature, topp, pre_min, pre_max, max_prob, max_prob_id, entropy);

  if (topp <= 0.0f || topp >= 1.0f) {
    // Simple random sample (no top-p filtering)
    //DEBUG_LLM_GENERATEF("[LLM] sample: categorical (topp=%.2f, no nucleus filter)", topp);
    float r = (float)esp_random() / (float)UINT32_MAX;
    float cdf = 0.0f;
    for (int i = 0; i < vocab_size; i++) {
      cdf += logits[i];
      if (cdf > r) return i;
    }
    return vocab_size - 1;
  }

  return sample_topp(logits, vocab_size, topp);
}

// Mirostat v2 sampling — adaptive surprise targeting.
// Maintains a running estimate `mu` of the distribution's perplexity per step.
// Tokens whose individual surprise (-log2 p) exceeds mu are excluded, then we
// sample from the remainder and update mu toward `tau` bits of target surprise.
//
// logits: raw logits (modified in place — apply temperature scaling + softmax here)
// mu:     persistent state across tokens within one generation (init to 2*tau)
// tau:    target surprise in bits (typical 3–7; higher = more diverse output)
// eta:    learning rate for mu update (typical 0.05–0.2)
static int sample_mirostat2(float* logits, int n, float temperature, float tau, float eta, float* mu) {
  if (temperature <= 0.0f) return sample_argmax(logits, n);

  // Apply temperature and convert to probabilities
  for (int i = 0; i < n; i++) logits[i] /= temperature;
  softmax(logits, n);  // logits now holds probabilities

  // Exclude tokens more surprising than mu bits: threshold = 2^(-mu)
  float threshold = powf(2.0f, -(*mu));

  // Sum probability mass of included tokens
  float included_sum = 0.0f;
  for (int i = 0; i < n; i++) {
    if (logits[i] >= threshold) included_sum += logits[i];
  }

  // Debug: log Mirostat state
  int included_count = 0;
  for (int i = 0; i < n; i++) if (logits[i] >= threshold) included_count++;
  DEBUG_LLM_GENERATEF("[LLM] mirostat2: mu=%.3f threshold=%.6f included=%d/%d mass=%.4f tau=%.1f eta=%.2f",
                      *mu, threshold, included_count, n, included_sum, tau, eta);

  // If nothing passes the surprise threshold, mu has drifted too low (threshold ≈ 1.0).
  // Reset mu to 2*tau and sample from the full distribution rather than collapsing to
  // argmax — argmax would corrupt mu further and create a feedback death spiral.
  if (included_sum <= 0.0f) {
    DEBUG_LLM_GENERATEF("[LLM] mirostat2: RESET mu from %.3f to %.3f (death spiral prevention)", *mu, 2.0f * tau);
    *mu = 2.0f * tau;
    return sample_argmax(logits, n);
  }

  // Sample from included tokens proportionally
  float r = ((float)esp_random() / (float)UINT32_MAX) * included_sum;
  float cdf = 0.0f;
  int chosen = -1;
  for (int i = 0; i < n; i++) {
    if (logits[i] >= threshold) {
      cdf += logits[i];
      if (chosen < 0 && r <= cdf) chosen = i;
    }
  }
  // Fallback if float rounding left chosen unset
  if (chosen < 0) {
    for (int i = 0; i < n; i++) {
      if (logits[i] >= threshold) { chosen = i; break; }
    }
  }
  if (chosen < 0) return sample_argmax(logits, n);

  // Update mu: error = (surprise of chosen token in bits) - tau
  float p_chosen = logits[chosen];  // still original softmax probability
  if (p_chosen > 0.0f) {
    float surprise_bits = -log2f(p_chosen);
    float old_mu = *mu;
    *mu -= eta * (surprise_bits - tau);
    // Clamp to a sane range
    if (*mu < 0.01f) *mu = 0.01f;
    if (*mu > tau * 20.0f) *mu = tau * 20.0f;
    DEBUG_LLM_GENERATEF("[LLM] mirostat2: chose tok=%d p=%.4f surprise=%.2f bits mu: %.3f -> %.3f",
                        chosen, p_chosen, surprise_bits, old_mu, *mu);
  }

  return chosen;
}

// ============================================================================
// 9. Tokenizer — merge-based BPE (LLM1 embedded format)
// ============================================================================

// Simple open-addressing hash map for merge lookups
static int mergeLookup(const TokenizerState* t, uint32_t left_id, uint32_t right_id,
                       uint32_t* out_merged, int* out_priority) {
  if (!t->merge_map || t->merge_map_capacity == 0) return 0;
  uint32_t key = (left_id << 16) | (right_id & 0xFFFF);
  uint32_t idx = key % (uint32_t)t->merge_map_capacity;
  for (int probe = 0; probe < t->merge_map_capacity; probe++) {
    MergeLookup* e = &t->merge_map[(idx + probe) % t->merge_map_capacity];
    if (e->key == 0 && e->merged_id == 0 && e->priority == 0) return 0;
    if (e->key == key) {
      *out_merged = e->merged_id;
      *out_priority = e->priority;
      return 1;
    }
  }
  return 0;
}

static void mergeMapInsert(TokenizerState* t, uint32_t left_id, uint32_t right_id,
                           uint32_t merged_id, int priority) {
  uint32_t key = (left_id << 16) | (right_id & 0xFFFF);
  uint32_t idx = key % (uint32_t)t->merge_map_capacity;
  for (int probe = 0; probe < t->merge_map_capacity; probe++) {
    MergeLookup* e = &t->merge_map[(idx + probe) % t->merge_map_capacity];
    if (e->key == 0 && e->merged_id == 0 && e->priority == 0) {
      e->key = key;
      e->merged_id = merged_id;
      e->priority = priority;
      return;
    }
  }
}

// Load tokenizer from embedded blob in an open LLM1 file.
// File position must be at the tok_byte_len field (offset 64).
static bool loadTokenizerFromFile(File& f) {
  TokenizerState* t = &gLLM.tokenizer;

  // Read blob size
  uint32_t tok_byte_len = 0;
  if (f.read((uint8_t*)&tok_byte_len, 4) != 4) return false;

  size_t blobStart = f.position();

  // Read tokenizer header
  uint32_t tok_vocab_size = 0, merge_count = 0;
  f.read((uint8_t*)&tok_vocab_size, 4);
  f.read((uint8_t*)&merge_count, 4);

  if (tok_vocab_size == 0 || tok_vocab_size > 131072) {
    ERROR_LLMF("Bad tokenizer vocab_size: %u", tok_vocab_size);
    return false;
  }

  t->vocab_size = (int)tok_vocab_size;
  t->merge_count = (int)merge_count;

  // Allocate vocab pointer array
  t->vocab = (char**)llmPsramAlloc(tok_vocab_size * sizeof(char*), "tok.vocab");
  if (!t->vocab) return false;

  // First pass: calculate total string pool size
  size_t vocabStart = f.position();
  size_t totalStringBytes = 0;
  for (uint32_t i = 0; i < tok_vocab_size; i++) {
    uint8_t byte_len = 0;
    f.read(&byte_len, 1);
    totalStringBytes += byte_len + 1; // +1 for null terminator
    if (byte_len > 0) f.seek(f.position() + byte_len);
  }

  // Allocate string pool
  char* stringPool = (char*)llmPsramAlloc(totalStringBytes, "tok.strings");
  if (!stringPool) return false;
  gLLM.tokenizerData = stringPool;

  // Second pass: read vocab strings
  f.seek(vocabStart);
  char* poolPtr = stringPool;
  for (uint32_t i = 0; i < tok_vocab_size; i++) {
    uint8_t byte_len = 0;
    f.read(&byte_len, 1);
    t->vocab[i] = poolPtr;
    if (byte_len > 0) {
      f.read((uint8_t*)poolPtr, byte_len);
    }
    poolPtr[byte_len] = '\0';
    poolPtr += byte_len + 1;
  }

  // Build byte_to_token lookup (single-byte vocab entries)
  // and collect multi-byte tokens for pre-split matching
  memset(t->byte_to_token, -1, sizeof(t->byte_to_token));
  int multiByteCount = 0;
  for (uint32_t i = 0; i < tok_vocab_size; i++) {
    const char* s = t->vocab[i];
    int slen = strlen(s);
    if (slen == 1) {
      t->byte_to_token[(uint8_t)s[0]] = (int)i;
    } else if (slen >= 2 && slen <= 8) {
      // Candidate for pre-split — count first, allocate after
      multiByteCount++;
    }
  }

  // Pre-split table is built AFTER merge table is loaded (see below)
  t->presplit = nullptr;
  t->presplit_count = 0;

  // Read merge table
  t->merges = nullptr;
  t->merge_map = nullptr;
  t->merge_map_capacity = 0;
  if (merge_count > 0) {
    t->merges = (MergeEntry*)llmPsramAlloc(merge_count * sizeof(MergeEntry), "tok.merges");
    if (!t->merges) return false;

    for (uint32_t i = 0; i < merge_count; i++) {
      f.read((uint8_t*)&t->merges[i].left_id, 4);
      f.read((uint8_t*)&t->merges[i].right_id, 4);
      f.read((uint8_t*)&t->merges[i].merged_id, 4);
    }

    // Build merge hash map (2x capacity for low collision rate)
    t->merge_map_capacity = (int)(merge_count * 2);
    if (t->merge_map_capacity < 16) t->merge_map_capacity = 16;
    t->merge_map = (MergeLookup*)llmPsramAlloc(
      t->merge_map_capacity * sizeof(MergeLookup), "tok.mergemap");
    if (!t->merge_map) return false;
    memset(t->merge_map, 0, t->merge_map_capacity * sizeof(MergeLookup));

    for (uint32_t i = 0; i < merge_count; i++) {
      mergeMapInsert(t, t->merges[i].left_id, t->merges[i].right_id,
                     t->merges[i].merged_id, (int)i);
    }
  }

  // Build pre-split table: only tokens that BPE merges CANNOT produce.
  // HuggingFace "added tokens" (like Q: and A:) are matched as whole strings
  // before BPE runs. On device we replicate this by pre-splitting only those
  // tokens whose ID never appears as a mergedId in any merge rule — meaning
  // BPE has no way to construct them from component tokens.
  if (multiByteCount > 0 && merge_count > 0) {
    // Build a set of token IDs that BPE merges can produce
    // Use a simple boolean array (vocab is small enough)
    bool* bpeReachable = (bool*)calloc(tok_vocab_size, sizeof(bool));
    if (bpeReachable) {
      for (uint32_t i = 0; i < merge_count; i++) {
        int mid = t->merges[i].merged_id;
        if (mid >= 0 && mid < (int)tok_vocab_size) {
          bpeReachable[mid] = true;
        }
      }

      // Count how many multi-byte tokens are NOT reachable by BPE
      int unreachableCount = 0;
      for (uint32_t i = 0; i < tok_vocab_size; i++) {
        const char* s = t->vocab[i];
        int slen = strlen(s);
        if (slen >= 2 && slen <= 8 && !bpeReachable[i]) {
          // Check all bytes are individually representable
          bool allMapped = true;
          for (int j = 0; j < slen; j++) {
            if (t->byte_to_token[(uint8_t)s[j]] == -1) { allMapped = false; break; }
          }
          if (allMapped) unreachableCount++;
        }
      }

      if (unreachableCount > 0) {
        t->presplit = (PreSplitToken*)llmPsramAlloc(
          unreachableCount * sizeof(PreSplitToken), "tok.presplit");
        if (t->presplit) {
          int idx = 0;
          for (uint32_t i = 0; i < tok_vocab_size; i++) {
            const char* s = t->vocab[i];
            int slen = strlen(s);
            if (slen < 2 || slen > 8 || bpeReachable[i]) continue;
            bool allMapped = true;
            for (int j = 0; j < slen; j++) {
              if (t->byte_to_token[(uint8_t)s[j]] == -1) { allMapped = false; break; }
            }
            if (!allMapped) continue;
            t->presplit[idx].str = s;
            t->presplit[idx].id = (int)i;
            t->presplit[idx].len = slen;
            idx++;
          }
          t->presplit_count = idx;
          // Sort by length descending so longer matches take priority
          for (int a = 0; a < idx - 1; a++) {
            for (int b = a + 1; b < idx; b++) {
              if (t->presplit[b].len > t->presplit[a].len) {
                PreSplitToken tmp = t->presplit[a];
                t->presplit[a] = t->presplit[b];
                t->presplit[b] = tmp;
              }
            }
          }
        }
      }
      free(bpeReachable);
    }
    DEBUG_LLM_TOKENIZERF("[LLM] Pre-split tokens: %d entries (from %d multi-byte vocab)", t->presplit_count, multiByteCount);
    for (int j = 0; j < t->presplit_count && j < 10; j++) {
      DEBUG_LLM_TOKENIZERF("[LLM]   presplit[%d] id=%d len=%d \"%s\"",
                            j, t->presplit[j].id, t->presplit[j].len, t->presplit[j].str);
    }
  }

  // Verify we consumed the right amount of the tokenizer blob
  size_t expectedEnd = blobStart + tok_byte_len;
  if (f.position() != expectedEnd) {
    DEBUG_LLM_TOKENIZERF("[LLM] Tokenizer blob pos mismatch: at %u, expected %u",
                         (unsigned)f.position(), (unsigned)expectedEnd);
    f.seek(expectedEnd);
  }

  DEBUG_LLM_TOKENIZERF("[LLM] Tokenizer loaded: vocab_size=%d merges=%d", t->vocab_size, t->merge_count);
  return true;
}

static void freeTokenizer() {
  TokenizerState* t = &gLLM.tokenizer;
  llmPsramFree((void**)&t->vocab);
  llmPsramFree((void**)&t->merges);
  llmPsramFree((void**)&t->merge_map);
  llmPsramFree((void**)&t->presplit);
  llmPsramFree((void**)&gLLM.tokenizerData);
  memset(t, 0, sizeof(TokenizerState));
}

// Encode a string into tokens using merge-based BPE
static int encode(const char* text, int* tokens, int maxTokens) {
  TokenizerState* t = &gLLM.tokenizer;
  int n_tokens = 0;

  DEBUG_LLM_TOKENIZERF("[LLM][encode] Input (%d chars): \"%.*s%s\"",
                        (int)strlen(text),
                        (int)(strlen(text) > 100 ? 100 : strlen(text)), text,
                        strlen(text) > 100 ? "..." : "");
  DEBUG_LLM_TOKENIZERF("[LLM][encode] Presplit table has %d entries", t->presplit_count);

  // Step 1: scan input, matching multi-byte presplit tokens first,
  // then falling back to single-byte token mapping.
  // This mirrors HuggingFace's handling of added/special tokens:
  // they are matched as whole strings before BPE runs.
  const char* c = text;
  int presplit_hits = 0;
  while (*c != '\0' && n_tokens < maxTokens) {
    // Try presplit tokens (longest first)
    bool matched = false;
    for (int p = 0; p < t->presplit_count; p++) {
      if (strncmp(c, t->presplit[p].str, t->presplit[p].len) == 0) {
        tokens[n_tokens++] = t->presplit[p].id;
        DEBUG_LLM_TOKENIZERF("[LLM][encode] PRESPLIT MATCH at pos %d: \"%s\" -> id=%d",
                              (int)(c - text), t->presplit[p].str, t->presplit[p].id);
        c += t->presplit[p].len;
        matched = true;
        presplit_hits++;
        break;
      }
    }
    if (matched) continue;

    // Single-byte fallback
    int id = t->byte_to_token[(uint8_t)*c];
    if (id != -1) {
      tokens[n_tokens++] = id;
    } else {
      DEBUG_LLM_TOKENIZERF("[LLM][encode] Unmapped byte 0x%02X '%c' at pos %d — skipped",
                            (uint8_t)*c, (*c >= 32 && *c < 127) ? *c : '?', (int)(c - text));
    }
    c++;
  }

  DEBUG_LLM_TOKENIZERF("[LLM][encode] After step 1 (presplit+bytes): %d tokens, %d presplit hits",
                        n_tokens, presplit_hits);

  // Debug: dump pre-BPE token list
  {
    int show = (n_tokens < 30) ? n_tokens : 30;
    for (int i = 0; i < show; i++) {
      const char* piece = (tokens[i] >= 0 && tokens[i] < t->vocab_size) ?
                           t->vocab[tokens[i]] : "?";
      DEBUG_LLM_TOKENIZERF("[LLM][encode]   pre-bpe[%d] = %d \"%s\"", i, tokens[i], piece);
    }
    if (n_tokens > 30) {
      DEBUG_LLM_TOKENIZERF("[LLM][encode]   ... (%d more)", n_tokens - 30);
    }
  }

  if (n_tokens < 2 || t->merge_count == 0) return n_tokens;

  // Step 2: repeatedly apply the highest-priority (lowest index) applicable merge
  int merge_rounds = 0;
  bool changed = true;
  while (changed) {
    changed = false;
    int best_priority = INT_MAX;
    int best_pos = -1;
    uint32_t best_merged = 0;

    for (int i = 0; i < n_tokens - 1; i++) {
      uint32_t merged;
      int priority;
      if (mergeLookup(t, (uint32_t)tokens[i], (uint32_t)tokens[i + 1], &merged, &priority)) {
        if (priority < best_priority) {
          best_priority = priority;
          best_pos = i;
          best_merged = merged;
        }
      }
    }

    if (best_pos >= 0) {
      if (merge_rounds < 20) {
        const char* lpiece = (tokens[best_pos] >= 0 && tokens[best_pos] < t->vocab_size) ?
                              t->vocab[tokens[best_pos]] : "?";
        const char* rpiece = (tokens[best_pos+1] >= 0 && tokens[best_pos+1] < t->vocab_size) ?
                              t->vocab[tokens[best_pos+1]] : "?";
        const char* mpiece = ((int)best_merged >= 0 && (int)best_merged < t->vocab_size) ?
                              t->vocab[best_merged] : "?";
        DEBUG_LLM_TOKENIZERF("[LLM][encode] BPE merge #%d: pos=%d \"%s\"(%d)+\"%s\"(%d) -> \"%s\"(%u) pri=%d",
                              merge_rounds, best_pos, lpiece, tokens[best_pos],
                              rpiece, tokens[best_pos+1], mpiece, best_merged, best_priority);
      }
      tokens[best_pos] = (int)best_merged;
      for (int i = best_pos + 1; i < n_tokens - 1; i++) {
        tokens[i] = tokens[i + 1];
      }
      n_tokens--;
      changed = true;
      merge_rounds++;
    }
  }

  DEBUG_LLM_TOKENIZERF("[LLM][encode] After step 2 (BPE): %d tokens, %d merge rounds", n_tokens, merge_rounds);

  // Debug: dump final token list with special token flags
  {
    int show = (n_tokens < 30) ? n_tokens : 30;
    for (int i = 0; i < show; i++) {
      const char* piece = (tokens[i] >= 0 && tokens[i] < t->vocab_size) ?
                           t->vocab[tokens[i]] : "?";
      const char* tag = "";
      if (tokens[i] == 3) tag = " <<<< Q: SPECIAL TOKEN";
      else if (tokens[i] == 4) tag = " <<<< A: SPECIAL TOKEN";
      else if (tokens[i] <= 4) tag = " (special)";
      DEBUG_LLM_TOKENIZERF("[LLM][encode]   final[%d] = %d \"%s\"%s", i, tokens[i], piece, tag);
    }
    if (n_tokens > 30) {
      DEBUG_LLM_TOKENIZERF("[LLM][encode]   ... (%d more)", n_tokens - 30);
    }
  }

  return n_tokens;
}

static const char* decode(int prev_token, int token) {
  TokenizerState* t = &gLLM.tokenizer;
  if (token < 0 || token >= t->vocab_size) return "";
  const char* piece = t->vocab[token];
  // Handle raw byte tokens like <0x0A>
  if (piece[0] == '<' && piece[1] == '0' && piece[2] == 'x') {
    static char byte_buf[2];
    unsigned int byte_val;
    sscanf(piece + 1, "0x%02x", &byte_val);
    byte_buf[0] = (char)byte_val;
    byte_buf[1] = '\0';
    return byte_buf;
  }
  return piece;
}

// ============================================================================
// 10. Model Loading (LLM1 format)
// ============================================================================

static bool llmValidationErr(char* err, size_t errLen, const char* msg) {
  if (err && errLen) snprintf(err, errLen, "%s", msg);
  return false;
}

static bool validateLlmConfig(const LLMConfig* p, char* err, size_t errLen) {
  if (p->dim < 32 || p->dim > 4096) {
    return llmValidationErr(err, errLen, "Invalid model: dim out of range");
  }
  if (p->hidden_dim < 32 || p->hidden_dim > 16384) {
    return llmValidationErr(err, errLen, "Invalid model: hidden_dim out of range");
  }
  if (p->n_layers < 1 || p->n_layers > 128) {
    return llmValidationErr(err, errLen, "Invalid model: n_layers out of range");
  }
  if (p->n_heads < 1 || p->n_heads > 128 || p->n_kv_heads < 1 || p->n_kv_heads > 128) {
    return llmValidationErr(err, errLen, "Invalid model: head counts out of range");
  }
  if (p->dim % p->n_heads != 0) {
    return llmValidationErr(err, errLen, "Invalid model: dim not divisible by n_heads");
  }
  if (p->n_heads % p->n_kv_heads != 0) {
    return llmValidationErr(err, errLen, "Invalid model: n_heads not divisible by n_kv_heads");
  }
  if (p->vocab_size < 64 || p->vocab_size > 131072) {
    return llmValidationErr(err, errLen, "Invalid model: vocab_size out of range");
  }
  if (p->seq_len < 1 || p->seq_len > 8192) {
    return llmValidationErr(err, errLen, "Invalid model: seq_len out of range");
  }
  if (p->quant_type > 2) {
    return llmValidationErr(err, errLen, "Invalid model: unknown quant_type (expected 0, 1, or 2)");
  }
  if ((p->quant_type == 1 || p->quant_type == 2) && p->group_size == 0) {
    return llmValidationErr(err, errLen, "Invalid model: quantized model with group_size=0");
  }
  if (p->quant_type == 2 && p->file_version < 3) {
    return llmValidationErr(err, errLen, "Invalid model: INT4_MIXED requires file version >= 3");
  }
  if (p->quant_type == 2 && (p->n_q8_start + p->n_q8_end) > p->n_layers) {
    return llmValidationErr(err, errLen, "Invalid model: n_q8_start + n_q8_end > n_layers");
  }
  if (p->arch_type > 1) {
    return llmValidationErr(err, errLen, "Unsupported model: unknown arch_type (expected 0=Llama or 1=GPT-2)");
  }
  if (p->arch_type == 1 && p->n_kv_heads != p->n_heads) {
    return llmValidationErr(err, errLen, "Invalid GPT-2 model: n_kv_heads must equal n_heads (no GQA)");
  }
  return true;
}

// Read chunked data from LittleFS into a buffer, yielding periodically.
static bool readChunked(File& f, uint8_t* dest, size_t bytes) {
  const size_t chunkSize = READ_CHUNK_SIZE;
  size_t remaining = bytes;
  while (remaining > 0) {
    size_t toRead = (remaining < chunkSize) ? remaining : chunkSize;
    size_t got = f.read(dest, toRead);
    if (got == 0) return false;
    dest += got;
    remaining -= got;
    if (remaining % (64 * 1024) < chunkSize) vTaskDelay(1);
  }
  return true;
}

// Read a single tensor from file into an FP32 destination buffer.
// If the file stores INT8, dequantizes on the fly.
// force_fp32: norm tensors are always FP32 in file regardless of quant_type.
static bool readTensor(File& f, float* dest, uint32_t expected_elements,
                       uint8_t quant_type, uint16_t group_size, bool force_fp32) {
  uint32_t n_elements = 0;
  if (f.read((uint8_t*)&n_elements, 4) != 4) return false;

  if (n_elements != expected_elements) {
    ERROR_LLMF("Tensor size mismatch: got %u, expected %u at offset %u",
               n_elements, expected_elements, (unsigned)(f.position() - 4));
    return false;
  }

  if (quant_type == 0 || force_fp32) {
    // FP32: read directly
    return readChunked(f, (uint8_t*)dest, n_elements * sizeof(float));
  }

  // INT8: read scales, then dequantize int8 values
  uint32_t n_groups = (n_elements + group_size - 1) / group_size;

  // Read scales into temp buffer (heap, not stack — could be large)
  float* scales = (float*)malloc(n_groups * sizeof(float));
  if (!scales) return false;
  if (!readChunked(f, (uint8_t*)scales, n_groups * sizeof(float))) {
    free(scales);
    return false;
  }

  // Read and dequantize one group at a time
  int8_t* tmp = (int8_t*)malloc(group_size);
  if (!tmp) { free(scales); return false; }

  for (uint32_t g = 0; g < n_groups; g++) {
    uint32_t start = g * (uint32_t)group_size;
    uint32_t count = ((n_elements - start) < group_size) ? (n_elements - start) : group_size;
    if (f.read((uint8_t*)tmp, count) != count) {
      free(scales); free(tmp);
      return false;
    }
    float scale = scales[g];
    for (uint32_t i = 0; i < count; i++) {
      dest[start + i] = (float)tmp[i] * scale;
    }
    if (g % 64 == 0) vTaskDelay(1);
  }

  free(scales);
  free(tmp);
  return true;
}

// Read an INT8 tensor directly into pre-allocated int8 data and float scale buffers.
// For use when quant_type==1 and the tensor is NOT a norm (i.e., not force_fp32).
static bool readTensorQ8(File& f, int8_t* dest_data, float* dest_scales,
                          uint32_t expected_elements, uint16_t group_size) {
  uint32_t n_elements = 0;
  if (f.read((uint8_t*)&n_elements, 4) != 4) return false;
  if (n_elements != expected_elements) {
    ERROR_LLMF("Tensor Q8 size mismatch: got %u, expected %u at offset %u",
               n_elements, expected_elements, (unsigned)(f.position() - 4));
    return false;
  }
  uint32_t n_groups = (n_elements + group_size - 1) / group_size;
  if (!readChunked(f, (uint8_t*)dest_scales, n_groups * sizeof(float))) return false;
  if (!readChunked(f, (uint8_t*)dest_data, n_elements)) return false;
  return true;
}

// Read an INT4 nibble-packed tensor directly into pre-allocated packed uint8 + float scale buffers.
// Nibble packing: low nibble = even index, high nibble = odd index.  Signed [-8, 7].
static bool readTensorQ4(File& f, uint8_t* dest_packed, float* dest_scales,
                          uint32_t expected_elements, uint16_t group_size) {
  uint32_t n_elements = 0;
  if (f.read((uint8_t*)&n_elements, 4) != 4) return false;
  if (n_elements != expected_elements) {
    ERROR_LLMF("Tensor Q4 size mismatch: got %u, expected %u at offset %u",
               n_elements, expected_elements, (unsigned)(f.position() - 4));
    return false;
  }
  uint32_t n_groups      = (n_elements + group_size - 1) / group_size;
  uint32_t packed_bytes  = (n_elements + 1) / 2;
  if (!readChunked(f, (uint8_t*)dest_scales, n_groups * sizeof(float))) return false;
  if (!readChunked(f, dest_packed, packed_bytes)) return false;
  return true;
}

// Compute the file size of a tensor block in the LLM1 file.
// tensorQt: per-tensor quant (0=FP32, 1=INT8, 2=INT4).  For VERSION<=2 pass
// the global quant_type; for VERSION=3 pass the per-tensor quant.
static size_t tensorFileSize(uint32_t n_elements, uint8_t tensorQt, uint16_t group_size, bool is_norm) {
  size_t sz = 4; // uint32 element count prefix
  if (tensorQt == 0 || is_norm) {
    sz += (size_t)n_elements * 4;                             // FP32
  } else if (tensorQt == 2) {
    uint32_t n_groups = (n_elements + group_size - 1) / group_size;
    sz += (size_t)n_groups * 4 + ((size_t)n_elements + 1) / 2; // scales + packed nibbles
  } else {
    uint32_t n_groups = (n_elements + group_size - 1) / group_size;
    sz += (size_t)n_groups * 4 + (size_t)n_elements;          // scales + INT8 data
  }
  return sz;
}

// Shared context passed between loadWeights helper functions
struct LoadContext {
  int D, H, L, V, kv_dim;
  uint8_t qt;
  uint16_t gs;
  bool isGPT2, hasNormBias, v3;
  bool shared_weights;  // set by prescanTiedWeights
  int pfx;  // per-tensor prefix byte count (1 for v3, 0 otherwise)
  size_t weightsBytes, weightsQ8Bytes;
  size_t weightsQ4Bytes, weightsQ4ScBytes, mixedMetaBytes;
  size_t kvCacheSize, hotSize, coldSize;
};

// Parse the 64-byte LLM1 header and validate the config.
// On success, gLLM.config is populated and ctx derived fields are set.
static bool parseModelHeader(File& f, LoadContext& ctx) {
  uint8_t hdr[64];
  if (f.read(hdr, 64) != 64) {
    snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Failed to read LLM1 header");
    return false;
  }

  uint32_t magic = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                   ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];
  if (magic != LLM1_MAGIC) {
    snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg),
             "Not an LLM1 file (magic=0x%08lX, expected 0x4C4C4D31)", (unsigned long)magic);
    return false;
  }

  uint8_t version = hdr[4];
  if (version < 1 || version > 3) {
    snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Unsupported LLM1 version: %u (supported: 1-3)", version);
    return false;
  }

  LLMConfig* p = &gLLM.config;
  p->file_version = version;
  p->quant_type = hdr[5];
  memcpy(&p->group_size, &hdr[6], 2);
  uint16_t tmp16;
  memcpy(&tmp16, &hdr[8], 2);  p->dim = tmp16;
  memcpy(&tmp16, &hdr[10], 2); p->hidden_dim = tmp16;
  p->n_layers = hdr[12];
  p->n_heads = hdr[13];
  p->n_kv_heads = hdr[14];
  uint32_t tmp32;
  memcpy(&tmp32, &hdr[15], 4); p->vocab_size = (int)tmp32;
  memcpy(&tmp16, &hdr[19], 2); p->seq_len = tmp16;
  p->arch_type = hdr[21];
  p->n_q8_start = (version >= 3) ? hdr[22] : 0;
  p->n_q8_end   = (version >= 3) ? hdr[23] : 0;

  if (!validateLlmConfig(p, gLLM.errorMsg, sizeof(gLLM.errorMsg)))
    return false;

  // Populate derived context fields
  ctx.D = p->dim;  ctx.H = p->hidden_dim;  ctx.L = p->n_layers;  ctx.V = p->vocab_size;
  ctx.kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
  ctx.qt = p->quant_type;  ctx.gs = p->group_size;
  ctx.isGPT2 = (p->arch_type == 1);
  ctx.hasNormBias = (version >= 2 && ctx.isGPT2);  // GPT-2 LayerNorm biases present in all v2+ models
  ctx.v3 = (version >= 3);
  ctx.pfx = ctx.v3 ? 1 : 0;

  return true;
}

// Pre-scan past all tensor data to read the tied-weights flag, then seek back.
// Sets ctx.shared_weights and returns the tensor data start position.
static bool prescanTiedWeights(File& f, LoadContext& ctx) {
  LLMConfig* p = &gLLM.config;
  const int D = ctx.D, H = ctx.H, L = ctx.L, V = ctx.V;
  const uint8_t qt = ctx.qt;
  const uint16_t gs = ctx.gs;
  const int kv_dim = ctx.kv_dim;
  const int pfx = ctx.pfx;

  size_t tensorDataStart = f.position();

  DEBUG_LLM_LOADF("[LLM] LLM1 v%d model: dim=%d hidden=%d layers=%d heads=%d kv_heads=%d vocab=%d seq=%d quant=%s",
                  p->file_version, D, H, L, p->n_heads, p->n_kv_heads,
                  V, p->seq_len, qt == 2 ? "MIXED(Q4/Q8)" : (qt == 1 ? "INT8" : "FP32"));
  if (qt == 2) {
    DEBUG_LLM_LOADF("[LLM] Mixed policy: first %d + last %d layers INT8, middle %d layers INT4",
                    p->n_q8_start, p->n_q8_end, L - p->n_q8_start - p->n_q8_end);
  }

  size_t tensorDataSize = 0;

  if (qt == 2) {
    const int nQ8s = p->n_q8_start, nQ8e = p->n_q8_end;
    tensorDataSize += pfx + tensorFileSize(V * D, 1, gs, false);
    if (ctx.isGPT2) tensorDataSize += pfx + tensorFileSize(p->seq_len * D, 0, gs, true);
    for (int l = 0; l < L; l++) {
      uint8_t layerQt = (l < nQ8s || l >= L - nQ8e) ? 1 : 2;
      int gateElements = ctx.isGPT2 ? 1 : H * D;
      tensorDataSize += pfx + tensorFileSize(D, 0, gs, true);
      if (ctx.hasNormBias) tensorDataSize += pfx + tensorFileSize(D, 0, gs, true);
      tensorDataSize += pfx + tensorFileSize(D * D, layerQt, gs, false);
      tensorDataSize += pfx + tensorFileSize(D * kv_dim, layerQt, gs, false);
      tensorDataSize += pfx + tensorFileSize(D * kv_dim, layerQt, gs, false);
      tensorDataSize += pfx + tensorFileSize(D * D, layerQt, gs, false);
      tensorDataSize += pfx + tensorFileSize(D, 0, gs, true);
      if (ctx.hasNormBias) tensorDataSize += pfx + tensorFileSize(D, 0, gs, true);
      tensorDataSize += pfx + tensorFileSize(gateElements, ctx.isGPT2 ? 0 : layerQt, gs, ctx.isGPT2);
      tensorDataSize += pfx + tensorFileSize(H * D, layerQt, gs, false);
      tensorDataSize += pfx + tensorFileSize(D * H, layerQt, gs, false);
    }
    tensorDataSize += pfx + tensorFileSize(D, 0, gs, true);
    if (ctx.hasNormBias) tensorDataSize += pfx + tensorFileSize(D, 0, gs, true);
  } else {
    tensorDataSize += tensorFileSize(V * D, qt, gs, false);
    if (ctx.isGPT2) tensorDataSize += tensorFileSize(p->seq_len * D, qt, gs, true);
    int gateElements = ctx.isGPT2 ? 1 : H * D;
    for (int l = 0; l < L; l++) {
      tensorDataSize += tensorFileSize(D, qt, gs, true);
      if (ctx.hasNormBias) tensorDataSize += tensorFileSize(D, qt, gs, true);
      tensorDataSize += tensorFileSize(D * D, qt, gs, false);
      tensorDataSize += tensorFileSize(D * kv_dim, qt, gs, false);
      tensorDataSize += tensorFileSize(D * kv_dim, qt, gs, false);
      tensorDataSize += tensorFileSize(D * D, qt, gs, false);
      tensorDataSize += tensorFileSize(D, qt, gs, true);
      if (ctx.hasNormBias) tensorDataSize += tensorFileSize(D, qt, gs, true);
      tensorDataSize += tensorFileSize(gateElements, qt, gs, ctx.isGPT2);
      tensorDataSize += tensorFileSize(H * D, qt, gs, false);
      tensorDataSize += tensorFileSize(D * H, qt, gs, false);
    }
    tensorDataSize += tensorFileSize(D, qt, gs, true);
    if (ctx.hasNormBias) tensorDataSize += tensorFileSize(D, qt, gs, true);
  }

  f.seek(tensorDataStart + tensorDataSize);
  uint8_t tied_flag = 1;
  f.read(&tied_flag, 1);
  ctx.shared_weights = (tied_flag != 0);
  DEBUG_LLM_LOADF("[LLM] Weights tied=%d", ctx.shared_weights);

  f.seek(tensorDataStart);
  return true;
}

// Compute PSRAM memory requirements for all weight blocks and run state.
// Populates ctx size fields and checks PSRAM budget. Returns false if OOM.
static bool computeMemoryLayout(LoadContext& ctx) {
  LLMConfig* p = &gLLM.config;
  const int D = ctx.D, H = ctx.H, L = ctx.L, V = ctx.V;
  const uint8_t qt = ctx.qt;
  const uint16_t gs = ctx.gs;
  const int kv_dim = ctx.kv_dim;

  // Start with model's seq_len, capped by user request if given
  int seq_ctx = p->seq_len;
  if (gLLM.requestedMaxCtx > 0 && seq_ctx > gLLM.requestedMaxCtx) seq_ctx = gLLM.requestedMaxCtx;

  ctx.weightsBytes = 0;
  ctx.weightsQ8Bytes = 0;
  ctx.weightsQ4Bytes = 0;
  ctx.weightsQ4ScBytes = 0;
  ctx.mixedMetaBytes = 0;

  if (qt == 0) {
    size_t w1Floats = ctx.isGPT2 ? (size_t)L : (size_t)L * H * D;
    size_t weightsFloats = (size_t)V * D
      + (ctx.isGPT2 ? (size_t)p->seq_len * D : 0)
      + (size_t)L * D
      + (size_t)L * D * D
      + (size_t)L * D * kv_dim
      + (size_t)L * D * kv_dim
      + (size_t)L * D * D
      + (size_t)L * D
      + w1Floats
      + (size_t)L * D * H
      + (size_t)L * H * D
      + (size_t)D
      + (ctx.hasNormBias ? (size_t)L * D * 2 + D : 0);
    if (!ctx.shared_weights) weightsFloats += (size_t)V * D;
    ctx.weightsBytes = weightsFloats * sizeof(float);
  } else if (qt == 1) {
    size_t normsFloats = (size_t)L * D + (size_t)L * D + D
      + (ctx.isGPT2 ? (size_t)p->seq_len * D + L : 0)
      + (ctx.hasNormBias ? (size_t)L * D * 2 + D : 0);
    size_t scalesFloats = scaleCount((size_t)V * D, gs)
      + (size_t)L * scaleCount((size_t)D * D, gs)
      + (size_t)L * scaleCount((size_t)D * kv_dim, gs)
      + (size_t)L * scaleCount((size_t)D * kv_dim, gs)
      + (size_t)L * scaleCount((size_t)D * D, gs)
      + (ctx.isGPT2 ? 0 : (size_t)L * scaleCount((size_t)H * D, gs))
      + (size_t)L * scaleCount((size_t)D * H, gs)
      + (size_t)L * scaleCount((size_t)H * D, gs);
    if (!ctx.shared_weights) scalesFloats += scaleCount((size_t)V * D, gs);
    ctx.weightsBytes = (normsFloats + scalesFloats) * sizeof(float);
    ctx.weightsQ8Bytes = (size_t)V * D
      + (size_t)L * D * D
      + (size_t)L * D * kv_dim
      + (size_t)L * D * kv_dim
      + (size_t)L * D * D
      + (ctx.isGPT2 ? 0 : (size_t)L * H * D)
      + (size_t)L * D * H
      + (size_t)L * H * D;
    if (!ctx.shared_weights) ctx.weightsQ8Bytes += (size_t)V * D;
  }

  if (qt == 2) {
    const int nQ8s = p->n_q8_start, nQ8e = p->n_q8_end;
    const int nQ8 = nQ8s + nQ8e, nQ4 = L - nQ8;

    size_t normsFloats = (size_t)L * D + (size_t)L * D + D
      + (ctx.isGPT2 ? (size_t)p->seq_len * D + L : 0)
      + (ctx.hasNormBias ? (size_t)L * D * 2 + D : 0);

    size_t scalesFloats = scaleCount((size_t)V * D, gs)
      + (size_t)L * scaleCount((size_t)D * D, gs)
      + (size_t)L * scaleCount((size_t)D * kv_dim, gs)
      + (size_t)L * scaleCount((size_t)D * kv_dim, gs)
      + (size_t)L * scaleCount((size_t)D * D, gs)
      + (ctx.isGPT2 ? 0 : (size_t)L * scaleCount((size_t)H * D, gs))
      + (size_t)L * scaleCount((size_t)D * H, gs)
      + (size_t)L * scaleCount((size_t)H * D, gs);
    if (!ctx.shared_weights) scalesFloats += scaleCount((size_t)V * D, gs);

    size_t q8ScalesFloats = scaleCount((size_t)V * D, gs)
      + (size_t)nQ8 * scaleCount((size_t)D * D, gs)
      + (size_t)nQ8 * scaleCount((size_t)D * kv_dim, gs)
      + (size_t)nQ8 * scaleCount((size_t)D * kv_dim, gs)
      + (size_t)nQ8 * scaleCount((size_t)D * D, gs)
      + (ctx.isGPT2 ? 0 : (size_t)nQ8 * scaleCount((size_t)H * D, gs))
      + (size_t)nQ8 * scaleCount((size_t)D * H, gs)
      + (size_t)nQ8 * scaleCount((size_t)H * D, gs);
    if (!ctx.shared_weights) q8ScalesFloats += scaleCount((size_t)V * D, gs);

    size_t q4ScalesFloats = scalesFloats - q8ScalesFloats;

    ctx.weightsBytes = (normsFloats + q8ScalesFloats) * sizeof(float);

    size_t perLayerQ8Data = (size_t)D * D + (size_t)D * kv_dim + (size_t)D * kv_dim + (size_t)D * D
      + (ctx.isGPT2 ? 0 : (size_t)H * D) + (size_t)D * H + (size_t)H * D;
    ctx.weightsQ8Bytes = (size_t)V * D + (size_t)nQ8 * perLayerQ8Data;
    if (!ctx.shared_weights) ctx.weightsQ8Bytes += (size_t)V * D;

    size_t perLayerQ4Packed = ((size_t)D * D + 1) / 2 + ((size_t)D * kv_dim + 1) / 2
      + ((size_t)D * kv_dim + 1) / 2 + ((size_t)D * D + 1) / 2
      + (ctx.isGPT2 ? 0 : ((size_t)H * D + 1) / 2)
      + ((size_t)D * H + 1) / 2 + ((size_t)H * D + 1) / 2;
    ctx.weightsQ4Bytes = (size_t)nQ4 * perLayerQ4Packed;
    ctx.weightsQ4ScBytes = q4ScalesFloats * sizeof(float);
    ctx.mixedMetaBytes = (size_t)L * sizeof(uint8_t) + (size_t)L * sizeof(TransformerWeights::Q4LayerOffsets);

    DEBUG_LLM_MEMORYF("[LLM] Mixed Q4/Q8: nQ8=%d nQ4=%d", nQ8, nQ4);
  }

  // Fixed-size memory (weights, not context-dependent)
  size_t fixedBytes = ctx.weightsBytes + ctx.weightsQ8Bytes + ctx.weightsQ4Bytes
                    + ctx.weightsQ4ScBytes + ctx.mixedMetaBytes;
  ctx.hotSize = (4 * D + 2 * H) * sizeof(float);
  fixedBytes += ctx.hotSize;

  size_t freePSRAM = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  size_t budget = (freePSRAM > LLM_PSRAM_RESERVE_BYTES) ? freePSRAM - LLM_PSRAM_RESERVE_BYTES : 0;

  // Check if even weights alone exceed budget (ctx-independent)
  // coldSize has a fixed V component too
  size_t fixedCold = (size_t)V * sizeof(float);
  if (fixedBytes + fixedCold > budget) {
    snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg),
             "Weights too large for PSRAM: need %uKB, have %uKB (short %uKB)",
             (unsigned)((fixedBytes + fixedCold)/1024), (unsigned)(budget/1024),
             (unsigned)((fixedBytes + fixedCold - budget)/1024));
    return false;
  }

  // Auto-fit: reduce context until total fits in PSRAM
  // Per-context-slot cost: KV cache + attention scores
  size_t perCtxSlot = 2 * L * kv_dim * sizeof(float)   // KV cache per position
                    + p->n_heads * sizeof(float);        // attention scores per position
  size_t remaining = budget - fixedBytes - fixedCold;
  int maxFitCtx = (int)(remaining / perCtxSlot);
  if (maxFitCtx < 1) maxFitCtx = 1;
  if (seq_ctx > maxFitCtx) {
    DEBUG_LLM_MEMORYF("[LLM] Auto-fit: ctx %d -> %d (PSRAM budget %uKB, weights %uKB, per-slot %u bytes)",
                      seq_ctx, maxFitCtx, (unsigned)(budget/1024), (unsigned)(fixedBytes/1024),
                      (unsigned)perCtxSlot);
    seq_ctx = maxFitCtx;
  }

  gLLM.seq_ctx = seq_ctx;
  if (seq_ctx < p->seq_len) {
    DEBUG_LLM_MEMORYF("[LLM] Context: model seq_len=%d -> runtime ctx=%d", p->seq_len, seq_ctx);
  }

  ctx.kvCacheSize = 2 * L * seq_ctx * kv_dim * sizeof(float);
  ctx.coldSize = (p->n_heads * seq_ctx + V) * sizeof(float);
  size_t totalNeeded = fixedBytes + ctx.kvCacheSize + ctx.coldSize;

  if (qt == 2) {
    DEBUG_LLM_MEMORYF("[LLM] Memory (MIXED): fp=%uKB q8=%uKB q4=%uKB q4sc=%uKB kv=%uKB act=%uKB total=%uKB free=%uKB",
                      (unsigned)(ctx.weightsBytes/1024), (unsigned)(ctx.weightsQ8Bytes/1024),
                      (unsigned)(ctx.weightsQ4Bytes/1024), (unsigned)(ctx.weightsQ4ScBytes/1024),
                      (unsigned)(ctx.kvCacheSize/1024), (unsigned)((ctx.hotSize + ctx.coldSize)/1024),
                      (unsigned)(totalNeeded/1024), (unsigned)(freePSRAM/1024));
  } else if (qt == 1) {
    DEBUG_LLM_MEMORYF("[LLM] Memory (INT8): fp_block=%uKB q8_block=%uKB kv=%uKB act=%uKB total=%uKB free=%uKB ctx=%d",
                      (unsigned)(ctx.weightsBytes/1024), (unsigned)(ctx.weightsQ8Bytes/1024),
                      (unsigned)(ctx.kvCacheSize/1024), (unsigned)((ctx.hotSize + ctx.coldSize)/1024),
                      (unsigned)(totalNeeded/1024), (unsigned)(freePSRAM/1024), seq_ctx);
  } else {
    DEBUG_LLM_MEMORYF("[LLM] Memory (FP32): weights=%uKB kv=%uKB act=%uKB total=%uKB free=%uKB ctx=%d",
                      (unsigned)(ctx.weightsBytes/1024), (unsigned)(ctx.kvCacheSize/1024),
                      (unsigned)((ctx.hotSize + ctx.coldSize)/1024), (unsigned)(totalNeeded/1024),
                      (unsigned)(freePSRAM/1024), seq_ctx);
  }

  // Final sanity check (should not fail after auto-fit, but be safe)
  if (totalNeeded + LLM_PSRAM_RESERVE_BYTES > freePSRAM) {
    snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg),
             "Not enough PSRAM: need %uKB + %uKB reserve = %uKB, have %uKB (short %uKB)",
             (unsigned)(totalNeeded/1024), (unsigned)(LLM_PSRAM_RESERVE_BYTES/1024),
             (unsigned)((totalNeeded + LLM_PSRAM_RESERVE_BYTES)/1024),
             (unsigned)(freePSRAM/1024),
             (unsigned)((totalNeeded + LLM_PSRAM_RESERVE_BYTES - freePSRAM)/1024));
    return false;
  }
  return true;
}

// Log min/max/mean for key tensors — cross-reference with converter PACK_SUMMARY.
static void spotCheckWeights(const LoadContext& ctx) {
  const TransformerWeights* w = &gLLM.weights;
  const LLMConfig* p = &gLLM.config;
  const int D = ctx.D, V = ctx.V;
  const uint16_t gs = ctx.gs;
  const bool isQ8  = (ctx.qt == 1);
  const bool isMix = (ctx.qt == 2);

  auto spotCheck = [](const float* data, int n, const char* name) {
    if (!data || n <= 0) return;
    float vmin = data[0], vmax = data[0], vsum = 0.f;
    int nans = 0;
    for (int i = 0; i < n; i++) {
      float v = data[i];
      if (isnan(v) || isinf(v)) { nans++; continue; }
      if (v < vmin) vmin = v;
      if (v > vmax) vmax = v;
      vsum += v;
    }
    int valid = n - nans;
    float mean = valid > 0 ? vsum / valid : 0.f;
    DEBUG_LLM_LOADF("[LLM] SPOT %s (%d): min=%.6f max=%.6f mean=%.6f nan=%d",
                    name, n, vmin, vmax, mean, nans);
  };

  auto spotCheckQ8 = [&](const int8_t* data, const float* scales, int n, int group_sz, const char* name) {
    if (!data || !scales || n <= 0) return;
    int sample_n = (n < 512) ? n : 512;
    float vmin = 1e30f, vmax = -1e30f, vsum = 0.f;
    int8_t imin = data[0], imax = data[0];
    for (int i = 0; i < sample_n; i++) {
      int8_t raw = data[i];
      if (raw < imin) imin = raw;
      if (raw > imax) imax = raw;
      float sc = scales[i / group_sz];
      float deq = (float)raw * sc;
      if (deq < vmin) vmin = deq;
      if (deq > vmax) vmax = deq;
      vsum += deq;
    }
    float sc0 = scales[0];
    float scLast = scales[(sample_n - 1) / group_sz];
    DEBUG_LLM_LOADF("[LLM] SPOT %s (%d, sampled %d): i8=[%d,%d] sc=[%.6f,%.6f] deq=[%.6f,%.6f] mean=%.6f",
                    name, n, sample_n, imin, imax, sc0, scLast, vmin, vmax, vsum / sample_n);
  };

  if (isQ8 || isMix) spotCheckQ8(w->emb_i8, w->emb_sc, V * D, gs, "embedding");
  else               spotCheck(w->token_embedding_table, V * D, "embedding");

  if (ctx.isGPT2 && w->pos_embedding_table)
    spotCheck(w->pos_embedding_table, p->seq_len * D, "pos_embedding");

  spotCheck(w->rms_att_weight, D, "L0_attn_norm");
  if (ctx.hasNormBias) spotCheck(w->rms_att_bias, D, "L0_attn_norm_bias");
  spotCheck(w->rms_ffn_weight, D, "L0_ffn_norm");
  if (ctx.hasNormBias) spotCheck(w->rms_ffn_bias, D, "L0_ffn_norm_bias");

  if (isQ8 || isMix)  spotCheckQ8(w->wq_i8, w->wq_sc, D * D, gs, "L0_wq");
  else if (w->wq)     spotCheck(w->wq, D * D, "L0_wq");

  spotCheck(w->rms_final_weight, D, "final_norm");
  if (ctx.hasNormBias) spotCheck(w->rms_final_bias, D, "final_norm_bias");

  if ((isQ8 || isMix) && w->wcls_i8) spotCheckQ8(w->wcls_i8, w->wcls_sc, V * D, gs, "lm_head");
  else if (w->wcls)                   spotCheck(w->wcls, V * D, "lm_head");

  DEBUG_LLM_LOADF("[LLM] ═══ Compare SPOT values above with converter PACK_SUMMARY ═══");
}

// Allocate hot (internal RAM) and cold (PSRAM) activation buffers.
static bool allocateRunState(const LoadContext& ctx) {
  const LLMConfig* p = &gLLM.config;
  const int D = ctx.D, H = ctx.H, L = ctx.L, V = ctx.V;
  const int kv_dim = ctx.kv_dim;
  const int seq_ctx = gLLM.seq_ctx;

  gLLM.stateHotData = (float*)heap_caps_calloc(1, ctx.hotSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!gLLM.stateHotData) {
    DEBUG_LLM_MEMORYF("[LLM] Internal RAM alloc failed (%uB), falling back to PSRAM for hot state", (unsigned)ctx.hotSize);
    gLLM.stateHotData = (float*)llmPsramAlloc(ctx.hotSize, "llm.state.hot");
    if (!gLLM.stateHotData) return false;
  }
  gLLM.stateHotSize = ctx.hotSize;

  size_t coldStateBytes = ctx.kvCacheSize + ctx.coldSize;
  gLLM.stateData = (float*)llmPsramAlloc(coldStateBytes, "llm.state.cold");
  if (!gLLM.stateData) {
    heap_caps_free(gLLM.stateHotData);
    gLLM.stateHotData = nullptr;
    return false;
  }
  gLLM.stateSize = coldStateBytes;

  RunState* s = &gLLM.state;
  float* hp = gLLM.stateHotData;
  s->x   = hp;  hp += D;
  s->xb  = hp;  hp += D;
  s->xb2 = hp;  hp += D;
  s->q   = hp;  hp += D;
  s->hb  = hp;  hp += H;
  s->hb2 = hp;  hp += H;

  float* cp = gLLM.stateData;
  s->key_cache   = cp;  cp += L * seq_ctx * kv_dim;
  s->value_cache = cp;  cp += L * seq_ctx * kv_dim;
  s->att    = cp;  cp += p->n_heads * seq_ctx;
  s->logits = cp;  cp += V;

  // Pre-allocate repetition penalty ring buffer (reused per generation call)
  int repSize = LLM_DEFAULT_REP_WINDOW;
  if (repSize > 256) repSize = 256;
  if (repSize > 0) {
    gLLM.repBuf = (int*)malloc(repSize * sizeof(int));
    gLLM.repBufSize = gLLM.repBuf ? repSize : 0;
  }

  return true;
}

static bool loadWeights(const char* path) {
  File f = VFS::open(path, "r");
  if (!f) {
    snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Cannot open model: %s", path);
    return false;
  }

  // ---- Parse header ----
  LoadContext ctx = {};
  if (!parseModelHeader(f, ctx)) { f.close(); return false; }

  LLMConfig* p = &gLLM.config;
  int kv_dim = ctx.kv_dim;

  // ---- Read embedded tokenizer ----
  if (!loadTokenizerFromFile(f)) {
    snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Failed to load embedded tokenizer");
    f.close();
    return false;
  }

  // ---- Pre-scan for tied weights and compute memory layout ----
  if (!prescanTiedWeights(f, ctx)) { f.close(); return false; }
  if (!computeMemoryLayout(ctx)) { f.close(); return false; }

  const int D = ctx.D, H = ctx.H, L = ctx.L, V = ctx.V;
  const uint8_t qt = ctx.qt;
  const uint16_t gs = ctx.gs;
  const bool isGPT2 = ctx.isGPT2;
  const bool hasNormBias = ctx.hasNormBias;
  const bool shared_weights = ctx.shared_weights;
  const bool v3 = ctx.v3;
  const size_t weightsBytes = ctx.weightsBytes;
  const size_t weightsQ8Bytes = ctx.weightsQ8Bytes;
  const size_t weightsQ4Bytes = ctx.weightsQ4Bytes;
  const size_t weightsQ4ScBytes = ctx.weightsQ4ScBytes;

  // ---- Allocate weight blocks and map pointers ----
  gLLM.weightsData = (float*)llmPsramAlloc(weightsBytes, "llm.weights");
  if (!gLLM.weightsData) { f.close(); return false; }
  gLLM.weightsSize = weightsBytes;

  TransformerWeights* w = &gLLM.weights;

  if (qt == 0) {
    // FP32 mode: single float block, same layout as before
    float* ptr = gLLM.weightsData;
    w->token_embedding_table = ptr; ptr += V * D;
    if (isGPT2) {
      w->pos_embedding_table = ptr; ptr += p->seq_len * D;
    } else {
      w->pos_embedding_table = nullptr;
    }
    w->rms_att_weight = ptr; ptr += L * D;
    w->wq = ptr;             ptr += L * D * D;
    w->wk = ptr;             ptr += L * D * kv_dim;
    w->wv = ptr;             ptr += L * D * kv_dim;
    w->wo = ptr;             ptr += L * D * D;
    w->rms_ffn_weight = ptr; ptr += L * D;
    w->w1 = ptr;             ptr += isGPT2 ? L : L * H * D;
    w->w2 = ptr;             ptr += L * D * H;
    w->w3 = ptr;             ptr += L * H * D;
    w->rms_final_weight = ptr; ptr += D;
    if (hasNormBias) {
      w->rms_att_bias   = ptr; ptr += L * D;
      w->rms_ffn_bias   = ptr; ptr += L * D;
      w->rms_final_bias = ptr; ptr += D;
    } else {
      w->rms_att_bias = w->rms_ffn_bias = w->rms_final_bias = nullptr;
    }
    if (!shared_weights) w->wcls = ptr;
  } else if (qt == 1) {
    // INT8 mode: float block = norms + scales; separate int8 data block
    gLLM.weightsQ8Data = (int8_t*)llmPsramAlloc(weightsQ8Bytes, "llm.q8");
    if (!gLLM.weightsQ8Data) { f.close(); return false; }
    gLLM.weightsQ8Size = weightsQ8Bytes;

    // -- FP32 pointer: norms first --
    float* fp = gLLM.weightsData;
    w->token_embedding_table = nullptr;         // INT8 — use emb_i8/emb_sc
    w->rms_att_weight  = fp; fp += L * D;
    w->rms_ffn_weight  = fp; fp += L * D;
    w->rms_final_weight = fp; fp += D;
    if (isGPT2) {
      w->pos_embedding_table = fp; fp += p->seq_len * D;
      w->w1 = fp; fp += L;           // dummy FP32 per layer (not used in fwd; must be read)
    } else {
      w->pos_embedding_table = nullptr;
      w->w1 = nullptr;               // Llama: w1 is INT8
    }
    if (hasNormBias) {
      w->rms_att_bias   = fp; fp += L * D;
      w->rms_ffn_bias   = fp; fp += L * D;
      w->rms_final_bias = fp; fp += D;
    } else {
      w->rms_att_bias = w->rms_ffn_bias = w->rms_final_bias = nullptr;
    }
    // Null out FP32 matrix pointers (INT8 path uses _i8/_sc instead)
    w->wq = w->wk = w->wv = w->wo = nullptr;
    w->w2 = w->w3 = w->wcls = nullptr;

    // -- Scales (all contiguous in the float block after norms) --
    w->emb_sc  = fp; fp += scaleCount((size_t)V * D, gs);
    w->wq_sc   = fp; fp += L * scaleCount((size_t)D * D, gs);
    w->wk_sc   = fp; fp += L * scaleCount((size_t)D * kv_dim, gs);
    w->wv_sc   = fp; fp += L * scaleCount((size_t)D * kv_dim, gs);
    w->wo_sc   = fp; fp += L * scaleCount((size_t)D * D, gs);
    if (!isGPT2) {
      w->w1_sc = fp; fp += L * scaleCount((size_t)H * D, gs);
    } else {
      w->w1_sc = nullptr;
    }
    w->w2_sc   = fp; fp += L * scaleCount((size_t)D * H, gs);
    w->w3_sc   = fp; fp += L * scaleCount((size_t)H * D, gs);
    if (!shared_weights) {
      w->wcls_sc = fp; // last field — no advance needed
    }

    // -- INT8 data pointers --
    int8_t* q8 = gLLM.weightsQ8Data;
    w->emb_i8  = q8; q8 += (size_t)V * D;
    w->wq_i8   = q8; q8 += (size_t)L * D * D;
    w->wk_i8   = q8; q8 += (size_t)L * D * kv_dim;
    w->wv_i8   = q8; q8 += (size_t)L * D * kv_dim;
    w->wo_i8   = q8; q8 += (size_t)L * D * D;
    if (!isGPT2) {
      w->w1_i8 = q8; q8 += (size_t)L * H * D;
    } else {
      w->w1_i8 = nullptr;
    }
    w->w2_i8   = q8; q8 += (size_t)L * D * H;
    w->w3_i8   = q8; q8 += (size_t)L * H * D;
    if (!shared_weights) {
      w->wcls_i8 = q8; // last field — no advance needed
    }
  } else {
    // ── INT4_MIXED mode (qt==2) ──
    // Allocate Q8 data block (embedding + INT8 layers)
    gLLM.weightsQ8Data = (int8_t*)llmPsramAlloc(weightsQ8Bytes, "llm.q8");
    if (!gLLM.weightsQ8Data) { f.close(); return false; }
    gLLM.weightsQ8Size = weightsQ8Bytes;

    // Allocate Q4 packed data block
    w->q4_data = (uint8_t*)llmPsramAlloc(weightsQ4Bytes, "llm.q4");
    if (!w->q4_data) { f.close(); return false; }
    w->q4_data_size = weightsQ4Bytes;

    // Allocate Q4 scales block
    w->q4_scales = (float*)llmPsramAlloc(weightsQ4ScBytes, "llm.q4sc");
    if (!w->q4_scales) { f.close(); return false; }
    w->q4_scales_size = weightsQ4ScBytes;

    // Allocate per-layer metadata
    w->layer_quant = (uint8_t*)llmPsramAlloc(L, "llm.lq");
    if (!w->layer_quant) { f.close(); return false; }
    w->q4_offsets = (TransformerWeights::Q4LayerOffsets*)llmPsramAlloc(
        L * sizeof(TransformerWeights::Q4LayerOffsets), "llm.q4off");
    if (!w->q4_offsets) { f.close(); return false; }
    memset(w->q4_offsets, 0, L * sizeof(TransformerWeights::Q4LayerOffsets));

    // Fill layer_quant array
    const int nQ8s = p->n_q8_start;
    const int nQ8e = p->n_q8_end;
    for (int l = 0; l < L; l++) {
      w->layer_quant[l] = (l < nQ8s || l >= L - nQ8e) ? 1 : 2;
    }

    // -- FP32 block: norms + Q8 scales --
    float* fp = gLLM.weightsData;
    w->token_embedding_table = nullptr;
    w->rms_att_weight  = fp; fp += L * D;
    w->rms_ffn_weight  = fp; fp += L * D;
    w->rms_final_weight = fp; fp += D;
    if (isGPT2) {
      w->pos_embedding_table = fp; fp += p->seq_len * D;
      w->w1 = fp; fp += L;
    } else {
      w->pos_embedding_table = nullptr;
      w->w1 = nullptr;
    }
    if (hasNormBias) {
      w->rms_att_bias   = fp; fp += L * D;
      w->rms_ffn_bias   = fp; fp += L * D;
      w->rms_final_bias = fp; fp += D;
    } else {
      w->rms_att_bias = w->rms_ffn_bias = w->rms_final_bias = nullptr;
    }
    w->wq = w->wk = w->wv = w->wo = nullptr;
    w->w2 = w->w3 = w->wcls = nullptr;

    // Q8 layer scales in the float block (only for INT8 layers)
    const int nQ8 = nQ8s + nQ8e;
    w->emb_sc  = fp; fp += scaleCount((size_t)V * D, gs);
    w->wq_sc   = fp; fp += nQ8 * scaleCount((size_t)D * D, gs);
    w->wk_sc   = fp; fp += nQ8 * scaleCount((size_t)D * kv_dim, gs);
    w->wv_sc   = fp; fp += nQ8 * scaleCount((size_t)D * kv_dim, gs);
    w->wo_sc   = fp; fp += nQ8 * scaleCount((size_t)D * D, gs);
    if (!isGPT2) {
      w->w1_sc = fp; fp += nQ8 * scaleCount((size_t)H * D, gs);
    } else {
      w->w1_sc = nullptr;
    }
    w->w2_sc   = fp; fp += nQ8 * scaleCount((size_t)D * H, gs);
    w->w3_sc   = fp; fp += nQ8 * scaleCount((size_t)H * D, gs);
    if (!shared_weights) {
      w->wcls_sc = fp;
    }

    // Q8 data block: embedding first, then INT8-layer weights (contiguous by tensor type)
    int8_t* q8 = gLLM.weightsQ8Data;
    w->emb_i8 = q8; q8 += (size_t)V * D;
    w->wq_i8  = q8; q8 += (size_t)nQ8 * D * D;
    w->wk_i8  = q8; q8 += (size_t)nQ8 * D * kv_dim;
    w->wv_i8  = q8; q8 += (size_t)nQ8 * D * kv_dim;
    w->wo_i8  = q8; q8 += (size_t)nQ8 * D * D;
    if (!isGPT2) {
      w->w1_i8 = q8; q8 += (size_t)nQ8 * H * D;
    } else {
      w->w1_i8 = nullptr;
    }
    w->w2_i8  = q8; q8 += (size_t)nQ8 * D * H;
    w->w3_i8  = q8; q8 += (size_t)nQ8 * H * D;
    if (!shared_weights) {
      w->wcls_i8 = q8;
    }

    DEBUG_LLM_LOADF("[LLM] Mixed Q4/Q8 pointers mapped: Q8 layers=%d, Q4 layers=%d", nQ8, L - nQ8);
  }

  // ---- Read tensors from file ----
  const bool isQ8  = (qt == 1);
  const bool isMix = (qt == 2);
  DEBUG_LLM_LOADF("[LLM] Loading weights from flash (%s)...",
                  isMix ? "MIXED Q4/Q8" : (isQ8 ? "INT8 in PSRAM" : "FP32"));

  if (isQ8 || isMix) {
    size_t sc_emb  = scaleCount((size_t)V * D, gs);
    size_t sc_wq   = scaleCount((size_t)D * D, gs);
    size_t sc_wkv  = scaleCount((size_t)D * kv_dim, gs);
    size_t sc_wdh  = scaleCount((size_t)D * H, gs);
    DEBUG_LLM_LOADF("[LLM] Scale geometry: dim=%d kv_dim=%d hidden=%d gs=%d", D, kv_dim, H, gs);
    DEBUG_LLM_LOADF("[LLM]   emb: %u  wq/wo: %u  wk/wv: %u  w1/w2/w3: %u",
                    (unsigned)sc_emb, (unsigned)sc_wq, (unsigned)sc_wkv, (unsigned)sc_wdh);
  }

  // Helper: read and discard a VERSION=3 per-tensor quant prefix byte.
  // Returns the prefix value, or -1 on read error.
  auto readPrefix = [&](File& file) -> int {
    if (!v3) return -2; // no prefix for VERSION<=2
    uint8_t pfxByte = 0;
    if (file.read(&pfxByte, 1) != 1) return -1;
    return (int)pfxByte;
  };

  // 1. Embedding (always Q8 for quantized models)
  if (isMix || isQ8) {
    if (v3) readPrefix(f);  // skip prefix byte
    if (!readTensorQ8(f, w->emb_i8, w->emb_sc, V * D, gs)) {
      snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Failed reading embedding tensor (Q8)");
      f.close(); return false;
    }
  } else {
    if (!readTensor(f, w->token_embedding_table, V * D, qt, gs, false)) {
      snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Failed reading embedding tensor");
      f.close(); return false;
    }
  }

  // 1b. Positional embedding (GPT-2 only, always FP32)
  if (isGPT2) {
    if (v3) readPrefix(f);
    if (!readTensor(f, w->pos_embedding_table, p->seq_len * D, qt, gs, true)) {
      snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Failed reading positional embedding");
      f.close(); return false;
    }
    DEBUG_LLM_LOADF("[LLM] Positional embedding loaded (%d x %d)", p->seq_len, D);
  }

  // 2. Per-layer tensors
  // For mixed mode: Q8 layer data is contiguous (indexed by q8_li, not global l)
  // and Q4 data/scales are tracked with running cursors to build the offset table.
  int q8_li = 0;                // Q8 layer counter for mixed mode
  size_t q4_data_cursor = 0;    // running byte offset into w->q4_data
  size_t q4_sc_cursor = 0;      // running float offset into w->q4_scales

  for (int l = 0; l < L; l++) {
    const bool isQ4Layer = isMix && (w->layer_quant[l] == 2);

    // Norm tensors: always FP32, always indexed by global layer l
    if (v3) readPrefix(f);
    if (!readTensor(f, w->rms_att_weight + l*D, D, qt, gs, true)) {
      snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Failed reading attn_norm layer %d", l);
      f.close(); return false;
    }
    if (hasNormBias) {
      if (v3) readPrefix(f);
      if (!readTensor(f, w->rms_att_bias + l*D, D, qt, gs, true)) {
        snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Failed reading attn_norm_bias layer %d", l);
        f.close(); return false;
      }
    }

    // QKV + output projections
    bool layerOk;
    if (isQ4Layer) {
      // INT4 layer: read Q4 tensors, build offset table
      auto& off = w->q4_offsets[l];
      uint32_t nWQ = D * D, nWK = D * kv_dim, nWV = D * kv_dim, nWO = D * D;

      off.wq_data = q4_data_cursor;  off.wq_sc = q4_sc_cursor;
      if (v3) readPrefix(f);
      layerOk = readTensorQ4(f, w->q4_data + q4_data_cursor, w->q4_scales + q4_sc_cursor, nWQ, gs);
      q4_data_cursor += ((size_t)nWQ + 1) / 2;  q4_sc_cursor += scaleCount(nWQ, gs);

      off.wk_data = q4_data_cursor;  off.wk_sc = q4_sc_cursor;
      if (v3) readPrefix(f);
      layerOk = layerOk && readTensorQ4(f, w->q4_data + q4_data_cursor, w->q4_scales + q4_sc_cursor, nWK, gs);
      q4_data_cursor += ((size_t)nWK + 1) / 2;  q4_sc_cursor += scaleCount(nWK, gs);

      off.wv_data = q4_data_cursor;  off.wv_sc = q4_sc_cursor;
      if (v3) readPrefix(f);
      layerOk = layerOk && readTensorQ4(f, w->q4_data + q4_data_cursor, w->q4_scales + q4_sc_cursor, nWV, gs);
      q4_data_cursor += ((size_t)nWV + 1) / 2;  q4_sc_cursor += scaleCount(nWV, gs);

      off.wo_data = q4_data_cursor;  off.wo_sc = q4_sc_cursor;
      if (v3) readPrefix(f);
      layerOk = layerOk && readTensorQ4(f, w->q4_data + q4_data_cursor, w->q4_scales + q4_sc_cursor, nWO, gs);
      q4_data_cursor += ((size_t)nWO + 1) / 2;  q4_sc_cursor += scaleCount(nWO, gs);
    } else if (isQ8 || isMix) {
      // INT8 layer (pure INT8 or Q8 layer in mixed mode)
      size_t q8lD2  = (size_t)q8_li * D * D;
      size_t q8lDkv = (size_t)q8_li * D * kv_dim;
      size_t q8scWQ  = (size_t)q8_li * scaleCount((size_t)D * D, gs);
      size_t q8scWKV = (size_t)q8_li * scaleCount((size_t)D * kv_dim, gs);
      if (v3) readPrefix(f);
      layerOk = readTensorQ8(f, w->wq_i8+q8lD2,  w->wq_sc+q8scWQ,  D*D,      gs);
      if (v3) readPrefix(f);
      layerOk = layerOk && readTensorQ8(f, w->wk_i8+q8lDkv, w->wk_sc+q8scWKV, D*kv_dim, gs);
      if (v3) readPrefix(f);
      layerOk = layerOk && readTensorQ8(f, w->wv_i8+q8lDkv, w->wv_sc+q8scWKV, D*kv_dim, gs);
      if (v3) readPrefix(f);
      layerOk = layerOk && readTensorQ8(f, w->wo_i8+q8lD2,  w->wo_sc+q8scWQ,  D*D,      gs);
    } else {
      // FP32 mode
      size_t lD2  = (size_t)l * D * D;
      size_t lDkv = (size_t)l * D * kv_dim;
      layerOk = readTensor(f, w->wq+lD2,  D*D,      qt, gs, false)
             && readTensor(f, w->wk+lDkv, D*kv_dim, qt, gs, false)
             && readTensor(f, w->wv+lDkv, D*kv_dim, qt, gs, false)
             && readTensor(f, w->wo+lD2,  D*D,      qt, gs, false);
    }
    if (!layerOk) {
      snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Failed reading QKV/O layer %d", l);
      f.close(); return false;
    }

    // FFN norm (always FP32)
    if (v3) readPrefix(f);
    if (!readTensor(f, w->rms_ffn_weight + l*D, D, qt, gs, true)) {
      snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Failed reading ffn_norm layer %d", l);
      f.close(); return false;
    }
    if (hasNormBias) {
      if (v3) readPrefix(f);
      if (!readTensor(f, w->rms_ffn_bias + l*D, D, qt, gs, true)) {
        snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Failed reading ffn_norm_bias layer %d", l);
        f.close(); return false;
      }
    }

    // Gate (w1): GPT-2 always FP32 dummy; Llama FP32/Q8/Q4
    if (isGPT2) {
      if (v3) readPrefix(f);
      if (!readTensor(f, w->w1 + l, 1, qt, gs, true)) {
        snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Failed reading GPT-2 gate layer %d", l);
        f.close(); return false;
      }
    } else if (isQ4Layer) {
      auto& off = w->q4_offsets[l];
      off.w1_data = q4_data_cursor;  off.w1_sc = q4_sc_cursor;
      if (v3) readPrefix(f);
      if (!readTensorQ4(f, w->q4_data + q4_data_cursor, w->q4_scales + q4_sc_cursor, H*D, gs)) {
        snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Failed reading gate(Q4) layer %d", l);
        f.close(); return false;
      }
      q4_data_cursor += ((size_t)H * D + 1) / 2;  q4_sc_cursor += scaleCount((size_t)H * D, gs);
    } else if (isQ8 || isMix) {
      size_t q8lDH  = (size_t)q8_li * (size_t)D * H;
      size_t q8scDH = (size_t)q8_li * scaleCount((size_t)H * D, gs);
      if (v3) readPrefix(f);
      if (!readTensorQ8(f, w->w1_i8+q8lDH, w->w1_sc+q8scDH, H*D, gs)) {
        snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Failed reading gate(Q8) layer %d", l);
        f.close(); return false;
      }
    } else {
      size_t lDH = (size_t)l * (size_t)D * H;
      if (!readTensor(f, w->w1+lDH, H*D, qt, gs, false)) {
        snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Failed reading gate layer %d", l);
        f.close(); return false;
      }
    }

    // Up (w3) + Down (w2)
    if (isQ4Layer) {
      auto& off = w->q4_offsets[l];
      off.w3_data = q4_data_cursor;  off.w3_sc = q4_sc_cursor;
      if (v3) readPrefix(f);
      if (!readTensorQ4(f, w->q4_data + q4_data_cursor, w->q4_scales + q4_sc_cursor, H*D, gs)) {
        snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Failed reading up(Q4) layer %d", l);
        f.close(); return false;
      }
      q4_data_cursor += ((size_t)H * D + 1) / 2;  q4_sc_cursor += scaleCount((size_t)H * D, gs);

      off.w2_data = q4_data_cursor;  off.w2_sc = q4_sc_cursor;
      if (v3) readPrefix(f);
      if (!readTensorQ4(f, w->q4_data + q4_data_cursor, w->q4_scales + q4_sc_cursor, D*H, gs)) {
        snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Failed reading down(Q4) layer %d", l);
        f.close(); return false;
      }
      q4_data_cursor += ((size_t)D * H + 1) / 2;  q4_sc_cursor += scaleCount((size_t)D * H, gs);
    } else if (isQ8 || isMix) {
      size_t q8lDH  = (size_t)q8_li * (size_t)D * H;
      size_t q8scDH = (size_t)q8_li * scaleCount((size_t)D * H, gs);
      if (v3) readPrefix(f);
      if (!readTensorQ8(f, w->w3_i8+q8lDH, w->w3_sc+q8scDH, H*D, gs)) {
        snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Failed reading up(Q8) layer %d", l);
        f.close(); return false;
      }
      if (v3) readPrefix(f);
      if (!readTensorQ8(f, w->w2_i8+q8lDH, w->w2_sc+q8scDH, D*H, gs)) {
        snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Failed reading down(Q8) layer %d", l);
        f.close(); return false;
      }
    } else {
      size_t lDH = (size_t)l * (size_t)D * H;
      if (!readTensor(f, w->w3+lDH, H*D, qt, gs, false)
       || !readTensor(f, w->w2+lDH, D*H, qt, gs, false)) {
        snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Failed reading FFN layer %d", l);
        f.close(); return false;
      }
    }

    if (!isQ4Layer) q8_li++;
    DEBUG_LLM_LOADF("[LLM] Layer %d/%d loaded (%s)", l + 1, L,
                    isQ4Layer ? "Q4" : (isQ8 || isMix ? "Q8" : "FP32"));
  }

  if (isMix) {
    DEBUG_LLM_LOADF("[LLM] Q4 cursors final: data=%u/%u scales=%u/%u",
                    (unsigned)q4_data_cursor, (unsigned)w->q4_data_size,
                    (unsigned)(q4_sc_cursor * sizeof(float)), (unsigned)w->q4_scales_size);
  }

  // 3. Final norm (always FP32)
  if (v3) readPrefix(f);
  if (!readTensor(f, w->rms_final_weight, D, qt, gs, true)) {
    snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Failed reading final norm");
    f.close(); return false;
  }
  if (hasNormBias) {
    if (v3) readPrefix(f);
    if (!readTensor(f, w->rms_final_bias, D, qt, gs, true)) {
      snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Failed reading final norm bias");
      f.close(); return false;
    }
  }

  // 4. Tied flag + optional LM head
  uint8_t tied_check = 1;
  f.read(&tied_check, 1);
  if (tied_check != 0) {
    // Tied: LM head shares embedding weights
    if (isQ8 || isMix) {
      w->wcls_i8 = w->emb_i8;
      w->wcls_sc = w->emb_sc;
    } else {
      w->wcls = w->token_embedding_table;
    }
  } else {
    if (shared_weights) {
      snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Tied flag mismatch");
      f.close(); return false;
    }
    // LM head is always Q8 (never Q4), even in mixed mode
    if (isQ8 || isMix) {
      if (v3) readPrefix(f);
      if (!readTensorQ8(f, w->wcls_i8, w->wcls_sc, V * D, gs)) {
        snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Failed reading LM head (Q8)");
        f.close(); return false;
      }
    } else {
      if (!readTensor(f, w->wcls, V * D, qt, gs, false)) {
        snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Failed reading LM head");
        f.close(); return false;
      }
    }
  }

  f.close();
  DEBUG_LLM_LOADF("[LLM] Weights loaded successfully (%s)",
                  isMix ? "MIXED Q4/Q8" : (isQ8 ? "INT8" : "FP32"));

  spotCheckWeights(ctx);

  // ---- Allocate run state ----
  if (!allocateRunState(ctx)) return false;

  return true;
}

// ============================================================================
// 11. Public API
// ============================================================================

void llmInit() {
  memset(&gLLM, 0, sizeof(gLLM));
  gLLM.runState = LLMState::UNLOADED;
  gLLM.mutex = xSemaphoreCreateMutex();
}

bool llmLoadModel(const char* modelPath, int maxCtx) {
  // Auto-init if not yet initialized
  if (!gLLM.mutex) llmInit();

  if (!filesystemReady) {
    strlcpy(gLLM.errorMsg, "Filesystem not ready", sizeof(gLLM.errorMsg));
    gLLM.runState = LLMState::ERROR;
    return false;
  }

  // Unload any existing model
  llmUnload();

  // Store the requested context cap (0 = auto-fit to available PSRAM)
  gLLM.requestedMaxCtx = maxCtx;

  gLLM.runState = LLMState::LOADING;
  strlcpy(gLLM.modelPath, modelPath, sizeof(gLLM.modelPath));

  DEBUG_LLM_LOADF("[LLM] Loading model: %s (maxCtx=%d)", modelPath, gLLM.requestedMaxCtx);

  // loadWeights() handles header, tokenizer, and weights (all in one file)
  if (!loadWeights(modelPath)) {
    gLLM.runState = LLMState::ERROR;
    return false;
  }

  gLLM.runState = LLMState::READY;
  gLLM.lastContextMax = gLLM.seq_ctx;  // expose active ctx window immediately (before first generation)
  DEBUG_LLM_LOADF("[LLM] Model ready. PSRAM used: %uKB (weights=%uKB state=%uKB) ctx=%d/%d",
                  (unsigned)((gLLM.weightsSize + gLLM.weightsQ8Size + gLLM.stateSize) / 1024),
                  (unsigned)((gLLM.weightsSize + gLLM.weightsQ8Size) / 1024),
                  (unsigned)(gLLM.stateSize / 1024),
                  gLLM.seq_ctx, gLLM.config.seq_len);
  return true;
}

void llmUnload() {
  if (gLLM.runState == LLMState::GENERATING) {
    gLLM.stopRequested = true;
    // Wait briefly for generation to stop
    for (int i = 0; i < 50 && gLLM.runState == LLMState::GENERATING; i++) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }

  freeTokenizer();
  llmPsramFree((void**)&gLLM.weightsData);
  llmPsramFree((void**)&gLLM.weightsQ8Data);
  // Free mixed-mode Q4 allocations
  llmPsramFree((void**)&gLLM.weights.q4_data);
  llmPsramFree((void**)&gLLM.weights.q4_scales);
  llmPsramFree((void**)&gLLM.weights.layer_quant);
  llmPsramFree((void**)&gLLM.weights.q4_offsets);
  llmPsramFree((void**)&gLLM.stateData);
  if (gLLM.stateHotData) {
    heap_caps_free(gLLM.stateHotData);
    gLLM.stateHotData = nullptr;
  }
  if (gLLM.repBuf) {
    free(gLLM.repBuf);
    gLLM.repBuf = nullptr;
    gLLM.repBufSize = 0;
  }

  memset(&gLLM.weights, 0, sizeof(gLLM.weights));
  memset(&gLLM.state, 0, sizeof(gLLM.state));
  gLLM.weightsSize = 0;
  gLLM.weightsQ8Size = 0;
  gLLM.stateSize = 0;
  gLLM.stateHotSize = 0;
  gLLM.seq_ctx = 0;
  gLLM.runState = LLMState::UNLOADED;
}

bool llmIsReady() {
  return gLLM.runState == LLMState::READY;
}

LLMStatus llmGetStatus() {
  LLMStatus status = {};
  status.state = gLLM.runState;
  strlcpy(status.modelPath, gLLM.modelPath, sizeof(status.modelPath));
  status.config = gLLM.config;
  size_t q4Total = gLLM.weights.q4_data_size + gLLM.weights.q4_scales_size;
  status.modelSizeBytes = gLLM.weightsSize + gLLM.weightsQ8Size + q4Total;
  status.runtimeSizeBytes = gLLM.stateSize + gLLM.stateHotSize;
  status.totalPsramUsed = gLLM.weightsSize + gLLM.weightsQ8Size + q4Total + gLLM.stateSize;
  status.lastTokensPerSec = gLLM.lastTokPerSec;
  status.lastTokenCount = gLLM.lastTokCount;
  status.lastContextUsed = gLLM.lastContextUsed;
  status.lastContextMax = gLLM.lastContextMax;
  strlcpy(status.errorMsg, gLLM.errorMsg, sizeof(status.errorMsg));
  return status;
}

int llmGenerate(const char* prompt, LLMTokenCallback tokenCb,
                int maxTokens, float temperature, float topp,
                bool useMirostat2, float mirostatTau, float mirostatEta,
                float repPenalty, int repWindow, int sentenceLimit, int hardCap,
                bool dynTemp,
                const int* suppressTokens, int suppressTokenCount) {
  if (gLLM.runState != LLMState::READY) {
    return -1;
  }

  // ── Setup ──────────────────────────────────────────────────────────────────
  gLLM.runState = LLMState::GENERATING;
  gLLM.stopRequested = false;
  gLLM.errorMsg[0] = '\0';

  LLMConfig* p = &gLLM.config;

  // ── Prompt normalization: strip filler phrases ────────────────────────────
  // Users type things like "I want to add a wifi network" or "Can you tell me
  // how to set up wifi". The filler wastes attention bandwidth on a dim=128
  // model. Strip common prefixes from the question portion of the prompt
  // (between "Q: " and "\nA:") so the model sees just the semantic core.
  // Ordered longest-first so "Can you tell me how to" matches before "Can you".
  // Only strip phrases where the remainder IS the topic — never strip words that
  // change meaning (Can I, Is the, Does it, Which, Who, Where, When).
  static const char* const FILLER_PREFIXES[] = {
    // Polite/indirect frames
    "Can you tell me how to ",    "Could you tell me how to ",
    "Can you show me how to ",    "Could you show me how to ",
    "I was wondering how to ",    "What is the best way to ",
    "How would I go about ",      "How do I go about ",
    "What is the way to ",        "Can you tell me ",
    "Could you tell me ",         "Please tell me ",
    "Do you know how to ",        "I was wondering ",
    "Is there a way to ",         "Is it possible to ",
    "I would like to ",           "Please help me ",
    "Tell me how to ",            "I'd like to ",
    "Do you know ",               "I want to ",
    "I need to ",                 "Could you ",
    "Can you ",                   "Help me ",
    "Tell me ",                   "Please ",
    // "How do I [verb]" — the verb+object IS the topic
    "How do I use the ",          "How do I use ",
    "How do I check the ",        "How do I check ",
    "How do I see the ",          "How do I see ",
    "How do I set up the ",       "How do I set up ",
    "How do I set the ",          "How do I set ",
    "How do I get the ",          "How do I get ",
    "How do I turn on the ",      "How do I turn off the ",
    "How do I turn on ",          "How do I turn off ",
    "How do I change the ",       "How do I change ",
    "How do I enable the ",       "How do I enable ",
    "How do I disable the ",      "How do I disable ",
    "How do I connect to ",       "How do I connect ",
    "How do I create ",           "How do I send ",
    "How do I add ",              "How do I update ",
    "How do I delete ",           "How do I remove ",
    "How do I read ",             "How do I list ",
    "How do I view ",             "How do I open ",
    "How do I start ",            "How do I stop ",
    "How do I save ",             "How do I find ",
    "How do I configure ",        "How do I measure ",
    "How do I detect ",           "How do I access ",
    "How do I make ",             "How do I scan ",
    "How do I pair ",             "How do I join ",
    "How do I leave ",            "How do I build ",
    "How do I flash ",            "How do I assign ",
    "How do I clear ",            "How do I run ",
    "How do I log ",              "How do I record ",
    "How do I schedule ",         "How do I broadcast ",
    "How do I transfer ",         "How do I switch ",
    "How do I sync ",             "How do I tune ",
    "How do I reset ",            "How do I reboot ",
    "How do I do ",               "How do I ",
    // "How does" — remainder is topic
    "How does the ",              "How does ",
    // "What is/are/does" — remainder is topic
    "What is the ",               "What is a ",
    "What is an ",                "What is ",
    "What are the ",              "What are ",
    "What does the ",             "What does ",
    // Quantity — remainder is topic
    "How much ",                  "How many ",
    "How long ",                  "How fast ",
    "How far ",                   "How often ",
    "How accurate ",
  };
  static constexpr int NUM_FILLER = sizeof(FILLER_PREFIXES) / sizeof(FILLER_PREFIXES[0]);

  // Work on a mutable copy for normalization
  char* norm_prompt = strdup(prompt);
  if (norm_prompt) {
    // Find the question body: after "Q: " (or "Q:" ) and before "\nA:"
    // Filler detection (log-only — no stripping). Stripping was removed because
    // it starved the KV cache of semantic context, causing topic drift in later
    // generation positions.  The CONTENT_BOOST STOP_WORDS list already prevents
    // filler words like "what", "how", "does" from getting boosted.
    char* qStart = strstr(norm_prompt, "Q: ");
    char* aMarker = strstr(norm_prompt, "\nA:");
    if (qStart && aMarker && aMarker > qStart) {
      char* body = qStart + 3;  // skip "Q: "
      int bodyLen = (int)(aMarker - body);
      for (int fi = 0; fi < NUM_FILLER; fi++) {
        int fLen = strlen(FILLER_PREFIXES[fi]);
        if (bodyLen >= fLen && strncasecmp(body, FILLER_PREFIXES[fi], fLen) == 0) {
          DEBUG_LLM_GENERATEF("[LLM] FILLER detected '%s' (kept in prompt for KV context)",
                              FILLER_PREFIXES[fi]);
          break;
        }
      }
    }
    prompt = norm_prompt;  // use (unmodified) copy
  }

  // Encode prompt — heap-allocated to avoid stack overflow on long prompts
  size_t prompt_buf_n = strlen(prompt) + 3;
  int* prompt_tokens = (int*)malloc(prompt_buf_n * sizeof(int));
  if (!prompt_tokens) {
    snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "OOM: prompt_tokens alloc");
    if (norm_prompt) free(norm_prompt);
    gLLM.runState = LLMState::ERROR;
    return -1;
  }
  int num_prompt_tokens = encode(prompt, prompt_tokens, prompt_buf_n - 1);

  // Debug: dump tokenized prompt
  DEBUG_LLM_TOKENIZERF("[LLM] Encoded prompt (%d chars -> %d tokens): \"%.*s%s\"",
                       (int)strlen(prompt), num_prompt_tokens,
                       (int)(strlen(prompt) > 80 ? 80 : strlen(prompt)), prompt,
                       strlen(prompt) > 80 ? "..." : "");
  {
    // Log first 20 token IDs and their decoded strings
    int show = (num_prompt_tokens < 20) ? num_prompt_tokens : 20;
    for (int ti = 0; ti < show; ti++) {
      int tok = prompt_tokens[ti];
      const char* piece = (tok >= 0 && tok < gLLM.tokenizer.vocab_size) ?
                           gLLM.tokenizer.vocab[tok] : "?";
      DEBUG_LLM_TOKENIZERF("[LLM]   tok[%d]=%d '%s'", ti, tok, piece);
    }
    if (num_prompt_tokens > 20) {
      DEBUG_LLM_TOKENIZERF("[LLM]   ... (%d more tokens)", num_prompt_tokens - 20);
    }
  }

  if (num_prompt_tokens < 1) {
    // Use BOS token if prompt is empty
    prompt_tokens[0] = 1; // BOS
    num_prompt_tokens = 1;
  }

  // ── Pre-forward embedding analysis ────────────────────────────────────────
  // Before any forward pass, dequantize raw embeddings for prompt tokens and
  // compare them against each other and known confuser tokens. This reveals
  // whether INT8 quantization collapsed semantically different tokens into
  // similar vectors, making it impossible for the model to distinguish them.
  {
    const int dim = p->dim;
    const int gs = p->group_size;
    TransformerWeights* w = &gLLM.weights;

    // Temp buffers for two embeddings (stack is fine for dim=128)
    float emb_a[512], emb_b[512];  // max dim we'll handle
    if (dim <= 512) {
      // Helper lambda: dequantize embedding for token id into buf
      auto deq_emb = [&](int tok_id, float* buf) {
        if (w->token_embedding_table) {
          memcpy(buf, w->token_embedding_table + tok_id * dim, dim * sizeof(float));
        } else {
          const int8_t* row = w->emb_i8 + (size_t)tok_id * dim;
          size_t flat_base = (size_t)tok_id * dim;
          for (int i = 0; i < dim; i++)
            buf[i] = (float)row[i] * w->emb_sc[(flat_base + i) / gs];
        }
      };

      // Helper: cosine similarity between two vectors
      auto cosine = [&](const float* a, const float* b, int n) -> float {
        float dot = 0.f, na = 0.f, nb = 0.f;
        for (int i = 0; i < n; i++) {
          dot += a[i] * b[i];
          na  += a[i] * a[i];
          nb  += b[i] * b[i];
        }
        na = sqrtf(na); nb = sqrtf(nb);
        return (na > 1e-8f && nb > 1e-8f) ? dot / (na * nb) : 0.f;
      };

      DEBUG_LLM_GENERATEF("[LLM] ═══ PRE-FORWARD EMBEDDING ANALYSIS ═══");

      // Compare all prompt tokens pairwise (skip if too many)
      if (num_prompt_tokens <= 20) {
        for (int ti = 0; ti < num_prompt_tokens; ti++) {
          deq_emb(prompt_tokens[ti], emb_a);
          float norm_a = 0.f;
          for (int i = 0; i < dim; i++) norm_a += emb_a[i] * emb_a[i];
          norm_a = sqrtf(norm_a);
          const char* name_a = (prompt_tokens[ti] >= 0 && prompt_tokens[ti] < gLLM.tokenizer.vocab_size) ?
                                gLLM.tokenizer.vocab[prompt_tokens[ti]] : "?";
          //DEBUG_LLM_GENERATEF("[LLM] EMB tok[%d]=%d('%s') L2=%.4f x[0..3]=[%.3f,%.3f,%.3f,%.3f]",
          //                    ti, prompt_tokens[ti], name_a, norm_a,
          //                    emb_a[0], dim>1?emb_a[1]:0, dim>2?emb_a[2]:0, dim>3?emb_a[3]:0);
          (void)name_a; (void)norm_a;
        }

        // Pairwise cosine for content tokens (skip Q: and A:)
        for (int ti = 0; ti < num_prompt_tokens; ti++) {
          for (int tj = ti + 1; tj < num_prompt_tokens; tj++) {
            // Only compare content tokens (skip special tokens 3=Q: and 4=A:)
            if (prompt_tokens[ti] == 3 || prompt_tokens[ti] == 4) continue;
            if (prompt_tokens[tj] == 3 || prompt_tokens[tj] == 4) continue;
            deq_emb(prompt_tokens[ti], emb_a);
            deq_emb(prompt_tokens[tj], emb_b);
            float cs = cosine(emb_a, emb_b, dim);
            if (cs > 0.5f || cs < -0.5f) {  // Only log notable similarities
              const char* na = gLLM.tokenizer.vocab[prompt_tokens[ti]];
              const char* nb = gLLM.tokenizer.vocab[prompt_tokens[tj]];
              //DEBUG_LLM_GENERATEF("[LLM] EMB_PAIR '%s'<->'%s' cosine=%.4f %s",
              //                    na, nb, cs, cs > 0.8f ? "HIGH_SIM!" : "");
              (void)na; (void)nb;
            }
          }
        }
      }

      // Compare prompt content tokens against known confusers.
      // For each content token in the prompt, find similar tokens in the
      // vocab that might cause the model to confuse topics.
      // Strategy: check a handful of topic-adjacent tokens.
      DEBUG_LLM_GENERATEF("[LLM] CONFUSER CHECK: comparing prompt tokens vs semantically adjacent vocab");
      for (int ti = 0; ti < num_prompt_tokens; ti++) {
        int ptok = prompt_tokens[ti];
        if (ptok == 3 || ptok == 4 || ptok < 10) continue;  // skip structural tokens

        deq_emb(ptok, emb_a);
        const char* pname = gLLM.tokenizer.vocab[ptok];

        // Scan vocab for high-similarity tokens (sample every 4th to keep it fast)
        int high_sim_count = 0;
        float best_sim = -1.f;
        int best_tok = -1;
        for (int vi = 5; vi < p->vocab_size && vi < gLLM.tokenizer.vocab_size; vi += 4) {
          if (vi == ptok) continue;
          deq_emb(vi, emb_b);
          float cs = cosine(emb_a, emb_b, dim);
          if (cs > best_sim) { best_sim = cs; best_tok = vi; }
          if (cs > 0.85f) high_sim_count++;
        }
        const char* best_name = (best_tok >= 0 && best_tok < gLLM.tokenizer.vocab_size) ?
                                 gLLM.tokenizer.vocab[best_tok] : "?";
        //DEBUG_LLM_GENERATEF("[LLM] CONFUSER tok=%d('%s'): most_similar=%d('%s') cosine=%.4f  "
        //                    "high_sim_count(>0.85)=%d",
        //                    ptok, pname, best_tok, best_name, best_sim, high_sim_count);
        (void)pname; (void)best_name; (void)best_sim; (void)high_sim_count;
      }

      DEBUG_LLM_GENERATEF("[LLM] ═══════════════════════════════════════════════════");
    }
  }

  // ── Identify prompt content tokens for logit boosting ──────────────────────
  // Extract "content" tokens from the prompt — words that carry semantic meaning
  // about what the user is asking. Skip structural tokens (Q:, A:, newline) and
  // very common function words. During generation, these get a small logit boost
  // to nudge the model toward on-topic answers.
  static constexpr int   MAX_CONTENT_TOKENS   = 16;
  static constexpr float CONTENT_LOGIT_BOOST       = 1.5f;  // logit bonus for content tokens (gentle nudge)
  static constexpr float CONTENT_LOGIT_BOOST_LATE  = 1.0f;  // sustained nudge after initial window (was 0.5)
  static constexpr int   CONTENT_BOOST_WINDOW      = 16;    // first N tokens get full boost (was 10)
  int content_tokens[MAX_CONTENT_TOKENS];
  int content_token_count = 0;
  {
    for (int ti = 0; ti < num_prompt_tokens && content_token_count < MAX_CONTENT_TOKENS; ti++) {
      int tok = prompt_tokens[ti];
      // Skip special/structural tokens: Q:(3), A:(4), newline, space, and very short tokens
      if (tok <= 4) continue;
      if (tok < 10) continue;  // single-char punctuation/digits
      // Skip common function words by checking token string length and stop word list
      const char* piece = (tok < gLLM.tokenizer.vocab_size) ? gLLM.tokenizer.vocab[tok] : nullptr;
      if (!piece) continue;
      int plen = strlen(piece);
      // Skip very short tokens (single chars, spaces) — keep 3+ char words
      if (plen < 3) continue;
      // Skip stop words — common function words that carry no topic signal.
      // These would pollute CONTENT_BOOST by nudging the model toward generic
      // words instead of topic-specific ones like "wifi", "sensor", "mqtt".
      {
        // Normalize: skip leading space for comparison
        const char* word = piece;
        if (*word == ' ') word++;
        static const char* const STOP_WORDS[] = {
          "want", "need", "please", "could", "would", "should", "shall",
          "tell", "know", "about", "think", "just", "really", "actually",
          "help", "like", "have", "make", "does", "this", "that", "with",
          "from", "what", "where", "when", "which", "there", "their",
          "some", "also", "been", "were", "will", "your", "they",
          "into", "only", "very", "much", "many", "each", "other",
        };
        static constexpr int NUM_STOP = sizeof(STOP_WORDS) / sizeof(STOP_WORDS[0]);
        bool isStop = false;
        for (int si = 0; si < NUM_STOP; si++) {
          if (strcasecmp(word, STOP_WORDS[si]) == 0) { isStop = true; break; }
        }
        if (isStop) continue;
      }
      content_tokens[content_token_count++] = tok;
    }
    if (content_token_count > 0) {
      DEBUG_LLM_GENERATEF("[LLM] CONTENT TOKENS for logit boost (early=%.1f late=%.1f window=%d):",
                          CONTENT_LOGIT_BOOST, CONTENT_LOGIT_BOOST_LATE, CONTENT_BOOST_WINDOW);
      for (int ci = 0; ci < content_token_count; ci++) {
        const char* cname = (content_tokens[ci] < gLLM.tokenizer.vocab_size) ?
                             gLLM.tokenizer.vocab[content_tokens[ci]] : "?";
        DEBUG_LLM_GENERATEF("[LLM]   content[%d]=%d('%s')", ci, content_tokens[ci], cname);
        (void)cname;
      }
    } else {
      DEBUG_LLM_GENERATEF("[LLM] CONTENT TOKENS: none extracted from prompt (all filtered as stop words or too short)");
    }
  }

  // Clear KV cache for fresh generation
  int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
  const int S = gLLM.seq_ctx;
  size_t kvSize = p->n_layers * S * kv_dim * sizeof(float);
  memset(gLLM.state.key_cache, 0, kvSize);
  memset(gLLM.state.value_cache, 0, kvSize);

  // ── Initialize prompt diagnostics ─────────────────────────────────────────
  // Makes prompt token info available to forward() for deep attention analysis.
  gPromptDiag.tokens = prompt_tokens;
  gPromptDiag.count  = num_prompt_tokens;
  gPromptDiag.num_prompt_tokens = num_prompt_tokens;
  gPromptDiag.active = true;
  memset(gPromptDiag.emb_norms, 0, sizeof(gPromptDiag.emb_norms));
  memset(gPromptDiag.emb_dots,  0, sizeof(gPromptDiag.emb_dots));

  int token = prompt_tokens[0]; // BOS or first prompt token
  int pos = 0;
  int generated = 0;

  const bool isGPT2gen = (p->arch_type == 1);
  const int eos_id = isGPT2gen ? EOS_TOKEN_GPT2 : EOS_TOKEN_LLAMA;

  // ── Do: command mode detection ────────────────────────────────────────────
  // When the prompt ends with the Do: token (id=5), the model should output
  // a short command (1-2 tokens) and stop.  We detect explanation tokens
  // (period, comma, "to", "for", etc.) and stop early to prevent the model
  // from appending "to show the current status..." after the command.
  static constexpr int DO_TOKEN_ID = 5;
  const bool isDoMode = (num_prompt_tokens > 0 &&
                         prompt_tokens[num_prompt_tokens - 1] == DO_TOKEN_ID);
  if (isDoMode) {
    DEBUG_LLM_GENERATEF("[LLM] Do: command mode detected — will stop on explanation tokens");
  }

  // ── Repetition penalty ring buffer ────────────────────────────────────────
  // Penalises tokens seen in the last repWindow positions to reduce loops.
  // repPenalty=1.0 disables it entirely; repWindow clamped to pre-allocated size.
  const int   REP_WINDOW  = (repWindow > 0) ? (repWindow < gLLM.repBufSize ? repWindow : gLLM.repBufSize) : 0;
  const float REP_PENALTY = (repPenalty > 1.0f) ? repPenalty : 1.0f;
  int* rep_buf = (REP_WINDOW > 0) ? gLLM.repBuf : nullptr;
  if (rep_buf) memset(rep_buf, -1, REP_WINDOW * sizeof(int));
  int rep_head = 0;   // next write position (circular)
  int rep_fill = 0;   // how many valid entries (caps at REP_WINDOW)

  // ── Sentence limit stop tracker ──────────────────────────────────────────
  // Stops generation after sentenceLimit sentence-ending punctuations followed
  // by a space or newline.  Keeps responses concise and avoids runaway output.
  // sentenceLimit=0 disables this entirely.
  int  sentence_count = 0;
  char sent_prev_char = '\0'; // last character seen across token pieces

  DEBUG_LLM_GENERATEF("[LLM] Generate: prompt_tokens=%d max=%d temp=%.2f topp=%.2f eos=%d mirostat=%d tau=%.1f",
                      num_prompt_tokens, maxTokens, temperature, topp, eos_id,
                      (int)useMirostat2, mirostatTau);
  DEBUG_LLM_GENERATEF("[LLM]   rep_penalty=%.2f rep_window=%d sentence_limit=%d hard_cap=%d suppress=%d",
                      REP_PENALTY, REP_WINDOW, sentenceLimit, hardCap, suppressTokenCount);
  DEBUG_LLM_GENERATEF("[LLM]   arch=%s dim=%d layers=%d heads=%d vocab=%d seq=%d quant=%s",
                      isGPT2gen ? "GPT2" : "Llama",
                      p->dim, p->n_layers, p->n_heads, p->vocab_size, p->seq_len,
                      (p->quant_type == 1) ? "INT8" : "FP32");

  // ── Generation loop ─────────────────────────────────────────────────────────
  unsigned long startMs = millis();

  int steps = std::min(maxTokens + num_prompt_tokens, S);
  float mirostat_mu = 2.0f * mirostatTau;

  while (pos < steps) {
    if (gLLM.stopRequested) break;

    float* logits = forward(token, pos);

    int next;
    if (pos < num_prompt_tokens - 1) {
      // Still in prompt — force next prompt token
      next = prompt_tokens[pos + 1];
      //DEBUG_LLM_GENERATEF("[LLM]  pos=%d prompt_token=%d", pos, next);

      // ── Prompt prediction tracking ────────────────────────────────────────
      // At each prompt position, check what the model WOULD predict next.
      // This reveals whether the model is understanding the prompt as it goes.
      // If the model correctly predicts upcoming prompt tokens (e.g. "Q:" → " How",
      // " turn" → " off"), it's tracking the input. If it predicts unrelated
      // tokens, the prompt content isn't being encoded into the hidden state.
      {
        int actual_next = prompt_tokens[pos + 1];
        float actual_logit = (actual_next >= 0 && actual_next < p->vocab_size) ? logits[actual_next] : -999.f;
        // Find rank
        int rank = 0;
        for (int vi = 0; vi < p->vocab_size; vi++) {
          if (logits[vi] > actual_logit) rank++;
        }
        // Find top predicted token
        int top_tok = 0;
        float top_logit = logits[0];
        for (int vi = 1; vi < p->vocab_size; vi++) {
          if (logits[vi] > top_logit) { top_logit = logits[vi]; top_tok = vi; }
        }
        const char* actual_name = (actual_next >= 0 && actual_next < gLLM.tokenizer.vocab_size) ?
                                   gLLM.tokenizer.vocab[actual_next] : "?";
        const char* top_name = (top_tok >= 0 && top_tok < gLLM.tokenizer.vocab_size) ?
                                gLLM.tokenizer.vocab[top_tok] : "?";
        DEBUG_LLM_GENERATEF("[LLM] PROMPT_PRED pos=%d: actual_next=%d('%s') rank=%d/%d logit=%.1f | "
                            "model_top=%d('%s') logit=%.1f %s",
                            pos, actual_next, actual_name, rank, p->vocab_size, actual_logit,
                            top_tok, top_name, top_logit,
                            rank == 0 ? "CORRECT!" : (rank < 10 ? "CLOSE" : "WRONG"));
      }

      // At the last prompt position, dump the logits the model WOULD produce
      // if it were generating. This shows what the model "thinks" should follow
      // the prompt — even though we override it with the next prompt token.
      // Reveals whether the model is tracking the prompt or already lost.
      if (pos == num_prompt_tokens - 2) {
        // Find top-5 predicted tokens at this position
        float top5v[5] = {-1e30f,-1e30f,-1e30f,-1e30f,-1e30f};
        int   top5i[5] = {0,0,0,0,0};
        for (int i = 0; i < p->vocab_size; i++) {
          float v = logits[i];
          if (isnan(v) || isinf(v)) continue;
          for (int k = 0; k < 5; k++) {
            if (v > top5v[k]) {
              for (int m = 4; m > k; m--) { top5i[m] = top5i[m-1]; top5v[m] = top5v[m-1]; }
              top5i[k] = i; top5v[k] = v;
              break;
            }
          }
        }
        // Check if the actual next prompt token is in the top predictions
        int actual_next = prompt_tokens[pos + 1];
        float actual_logit = (actual_next >= 0 && actual_next < p->vocab_size) ? logits[actual_next] : -999.f;
        int actual_rank = -1;
        int rank = 0;
        for (int i = 0; i < p->vocab_size; i++) {
          if (logits[i] > actual_logit) rank++;
        }
        actual_rank = rank;

        const char* next_name = (actual_next >= 0 && actual_next < gLLM.tokenizer.vocab_size) ?
                                 gLLM.tokenizer.vocab[actual_next] : "?";
        DEBUG_LLM_GENERATEF("[LLM] PROMPT_TRACK pos=%d: model predicts top5=[%d(%s:%.1f) %d(%s:%.1f) %d(%s:%.1f) %d(%s:%.1f) %d(%s:%.1f)]",
                            pos,
                            top5i[0], (top5i[0]<gLLM.tokenizer.vocab_size?gLLM.tokenizer.vocab[top5i[0]]:"?"), top5v[0],
                            top5i[1], (top5i[1]<gLLM.tokenizer.vocab_size?gLLM.tokenizer.vocab[top5i[1]]:"?"), top5v[1],
                            top5i[2], (top5i[2]<gLLM.tokenizer.vocab_size?gLLM.tokenizer.vocab[top5i[2]]:"?"), top5v[2],
                            top5i[3], (top5i[3]<gLLM.tokenizer.vocab_size?gLLM.tokenizer.vocab[top5i[3]]:"?"), top5v[3],
                            top5i[4], (top5i[4]<gLLM.tokenizer.vocab_size?gLLM.tokenizer.vocab[top5i[4]]:"?"), top5v[4]);
        DEBUG_LLM_GENERATEF("[LLM] PROMPT_TRACK actual_next=%d('%s') logit=%.2f rank=%d/%d %s",
                            actual_next, next_name, actual_logit, actual_rank, p->vocab_size,
                            actual_rank < 5 ? "IN_TOP5" : (actual_rank < 20 ? "IN_TOP20" : "LOW_RANK"));
      }
    } else {
      // ── First generation position diagnostics ────────────────────────────────
      // Fires once at the critical prompt→generation boundary. Dumps:
      //   - Embedding similarity summary across all prompt tokens
      //   - KV cache health check (are prompt KV entries non-zero?)
      //   - Logits for prompt tokens themselves (does the model recall them?)
      if (pos == num_prompt_tokens - 1) {
        DEBUG_LLM_GENERATEF("[LLM] ═══ GENERATION START ═══ pos=%d after %d prompt tokens",
                            pos, num_prompt_tokens);

        // Embedding similarity summary
        //DEBUG_LLM_GENERATEF("[LLM] EMBEDDING SIMILARITY (cosine) across prompt tokens:");
        for (int ti = 1; ti < num_prompt_tokens && ti < 32; ti++) {
          float prev_norm = gPromptDiag.emb_norms[ti - 1];
          float cur_norm  = gPromptDiag.emb_norms[ti];
          float dot       = gPromptDiag.emb_dots[ti];
          float cosim = (prev_norm > 1e-8f && cur_norm > 1e-8f) ?
                        dot / (prev_norm * cur_norm) : 0.f;
          const char* prev_name = (prompt_tokens[ti-1] >= 0 && prompt_tokens[ti-1] < gLLM.tokenizer.vocab_size) ?
                                   gLLM.tokenizer.vocab[prompt_tokens[ti-1]] : "?";
          const char* cur_name  = (prompt_tokens[ti] >= 0 && prompt_tokens[ti] < gLLM.tokenizer.vocab_size) ?
                                   gLLM.tokenizer.vocab[prompt_tokens[ti]] : "?";
          //DEBUG_LLM_GENERATEF("[LLM]   '%s'<->'%s': cosine=%.4f norm=[%.3f,%.3f]",
          //                    prev_name, cur_name, cosim, prev_norm, cur_norm);
          (void)prev_name; (void)cur_name; (void)cosim; (void)prev_norm; (void)cur_norm; (void)dot;
        }

        // KV cache health: check that prompt positions have non-zero K/V
        int kv_d = (p->dim * p->n_kv_heads) / p->n_heads;
        int check_layers[] = {0, p->n_layers/2, p->n_layers-1};
        for (int cli = 0; cli < 3; cli++) {
          int cl = check_layers[cli];
          int cloff = cl * S * kv_d;
          for (int t = 0; t < num_prompt_tokens; t++) {
            float k_l2 = 0.f, v_l2 = 0.f;
            float* krow = gLLM.state.key_cache + cloff + t * kv_d;
            float* vrow = gLLM.state.value_cache + cloff + t * kv_d;
            for (int i = 0; i < kv_d; i++) {
              k_l2 += krow[i] * krow[i];
              v_l2 += vrow[i] * vrow[i];
            }
            k_l2 = sqrtf(k_l2 / kv_d);
            v_l2 = sqrtf(v_l2 / kv_d);
            const char* tname = (prompt_tokens[t] >= 0 && prompt_tokens[t] < gLLM.tokenizer.vocab_size) ?
                                 gLLM.tokenizer.vocab[prompt_tokens[t]] : "?";
            (void)tname;
            if (k_l2 < 0.001f || v_l2 < 0.001f) {
              DEBUG_LLM_GENERATEF("[LLM] WARNING KV_CACHE L%d pos=%d '%s': K_L2=%.4f V_L2=%.4f NEARLY_ZERO!",
                                  cl, t, tname, k_l2, v_l2);
            }
          }
        }

        // Check logits for prompt content tokens — does model "recall" them?
        // If wifi (tok 485) has low logit at generation start, model didn't attend to it
        DEBUG_LLM_GENERATEF("[LLM] PROMPT TOKEN LOGIT CHECK at generation start:");
        for (int ti = 0; ti < num_prompt_tokens; ti++) {
          int ptok = prompt_tokens[ti];
          if (ptok >= 0 && ptok < p->vocab_size) {
            float ptok_logit = logits[ptok];
            // Find rank of this token
            int rank = 0;
            for (int vi = 0; vi < p->vocab_size; vi++) {
              if (logits[vi] > ptok_logit) rank++;
            }
            const char* pname = (ptok < gLLM.tokenizer.vocab_size) ?
                                 gLLM.tokenizer.vocab[ptok] : "?";
            DEBUG_LLM_GENERATEF("[LLM]   prompt_tok[%d]=%d('%s') logit=%.2f rank=%d/%d",
                                ti, ptok, pname, ptok_logit, rank, p->vocab_size);
            (void)ptok_logit; (void)rank; (void)pname;
          }
        }
        DEBUG_LLM_GENERATEF("[LLM] ═══════════════════════════════════════════════════");
      }

      // ── Repetition penalty ─────────────────────────────────────────────────
      // Apply to raw logits before temperature scaling.  Tokens in the recent
      // window have their logit divided (if positive) or multiplied (if negative)
      // by REP_PENALTY, making them less likely without zeroing them entirely.
      // Content tokens (topic words from the prompt) are EXEMPT — the model
      // should be allowed to repeat "WiFi" or "sensor" across sentences.
      if (rep_buf && REP_PENALTY > 1.0f) {
        int count = (rep_fill < REP_WINDOW) ? rep_fill : REP_WINDOW;
        int penalized = 0;
        for (int ri = 0; ri < count; ri++) {
          int tok = rep_buf[ri];
          if (tok >= 0 && tok < p->vocab_size) {
            // Skip content tokens — don't penalize on-topic words
            bool isContent = false;
            for (int ci = 0; ci < content_token_count; ci++) {
              if (content_tokens[ci] == tok) { isContent = true; break; }
            }
            if (isContent) continue;
            if (logits[tok] > 0.0f) logits[tok] /= REP_PENALTY;
            else                     logits[tok] *= REP_PENALTY;
            penalized++;
          }
        }
        DEBUG_LLM_GENERATEF("[LLM] rep_penalty: penalized %d/%d tokens (window=%d penalty=%.2f, %d content-exempt)",
                            penalized, count, REP_WINDOW, REP_PENALTY, content_token_count);
      }

      // ── Suppress penalty (retry mechanism) ──────────────────────────────────
      // Penalize tokens from previous answers so the model generates a different
      // response. Unlike rep penalty (ring buffer), this persists for the entire
      // generation. Skips tokens that appear in the prompt to avoid penalizing
      // on-topic words (e.g. "wifi" in a WiFi question).
      if (suppressTokens && suppressTokenCount > 0) {
        static constexpr float SUPPRESS_PENALTY = 2.0f;
        int penalized = 0, skipped_prompt = 0;
        for (int si = 0; si < suppressTokenCount; si++) {
          int tok = suppressTokens[si];
          if (tok <= 4 || tok >= p->vocab_size) continue;  // skip special tokens
          // Don't penalize tokens that appear in the prompt — they're on-topic
          bool inPrompt = false;
          for (int pi = 0; pi < num_prompt_tokens; pi++) {
            if (prompt_tokens[pi] == tok) { inPrompt = true; break; }
          }
          if (inPrompt) { skipped_prompt++; continue; }
          if (logits[tok] > 0.0f) logits[tok] /= SUPPRESS_PENALTY;
          else                     logits[tok] *= SUPPRESS_PENALTY;
          penalized++;
        }
        if (pos == num_prompt_tokens || pos % 16 == 0) {
          DEBUG_LLM_GENERATEF("[LLM] suppress: penalized %d/%d tokens, skipped %d prompt tokens (penalty=%.1f)",
                              penalized, suppressTokenCount, skipped_prompt, SUPPRESS_PENALTY);
        }
      }

      // ── Prompt content logit boost (first 10 generated tokens only) ────────
      // Nudge the model toward on-topic answers by boosting logits for tokens
      // that appeared as content words in the prompt. Applied only during the
      // first 10 generated tokens get full boost to latch onto the right topic.
      // After that, a weaker "late" boost continues through the entire generation
      // to prevent sentence 2 from drifting off-topic.
      int gen_pos = pos - num_prompt_tokens + 1;  // 0-based generated token index
      if (content_token_count > 0) {
        float boost = (gen_pos < CONTENT_BOOST_WINDOW) ? CONTENT_LOGIT_BOOST : CONTENT_LOGIT_BOOST_LATE;
        int boosted = 0;
        for (int ci = 0; ci < content_token_count; ci++) {
          int ctok = content_tokens[ci];
          if (ctok >= 0 && ctok < p->vocab_size) {
            float old_logit = logits[ctok];
            logits[ctok] += boost;
            boosted++;
            if (pos == num_prompt_tokens - 1 || pos % 8 == 0) {
              const char* cname = (ctok < gLLM.tokenizer.vocab_size) ?
                                   gLLM.tokenizer.vocab[ctok] : "?";
              DEBUG_LLM_GENERATEF("[LLM] CONTENT_BOOST pos=%d gen=%d tok=%d('%s') %.2f -> %.2f (+%.1f)",
                                  pos, gen_pos, ctok, cname,
                                  old_logit, logits[ctok], boost);
            }
          }
        }
      }

      // Sentence-aware temperature taper: reduce temperature slightly after the
      // first sentence to prevent second-sentence drift. The model is less
      // confident later in generation, so tighter sampling keeps it on-topic.
      // Scales temp by 0.8 once a sentence boundary has been seen.
      float effective_temp = temperature;
      if (sentence_count > 0 && temperature > 0.0f) {
        effective_temp = temperature * 0.8f;
        if (gen_pos % 8 == 0) {
          DEBUG_LLM_GENERATEF("[LLM] TEMP_TAPER gen=%d sent=%d: base=%.3f -> eff=%.3f (x0.8)",
                              gen_pos, sentence_count, temperature, effective_temp);
        }
      }

      // Dynamic temperature: optionally scale base temperature using top logit as a
      // confidence proxy. Disabled by default — for memorization/domain models, flat
      // temperature produces better recall. Enable via dyn_temp=true in the generate API.
      if (dynTemp && effective_temp > 0.0f) {
        float max_logit = logits[0];
        for (int vi = 1; vi < p->vocab_size; vi++) {
          if (logits[vi] > max_logit) max_logit = logits[vi];
        }
        float boost = 1.0f + (max_logit - 6.0f) * 0.08f;
        if (boost < 0.6f) boost = 0.6f;
        if (boost > 1.8f) boost = 1.8f;
        effective_temp = temperature * boost;
        DEBUG_LLM_GENERATEF("[LLM] pos=%d dyn_temp: max_logit=%.2f boost=%.2f eff=%.3f (base=%.2f)",
                            pos, max_logit, boost, effective_temp, temperature);
      }

      int topId = 0;
      float topLogit = logits[0];
      for (int vi = 1; vi < p->vocab_size; vi++) {
        if (logits[vi] > topLogit) { topLogit = logits[vi]; topId = vi; }
      }
      // topId and topLogit used by per-token sample debug line below

      // Tighten nucleus after first sentence to reduce second-sentence drift
      float effective_topp = topp;
      if (sentence_count > 0 && topp > 0.0f) {
        effective_topp = topp * 0.75f;  // 0.8 -> 0.6 after first sentence
        if (gen_pos % 8 == 0) {
          DEBUG_LLM_GENERATEF("[LLM] TOPP_TAPER gen=%d sent=%d: base=%.3f -> eff=%.3f (x0.75)",
                              gen_pos, sentence_count, topp, effective_topp);
        }
      }

      if (useMirostat2) {
        next = sample_mirostat2(logits, p->vocab_size, effective_temp,
                                mirostatTau, mirostatEta, &mirostat_mu);
      } else {
        next = sample(logits, p->vocab_size, effective_temp, effective_topp);
      }

      // Clamp to valid vocab range — prevents OOV tokens from corrupting output
      if (next < 0 || next >= p->vocab_size) {
        DEBUG_LLM_GENERATEF("[LLM] WARNING: Sampled OOV token %d (vocab_size=%d), clamped to 0", next, p->vocab_size);
        next = 0;  // fallback to first token if somehow OOV
      }

      const char* piece = decode(token, next);
      bool has_leading_space = (piece && piece[0] == ' ');
      int piece_len = piece ? strlen(piece) : 0;
      DEBUG_LLM_GENERATEF("[LLM]  pos=%d sampled=%d('%s') top=%d(%.2f) eff_temp=%.2f eff_topp=%.2f sent=%d gen=%d",
                          pos, next, piece ? piece : "?", topId, topLogit,
                          effective_temp, effective_topp, sentence_count, gen_pos);
    }

    pos++;

    // EOS check
    if (next == eos_id) {
      DEBUG_LLM_GENERATEF("[LLM]  EOS token %d at pos=%d — stopping", eos_id, pos);
      break;
    }

    // Stop if model tries to start a new Q&A pair (token 3 = "Q:")
    // Only during generation (not prompt feeding), prevents runaway Q&A continuation
    if (pos >= num_prompt_tokens && next == 3) {
      DEBUG_LLM_GENERATEF("[LLM]  Q: token (id=3) at pos=%d — stopping to prevent Q&A continuation", pos);
      break;
    }

    // ── Do: mode smart stop ──────────────────────────────────────────────
    // In Do: mode, the model should output a command (1-2 tokens) and stop.
    // After at least 1 generated token, stop if we see punctuation or
    // explanation words — these signal the model is explaining, not commanding.
    if (isDoMode && pos >= num_prompt_tokens && generated > 0) {
      bool doStop = false;
      const char* dp = (next >= 0 && next < gLLM.tokenizer.vocab_size)
                        ? gLLM.tokenizer.vocab[next] : nullptr;
      if (dp) {
        // Normalize: skip leading space
        const char* dw = dp;
        if (*dw == ' ') dw++;
        // Stop on punctuation
        if (dw[0] == '.' || dw[0] == ',' || dw[0] == '?' || dw[0] == '!' || dw[0] == ';') {
          DEBUG_LLM_GENERATEF("[LLM] Do: mode — stopping on punctuation '%s' at generated=%d", dp, generated);
          doStop = true;
        }
        // Stop on explanation words (model is adding "to show...", "for the...", etc.)
        if (!doStop) {
          static const char* const DO_STOP_WORDS[] = {
            "to", "for", "and", "the", "is", "it", "that", "this", "which",
            "will", "can", "shows", "displays", "checks", "reads",
          };
          static constexpr int NUM_DO_STOP = sizeof(DO_STOP_WORDS) / sizeof(DO_STOP_WORDS[0]);
          for (int dsi = 0; dsi < NUM_DO_STOP; dsi++) {
            if (strcasecmp(dw, DO_STOP_WORDS[dsi]) == 0) {
              DEBUG_LLM_GENERATEF("[LLM] Do: mode — stopping on explanation word '%s' at generated=%d", dp, generated);
              doStop = true;
              break;
            }
          }
        }
      }
      if (doStop) break;
    }

    // Decode and emit token
    if (pos >= num_prompt_tokens) {
      const char* piece = decode(token, next);

      // ── Sentence stop (checked BEFORE emit) ──────────────────────────────
      // Scan each character in the decoded piece.  A sentence ends when a
      // '.', '?', or '!' is followed by a space, newline, '"', or '\'' (to
      // handle quoted-speech endings like `."` or `!'`).
      // sentenceLimit=0 disables this entirely.
      // We check before emitting so we don't output a dangling token from
      // the start of the next sentence (e.g. "They" after 3 complete sentences).
      if (sentenceLimit > 0 && piece) {
        for (int ci = 0; piece[ci] != '\0'; ci++) {
          char c = piece[ci];
          if ((sent_prev_char == '.' || sent_prev_char == '?' || sent_prev_char == '!')
              && (c == ' ' || c == '\n' || c == '"' || c == '\'')) {
            sentence_count++;
            DEBUG_LLM_GENERATEF("[LLM] Sentence %d ended at generated=%d", sentence_count, generated);
            if (sentence_count >= sentenceLimit) break;
          }
          sent_prev_char = c;
        }
        if (sentence_count >= sentenceLimit) {
          DEBUG_LLM_GENERATEF("[LLM] Sentence limit (%d) reached at generated=%d — suppressing token", sentenceLimit, generated);
          break;
        }
      }

      if (piece && tokenCb) {
        if (!tokenCb(piece)) break;  // callback requested stop
      }
      generated++;

      // ── Hard token cap ────────────────────────────────────────────────────
      // Stop unconditionally after hardCap generated tokens (0 = disabled).
      if (hardCap > 0 && generated >= hardCap) {
        DEBUG_LLM_GENERATEF("[LLM] Hard cap (%d) reached at generated=%d", hardCap, generated);
        break;
      }

      // Update sent_prev_char for tokens that don't trigger the limit
      // (already updated in the sentence-check loop above)
    }

    // ── Update repetition-penalty ring buffer ─────────────────────────────
    if (rep_buf && REP_WINDOW > 0) {
      rep_buf[rep_head] = next;
      rep_head = (rep_head + 1) % REP_WINDOW;
      if (rep_fill < REP_WINDOW) rep_fill++;
    }

    token = next;

    // Periodic generation health check
    if (generated > 0 && generated % HEALTH_LOG_INTERVAL == 0) {
      unsigned long nowMs = millis();
      float elapsed_s = (float)(nowMs - startMs) / 1000.0f;
      float tps = elapsed_s > 0 ? (float)generated / elapsed_s : 0;
      // Count unique tokens in recent window to detect repetition collapse
      int unique = 0;
      if (rep_buf && rep_fill > 0) {
        int count = (rep_fill < REP_WINDOW) ? rep_fill : REP_WINDOW;
        // Simple uniqueness: count distinct values in rep buffer
        for (int ri = 0; ri < count; ri++) {
          bool dup = false;
          for (int rj = 0; rj < ri; rj++) {
            if (rep_buf[ri] == rep_buf[rj]) { dup = true; break; }
          }
          if (!dup) unique++;
        }
      }
      DEBUG_LLM_GENERATEF("[LLM] gen_health: generated=%d tok/s=%.1f mu=%.2f unique=%d/%d ctx=%d/%d",
                          generated, tps, mirostat_mu, unique,
                          rep_buf ? ((rep_fill < REP_WINDOW) ? rep_fill : REP_WINDOW) : 0,
                          pos, S);
    }

    // Yield periodically to avoid watchdog
    if (generated % YIELD_INTERVAL == 0) {
      vTaskDelay(1);
    }
  }

  // ── Finalization ────────────────────────────────────────────────────────────
  // Clean up prompt diagnostics
  gPromptDiag.active = false;
  gPromptDiag.tokens = nullptr;
  gPromptDiag.count  = 0;
  gPromptDiag.num_prompt_tokens = 0;

  unsigned long elapsed = millis() - startMs;
  if (elapsed > 0 && generated > 0) {
    gLLM.lastTokPerSec = (float)generated / ((float)elapsed / 1000.0f);
  } else {
    gLLM.lastTokPerSec = 0;
  }
  gLLM.lastTokCount = generated;
  gLLM.lastContextUsed = pos;      // total positions consumed (prompt + generated)
  gLLM.lastContextMax = S;         // KV cache capacity for this run

  DEBUG_LLM_GENERATEF("[LLM] Generated %d tokens in %lums (%.1f tok/s) ctx=%d/%d stopped=%s",
                      generated, elapsed, gLLM.lastTokPerSec, pos, S,
                      gLLM.stopRequested ? "user" : (generated == 0 ? "eos/empty" : "maxlen"));

  free(prompt_tokens);
  if (norm_prompt) free(norm_prompt);

  gLLM.runState = LLMState::READY;
  return generated;
}

void llmStop() {
  gLLM.stopRequested = true;
}

int llmTokenize(const char* text, int* outTokens, int maxTokens) {
  if (!text || !outTokens || maxTokens <= 0) return 0;
  if (gLLM.tokenizer.vocab_size == 0) return 0;  // no model loaded
  return encode(text, outTokens, maxTokens);
}

String llmListModels() {
  String json = "[";
  bool first = true;

  // Scan a directory and append model entries to json
  // dirPath: full VFS path e.g. "/system/llm" or "/sd/llm"
  // storage: "internal" or "sd"
  auto scanDir = [&](const char* dirPath, const char* storage) {
    File dir = VFS::open(dirPath, FILE_READ);
    if (!dir || !dir.isDirectory()) return;
    File entry;
    while ((entry = dir.openNextFile())) {
      String name = entry.name();
      // Strip directory prefix — SD library may return full path, LittleFS returns just filename
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      String nameLower = name; nameLower.toLowerCase();
      if (nameLower.endsWith(".bin")) {
        if (!first) json += ",";
        // Build full path for loading (pre-reserved to avoid chained allocs)
        String fullPath;
        fullPath.reserve(strlen(dirPath) + 1 + name.length());
        fullPath = dirPath;
        fullPath += '/';
        fullPath += name;
        json += "{\"name\":\"";
        json += name;
        json += "\",\"size\":";
        json += (unsigned long)entry.size();
        json += ",\"path\":\"";
        json += fullPath;
        json += "\",\"storage\":\"";
        json += storage;
        json += "\"}";
        first = false;
      }
      entry.close();
    }
    dir.close();
  };

  // Always scan internal LittleFS location
  scanDir("/system/llm", "internal");

  // Scan SD card if present
  if (VFS::isSDAvailable()) {
    scanDir("/sd/llm", "sd");
  }

  json += "]";
  return json;
}

// ============================================================================
// 12. CLI Commands
// ============================================================================

#include "System_Utils.h"  // CommandEntry

static char llmCmdBuf[512];

static const char* cmd_llm_status(const String&) {
  LLMStatus st = llmGetStatus();
  const char* stateStr = "UNLOADED";
  switch (st.state) {
    case LLMState::LOADING:    stateStr = "LOADING"; break;
    case LLMState::READY:      stateStr = "READY"; break;
    case LLMState::GENERATING: stateStr = "GENERATING"; break;
    case LLMState::ERROR:      stateStr = "ERROR"; break;
    default: break;
  }
  snprintf(llmCmdBuf, sizeof(llmCmdBuf),
    "LLM State: %s\n"
    "Model: %s\n"
    "Config: dim=%d layers=%d heads=%d vocab=%d seq=%d ctx=%d\n"
    "PSRAM: %uKB (weights=%uKB runtime=%uKB)\n"
    "Last: %d tokens @ %.1f tok/s\n"
    "%s%s",
    stateStr, st.modelPath,
    st.config.dim, st.config.n_layers, st.config.n_heads,
    st.config.vocab_size, st.config.seq_len, st.lastContextMax,
    (unsigned)(st.totalPsramUsed / 1024),
    (unsigned)(st.modelSizeBytes / 1024),
    (unsigned)(st.runtimeSizeBytes / 1024),
    st.lastTokenCount, st.lastTokensPerSec,
    st.errorMsg[0] ? "Error: " : "",
    st.errorMsg[0] ? st.errorMsg : "");
  return llmCmdBuf;
}

static const char* cmd_llm_load(const String& args) {
  // Parse model name from args: "llm load [model.bin]"
  String a = args;
  a.trim();
  int sp = a.indexOf(' ');
  if (sp > 0) a = a.substring(sp + 1);  // skip "load"
  a.trim();
  sp = a.indexOf(' ');
  if (sp > 0) a = a.substring(sp + 1);  // skip second word if present
  a.trim();

  const char* modelPath = LLM_DEFAULT_MODEL_PATH;

  char customPath[96];
  if (a.length() > 0) {
    if (a.startsWith("/")) {
      // Full path provided — use as-is
      strlcpy(customPath, a.c_str(), sizeof(customPath));
    } else {
      // Bare filename — try SD card first, then internal
      snprintf(customPath, sizeof(customPath), "/sd/llm/%s", a.c_str());
      if (!VFS::isSDAvailable() || !VFS::exists(customPath)) {
        snprintf(customPath, sizeof(customPath), "/system/llm/%s", a.c_str());
      }
    }
    modelPath = customPath;
  }

  bool ok = llmLoadModel(modelPath);
  if (ok) {
    snprintf(llmCmdBuf, sizeof(llmCmdBuf), "Model loaded: %s", modelPath);
  } else {
    LLMStatus st = llmGetStatus();
    snprintf(llmCmdBuf, sizeof(llmCmdBuf), "Load failed: %s", st.errorMsg);
  }
  return llmCmdBuf;
}

static const char* cmd_llm_unload(const String&) {
  llmUnload();
  return "Model unloaded";
}

static const char* cmd_llm_models(const String&) {
  String models = llmListModels();
  bool sdAvail = VFS::isSDAvailable();
  snprintf(llmCmdBuf, sizeof(llmCmdBuf), "Models (internal + %s):\n%s",
           sdAvail ? "SD card" : "no SD card", models.c_str());
  return llmCmdBuf;
}

static const char* cmd_llm_generate(const String& args) {
  if (!llmIsReady()) return "Error: no model loaded";

  // Extract prompt from "llm generate <prompt>"
  String a = args;
  a.trim();
  // Skip "llm"
  int sp = a.indexOf(' ');
  if (sp > 0) a = a.substring(sp + 1);
  a.trim();
  // Skip "generate"
  sp = a.indexOf(' ');
  if (sp > 0) a = a.substring(sp + 1);
  a.trim();

  if (a.length() == 0) return "Usage: llm generate <prompt>";

  // Build output into buffer
  String output;
  output.reserve(1024);

  int result = llmGenerate(a.c_str(), [&output](const char* token) -> bool {
    output += token;
    return (output.length() < 2000);  // safety limit for CLI
  }, 128, LLM_DEFAULT_TEMPERATURE);

  if (result < 0) {
    snprintf(llmCmdBuf, sizeof(llmCmdBuf), "Generation error");
  } else {
    // Truncate if needed
    if (output.length() >= sizeof(llmCmdBuf) - 32) {
      output = output.substring(0, sizeof(llmCmdBuf) - 32);
      output += "\n[truncated]";
    }
    snprintf(llmCmdBuf, sizeof(llmCmdBuf), "%s\n(%d tokens)", output.c_str(), result);
  }
  return llmCmdBuf;
}

static const char* cmd_llm_stop(const String&) {
  llmStop();
  return "Stop requested";
}

const CommandEntry llmCommands[] = {
  { "llmstatus",   "Show LLM engine status",           false, cmd_llm_status },
  { "llmload",     "Load model [model.bin]",            true,  cmd_llm_load,     "Usage: llmload [filename.bin]" },
  { "llmunload",   "Unload model and free PSRAM",       true,  cmd_llm_unload },
  { "llmmodels",   "List available model files",        false, cmd_llm_models },
  { "llmgenerate", "Generate text from prompt",         false, cmd_llm_generate, "Usage: llmgenerate <prompt text>" },
  { "llmstop",     "Stop in-progress generation",       false, cmd_llm_stop },
};
const size_t llmCommandsCount = sizeof(llmCommands) / sizeof(llmCommands[0]);

#endif // ENABLE_ONDEVICE_LLM
