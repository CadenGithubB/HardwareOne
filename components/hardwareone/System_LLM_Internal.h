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

// KV-cache storage precision, chosen at model load (RunState binding + forward()
// branch on it). FP32 = baseline. FP16 = half the PSRAM, ~no quality loss, no
// scales. INT8 reserved (not yet implemented; selecting it falls back to FP32).
enum KVPrecision : uint8_t { KV_FP32 = 0, KV_FP16 = 1, KV_INT8 = 2 };

// Total cold-block bytes for the KV cache (K+V) at the given precision and ctx.
// INT8 also carries per-(layer,pos,kv-head) scales (FP32), 4-byte aligned after
// the int8 data. Single source of truth shared by sizing, alloc, and binding.
static inline size_t llmKvCacheBytes(uint8_t prec, int L, int ctx, int kv_dim, int n_kv_heads) {
  if (prec == KV_INT8) {
    size_t data = 2 * (size_t)L * ctx * kv_dim;        // int8 K + V
    data = (data + 3u) & ~(size_t)3u;                  // align the trailing FP32 scales
    size_t scales = 2 * (size_t)L * ctx * (size_t)n_kv_heads * sizeof(float);
    return data + scales;
  }
  size_t elem = (prec == KV_FP16) ? sizeof(uint16_t) : sizeof(float);
  return 2 * (size_t)L * ctx * kv_dim * elem;
}

struct RunState {
  float* x;          // activation at current position (dim,)
  float* xb;         // same, inside a residual branch (dim,)
  float* xb2;        // additional buffer (dim,)
  float* hb;         // buffer for hidden dimension in ffn (hidden_dim,)
  float* hb2;        // buffer for hidden dimension in ffn (hidden_dim,)
  float* q;          // query (dim,)
  float* key_cache;  // FP32 KV cache (layer, seq_len, kv_dim) — used when kvPrecision==FP32
  float* value_cache;// FP32 KV cache (layer, seq_len, kv_dim) — used when kvPrecision==FP32
  // FP16 KV cache (half-float storage). Used when kvPrecision==FP16; the FP32
  // pointers above are null in that mode (and vice-versa). att/logits stay FP32.
  uint16_t* key_cache_f16;
  uint16_t* value_cache_f16;
  // INT8 KV cache (per-kv-head symmetric quant). Used when kvPrecision==INT8;
  // null otherwise. Scales are per (layer, position, kv-head): max|x|/127.
  int8_t*   key_cache_q8;
  int8_t*   value_cache_q8;
  float*    key_scales;      // (n_layers * seq_ctx * n_kv_heads)
  float*    value_scales;    // (n_layers * seq_ctx * n_kv_heads)
  // Working scratch for non-FP32 KV: linear() emits FP32 into k_tmp/v_tmp, then
  // we pack into the cache; kv_deq dequantizes a head-slice on read. Hot block,
  // only carved when kvPrecision != FP32 (nullptr otherwise → no DRAM cost).
  float* k_tmp;      // (kv_dim,)
  float* v_tmp;      // (kv_dim,)
  float* kv_deq;     // (head_size,)
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

// One guided-menu group's location inside gLLM.menuBlob. Offsets are byte
// offsets into the blob; the template/entity lists are consecutive
// length-prefixed items ([u8 len][UTF-8]) walked on demand. See
// LLM_GUIDED_MENU_SPEC §5 and System_LLM_Menu.cpp.
struct LLMMenuGroupDesc {
  uint32_t nameOff;   // offset of the group name bytes (nameLen bytes)
  uint32_t tplOff;    // offset of the first template item
  uint32_t entOff;    // offset of the first entity item
  uint16_t tplCount;  // number of templates (<=64)
  uint16_t entCount;  // number of entities (<=1024)
  uint8_t  flags;     // bit0 = Do-mode; bits1-7 reserved 0
  uint8_t  nameLen;   // group name length (<=32)
};

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
  float lastMeanLogprob; // Phase 2: mean log-prob of generated tokens (0 = no signal). Less negative = more confident.
  int lastConfTokens;    // Phase 2: # generated tokens that contributed to lastMeanLogprob
  char modelPath[64];
  char errorMsg[128];

  // Optional per-model metadata from the LLM1 "info block" (header offset 24 =
  // info_len; block sits between the 64-byte header and the tokenizer). Zeroed on
  // every load/unload; populated by loadInfoBlockFromFile() when present. Fixed
  // storage — llmModelDescription()/llmModelIcon() hand out zero-copy pointers.
  char    modelDesc[256];   // NUL-terminated UTF-8 description ("" if none)
  // Capabilities the MODEL declares about itself (CAPS section, id 5). Zero when
  // the section is absent, malformed, or carries an unrecognised version --
  // i.e. a model that does not say it is capable is treated as not capable.
  uint16_t modelCaps;
  uint8_t modelIcon[128];   // 1bpp MSB-first, row-major; up to 32x32
  uint8_t modelIconW;       // icon width in pixels (0 if no icon)
  uint8_t modelIconH;       // icon height in pixels (0 if no icon)
  bool    modelHasIcon;     // true when modelIcon holds a valid bitmap

  // Domain refusal gate (optional, appended to the info block after the icon).
  // When domainVocabCount>0 and gSettings.llmDomainGate is on, a prompt containing
  // none of these words is refused (modelRefusal) instead of generated. The vocab
  // blob is PSRAM-allocated at load and freed in llmUnload (kept off internal DRAM).
  char     modelRefusal[256];  // NUL-terminated refusal answer ("" = compiled default)
  uint8_t* domainVocab;        // PSRAM: packed allow-list, each entry [u8 len][lowercase word]; nullptr if none
  uint16_t domainVocabCount;   // number of words (0 = gate inactive)
  size_t   domainVocabBytes;   // valid packed length in domainVocab (bounds the matcher walk)

  // Guided-input menu (optional MENU section in the info block v2). Parsed at
  // load into one PSRAM blob; the descriptors below index into it. Mutated ONLY
  // under System_LLM_Menu.cpp's sMenuLock — readers run on the OLED loop, httpd,
  // g2_tap_disp and cmd_exec while llmUnload can free the blob from another task,
  // so every accessor copies out under the lock (no interior pointer escapes).
  // menuGeneration bumps on EVERY load and unload so a surface can detect a model
  // swap and refetch. Absence of a menu (menuGroupCount==0) is a first-class
  // state, never an error. See LLM_GUIDED_MENU_SPEC §4-5.
  uint8_t*         menuBlob;        // PSRAM via llmPsramAlloc("llm.menu"); nullptr if none
  size_t           menuBytes;       // valid length of menuBlob
  uint8_t          menuGroupCount;  // 0 = no guided input
  LLMMenuGroupDesc menuGroups[8];
  uint16_t         menuGeneration;  // bumped on EVERY llmLoadModel AND llmUnload

  // Effective context for KV cache (<= config.seq_len); may be capped by requestedMaxCtx
  int seq_ctx;
  int requestedMaxCtx;   // set by llmLoadModel before loadWeights is called
  uint8_t kvPrecision;   // KVPrecision: KV-cache storage format, set at load from gSettings.llmKvPrecision
  volatile bool injectCorruptOnce;  // debug: llmcorrupttest sets this to force one RunState corruption next generation

  // Repetition penalty ring buffer (allocated at model load, reused per generation)
  int* repBuf;
  int repBufSize;  // = LLM_DEFAULT_REP_WINDOW (capped 1-256)

  // Top-p sampling index buffer (allocated once at model load, reused per token)
  int* sampleIndices;
  int sampleIndicesSize;  // = vocab_size

  // Generated-token history for the no-repeat n-gram blocker (PSRAM, seq_ctx
  // ints, reused per generation). Unlike repBuf's small ring, this keeps EVERY
  // sampled token of the current run so n-gram matches can't age out.
  int* genHist;
  int genHistSize;  // = seq_ctx at model load

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

// Re-derive all RunState pointers (s->x .. s->logits) from the two surviving
// base blocks (gLLM.stateHotData / gLLM.stateData) using gLLM.config + seq_ctx.
// Single source of truth for state-pointer binding: called once at model load
// and again as a recovery step if forward() detects a zeroed RunState pointer
// (memory-corruption soft-recovery). Returns false if a base block is gone or
// the config is unusable — in that case only a full reload can recover.
// Defined in System_LLM_Model.cpp.
bool  llmBindRunState();

// ============================================================================
// Guided-menu lifecycle hooks (defined in System_LLM_Menu.cpp)
// ============================================================================
// These perform the LOCKED mutation of the menu state above. loadInfoBlockFromFile
// (model loader) publishes a freshly parsed blob; llmUnload clears it. Both take
// sMenuLock and bump menuGeneration so concurrent readers never see a torn state.

// Create sMenuLock (idempotent). Called from llmInit.
void llmMenuInit(void);

// Install a validated menu blob + descriptors as the live menu, taking ownership
// of `blob` (PSRAM). Frees any previous blob, bumps menuGeneration. count==0 with
// blob==nullptr is equivalent to llmMenuClear().
void llmMenuPublish(uint8_t* blob, size_t bytes,
                    const LLMMenuGroupDesc* groups, uint8_t count);

// Free the menu blob, zero the descriptors/count, bump menuGeneration.
void llmMenuClear(void);

// Guided-menu command handlers (defined in System_LLM_Menu.cpp; registered
// NON-admin in llmCommands[] in System_LLM.cpp).
const char* cmd_llm_menu(const String& args);
const char* cmd_llm_ask(const String& args);
