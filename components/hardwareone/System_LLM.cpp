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
#include "System_Events.h"  // systemEventPost — event register producer
#include <esp_attr.h>
#include <ctype.h>   // isalpha/isalnum — vocab-aware prompt casing

#if ENABLE_ONDEVICE_LLM

#include "System_LLM.h"
#include "System_LLM_Internal.h"  // private engine types + gLLM runtime singleton
#include "System_LLM_Kernels.h"   // rmsnorm/layernorm/softmax/wmatmul/scaleCount
#include "System_LLM_Sampler.h"   // sample()
#include "System_LLM_Tokenizer.h" // encode / decode / loadTokenizerFromFile / freeTokenizer
#include "System_LLM_Model.h"     // loadWeights
#include "System_Command.h"
#include "System_Debug.h"
#include "System_MemUtil.h"
#include "System_Filesystem.h"
#include "System_VFS.h"
#include "System_Settings.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <climits>
#include <algorithm>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"

// esp-dsp for accelerated dot product on S3
#include "dsps_dotprod.h"
#include "esp_timer.h"   // esp_timer_get_time() for the per-section forward profiler

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
//  8. Sampling (argmax, top-p, min-p, dispatcher)
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

// (LOGIT_CLAMP_MAX/MIN moved to System_LLM_Sampler.cpp with the sampler.)

// (READ_CHUNK_SIZE moved to System_LLM_Model.cpp with the loader.)

// EOS token IDs by architecture
static constexpr int EOS_TOKEN_LLAMA = 2;   // Llama/SentencePiece
static constexpr int EOS_TOKEN_GPT2  = 0;   // GPT-2 (<|endoftext|>)

// Generation loop intervals
// YIELD_INTERVAL = 1: yield once per generated token. The task runs at
// priority 3 pinned to core 1, where it outranks the Arduino loop and the
// (priority-1) command task that services BLE/serial. With weights in PSRAM a
// token takes ~1 s, so yielding every 4th token left ~4-5 s between yields —
// right at the 5 s task-WDT limit (IDLE1 starved → WDT on llm_gen), and it
// blocked command replies for the whole generation (the app saw "no response"
// while streaming). Yielding every token keeps core 1 cooperative.
static constexpr int YIELD_INTERVAL      = 1;   // vTaskDelay every N generated tokens
static constexpr int HEALTH_LOG_INTERVAL = 16;   // log generation health every N tokens

// ============================================================================
// 2. Data Structures — moved to System_LLM_Internal.h
//    TransformerWeights, RunState, the tokenizer structs, and the LLMRuntime
//    singleton type now live in the shared internal header so the kernels,
//    sampler, tokenizer, and model-loader TUs operate on the same `gLLM`.
// ============================================================================

// ============================================================================
// 3. Module State — LLMRuntime type defined in System_LLM_Internal.h
// ============================================================================

// PSRAM-resident: the runtime struct's always-on inline footprint (~2 KB —
// metadata strings, the 1 KB tokenizer byte_to_token table, path/error buffers)
// stays off scarce internal DRAM. Hot paths dereference the buffer pointers it
// holds (which keep their own allocations), not the struct fields, so PSRAM
// placement is negligible for the memory-bound engine. Mirrors gLLMAsyncCtx.
EXT_RAM_BSS_ATTR LLMRuntime gLLM = {};

// ============================================================================
// Async Generation State (background task + PSRAM result buffer)
// ============================================================================

#define LLM_RESULT_BUF_SIZE (8 * 1024)

static char*          gLLMResultBuf  = nullptr;  // PSRAM, allocated on first use
static volatile int   gLLMResultLen  = 0;         // bytes written (written by task, read by HTTP)
static volatile bool  gLLMResultDone = true;      // true = idle / finished
static volatile int   gLLMSessionId  = 0;         // bumped per llmStartAsync call
static TaskHandle_t   gLLMTask       = nullptr;   // persistent gen worker handle (null until first create)
// Stack + TCB for the persistent generation worker, claimed from internal DRAM
// at model load (llmEnsureWorker) and retained for the rest of the boot. Devices
// that never load a model never pay the ~12.4 KB — the reason these are pointers
// and not link-time .bss.
//
// The worker is created once and never deleted: it parks on its notify between
// generations, including across llmunload. Retiring it would buy nothing (these
// blocks are deliberately not freed, so the task is the only thing that would go
// away) while costing a race that has no safe form — a task cannot observe its
// own deletion, the IDLE task unlinks its TCB some unbounded time after it stops
// running, and re-creating on the same TCB before that lands corrupts the
// termination list.
//
// They must be INTERNAL: the scheduler and ISR path touch a task's stack and TCB,
// and task stacks in PSRAM have never been safe on this HW. Sized in StackType_t
// units; on this build sizeof(StackType_t)==1 so the stack is LLM_TASK_STACK_SIZE
// bytes. Static-task creation (rather than plain xTaskCreate) is what lets the
// allocation happen exactly once, at model load on a clean heap — see the
// fragmentation note in llmEnsureWorker.
static StackType_t*   gLLMWorkerStack = nullptr;
static StaticTask_t*  gLLMWorkerTcb   = nullptr;

struct LLMAsyncContext {
  char         prompt[1024];  // callers cap the prompt at 1024 (see llmStartAsync sites)
  LLMGenParams params;
};
// PSRAM-resident: this ~1.6 KB context was sitting in scarce internal DRAM for no
// reason — the async task reads it off the LLM (PSRAM-bound) path anyway.
EXT_RAM_BSS_ATTR static LLMAsyncContext gLLMAsyncCtx;

// Persistent generation worker. Created ONCE (at model load, on a clean heap)
// and reused for every generation — it parks on a task-notification between
// requests instead of being re-spawned per generation. That is what makes
// generation immune to internal-DRAM fragmentation: the 12 KB stack is claimed
// one time and never re-acquired against a fragmented heap. llmStartAsync stages
// gLLMAsyncCtx + resets the result buffer, then xTaskNotifyGive()s us.
static void llmWorkerTask(void* /*pv*/) {
  for (;;) {
    // Park until a request arrives. pdTRUE clears the notification count on
    // exit, so two coalesced gives still yield exactly one generation (no
    // stale-context replay). The give/take pair is a full memory barrier, so
    // the gLLMAsyncCtx writes llmStartAsync made before its give are visible
    // here after the take — the same publish edge the old per-gen xTaskCreate
    // provided.
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    const LLMAsyncContext* ac = &gLLMAsyncCtx;
    llmGenerate(ac->prompt, [](const char* token) -> bool {
      if (!gLLMResultBuf) return false;
      int tlen = (int)strlen(token);
      int cur  = gLLMResultLen;
      if (cur + tlen >= LLM_RESULT_BUF_SIZE - 1) return false;  // buffer full
      memcpy(gLLMResultBuf + cur, token, tlen);
      gLLMResultBuf[cur + tlen] = '\0';
      gLLMResultLen = cur + tlen;  // publish after write
      return true;
    },
    ac->params.maxTokens,    ac->params.temperature,  ac->params.topp,
    ac->params.repPenalty,   ac->params.repWindow,    ac->params.sentenceLimit,
    ac->params.hardCap,
    ac->params.suppressCount > 0 ? ac->params.suppressTokens : nullptr,
    ac->params.suppressCount, ac->params.minP);

    // The chat layer's drainEngineLocked finalizes on this flag, so publish it
    // before the epilogue below and then loop back to park. This task never
    // exits — see gLLMWorkerStack for why retiring it has no safe form.
    gLLMResultDone = true;

    // One-shot: report the deepest this task's stack got during a real
    // generation, so the static LLM_TASK_STACK_SIZE can be trusted/trimmed with
    // HW data instead of guesswork. uxTaskGetStackHighWaterMark returns the
    // minimum free stack ever seen (in StackType_t words == bytes on this build).
    static bool sLoggedStackHwm = false;
    if (!sLoggedStackHwm) {
      sLoggedStackHwm = true;
      UBaseType_t freeWords = uxTaskGetStackHighWaterMark(nullptr);
      DEBUG_LLM_GENERATEF("[LLM] llm_gen stack: %u B used of %u B (min free %u B)",
                          (unsigned)(LLM_TASK_STACK_SIZE - freeWords * sizeof(StackType_t)),
                          (unsigned)LLM_TASK_STACK_SIZE,
                          (unsigned)(freeWords * sizeof(StackType_t)));
    }
  }
}

// Create the persistent worker once, on demand. Idempotent + serialized under
// gLLM.mutex so a model-load and a lazy fallback can't both create it (which
// would leak a 12 KB task and double-consume the shared context).
static bool llmEnsureWorker() {
  bool ok = true;
  if (gLLM.mutex) xSemaphoreTake(gLLM.mutex, portMAX_DELAY);
  if (!gLLMTask) {
    // Claim the stack + TCB HERE — at model load, on the cleanest heap this
    // device will see (the G2 viewer / map stacks are usually not up yet) — and
    // then keep them for every subsequent generation. Timing is the whole design:
    // the old per-generation xTaskCreatePinnedToCore asked for the same ~12 KB
    // contiguous INTERNAL block at the worst possible moment, and once a model is
    // loaded and the G2 page has fragmented the heap the largest free internal
    // block is only ~9 KB — so generation returned "busy or failed to start"
    // every time the viewer was open. Claiming once, early, dodges that; the
    // static-task form is what makes "once" expressible.
    //
    // A failure here is real (unlike the old link-time .bss, which could not
    // fail) but benign: both callers treat it as "retry later" — llmLoadModel
    // logs and loads anyway, llmStartAsync bails and re-enters on the next
    // generation, by which point the heap may have recovered.
    if (!gLLMWorkerStack) {
      gLLMWorkerStack = (StackType_t*)heap_caps_malloc(LLM_TASK_STACK_SIZE,
                                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!gLLMWorkerTcb) {
      gLLMWorkerTcb = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t),
                                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (gLLMWorkerStack && gLLMWorkerTcb) {
      // xTaskCreateStatic* only returns null on invalid params, never on OOM.
      gLLMTask = xTaskCreateStaticPinnedToCore(
        llmWorkerTask, "llm_gen", LLM_TASK_STACK_SIZE / sizeof(StackType_t), nullptr,
        LLM_TASK_PRIORITY, gLLMWorkerStack, gLLMWorkerTcb, 1);
    }
    if (!gLLMTask) {
      // No task ever ran on these, so releasing the partial pair is unambiguously
      // safe here — and it keeps a failed create from pinning 12 KB that the
      // retry would only ask for again.
      heap_caps_free(gLLMWorkerStack);
      heap_caps_free(gLLMWorkerTcb);
      gLLMWorkerStack = nullptr;
      gLLMWorkerTcb   = nullptr;
      ok = false;
    }
  }
  if (gLLM.mutex) xSemaphoreGive(gLLM.mutex);
  return ok;
}

// ============================================================================
// 4. PSRAM Allocation Helpers
// ============================================================================

void* llmPsramAlloc(size_t size, const char* tag) {
  void* p = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM);
  if (!p) {
    ERROR_LLMF("PSRAM alloc failed: %s (%u bytes)", tag, (unsigned)size);
  }
  return p;
}

void llmPsramFree(void** ptr) {
  if (ptr && *ptr) {
    heap_caps_free(*ptr);
    *ptr = nullptr;
  }
}

// printf-style setter for gLLM.errorMsg — replaces the repeated
// setLlmError( ...) boilerplate. Message only;
// runState transitions stay explicit at the call sites that need them.
void setLlmError(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), fmt, ap);
  va_end(ap);
}

// ============================================================================
// 5. Math Primitives  →  moved to System_LLM_Kernels.{h,cpp}
//    rmsnorm, layernorm, softmax, scaleCount, the quant matmuls, and wmatmul.
//    They are pure functions (no engine state) — the compute seam for SIMD /
//    per-target backends. forward() and the sampler call them via the header.
// ============================================================================

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

// At logged positions, compute vector health stats and warn on NaN/Inf.
// Replaces the repeated "if (FORWARD_DBG_POS) { VecStats x = vecstats(...);
// if (x.nans||x.infs) DEBUG(...) }" blocks scattered through forward(). Pass
// l = -1 for non-layer points. Debug-only — never touches the activations, so
// generation output is unchanged.
static inline void checkVec(const char* what, const float* v, int n, int pos, int l) {
  if (!FORWARD_DBG_POS(pos)) return;
  VecStats s = vecstats(v, n);
  if (s.nans || s.infs) {
    DEBUG_LLM_FORWARDF("[LLM] CRITICAL: NaN/Inf in %s at L%d pos=%d (nan=%d inf=%d)",
                       what, l, pos, s.nans, s.infs);
  }
}

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
// Per-section forward-pass profiler (llmprofile)
// ----------------------------------------------------------------------------
// Accumulates wall-clock µs per section across a whole generation and dumps a
// breakdown at the end (see llmGenerate). Answers "where does per-token time go
// — weight matmul vs attention/KV vs the rest?" without guessing. Zero overhead
// unless gProfActive (latched from gSettings.llmProfile at generation start):
// profNow() skips the timer read when inactive, PROF_ADD skips read+add. An
// esp_timer read is ~0.2µs, negligible vs the ~ms sections it brackets. Run
// with the other debugllm* flags OFF for clean numbers.
// ============================================================================
struct LLMProfile {
  int64_t embed, norm, qkv, kvpack, attn, attnOut, ffnUp, ffnDown, act, cls;
  int32_t tokens;
};
static LLMProfile gProf = {};
static bool gProfActive = false;

static inline int64_t profNow() { return gProfActive ? esp_timer_get_time() : 0; }
#define PROF_ADD(bucket, t0) do { if (gProfActive) gProf.bucket += esp_timer_get_time() - (t0); } while (0)

// ============================================================================
// 7. Forward Pass
// ============================================================================

static float* forward(int token, int pos) {
  LLMConfig* p = &gLLM.config;
  TransformerWeights* w = &gLLM.weights;
  RunState* s = &gLLM.state;
  const int S = gLLM.seq_ctx;
  const bool isGPT2 = (p->arch_type == 1);

  // Corruption guard: a memory overrun elsewhere can zero RunState pointers
  // (observed: s->x → null mid-generation → LoadProhibited panic in
  // vecAddInPlace). Detect it here and return nullptr so the generation loop
  // can attempt a soft recovery (llmBindRunState) instead of dereferencing null.
  const bool kvOk = (gLLM.kvPrecision == KV_FP16) ? (s->key_cache_f16 && s->value_cache_f16)
                  : (gLLM.kvPrecision == KV_INT8) ? (s->key_cache_q8  && s->value_cache_q8)
                  :                                 (s->key_cache     && s->value_cache);
  if (!s->x || !s->xb || !s->xb2 || !s->q || !s->logits || !kvOk || !s->att) {
    DEBUG_LLM_GENERATEF("[LLM] forward: RunState corrupted at pos=%d (x=%p logits=%p kvOk=%d) — bailing",
                        pos, (void*)s->x, (void*)s->logits, (int)kvOk);
    return nullptr;
  }

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
    vecAddInPlace(s->x, pe, dim);
  }

  // Debug: post-embedding activation health
  checkVec("embedding", s->x, dim, pos, -1);

  // Precompute this token's RoPE cos/sin once (identical across all layers).
  // Llama only — GPT-2 uses absolute positional embeddings added above.
  if (!isGPT2) {
    for (int i = 0; i < dim; i += 2) {
      int head_dim = i % head_size;
      float val = pos * s->rope_inv[head_dim];
      s->rope_cos[i >> 1] = cosf(val);
      s->rope_sin[i >> 1] = sinf(val);
    }
  }

  // Forward through all layers
  for (int l = 0; l < p->n_layers; l++) {
    const TransformerWeights::LayerTensors& T = w->layerT[l];

    // Attention norm (LayerNorm for GPT-2, RMSNorm for Llama)
    int64_t _pnorm = profNow();
    if (isGPT2) {
      layernorm(s->xb, s->x, w->rms_att_weight + l * dim, w->rms_att_bias ? w->rms_att_bias + l * dim : nullptr, dim);
    } else {
      rmsnorm(s->xb, s->x, w->rms_att_weight + l * dim, dim);
    }
    PROF_ADD(norm, _pnorm);

    // Key and value point to the KV cache
    int loff = l * S * kv_dim;
    const uint8_t kvPrec = gLLM.kvPrecision;
    const bool kvPacked = (kvPrec != KV_FP32);  // FP16/INT8 produce into temps, then pack
    // K/V are produced in FP32 working buffers. FP32 mode points these straight
    // at the cache row (zero-copy, identical to before); FP16/INT8 use hot temps
    // and pack into the compressed cache after RoPE (below).
    float* key_cache_row   = kvPacked ? s->k_tmp : (s->key_cache   + loff + pos * kv_dim);
    float* value_cache_row = kvPacked ? s->v_tmp : (s->value_cache + loff + pos * kv_dim);

    // QKV matmuls (weights prepackaged at load — see buildLayerTensors)
    int64_t _pqkv = profNow();
    linear(s->q,            s->xb, T.wq);
    linear(key_cache_row,   s->xb, T.wk);
    linear(value_cache_row, s->xb, T.wv);
    PROF_ADD(qkv, _pqkv);

    // Debug: Q/K/V matmul health
    checkVec("Q", s->q,            dim,    pos, l);
    checkVec("K", key_cache_row,   kv_dim, pos, l);
    checkVec("V", value_cache_row, kv_dim, pos, l);

    // RoPE relative positional encoding (Llama only — GPT-2 uses absolute pos
    // embeddings). cos/sin were precomputed once per token above.
    int64_t _pkv = profNow();
    if (!isGPT2) {
      for (int i = 0; i < dim; i += 2) {
        float fcr = s->rope_cos[i >> 1];
        float fci = s->rope_sin[i >> 1];
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

    // Pack this position's K/V (post-RoPE) into the compressed cache.
    // FP32 mode already wrote straight into the cache above — nothing to do.
    if (kvPrec == KV_FP16) {
      uint16_t* kdst = s->key_cache_f16   + loff + pos * kv_dim;
      uint16_t* vdst = s->value_cache_f16 + loff + pos * kv_dim;
      for (int i = 0; i < kv_dim; i++) {
        kdst[i] = f32_to_f16(key_cache_row[i]);
        vdst[i] = f32_to_f16(value_cache_row[i]);
      }
    } else if (kvPrec == KV_INT8) {
      // Per-(kv-head) symmetric INT8: scale = max|x|/127 over each head-slice.
      const int nkv = p->n_kv_heads;
      int8_t* kdst = s->key_cache_q8   + loff + pos * kv_dim;
      int8_t* vdst = s->value_cache_q8 + loff + pos * kv_dim;
      float*  ksc  = s->key_scales   + (size_t)(l * S + pos) * nkv;
      float*  vsc  = s->value_scales + (size_t)(l * S + pos) * nkv;
      for (int kh = 0; kh < nkv; kh++) {
        const int base = kh * head_size;
        float kmax = 0.0f, vmax = 0.0f;
        for (int i = 0; i < head_size; i++) {
          float ak = fabsf(key_cache_row[base + i]);   if (ak > kmax) kmax = ak;
          float av = fabsf(value_cache_row[base + i]); if (av > vmax) vmax = av;
        }
        float ks = (kmax > 0.0f) ? (kmax / 127.0f) : 1.0f;
        float vs = (vmax > 0.0f) ? (vmax / 127.0f) : 1.0f;
        ksc[kh] = ks;  vsc[kh] = vs;
        const float kinv = 1.0f / ks, vinv = 1.0f / vs;
        for (int i = 0; i < head_size; i++) {
          int qk = (int)lrintf(key_cache_row[base + i]   * kinv);
          int qv = (int)lrintf(value_cache_row[base + i] * vinv);
          if (qk > 127) qk = 127; else if (qk < -127) qk = -127;
          if (qv > 127) qv = 127; else if (qv < -127) qv = -127;
          kdst[base + i] = (int8_t)qk;
          vdst[base + i] = (int8_t)qv;
        }
      }
    }
    PROF_ADD(kvpack, _pkv);

    // Multi-head attention
    int64_t _pattn = profNow();
    for (int h = 0; h < p->n_heads; h++) {
      float* q_h = s->q + h * head_size;
      float* att_h = s->att + h * S;
      // Iterate over all timesteps including current
      for (int t = 0; t <= pos; t++) {
        // FP16: dequant this position's K head-slice into scratch, then run the
        // same SIMD dot-product. FP32: point straight at the cache.
        float* k;
        if (kvPrec == KV_FP16) {
          const uint16_t* kf = s->key_cache_f16 + loff + t * kv_dim + (h / kv_mul) * head_size;
          for (int i = 0; i < head_size; i++) s->kv_deq[i] = f16_to_f32(kf[i]);
          k = s->kv_deq;
        } else if (kvPrec == KV_INT8) {
          const int kvh = h / kv_mul;
          const int8_t* kf = s->key_cache_q8 + loff + t * kv_dim + kvh * head_size;
          const float ks = s->key_scales[(size_t)(l * S + t) * p->n_kv_heads + kvh];
          for (int i = 0; i < head_size; i++) s->kv_deq[i] = (float)kf[i] * ks;
          k = s->kv_deq;
        } else {
          k = s->key_cache + loff + t * kv_dim + (h / kv_mul) * head_size;
        }
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
        float a = att_h[t];
        if (kvPrec == KV_FP16) {
          const uint16_t* vf = s->value_cache_f16 + loff + t * kv_dim + (h / kv_mul) * head_size;
          for (int i = 0; i < head_size; i++) xb_h[i] += a * f16_to_f32(vf[i]);
        } else if (kvPrec == KV_INT8) {
          const int kvh = h / kv_mul;
          const int8_t* vf = s->value_cache_q8 + loff + t * kv_dim + kvh * head_size;
          const float avs = a * s->value_scales[(size_t)(l * S + t) * p->n_kv_heads + kvh];
          for (int i = 0; i < head_size; i++) xb_h[i] += avs * (float)vf[i];
        } else {
          float* v = s->value_cache + loff + t * kv_dim + (h / kv_mul) * head_size;
          for (int i = 0; i < head_size; i++) xb_h[i] += a * v[i];
        }
      }
    }
    PROF_ADD(attn, _pattn);

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
    int64_t _pout = profNow();
    linear(s->xb2, s->xb, T.wo);
    PROF_ADD(attnOut, _pout);

    // Residual connection
    vecAddInPlace(s->x, s->xb2, dim);

    // Debug: post-attention residual health
    checkVec("post_attn_res", s->x, dim, pos, l);

    // FFN norm (LayerNorm for GPT-2, RMSNorm for Llama)
    int64_t _pfnorm = profNow();
    if (isGPT2) {
      layernorm(s->xb, s->x, w->rms_ffn_weight + l * dim, w->rms_ffn_bias ? w->rms_ffn_bias + l * dim : nullptr, dim);
    } else {
      rmsnorm(s->xb, s->x, w->rms_ffn_weight + l * dim, dim);
    }
    PROF_ADD(norm, _pfnorm);

    if (isGPT2) {
      // GPT-2 FFN: up projection → GELU → down projection (no gate)
      int64_t _pup = profNow();
      linear(s->hb, s->xb, T.w3);
      PROF_ADD(ffnUp, _pup);

      // Debug: pre-GELU activation health
      checkVec("pre_gelu", s->hb, hidden_dim, pos, l);

      // GELU (tanh approximation — matches HF gelu_new)
      int64_t _pact = profNow();
      for (int i = 0; i < hidden_dim; i++) {
        float x = s->hb[i];
        s->hb[i] = 0.5f * x * (1.0f + tanhf(GELU_COEFF_A * (x + GELU_COEFF_B * x * x * x)));
      }
      PROF_ADD(act, _pact);

      // Debug: post-GELU health
      checkVec("post_gelu", s->hb, hidden_dim, pos, l);

      int64_t _pdown = profNow();
      linear(s->xb, s->hb, T.w2);
      PROF_ADD(ffnDown, _pdown);
    } else {
      // Llama FFN: SwiGLU (gate * silu(up)) → down
      int64_t _pup = profNow();
      linear(s->hb,  s->xb, T.w1);
      linear(s->hb2, s->xb, T.w3);
      PROF_ADD(ffnUp, _pup);
      int64_t _pact = profNow();
      for (int i = 0; i < hidden_dim; i++) {
        float val = s->hb[i];
        val *= (1.0f / (1.0f + expf(-val)));
        val *= s->hb2[i];
        s->hb[i] = val;
      }
      PROF_ADD(act, _pact);
      int64_t _pdown = profNow();
      linear(s->xb,  s->hb, T.w2);
      PROF_ADD(ffnDown, _pdown);
    }

    // Residual
    vecAddInPlace(s->x, s->xb, dim);

    // Debug: post-FFN residual stream health (NaN/Inf = numerical blowup)
    checkVec("post_ffn_res", s->x, dim, pos, l);

  }

  // Final norm (LayerNorm for GPT-2, RMSNorm for Llama)
  int64_t _pfinal = profNow();
  if (isGPT2) {
    layernorm(s->x, s->x, w->rms_final_weight, w->rms_final_bias, dim);
  } else {
    rmsnorm(s->x, s->x, w->rms_final_weight, dim);
  }
  PROF_ADD(norm, _pfinal);

  // Debug: post-final-norm activation health
  checkVec("final_norm", s->x, dim, pos, -1);

  // Classifier into logits (always FP32 or Q8, never Q4).
  // Prompt-phase skip: during prompt ingestion (pos < num_prompt_tokens-1) the
  // next token is forced from the prompt, so nothing functional reads these
  // logits — the ~594KB classifier stream per position is pure waste (~8% of
  // per-token weight traffic). They ARE still computed when LLM debug is on,
  // because the PROMPT_PRED / PROMPT_TRACK / logit-stats diagnostics read them.
  // If a real prompt-phase logit consumer is ever added (prefill perplexity,
  // speculative decode), revisit this gate.
  const bool inPromptPrefill = gPromptDiag.active &&
                               (pos < gPromptDiag.num_prompt_tokens - 1);
  const bool debugWantsLogits = (getLogLevel() >= LOG_LEVEL_DEBUG) &&
      (isDebugFlagSet(DEBUG_LLM_FORWARD) || isDebugFlagSet(DEBUG_LLM_GENERATE));
  const bool logitsComputed = !inPromptPrefill || debugWantsLogits;
  int64_t _pcls = profNow();
  if (logitsComputed) {
    linear(s->logits, s->x, w->clsT);
  }
  PROF_ADD(cls, _pcls);
  if (gProfActive) gProf.tokens++;

  // Dump logit distribution stats — healthy model has wide spread and clear top candidates.
  // Flat/uniform logits = broken weights. NaN = numerical explosion.
  // Co-gated with the skip above so a stale buffer is never scanned.
  if (logitsComputed && FORWARD_DBG_POS(pos)) {
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
// 8. Sampling  →  moved to System_LLM_Sampler.{h,cpp}
//    sample_argmax/sample_topp (static there), sample().
//    forward()/llmGenerate call sample() via the header.
// ============================================================================

// ============================================================================
// 9. Tokenizer  →  moved to System_LLM_Tokenizer.{h,cpp}
//    loadTokenizerFromFile / freeTokenizer / encode / decode (called via header).
// ============================================================================

// ============================================================================
// 10. Model Loading  →  moved to System_LLM_Model.{h,cpp}
//    loadWeights() (called by llmLoadModel via header); helpers + LoadContext
//    are static inside that TU.
// ============================================================================

// ============================================================================
// 11. Public API
// ============================================================================

void llmInit() {
  memset(&gLLM, 0, sizeof(gLLM));
  gLLM.runState = LLMState::UNLOADED;
  gLLM.mutex = xSemaphoreCreateMutex();
  llmMenuInit();   // create the guided-menu lock (System_LLM_Menu.cpp)
}

bool llmLoadModel(const char* modelPath, int maxCtx) {
  // Auto-init if not yet initialized
  if (!gLLM.mutex) llmInit();

  if (!filesystemReady) {
    setLlmError("Filesystem not ready");
    gLLM.runState = LLMState::ERROR;
    systemEventPost(SYSEVT_LLM_LOAD_FAILED, gLLM.errorMsg, modelPath);
    return false;
  }

  // Unload any existing model
  llmUnload();

  // Store the requested context cap (0 = auto-fit to available PSRAM). Any
  // per-model context policy lives in the llmmaxcontext setting, not a
  // filename sniff — a caller that needs to spare PSRAM passes an explicit cap.
  gLLM.requestedMaxCtx = maxCtx;

  // KV-cache precision is a load-time choice (the buffers are sized/typed now and
  // immutable until reload). 0=FP32, 1=FP16, 2=INT8.
  gLLM.kvPrecision = (uint8_t)gSettings.llmKvPrecision;
  if (gLLM.kvPrecision > KV_INT8) gLLM.kvPrecision = KV_FP32;  // clamp unknown values
  DEBUG_LLM_LOADF("[LLM] KV cache precision: %s",
                  gLLM.kvPrecision == KV_FP16 ? "FP16 (half PSRAM)" :
                  gLLM.kvPrecision == KV_INT8 ? "INT8 (quarter PSRAM, per-head scales)" : "FP32");

  gLLM.runState = LLMState::LOADING;
  strlcpy(gLLM.modelPath, modelPath, sizeof(gLLM.modelPath));

  DEBUG_LLM_LOADF("[LLM] Loading model: %s (maxCtx=%d)", modelPath, gLLM.requestedMaxCtx);

  // loadWeights() handles header, tokenizer, and weights (all in one file)
  if (!loadWeights(modelPath)) {
    gLLM.runState = LLMState::ERROR;
    systemEventPost(SYSEVT_LLM_LOAD_FAILED, gLLM.errorMsg, modelPath);
    return false;
  }

  gLLM.runState = LLMState::READY;
  gLLM.lastContextMax = gLLM.seq_ctx;  // expose active ctx window immediately (before first generation)
  {
    const char* base = modelPath;
    const char* slash = strrchr(modelPath, '/');
    if (slash) base = slash + 1;
    char det[24];
    snprintf(det, sizeof(det), "ctx %d", gLLM.seq_ctx);
    systemEventPost(SYSEVT_LLM_MODEL_LOADED, base, det);
  }
  // Reserve the persistent generation worker now, while the heap is at its
  // cleanest (model just loaded; the G2 viewer / map stacks are usually not yet
  // allocated). Best-effort: on an already-fragmented heap this can fail, in
  // which case llmStartAsync retries lazily. Never fail the load on this.
  if (!llmEnsureWorker()) {
    DEBUG_LLM_LOADF("[LLM] gen worker create deferred (heap fragmented); will retry at first generate");
  }
  DEBUG_LLM_LOADF("[LLM] Model ready. PSRAM used: %uKB (weights=%uKB state=%uKB) ctx=%d/%d",
                  (unsigned)((gLLM.weightsSize + gLLM.weightsQ8Size + gLLM.stateSize) / 1024),
                  (unsigned)((gLLM.weightsSize + gLLM.weightsQ8Size) / 1024),
                  (unsigned)(gLLM.stateSize / 1024),
                  gLLM.seq_ctx, gLLM.config.seq_len);
  // Load-time guard: if PSRAM auto-fit shrank the context below usable, this was
  // previously silent — the model loaded and then produced 0-1 tokens with no
  // explanation. Flag it loudly at the source (ERROR_LLMF is always-on, unlike
  // the DEBUG_LLM_LOAD lines above) and record a system event, so it surfaces in
  // the serial log, event history and notifications the moment it happens. Every
  // interactive surface warns too, via llmContextDegraded(). Model stays loaded.
  {
    int degCtx = 0, degSeq = 0;
    if (llmContextDegraded(&degCtx, &degSeq)) {
      ERROR_LLMF("Context auto-fit to %d tokens (model seq=%d) - PSRAM was low at load; "
                 "answers will truncate. Reload the model or restart the device.",
                 degCtx, degSeq);
      logSystemEvent("LLM", "context degraded: only %d tokens (model supports %d) - "
                     "PSRAM low at load; reload/restart recommended", degCtx, degSeq);
    }
  }
  // Post-load budgeting line: KV precision + headroom left for fact tables /
  // bigger models. Compare kvPrec=FP16 vs FP32 across two loads to see the win.
  DEBUG_LLM_LOADF("[LLM] Model ready. kvPrec=%s  PSRAM free after load: %uKB (largest block %uKB)",
                  (gLLM.kvPrecision == KV_FP16) ? "FP16" :
                  (gLLM.kvPrecision == KV_INT8) ? "INT8" : "FP32",
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                  (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
  return true;
}

void llmUnload() {
  // Edge: only a real teardown (model present -> no-model) should post. A bare
  // unload with nothing loaded (pre-load cleanup, double-unload) must stay quiet.
  const bool hadModel = (gLLM.runState != LLMState::UNLOADED);
  // Quiesce any LIVE generation before freeing the weights it reads. A gen is
  // live from llmStartAsync accepting it (gLLMResultDone=false) through the
  // worker's epilogue (gLLMResultDone=true) — a window that includes an async
  // gen ACCEPTED but not yet inside llmGenerate, where runState is still READY.
  // The old runState==GENERATING-only wait missed that window and could free
  // weights out from under a generation about to start (use-after-free). Cover
  // both the synchronous path (runState) and the async accept window (done).
  if (gLLM.runState == LLMState::GENERATING || !gLLMResultDone) {
    // Let an accepted-but-not-started async gen actually enter llmGenerate
    // first; otherwise its entry reset of stopRequested (llmGenerate ~:1049)
    // clears the stop we set and it runs to completion under us.
    for (int i = 0; i < 50 && gLLM.runState != LLMState::GENERATING && !gLLMResultDone; i++) {
      vTaskDelay(pdMS_TO_TICKS(2));
    }
    vTaskDelay(pdMS_TO_TICKS(2));  // let that entry reset land before we set stop
    gLLM.stopRequested = true;     // observed per-token, breaks the gen loop
    for (int i = 0; i < 100 && (gLLM.runState == LLMState::GENERATING || !gLLMResultDone); i++) {
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
  llmPsramFree((void**)&gLLM.weights.layerT);
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
  if (gLLM.sampleIndices) {
    llmPsramFree((void**)&gLLM.sampleIndices);  // PSRAM-allocated (nulls the ptr)
    gLLM.sampleIndicesSize = 0;
  }
  if (gLLM.genHist) {
    llmPsramFree((void**)&gLLM.genHist);        // PSRAM-allocated (nulls the ptr)
    gLLM.genHistSize = 0;
  }

  memset(&gLLM.weights, 0, sizeof(gLLM.weights));
  memset(&gLLM.state, 0, sizeof(gLLM.state));
  gLLM.weightsSize = 0;
  gLLM.weightsQ8Size = 0;
  gLLM.stateSize = 0;
  gLLM.stateHotSize = 0;
  gLLM.seq_ctx = 0;
  gLLM.runState = LLMState::UNLOADED;

  // Drop any info-block metadata so a failed/early-aborted next load can't show
  // the previous model's description or icon.
  gLLM.modelDesc[0] = '\0';
  gLLM.modelHasIcon = false;
  gLLM.modelIconW   = 0;
  gLLM.modelIconH   = 0;
  gLLM.modelRefusal[0] = '\0';
  if (gLLM.domainVocab) llmPsramFree((void**)&gLLM.domainVocab);
  gLLM.domainVocabCount = 0;
  gLLM.domainVocabBytes = 0;

  // Drop the guided-input menu (frees the PSRAM blob + bumps menuGeneration under
  // sMenuLock so a reader on another task can't touch a freed blob). See
  // LLM_GUIDED_MENU_SPEC §5.
  llmMenuClear();

  if (hadModel) {
    const char* base = gLLM.modelPath;
    const char* slash = strrchr(gLLM.modelPath, '/');
    if (slash) base = slash + 1;
    systemEventPost(SYSEVT_LLM_MODEL_UNLOADED, base, nullptr);
  }
}

bool llmIsReady() {
  return gLLM.runState == LLMState::READY;
}

bool llmContextDegraded(int* outCtx, int* outModelSeq) {
  const int ctx      = gLLM.seq_ctx;          // 0 when no model is loaded
  const int modelSeq = gLLM.config.seq_len;
  if (outCtx)      *outCtx = ctx;
  if (outModelSeq) *outModelSeq = modelSeq;
  if (ctx <= 0) return false;                 // no model → nothing to warn about
  // "requested" is the ceiling we tried to fit: the explicit llmMaxContext
  // (requestedMaxCtx) when set, else the model's own seq_len. If the fitted ctx
  // equals that, PSRAM did NOT cut it — a small value is then intentional, not a
  // fault, so we stay quiet. Only warn when auto-fit shrank it below both the
  // request AND the usable floor.
  const int requested = (gLLM.requestedMaxCtx > 0) ? gLLM.requestedMaxCtx : modelSeq;
  const bool shrunk   = ctx < requested;
  const bool unusable = ctx < LLM_MIN_USABLE_CONTEXT;
  return shrunk && unusable;
}

size_t llmContextWarning(char* buf, size_t cap) {
  if (!buf || cap == 0) return 0;
  int ctx = 0, modelSeq = 0;
  if (!llmContextDegraded(&ctx, &modelSeq)) { buf[0] = '\0'; return 0; }
  int len = snprintf(buf, cap,
    "LLM context is only %d tokens (model supports %d) - PSRAM was low when the "
    "model loaded, so answers get truncated. Reload the model or restart the device.",
    ctx, modelSeq);
  return (len < 0) ? 0 : ((size_t)len < cap ? (size_t)len : cap - 1);
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
  status.lastMeanLogprob = gLLM.lastMeanLogprob;
  status.lastConfTokens = gLLM.lastConfTokens;
  strlcpy(status.errorMsg, gLLM.errorMsg, sizeof(status.errorMsg));
  return status;
}

const char* llmModelDescription() {
  return gLLM.modelDesc;  // always NUL-terminated; "" when the model has no info block
}

bool llmModelIcon(const uint8_t** bits, uint8_t* width, uint8_t* height) {
  if (!gLLM.modelHasIcon) return false;
  if (bits)   *bits   = gLLM.modelIcon;
  if (width)  *width  = gLLM.modelIconW;
  if (height) *height = gLLM.modelIconH;
  return true;
}

// Whole-word, case-insensitive test: does the prompt contain at least one word
// from the model's embedded domain allow-list? Returns true (allow) when the model
// carries no domain vocab, so a plain model is never gated. Cold path — runs once
// per prompt over a packed [u8 len][lowercase word] blob (PSRAM).
static bool llmPromptInDomain(const char* prompt) {
  if (gLLM.domainVocabCount == 0 || !gLLM.domainVocab || gLLM.domainVocabBytes == 0)
    return true;
  char lw[256];
  for (const char* p = prompt; *p; ) {
    while (*p && !isalnum((unsigned char)*p)) p++;      // skip to a word
    const char* ws = p;
    while (*p && isalnum((unsigned char)*p)) p++;        // span the word
    int wl = (int)(p - ws);
    if (wl <= 0 || wl > 255) continue;                   // no word, or too long to be a uint8-len entry
    for (int i = 0; i < wl; i++) {
      char c = ws[i];
      lw[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    const uint8_t* v = gLLM.domainVocab;
    const uint8_t* vend = v + gLLM.domainVocabBytes;
    while (v < vend) {
      uint8_t vl = *v++;
      if (v + vl > vend) break;
      if (vl == wl && memcmp(v, lw, (size_t)wl) == 0) return true;
      v += vl;
    }
  }
  return false;
}

// Universal capability / identity / help questions that ANY assistant model
// should answer even though they carry no domain content word (they are all
// function words), so the domain gate must not blank them out. Case-insensitive
// substring match on the raw prompt; the "Q: ...\nA:" framing doesn't interfere.
static bool llmIsMetaPrompt(const char* prompt) {
  char lw[192];
  size_t n = 0;
  for (const char* p = prompt; *p && n < sizeof(lw) - 1; ++p) {
    char c = *p;
    lw[n++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
  }
  lw[n] = '\0';
  static const char* const META[] = {
    "what are you", "who are you", "what do you do", "what can you do",
    "what do you know", "what can i ask", "what is this", "how do you work",
    "tell me about yourself", "what are you for", "what are your", "help",
  };
  for (size_t i = 0; i < sizeof(META) / sizeof(META[0]); ++i)
    if (strstr(lw, META[i])) return true;
  return false;
}

// See System_LLM.h for the contract. The template is spelled here rather than in
// each caller because it used to live only in the web page's JavaScript, which is
// why the web was the one surface that got real answers while the CLI, OLED and
// BLE quietly asked the model to continue prose instead.
//
// The idempotence test is a leading "Q:" rather than a search for "\nA:": BLE
// intake flattens newlines to spaces, so an already-framed prompt can arrive as
// "Q: ... A:" and must not be wrapped twice — the normalization below restores
// that newline itself.
const char* llmFramePrompt(const char* userText, char* out, size_t outSize) {
  if (!userText || !out || outSize < 8) return userText;

  const char* p = userText;
  while (*p == ' ' || *p == '\t') p++;
  if (strncasecmp(p, "Q:", 2) == 0) return userText;  // already framed — pass through

  const char* tail = "\nA:";
  if (strncasecmp(p, "do:", 3) == 0) {
    tail = "\nDo:";
    p += 3;
    while (*p == ' ') p++;
  }

  // Truncate the question, never the tail. Prompts are capped at
  // sizeof(gLLMAsyncCtx.prompt), so letting a long question push the A: off the
  // end would silently un-frame it — the exact failure this exists to prevent,
  // and an invisible one.
  const size_t tailLen = strlen(tail);
  const size_t maxBody = outSize - 3 - tailLen - 1;  // "Q: " + tail + NUL
  size_t bodyLen = strlen(p);
  if (bodyLen > maxBody) bodyLen = maxBody;

  memcpy(out, "Q: ", 3);
  memcpy(out + 3, p, bodyLen);
  memcpy(out + 3 + bodyLen, tail, tailLen);
  out[3 + bodyLen + tailLen] = '\0';
  return out;
}

int llmGenerate(const char* prompt, LLMTokenCallback tokenCb,
                int maxTokens, float temperature, float topp,
                float repPenalty, int repWindow, int sentenceLimit, int hardCap,
                const int* suppressTokens, int suppressTokenCount,
                float minP) {
  if (gLLM.runState != LLMState::READY) {
    return -1;
  }

  // ── Setup ──────────────────────────────────────────────────────────────────
  gLLM.runState = LLMState::GENERATING;
  gLLM.stopRequested = false;
  gLLM.errorMsg[0] = '\0';

  // ── Domain refusal gate ──────────────────────────────────────────────────
  // If the loaded .bin carries a domain allow-list and the setting is on, refuse
  // prompts that contain none of the domain words (deterministic, pre-generation).
  // Skipped for Do:/command prompts (machine-parsed) and for universal meta/help
  // questions (what-are-you, help, ...) that any model should answer despite
  // carrying no domain word. Nothing allocated yet, so the early return only has
  // to restore runState + emit the text.
  if (gSettings.llmDomainGate && gLLM.domainVocabCount > 0) {
    // Do:-mode prompts (natural-language → device command) are machine-parsed and
    // must bypass the gate. The app frames them "...\nDo:", but BLE intake
    // (Bluetooth.cpp) normalizes \n→space, so over BLE it arrives as "... Do:";
    // norm_prompt (which restores the newline) isn't built until below, so detect
    // BOTH forms on the raw prompt here — otherwise a domain-gated model refuses
    // every BLE command instead of translating it.
    const size_t promptLen = strlen(prompt);
    const bool doMode = strstr(prompt, "\nDo:") != nullptr ||
                        (promptLen >= 4 && strcmp(prompt + promptLen - 4, " Do:") == 0);
    if (!doMode && !llmIsMetaPrompt(prompt) && !llmPromptInDomain(prompt)) {
      const char* msg = gLLM.modelRefusal[0]
                          ? gLLM.modelRefusal
                          : "I can only answer questions about this model's topic.";
      if (tokenCb) tokenCb(msg);
      gLLM.lastTokCount  = 0;
      gLLM.lastTokPerSec = 0.0f;
      gLLM.runState = LLMState::READY;
      return 0;
    }
  }

  LLMConfig* p = &gLLM.config;

  // ── Prompt normalization: strip filler phrases ────────────────────────────
  // Users type things like "I want to add a wifi network" or "Can you tell me
  // how to set up wifi". The filler wastes attention bandwidth on a dim=128
  // model. Strip common prefixes from the question portion of the prompt
  // (between "Q: " and "\nA:") so the model sees just the semantic core.
  // Ordered longest-first so "Can you tell me how to" matches before "Can you".
  // Only strip phrases where the remainder IS the topic — never strip words that
  // change meaning (Can I, Is the, Does it, Which, Who, Where, When).
  //
  // GUIDED-MENU CONTRACT (LLM_GUIDED_MENU_SPEC §12.4): this list is detect-only —
  // it must NOT strip. Guided-menu questions are corpus-EXACT templates (e.g.
  // "Tell me about {}") whose leading words ARE the trained phrasing; re-enabling
  // stripping here would mutate the composed prompt and break the memorized
  // answer. Composed questions also rely on the title-case + vocab-aware recasing
  // below being non-destructive to already-correct casing (templates ship
  // capitalized, entities corpus-cased). Keep both passes read-only-shaping.
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
    // BLE intake (Bluetooth.cpp processBleCommandLine) normalizes \r\n\t → space,
    // so the app's "Q: ...\nA:" / "Q: ...\nDo:" framing arrives as "Q: ... A:" /
    // "Q: ... Do:". Restore the trailing newline the model was trained on, so the
    // tokenizer emits the A:(4)/Do:(5) special token as the last prompt token and
    // the Do:-mode + "\nA:"-based filler logic below behave identically to the web
    // path. No-op for web/serial, which already deliver a real newline.
    {
      size_t npLen = strlen(norm_prompt);
      if (npLen >= 4 && strcmp(norm_prompt + npLen - 4, " Do:") == 0) {
        norm_prompt[npLen - 4] = '\n';
      } else if (npLen >= 3 && strcmp(norm_prompt + npLen - 3, " A:") == 0) {
        norm_prompt[npLen - 3] = '\n';
      }
    }
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
      // Phase 0 (casing): the model was trained on title-cased "Q:" lines, and
      // lowercase question words tokenize to different ids (who=1558 vs Who=2387),
      // producing visibly worse answers. Title-case the first letter of the
      // question body to match the training distribution. Q&A path only — Do:
      // mode ends in "\nDo:" (no "\nA:"), so this block is skipped by design.
      // Only fires when the lead char is lowercase, so well-cased web prompts
      // (e.g. "What is ESPNOW") stay byte-identical.
      if (bodyLen > 0 && body[0] >= 'a' && body[0] <= 'z') {
        DEBUG_LLM_GENERATEF("[LLM] casing: title-cased '%c'->'%c' to match training Q: format",
                            body[0], (char)(body[0] - 32));
        body[0] = (char)(body[0] - 32);
      }
      // Phase 0b (vocab-aware casing): map casual casing onto whatever whole-token
      // form the loaded model actually has. Entity names are single vocab tokens
      // in exactly one case ("Bulbasaur"=1 token; "bulbasaur"/"BULBASAUR"=multiple
      // fragments the model never learned) — so casually-typed names miss the
      // trained token. For each word, tokenize it as-typed, all-lowercase, and
      // Title-case; if a different casing yields STRICTLY fewer tokens (i.e. hits
      // a whole-word token the typed spelling missed), rewrite the word to it.
      // This resolves bulbasaur / BULBASAUR / Bulbasaur to the model's one name
      // token whether it was trained proper-case OR lowercase — model-agnostic,
      // no hardcoded name list. Words already at their fewest-token form (normal
      // words; the sentence-start handled just above) are left untouched.
      {
        int wi = 0;
        while (wi < bodyLen) {
          while (wi < bodyLen && !isalpha((unsigned char)body[wi])) wi++;   // skip to a word
          int ws = wi;
          while (wi < bodyLen && isalnum((unsigned char)body[wi])) wi++;    // span the word
          int wl = wi - ws;
          if (wl <= 0 || wl > 32) continue;
          // Score each casing by BOTH its space-prefixed token count (the true
          // in-context form — BPE learns " Kanto"/" Potion" as single tokens,
          // where the bare compare ties 3-vs-3 and misses the rewrite) AND the
          // bare count as tiebreak (whole-name added tokens like "Bulbasaur"
          // are stored bare, so their space forms tie at 2 while bare shows
          // 1-vs-3). Rewrite when a casing strictly wins lexicographically.
          char asBuf[36], loBuf[36], tiBuf[36];
          asBuf[0] = loBuf[0] = tiBuf[0] = ' ';           // [0] = space prefix; bare form starts at [1]
          for (int k = 0; k < wl; k++) {
            char ch = body[ws + k];
            char lo = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
            asBuf[k + 1] = ch;
            loBuf[k + 1] = lo;
            tiBuf[k + 1] = (k == 0 && lo >= 'a' && lo <= 'z') ? (char)(lo - 32) : lo;
          }
          asBuf[wl + 1] = loBuf[wl + 1] = tiBuf[wl + 1] = '\0';
          int tmpTok[48];
          int sAs = encode(asBuf, tmpTok, 48), bAs = encode(asBuf + 1, tmpTok, 48);
          int sLo = encode(loBuf, tmpTok, 48), bLo = encode(loBuf + 1, tmpTok, 48);
          int sTi = encode(tiBuf, tmpTok, 48), bTi = encode(tiBuf + 1, tmpTok, 48);
          const char* pick = asBuf; int sPick = sAs, bPick = bAs;
          if (sLo > 0 && bLo > 0 && (sLo < sPick || (sLo == sPick && bLo < bPick))) { pick = loBuf; sPick = sLo; bPick = bLo; }
          if (sTi > 0 && bTi > 0 && (sTi < sPick || (sTi == sPick && bTi < bPick))) { pick = tiBuf; sPick = sTi; bPick = bTi; }
          if (pick != asBuf) {                             // a cleaner casing exists
            DEBUG_LLM_GENERATEF("[LLM] casing: '%.*s' -> '%s' (space %d->%d, bare %d->%d tok)",
                                wl, asBuf + 1, pick + 1, sAs, sPick, bAs, bPick);
            memcpy(body + ws, pick + 1, wl);
          }
        }
      }
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
    setLlmError("OOM: prompt_tokens alloc");
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

    // Temp buffers for two embeddings. Hoisted to PSRAM .bss (was 4 KB — two
    // float[512] — on the llm_gen task stack, the single biggest stack consumer
    // in this whole function). Moving it off-stack is what lets the gen task run
    // from a modest 12 KB internal stack instead of needing the 16 KB contiguous
    // block that the fragmented heap can't supply. Safe as a shared static: this
    // block is diagnostic-only and generation is single-flight (one persistent
    // worker; llmGenerate early-returns unless runState==READY).
    static EXT_RAM_BSS_ATTR float emb_a[512], emb_b[512];  // max dim we'll handle
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
  // Full boost is a runtime setting (llmcontentboost, default 1.5, 0=off); the
  // late boost stays proportional so the default reproduces the old 1.0 late
  // value (1.5 * 0.667 = 1.0). Window (first N tokens get full boost) is fixed.
  static constexpr float CONTENT_LOGIT_BOOST_LATE_RATIO = 0.667f;
  static constexpr int   CONTENT_BOOST_WINDOW      = 16;
  const float CONTENT_LOGIT_BOOST      = gSettings.llmContentBoost;
  const float CONTENT_LOGIT_BOOST_LATE = CONTENT_LOGIT_BOOST * CONTENT_LOGIT_BOOST_LATE_RATIO;
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

  // Clear KV cache for fresh generation (zero bits == 0.0 for both float and half)
  int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
  const int S = gLLM.seq_ctx;
  size_t kvCount = (size_t)p->n_layers * S * kv_dim;
  if (gLLM.kvPrecision == KV_FP16) {
    memset(gLLM.state.key_cache_f16,   0, kvCount * sizeof(uint16_t));
    memset(gLLM.state.value_cache_f16, 0, kvCount * sizeof(uint16_t));
  } else if (gLLM.kvPrecision == KV_INT8) {
    // Only the int8 data needs clearing; scales are written before any read.
    memset(gLLM.state.key_cache_q8,   0, kvCount);
    memset(gLLM.state.value_cache_q8, 0, kvCount);
  } else {
    memset(gLLM.state.key_cache,   0, kvCount * sizeof(float));
    memset(gLLM.state.value_cache, 0, kvCount * sizeof(float));
  }

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

  DEBUG_LLM_GENERATEF("[LLM] Generate: prompt_tokens=%d max=%d temp=%.2f topp=%.2f eos=%d",
                      num_prompt_tokens, maxTokens, temperature, topp, eos_id);
  DEBUG_LLM_GENERATEF("[LLM]   rep_penalty=%.2f rep_window=%d sentence_limit=%d hard_cap=%d suppress=%d",
                      REP_PENALTY, REP_WINDOW, sentenceLimit, hardCap, suppressTokenCount);
  DEBUG_LLM_GENERATEF("[LLM]   arch=%s dim=%d layers=%d heads=%d vocab=%d seq=%d quant=%s",
                      isGPT2gen ? "GPT2" : "Llama",
                      p->dim, p->n_layers, p->n_heads, p->vocab_size, p->seq_len,
                      (p->quant_type == 1) ? "INT8" : "FP32");
  // One-shot verification line for this session's new features. Grep "[LLM] feat:".
  //   casing  → also look for "[LLM] casing:" (fires only on lowercase input)
  //   minP    → also look for "[LLM] min-p:"  (fires only when minP>0)
  //   conf    → reported on the final "Generated N tokens ... conf=" line
  //   kvPrec  → FP16 halves the KV cache (see load-time "KV cache precision:")
  //   guard   → "[LLM] forward: RunState corrupted" / "rebound RunState" on trigger
  DEBUG_LLM_GENERATEF("[LLM] feat: casing=on minP=%.2f%s confidence=on kvPrec=%s corruptGuard=armed",
                      minP, (minP > 0.0f) ? "(active)" : "(off)",
                      (gLLM.kvPrecision == KV_FP16) ? "FP16" :
                      (gLLM.kvPrecision == KV_INT8) ? "INT8" : "FP32");
  //   ngram    → "[LLM] ngram_block:" fires when a loop is broken
  //   confGate → "[LLM] conf_gate:" fires when an answer is hedged
  //   prefill  → classifier skipped during prompt (no line; prompt just gets faster)
  DEBUG_LLM_GENERATEF("[LLM] feat2: noRepeatNgram=%d%s confGate=%.2f%s contentBoost=%.2f%s prefillSkip=on",
                      gSettings.llmNoRepeatNgram,
                      (gSettings.llmNoRepeatNgram >= 2) ? "(active)" : "(off)",
                      gSettings.llmConfThreshold,
                      (gSettings.llmConfThreshold < 0.0f) ? "(armed)" : "(off)",
                      CONTENT_LOGIT_BOOST,
                      (CONTENT_LOGIT_BOOST > 0.0f) ? "(active)" : "(off)");

  // ── Generation loop ─────────────────────────────────────────────────────────
  unsigned long startMs = millis();

  int steps = std::min(maxTokens + num_prompt_tokens, S);

  // Phase 2: confidence signal. Accumulate the mean log-probability of the
  // tokens the model actually generated. Low mean-logprob = the model was
  // flailing through flat distributions (open-domain uncertainty) — a backstop
  // for the no-fact lane, NOT a detector of confident-wrong answers (see plan).
  // Pure observability: does not alter sampling or generation in any way.
  float confSumLogprob = 0.0f;
  int   confCount = 0;

  // ── No-repeat n-gram history ────────────────────────────────────────────
  // Every sampled token is appended; the blocker in the sampling path bans any
  // token that would complete an n-gram already present in this history.
  int hist_len = 0;

  // ── Confidence gate ─────────────────────────────────────────────────────
  // Hold the first CONF_GATE_TOKENS generated tokens; if the running mean
  // logprob is below the threshold, prefix "I'm not sure, but " before
  // releasing. All sinks poll the result buffer, so holding is invisible to
  // them (they just see the first bytes a few seconds later). Fail-open:
  // greedy sampling produces no confidence signal (confCount stays 0 → no
  // hedge), and Do: output is machine-parsed by the web UI so it bypasses
  // the gate entirely.
  const float confGateThresh = gSettings.llmConfThreshold;  // <0 = armed, 0 = off
  static constexpr int CONF_GATE_TOKENS = 6;                // decision window
  EXT_RAM_BSS_ATTR static char confHoldBuf[192];            // PSRAM; single-flight via the runState!=READY gate at entry (same guarantee as repBuf/genHist)
  int  confHoldLen = 0;
  // Greedy (temp<=0) sampling never sets chosenProb, so confCount would stay 0
  // and the hold would only release at end-of-generation (no streaming). Disarm
  // up-front on that path instead of degrading it.
  bool confHolding = (confGateThresh < 0.0f) && !isDoMode && (tokenCb != nullptr) &&
                     (temperature > 0.0f);
  confHoldBuf[0] = '\0';
  auto confRelease = [&]() -> bool {
    confHolding = false;
    bool cbOk = true;
    if (confCount > 0 && (confSumLogprob / (float)confCount) < confGateThresh) {
      DEBUG_LLM_GENERATEF("[LLM] conf_gate: mean=%.3f < %.2f after %d tokens — hedging",
                          confSumLogprob / (float)confCount, confGateThresh, confCount);
      cbOk = tokenCb("I'm not sure, but ");
    }
    if (cbOk && confHoldLen > 0) cbOk = tokenCb(confHoldBuf);
    return cbOk;
  };

  // Soft-recovery budget for RunState-pointer corruption (see forward() guard).
  const int LLM_MAX_CORRUPTION_RETRIES = 2;
  int  corruptionRetries = 0;
  bool corruptionFatal = false;

  // Per-section profiler (llmprofile): latch on and zero the accumulators for
  // this run. Off = forward()'s brackets compile to a single predicted branch.
  gProfActive = gSettings.llmProfile;
  if (gProfActive) gProf = LLMProfile{};

  while (pos < steps) {
    if (gLLM.stopRequested) break;

    // Debug hook (llmcorrupttest): force one RunState-pointer corruption to prove
    // the guard + rebind + retry path end-to-end. Fires once, then self-clears.
    if (gLLM.injectCorruptOnce) {
      gLLM.injectCorruptOnce = false;
      gLLM.state.x = nullptr;
      DEBUG_LLM_GENERATEF("[LLM] TEST: injected RunState corruption (s->x=null) at pos=%d — expect guard+rebound below", pos);
    }

    float* logits = forward(token, pos);
    if (!logits) {
      // forward() bailed on a corrupted RunState. Recover by re-binding the
      // pointers from the surviving base blocks, then retry this position — the
      // KV-cache contents for earlier positions stay valid, so generation can
      // continue. Bounded so we never spin if a base block is gone or the
      // corruption is ongoing; otherwise we stop cleanly instead of panicking.
      if (corruptionRetries < LLM_MAX_CORRUPTION_RETRIES && llmBindRunState()) {
        corruptionRetries++;
        DEBUG_LLM_GENERATEF("[LLM] state corruption at pos=%d — rebound RunState, retry %d/%d",
                            pos, corruptionRetries, LLM_MAX_CORRUPTION_RETRIES);
        logits = forward(token, pos);
      }
      if (!logits) {
        setLlmError("LLM state corruption at pos=%d (recovery failed after %d tries)", pos, corruptionRetries);
        // errorMsg above is RAM-only; record durably. corruptionFatal breaks the
        // generation loop right after, so this fires once per failed generation.
        logSystemEvent("LLM", "state corruption at pos=%d — recovery failed after %d tries; ERROR state until reload", pos, corruptionRetries);
        systemEventPost(SYSEVT_LLM_STATE_CORRUPT, "run-state corruption", gLLM.errorMsg);
        corruptionFatal = true;
        break;
      }
    }

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
      // Gated on the same condition that makes forward() compute prompt-phase
      // logits (see the classifier skip): with debug off, `logits` holds stale
      // values here and these scans would both lie and waste two full-vocab
      // passes per prompt token.
      const bool promptDbg = (getLogLevel() >= LOG_LEVEL_DEBUG) &&
                             isDebugFlagSet(DEBUG_LLM_GENERATE);
      if (promptDbg) {
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
      if (promptDbg && pos == num_prompt_tokens - 2) {
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

        // KV cache health: check that prompt positions have non-zero K/V.
        // FP32-only diagnostic — in FP16 mode key_cache is null (half-float store).
        int kv_d = (p->dim * p->n_kv_heads) / p->n_heads;
        int check_layers[] = {0, p->n_layers/2, p->n_layers-1};
        if (gLLM.kvPrecision == KV_FP32)
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
      if (content_token_count > 0 && CONTENT_LOGIT_BOOST > 0.0f) {
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

      // ── No-repeat n-gram blocker ────────────────────────────────────────
      // Ban any token that would complete an n-gram already generated in this
      // run. Deliberately applied AFTER the rep penalty and content boost, and
      // with NO content-token exemption: exempt+boosted prompt entities are
      // exactly what stabilizes verbatim phrase loops ("It 16. It 16.") — the
      // rep penalty structurally cannot break them, a hard ban can. EOS and
      // the "Q:" stop token are never banned so generation can always end.
      const int NGRAM_N = gSettings.llmNoRepeatNgram;  // 0/1 = off
      if (NGRAM_N >= 2 && gLLM.genHist && hist_len >= NGRAM_N - 1) {
        const int* ctxTail = &gLLM.genHist[hist_len - (NGRAM_N - 1)];
        int nbanned = 0;
        for (int hi = 0; hi <= hist_len - NGRAM_N; hi++) {
          bool ngMatch = true;
          for (int k = 0; k < NGRAM_N - 1; k++) {
            if (gLLM.genHist[hi + k] != ctxTail[k]) { ngMatch = false; break; }
          }
          if (!ngMatch) continue;
          int banTok = gLLM.genHist[hi + NGRAM_N - 1];
          if (banTok == eos_id || banTok == 3) continue;  // never ban stop tokens
          if (banTok >= 0 && banTok < p->vocab_size && logits[banTok] > -1e8f) {
            logits[banTok] = -1e9f;
            nbanned++;
          }
        }
        if (nbanned > 0) {
          DEBUG_LLM_GENERATEF("[LLM] ngram_block: banned %d token(s) (n=%d hist=%d)",
                              nbanned, NGRAM_N, hist_len);
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

      float chosenProb = -1.0f;  // <0 = no signal (greedy temp==0 path)
      next = sample(logits, p->vocab_size, effective_temp, effective_topp, minP, &chosenProb);

      // Phase 2: fold this generated token's confidence into the running mean.
      if (chosenProb > 0.0f) {
        confSumLogprob += logf(chosenProb);
        confCount++;
      }

      // Clamp to valid vocab range — prevents OOV tokens from corrupting output
      if (next < 0 || next >= p->vocab_size) {
        DEBUG_LLM_GENERATEF("[LLM] WARNING: Sampled OOV token %d (vocab_size=%d), clamped to 0", next, p->vocab_size);
        next = 0;  // fallback to first token if somehow OOV
      }

      // Record for the no-repeat n-gram blocker (sampled tokens only — prompt
      // tokens never enter, so the model may still quote a prompt phrase once).
      if (gLLM.genHist && hist_len < gLLM.genHistSize) {
        gLLM.genHist[hist_len++] = next;
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
        bool cbOk;
        if (confHolding) {
          // Confidence gate: buffer instead of emitting until the decision
          // window fills (or the buffer would overflow — decide immediately
          // on the mean so far, then emit this piece normally).
          int plen = (int)strlen(piece);
          if (confHoldLen + plen < (int)sizeof(confHoldBuf) - 1) {
            memcpy(confHoldBuf + confHoldLen, piece, plen);
            confHoldLen += plen;
            confHoldBuf[confHoldLen] = '\0';
            cbOk = (confCount >= CONF_GATE_TOKENS) ? confRelease() : true;
          } else {
            cbOk = confRelease();
            if (cbOk) cbOk = tokenCb(piece);
          }
        } else {
          cbOk = tokenCb(piece);
        }
        if (!cbOk) break;  // callback requested stop
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
      DEBUG_LLM_GENERATEF("[LLM] gen_health: generated=%d tok/s=%.1f unique=%d/%d ctx=%d/%d",
                          generated, tps, unique,
                          rep_buf ? ((rep_fill < REP_WINDOW) ? rep_fill : REP_WINDOW) : 0,
                          pos, S);
    }

    // Yield to IDLE/loop/command tasks on core 1 (see YIELD_INTERVAL note).
    // At YIELD_INTERVAL=1 this fires every token, keeping IDLE1 fed (task-WDT)
    // and letting cmd_exec answer BLE/serial polls mid-generation.
    if (generated % YIELD_INTERVAL == 0) {
      vTaskDelay(1);
    }
  }

  // ── Finalization ────────────────────────────────────────────────────────────
  // Confidence gate: flush anything still held — short answers (EOS, sentence
  // limit, hard cap, Q:-stop, user stop) can end inside the decision window.
  if (confHolding) confRelease();

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

  // Phase 2: finalize the confidence signal for this run.
  gLLM.lastConfTokens = confCount;
  gLLM.lastMeanLogprob = (confCount > 0) ? (confSumLogprob / (float)confCount) : 0.0f;

  DEBUG_LLM_GENERATEF("[LLM] Generated %d tokens in %lums (%.1f tok/s) ctx=%d/%d stopped=%s conf=%.3f(n=%d)",
                      generated, elapsed, gLLM.lastTokPerSec, pos, S,
                      gLLM.stopRequested ? "user" : (generated == 0 ? "eos/empty" : "maxlen"),
                      gLLM.lastMeanLogprob, gLLM.lastConfTokens);
  // Caution for rule authors: an automation that runs an LLM command on
  // llm_gen_done will chain generations back-to-back (bounded by generation
  // latency, but still a loop).
  {
    char det[32];
    snprintf(det, sizeof(det), "%d tok, %.1f tok/s", generated, (double)gLLM.lastTokPerSec);
    systemEventPost(SYSEVT_LLM_GEN_DONE,
                    gLLM.stopRequested ? "user" : (generated == 0 ? "eos" : "maxlen"), det);
  }

  // Per-section profiler dump (llmprofile). Unconditional print (own toggle);
  // answers "matmul vs attention vs the rest" per forward pass. "matmul" = the
  // five weight linears (qkv+out+ffnUp+ffnDown+cls); the gap to wall is embed,
  // residuals, sampling, and logit post-processing.
  if (gProfActive && gProf.tokens > 0) {
    const int64_t mm   = gProf.qkv + gProf.attnOut + gProf.ffnUp + gProf.ffnDown + gProf.cls;
    const int64_t prof = mm + gProf.attn + gProf.kvpack + gProf.act + gProf.norm;
    int64_t wall = (int64_t)elapsed * 1000;                 // µs
    if (wall < prof) wall = prof;                           // guard: never < profiled
    if (wall <= 0)   wall = 1;
    #define _PROF_MS(v)  ((long)(((v) + 500) / 1000))
    #define _PROF_PCT(v) ((int)(((v) * 100 + wall / 2) / wall))
    #define _PROF_US_PER(v) ((long)((v) / gProf.tokens))
    DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_LLM,
      "[LLM][PROF] %d fwd | profiled %ldms of %ldms wall (%d%%) | matmul %ldms(%d%%) attn %ldms(%d%%)",
      gProf.tokens, _PROF_MS(prof), _PROF_MS(wall), _PROF_PCT(prof),
      _PROF_MS(mm), _PROF_PCT(mm), _PROF_MS(gProf.attn), _PROF_PCT(gProf.attn));
    DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_LLM,
      "[LLM][PROF]   matmul: qkv=%ld out=%ld ffnUp=%ld ffnDown=%ld cls=%ld | kvpack=%ld act=%ld norm=%ld (ms)",
      _PROF_MS(gProf.qkv), _PROF_MS(gProf.attnOut), _PROF_MS(gProf.ffnUp),
      _PROF_MS(gProf.ffnDown), _PROF_MS(gProf.cls),
      _PROF_MS(gProf.kvpack), _PROF_MS(gProf.act), _PROF_MS(gProf.norm));
    DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_LLM,
      "[LLM][PROF]   per-fwd µs: qkv=%ld attn=%ld out=%ld ffnUp=%ld ffnDown=%ld cls=%ld kvpack=%ld act=%ld norm=%ld",
      _PROF_US_PER(gProf.qkv), _PROF_US_PER(gProf.attn), _PROF_US_PER(gProf.attnOut),
      _PROF_US_PER(gProf.ffnUp), _PROF_US_PER(gProf.ffnDown), _PROF_US_PER(gProf.cls),
      _PROF_US_PER(gProf.kvpack), _PROF_US_PER(gProf.act), _PROF_US_PER(gProf.norm));
    #undef _PROF_MS
    #undef _PROF_PCT
    #undef _PROF_US_PER
  }

  free(prompt_tokens);
  if (norm_prompt) free(norm_prompt);

  // If state corruption could not be recovered, surface ERROR rather than
  // masking it as a normal (truncated) completion.
  gLLM.runState = corruptionFatal ? LLMState::ERROR : LLMState::READY;
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
    File dir = VFS::openGuarded(dirPath, FILE_READ, VFS::systemAuth("llm.list_models"));
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
// 11b. Async Public API
// ============================================================================

int llmStartAsync(const char* prompt, const LLMGenParams& params) {
  if (!llmIsReady()) return 0;

  // The generation worker is now PERSISTENT — created once at model load and
  // reused (no per-generation task spawn, so a fragmented heap can never fail a
  // generation). Ensure it exists here as a lazy fallback (model loaded before
  // the worker, or a load-time create that lost a fragmentation race).
  if (!llmEnsureWorker()) return 0;

  // Stop any in-progress generation and wait for it to finish. The worker is
  // always alive now, so "busy" is the done flag — not a live task handle.
  if (!gLLMResultDone) {
    llmStop();
    for (int i = 0; i < 50 && !gLLMResultDone; i++) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!gLLMResultDone) return 0;  // previous generation didn't stop in time
  }

  // Allocate PSRAM result buffer on first call
  if (!gLLMResultBuf) {
    gLLMResultBuf = (char*)heap_caps_malloc(LLM_RESULT_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!gLLMResultBuf) return 0;
  }

  // MANDATORY publish order (do not reorder): we observed the prior gen's
  // gLLMResultDone==true above; now reset the buffer, clear done, bump session,
  // and stage the context — then, and only then, wake the worker. xTaskNotifyGive
  // is a memory barrier, so the parked worker sees a zeroed length and the fresh
  // context; and because 'done' is set false HERE (before the give) and true only
  // in the worker epilogue, done=true(N) strictly happens-before done=false(N+1).
  gLLMResultLen    = 0;
  gLLMResultDone   = false;
  gLLMResultBuf[0] = '\0';
  int newSession   = (int)gLLMSessionId + 1;
  gLLMSessionId    = newSession;
  strlcpy(gLLMAsyncCtx.prompt, prompt, sizeof(gLLMAsyncCtx.prompt));
  gLLMAsyncCtx.params = params;

  // Wake the persistent worker (replaces the old per-generation xTaskCreate).
  xTaskNotifyGive(gLLMTask);

  DEBUG_HTTPF("[LLM] Async gen started: session=%d prompt='%.40s%s'",
              newSession, prompt, strlen(prompt) > 40 ? "..." : "");
  return newSession;
}

int llmGetResultChunk(int offset, char* buf, int maxLen) {
  if (!gLLMResultBuf || offset < 0 || maxLen <= 0) return 0;
  int avail = (int)gLLMResultLen - offset;
  if (avail <= 0) return 0;
  if (avail > maxLen - 1) avail = maxLen - 1;
  memcpy(buf, gLLMResultBuf + offset, avail);
  buf[avail] = '\0';
  return avail;
}

int  llmGetResultLen()     { return (int)gLLMResultLen; }
bool llmIsGenerationDone() { return (bool)gLLMResultDone; }
int  llmGetSessionId()     { return (int)gLLMSessionId; }

// ============================================================================
// 12. CLI Commands
// ============================================================================

#include "System_Utils.h"      // CommandEntry, argWantsJson
#include "System_LLMChat.h"    // chatBeginTurn — shared async conversation layer

EXT_RAM_BSS_ATTR static char llmCmdBuf[512];

// Debug: arm a one-shot RunState corruption to verify the forward() guard +
// llmBindRunState rebind + retry recovery path. The next generation nulls s->x;
// the guard catches it, rebinds from the intact base blocks, and continues — so
// the answer still completes. Lets the corruption-recovery path be proven on demand.
static const char* cmd_llm_corrupt_test(const String& /*args*/) {
  if (!llmIsReady()) return "Error: LLM not loaded — load a model first";
  gLLM.injectCorruptOnce = true;
  return "Armed: next generation injects one RunState corruption. Run a prompt, then look for "
         "'[LLM] TEST: injected' followed by 'rebound RunState, retry 1/2' — the answer should still complete.";
}

static const char* cmd_llm_status(const String& argsInput) {
  LLMStatus st = llmGetStatus();
  const char* stateStr = "UNLOADED";
  switch (st.state) {
    case LLMState::LOADING:    stateStr = "LOADING"; break;
    case LLMState::READY:      stateStr = "READY"; break;
    case LLMState::GENERATING: stateStr = "GENERATING"; break;
    case LLMState::ERROR:      stateStr = "ERROR"; break;
    default: break;
  }

  // Structured path: one verbatim JSON blob via a PSRAM buffer (no
  // broadcastOutput). This is what the app's Chat page polls for
  // state / model / tok-s. Schema:
  //   {"v":1,"state","model","tokPerSec","error","psramKB",
  //    "contextUsed","contextMax","tokens"}
  if (argWantsJson(argsInput)) {
    const char* model = st.modelPath;
    const char* slash = strrchr(st.modelPath, '/');
    if (slash) model = slash + 1;            // filename only, per contract
    PSRAM_JSON_DOC(doc);
    // Degraded-context warning (shared helper — same text every surface uses).
    // char[] so ArduinoJson copies it into the doc pool (safe past this scope).
    char ctxWarnBuf[192];
    const bool ctxDeg = llmContextWarning(ctxWarnBuf, sizeof(ctxWarnBuf)) > 0;
    doc["schema"]           = 1;
    doc["state"]       = stateStr;
    doc["model"]       = model;
    doc["tokPerSec"]   = st.lastTokensPerSec;
    doc["error"]       = st.errorMsg;        // "" when no error
    doc["psramKB"]     = (unsigned)(st.totalPsramUsed / 1024);
    doc["contextUsed"] = st.lastContextUsed;
    doc["contextMax"]  = st.lastContextMax;
    doc["tokens"]      = st.lastTokenCount;
    doc["meanLogprob"] = st.lastMeanLogprob;   // Phase 2 confidence (0 = no signal)
    doc["confTokens"]  = st.lastConfTokens;
    doc["ctxWarn"]     = ctxDeg;               // true → context auto-shrunk unusably low
    if (ctxDeg) doc["ctxWarning"] = ctxWarnBuf;
    // Guided-input menu: presence + generation so a surface can show/hide the
    // guided picker and detect a model swap (refetch on gen change).
    JsonObject menu = doc["menu"].to<JsonObject>();
    menu["groups"] = llmMenuGroupCount();
    menu["gen"]    = llmMenuGeneration();
    static char* jbuf = nullptr;
    if (!jbuf) jbuf = (char*)ps_alloc(640, AllocPref::PreferPSRAM, "llmstatus.json");
    if (!jbuf) return "{\"error\":\"oom\"}";
    serializeJson(doc, jbuf, 640);
    return jbuf;
  }

  char ctxWarnLine[224];
  {
    char w[192];
    if (llmContextWarning(w, sizeof(w)) > 0)
      snprintf(ctxWarnLine, sizeof(ctxWarnLine), "WARNING: %s\n", w);
    else
      ctxWarnLine[0] = '\0';
  }
  snprintf(llmCmdBuf, sizeof(llmCmdBuf),
    "LLM State: %s\n"
    "Model: %s\n"
    "Config: dim=%d layers=%d heads=%d vocab=%d seq=%d ctx=%d\n"
    "PSRAM: %uKB (weights=%uKB runtime=%uKB)\n"
    "Last: %d tokens @ %.1f tok/s\n"
    "%s%s%s",
    stateStr, st.modelPath,
    st.config.dim, st.config.n_layers, st.config.n_heads,
    st.config.vocab_size, st.config.seq_len, st.lastContextMax,
    (unsigned)(st.totalPsramUsed / 1024),
    (unsigned)(st.modelSizeBytes / 1024),
    (unsigned)(st.runtimeSizeBytes / 1024),
    st.lastTokenCount, st.lastTokensPerSec,
    ctxWarnLine,
    st.errorMsg[0] ? "Error: " : "",
    st.errorMsg[0] ? st.errorMsg : "");
  return llmCmdBuf;
}

static const char* cmd_llm_load(const String& args) {
  if (!gSettings.llmEnabled) {
    return "{\"schema\":1,\"ok\":false,\"error\":\"LLM disabled - run llmenabled 1 first\"}";
  }
  CommandArgs ca(args);
  String a = ca.arg(0);  // optional model filename

  const char* modelPath = LLM_DEFAULT_MODEL_PATH;

  char customPath[96];
  if (a.length() > 0) {
    if (a.startsWith("/")) {
      // Full path provided — use as-is
      strlcpy(customPath, a.c_str(), sizeof(customPath));
    } else {
      // Bare filename — try SD card first, then internal
      snprintf(customPath, sizeof(customPath), "/sd/llm/%s", a.c_str());
      if (!VFS::isSDAvailable() || !VFS::existsGuarded(customPath, VFS::systemAuth("llm.load_check"))) {
        snprintf(customPath, sizeof(customPath), "/system/llm/%s", a.c_str());
      }
    }
    modelPath = customPath;
  }

  // JSON reply mirrors POST /api/llm/load ({"ok":true} / {"ok":false,"error"}).
  // The app sends `llmload <name>` (no `json` token) and parses the reply like
  // the web, so this must be a JSON object, not the old human "Model loaded:".
  bool ok = llmLoadModel(modelPath);
  if (ok) {
    snprintf(llmCmdBuf, sizeof(llmCmdBuf), "{\"schema\":1,\"ok\":true}");
  } else {
    LLMStatus st = llmGetStatus();
    // errorMsg is firmware-controlled (short, no quotes/backslashes) — safe to inline.
    snprintf(llmCmdBuf, sizeof(llmCmdBuf), "{\"schema\":1,\"ok\":false,\"error\":\"%s\"}", st.errorMsg);
  }
  return llmCmdBuf;
}

static const char* cmd_llm_unload(const String&) {
  // llmUnload() already stops a live generation and waits for it before freeing
  // the weights, so there is nothing to add here. The generation worker stays
  // parked on its notify holding its stack — see gLLMWorkerStack.
  llmUnload();
  return "{\"schema\":1,\"ok\":true}";   // mirror POST /api/llm/unload
}

static const char* cmd_llm_models(const String& argsInput) {
  // Structured path: {"v":1,"models":["a.bin","b.bin"]} — names only, the
  // shape the app's model picker consumes. Reuse llmListModels() (the single
  // source of truth for the LittleFS + SD scan, which emits rich objects) and
  // project it down to the filename list the contract asks for.
  if (argWantsJson(argsInput)) {
    String rich = llmListModels();   // [{"name","size","path","storage"},...]
    PSRAM_JSON_DOC(src);
    PSRAM_JSON_DOC(out);
    out["schema"] = 1;
    JsonArray names = out["models"].to<JsonArray>();
    if (deserializeJson(src, rich) == DeserializationError::Ok) {
      for (JsonObject m : src.as<JsonArray>()) {
        const char* n = m["name"] | "";
        if (n[0]) names.add(n);              // linked into src, alive until serialize
      }
    }
    static char* jbuf = nullptr;
    if (!jbuf) jbuf = (char*)ps_alloc(1024, AllocPref::PreferPSRAM, "llmmodels.json");
    if (!jbuf) return "{\"error\":\"oom\"}";
    serializeJson(out, jbuf, 1024);
    return jbuf;
  }

  String models = llmListModels();
  bool sdAvail = VFS::isSDAvailable();
  snprintf(llmCmdBuf, sizeof(llmCmdBuf), "Models (internal + %s):\n%s",
           sdAvail ? "SD card" : "no SD card", models.c_str());
  return llmCmdBuf;
}

static const char* cmd_llm_generate(const String& args) {
  // Structured (async, non-blocking) path. Contract: `llmgenerate json <prompt>`
  // — the leading `json` token is the mode flag, everything after it is the
  // prompt (which may contain spaces, and may itself mention the word "json").
  // So detect the LEADING token here rather than via argWantsJson(), which
  // scans the whole string and would false-trigger on a prompt that merely
  // says "json". Kicks generation off via the shared chat layer and returns
  // {session} immediately — it must NOT block until generation finishes (that
  // is the whole reason this path exists; the blocking human path below would
  // tie up the BLE channel for the entire run and trip the app's watchdog).
  {
    String a = args; a.trim();
    if (argLeadingTokenIsJson(a)) {
      if (!llmIsReady()) return "{\"schema\":1,\"ok\":false,\"error\":\"model not ready\"}";
      String payload = a.startsWith("json ") ? a.substring(5) : String();
      payload.trim();
      if (payload.length() == 0) return "{\"schema\":1,\"ok\":false,\"error\":\"empty prompt\"}";

      // Two accepted shapes:
      //   json <raw prompt text>                    → no overrides
      //   json {"prompt":"...","params":{...}}       → per-request overrides
      // The object form lets the app tune a single reply (e.g. hard_cap +
      // sentence_limit for a Do:-style short answer) without mutating settings.
      // See docs/LLM_BLE_GENERATE_OVERRIDES.md for the wire contract.
      String prompt;
      ChatParamOverride ov;
      bool haveOverrides = false;
      if (payload[0] == '{') {
        PSRAM_JSON_DOC(body);   // parse pool in PSRAM, not the tight internal DRAM
        if (deserializeJson(body, payload) != DeserializationError::Ok)
          return "{\"schema\":1,\"ok\":false,\"error\":\"invalid JSON payload\"}";
        prompt = (const char*)(body["prompt"] | "");
        prompt.trim();
        if (prompt.length() == 0) return "{\"schema\":1,\"ok\":false,\"error\":\"empty prompt\"}";
        JsonObjectConst params = body["params"].as<JsonObjectConst>();
        if (!params.isNull()) { chatParamOverrideFromJson(params, ov); haveOverrides = true; }
      } else {
        prompt = payload;  // raw text form
      }

      int session = chatBeginTurn(prompt.c_str(), haveOverrides ? &ov : nullptr);
      if (session <= 0) return "{\"schema\":1,\"ok\":false,\"error\":\"busy or failed to start\"}";
      // Mirror the web's {"ok":true,"session":N} (POST /api/llm/generate &
      // /chat/retry). The app validates the start by `ok`, so omitting it reads
      // as "command not recognized" → its "streaming not supported" fallback.
      // Generation runs async: this only starts it. The reply streams in
      // separately, so point the caller at the poll command (the `hint` key
      // mirrors the human-path cliHint — same text either way).
      snprintf(llmCmdBuf, sizeof(llmCmdBuf),
               "{\"schema\":1,\"ok\":true,\"session\":%d,"
               "\"hint\":\"the reply streams in asynchronously - read it with 'llmresult json 0'\"}",
               session);
      return llmCmdBuf;
    }
  }

  if (!llmIsReady()) return "Error: no model loaded";

  CommandArgs ca(args);
  if (ca.count() == 0) return "Error: invalid arguments — Usage: llm generate <prompt>";
  String a = ca.raw();  // full prompt text

  // This path drives the engine directly instead of going through chatBeginTurn,
  // so it has to frame the prompt itself — otherwise `llmgenerate <question>`
  // would keep completing prose while `llmgenerate json <question>` answered
  // properly. Static rather than a stack buffer: llmGenerate runs on this same
  // task and already wants ~5 KB of its stack, and llmGenerate's
  // READY->GENERATING gate is what stops two synchronous generations — hence two
  // users of this buffer — from overlapping.
  EXT_RAM_BSS_ATTR static char framedBuf[1024];
  const char* framed = llmFramePrompt(a.c_str(), framedBuf, sizeof(framedBuf));

  // Build output into buffer
  String output;
  output.reserve(1024);

  int result = llmGenerate(framed, [&output](const char* token) -> bool {
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

static const char* cmd_llm_result(const String& args) {
  // Poll for streamed tokens. Contract: `llmresult json <offset>` →
  //   {"v":1,"text":"<bytes since offset>","done":<bool>,"len":<total so far>}
  // Mirrors GET /api/llm/result. We read straight from the engine's result
  // buffer (llmGetResult*) rather than the chat layer's streaming cursor:
  // the engine buffer persists after generation ends, so the final chunk and
  // the total length stay readable on the very poll where done flips true —
  // whereas chatReadStream()/chatGetStreamLen() return 0 the instant the
  // streaming turn is finalized, which would silently drop the tail.
  String a = args; a.trim();
  if (!argLeadingTokenIsJson(a))
    return "Error: invalid arguments — Usage: llmresult json <offset>";

  int offset = 0;
  if (a.startsWith("json ")) {
    String rest = a.substring(5); rest.trim();
    offset = rest.toInt();
    if (offset < 0) offset = 0;
  }

  // 512-byte read window mirrors the web poller. The app polls ~every 350 ms
  // and advances offset = len, so this easily outpaces on-device generation;
  // any backlog (engine ran ahead) just drains across successive polls — no
  // data is lost because the engine buffer is not cleared until the next gen.
  char chunk[512];
  int  n     = llmGetResultChunk(offset, chunk, sizeof(chunk));
  int  total = llmGetResultLen();
  bool done  = llmIsGenerationDone();

  PSRAM_JSON_DOC(doc);
  doc["schema"]    = 1;
  doc["text"] = (n > 0) ? (const char*)chunk : "";   // linked; chunk outlives serialize
  doc["done"] = done;
  doc["len"]  = total;
  // Sized for the worst case: a full 512-byte window where every byte needs
  // \uXXXX escaping (512×6) + envelope, still inside the 4 KB command-return
  // cap. Real LLM text escapes to ~1.1× so the typical blob is ~550 B.
  static char* jbuf = nullptr;
  if (!jbuf) jbuf = (char*)ps_alloc(3200, AllocPref::PreferPSRAM, "llmresult.json");
  if (!jbuf) return "{\"error\":\"oom\"}";
  serializeJson(doc, jbuf, 3200);
  return jbuf;
}

static const char* cmd_llm_stop(const String&) {
  llmStop();
  return "{\"schema\":1,\"ok\":true}";   // mirror POST /api/llm/stop
}

static const char* cmd_llm_clear(const String&) {
  // Reset the shared conversation. Thin wrapper over chatClear() — the same
  // call POST /api/llm/chat/clear makes. Without this, a BLE app's "New chat"
  // only wipes its own bubbles while the device keeps accumulating turns
  // (the model still "remembers" cleared messages and contextUsed creeps up).
  // chatClear() refuses mid-generation, so surface that as a stop-first hint.
  // JSON mirrors POST /api/llm/chat/clear so the app parses it like the web.
  if (!chatClear()) return "{\"schema\":1,\"ok\":false,\"error\":\"busy — stop first\"}";
  return "{\"schema\":1,\"ok\":true}";
}

static const char* cmd_llm_retry(const String&) {
  // Regenerate the last assistant reply. Thin wrapper over chatRetryLast()
  // (mirror of POST /api/llm/chat/retry): it drops the last reply, steers the
  // model away from repeating it, and kicks off a fresh async generation.
  // Returns a session exactly like `llmgenerate json` — the app then polls
  // `llmresult json <offset>` to stream the new reply.
  if (!llmIsReady()) return "{\"schema\":1,\"ok\":false,\"error\":\"model not ready\"}";
  int session = chatRetryLast(nullptr);
  if (session <= 0) return "{\"schema\":1,\"ok\":false,\"error\":\"no prior turn or busy\"}";
  // Like llmgenerate, this only kicks off the regeneration; the new reply
  // streams in separately via the result poller.
  snprintf(llmCmdBuf, sizeof(llmCmdBuf),
           "{\"schema\":1,\"session\":%d,"
           "\"hint\":\"the new reply streams in asynchronously - read it with 'llmresult json 0'\"}",
           session);
  return llmCmdBuf;
}

static const char* cmd_llm_turns(const String& args) {
  // Resync the conversation after a reconnect. Wraps the same turn-reader
  // functions the web's GET /api/llm/chat/turns uses (chatGetTurnCount /
  // chatGetTurnInfo / chatReadTurn — web-only until now).
  //
  // The web endpoint streams an unbounded array; a BLE command reply is capped
  // at 4 KB and can't chunk arbitrarily. So this is paginated ONE TURN PER
  // CALL by index — a single turn (<=2 KB body) always fits. The app reads
  // `count` from any response, then fetches index 0..count-1. Schema:
  //   {"v":1,"count":N,"index":I,"role":"user|assistant","text":"…",
  //    "tokens":T,"tokPerSec":F,"streaming":bool}
  //   out-of-range → {"v":1,"count":N,"index":I,"end":true}
  String a = args; a.trim();
  if (!argLeadingTokenIsJson(a))
    return "Error: invalid arguments — Usage: llmturns json <index>";

  int index = 0;
  if (a.startsWith("json ")) {
    String rest = a.substring(5); rest.trim();
    index = rest.toInt();
    if (index < 0) index = 0;
  }

  int count = chatGetTurnCount();
  PSRAM_JSON_DOC(doc);
  doc["schema"]     = 1;
  doc["count"] = count;
  doc["index"] = index;

  ChatTurnInfo info;
  if (index >= count || !chatGetTurnInfo(index, &info)) {
    doc["end"] = true;
    static char* ebuf = nullptr;
    if (!ebuf) ebuf = (char*)ps_alloc(96, AllocPref::PreferPSRAM, "llmturns_end.json");
    if (!ebuf) return "{\"error\":\"oom\"}";
    serializeJson(doc, ebuf, 96);
    return ebuf;
  }

  // Read the turn body into a reusable PSRAM scratch buffer. Assigned to the
  // doc as a (non-const) char* so ArduinoJson COPIES it into the doc pool —
  // the scratch buffer can then be reused safely on the next call.
  static char* turnBuf = nullptr;
  if (!turnBuf) turnBuf = (char*)ps_alloc(LLM_CHAT_TURN_MAX_BYTES + 1, AllocPref::PreferPSRAM, "llmturn.txt");
  if (!turnBuf) return "{\"error\":\"oom\"}";
  chatReadTurn(index, 0, turnBuf, LLM_CHAT_TURN_MAX_BYTES + 1);

  doc["role"]      = (info.role == ChatTurnRole::USER) ? "user" : "assistant";
  doc["text"]      = turnBuf;                       // char* → copied into doc
  doc["tokens"]    = info.tokenCount;
  doc["tokPerSec"] = info.tokensPerSecX10 / 10.0f;
  doc["streaming"] = info.isStreaming;

  // One turn body is <=2 KB; realistic text escapes to ~1.1×, so the blob sits
  // well under the 4 KB command-return cap.
  static char* jbuf = nullptr;
  if (!jbuf) jbuf = (char*)ps_alloc(4096, AllocPref::PreferPSRAM, "llmturns.json");
  if (!jbuf) return "{\"error\":\"oom\"}";
  serializeJson(doc, jbuf, 4096);
  return jbuf;
}

// ============================================================================
// LLM Settings Module (generation defaults, persisted)
// ============================================================================

// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options, isSecret, group, cmdKey
static const SettingEntry llmSettingEntries[] = {
  { "llmEnabled", SETTING_BOOL, &gSettings.llmEnabled, 1, 0, nullptr, 0, 1, "Enabled", nullptr, false, nullptr, "llmenabled" },
  { "autoStart",     SETTING_BOOL,   &gSettings.llmAutoStart,     0, 0,    nullptr,    0,    1, "Auto-start at boot",   nullptr, false, nullptr, "llmautostart" },  // idx 1
  { "temperature",   SETTING_FLOAT,  &gSettings.llmTemperature,  0,   0.5f, nullptr,    0,    0, "Temperature",          nullptr, false, nullptr, "llmtemperature"   },
  { "topP",          SETTING_FLOAT,  &gSettings.llmTopP,         0,   0.8f, nullptr,    0,    1, "Top-P",                nullptr, false, nullptr, "llmtopp"          },
  { "maxTokens",     SETTING_INT,    &gSettings.llmMaxTokens,    256, 0,    nullptr,    1,  512, "Max Tokens",           nullptr, false, nullptr, "llmmaxtokens"     },
  { "sentenceLimit", SETTING_INT,    &gSettings.llmSentenceLimit,  2, 0,    nullptr,    0,   20, "Sentence Limit",       nullptr, false, nullptr, "llmsentencelimit" },
  { "hardCap",       SETTING_INT,    &gSettings.llmHardCap,       80, 0,    nullptr,    0,  512, "Hard Cap",             nullptr, false, nullptr, "llmhardcap"       },
  { "repPenalty",    SETTING_FLOAT,  &gSettings.llmRepPenalty,    0,  1.3f, nullptr,    1,    3, "Rep Penalty",          nullptr, false, nullptr, "llmreppenalty"    },
  { "repWindow",     SETTING_INT,    &gSettings.llmRepWindow,     32, 0,    nullptr,    1, LLM_DEFAULT_REP_WINDOW, "Rep Window", nullptr, false, nullptr, "llmrepwindow" },  // max = ring size; higher values were silently truncated
  { "maxContext",    SETTING_INT,    &gSettings.llmMaxContext,     0, 0,    nullptr,    0, LLM_SETTING_MAX_CONTEXT, "Max Context (0=auto)", nullptr, false, nullptr, "llmmaxcontext"    },
  { "defaultModel",  SETTING_STRING, &gSettings.llmDefaultModel,  0, 0,    "model.bin",0,    0, "Default Model",        nullptr, false, nullptr, "llmdefaultmodel"  },
  // ── APPEND-ONLY below this line ───────────────────────────────────────────
  // The CLI setting commands (LLM_SETTING_CMD) map to this table by INDEX, so
  // inserting mid-table silently misroutes every command after the insert point.
  // New settings go HERE, at the end, with a matching macro+command index below.
  // (Mirostat + dynTemp rows were removed 2026-07-10; the whole table + macros
  //  were renumbered in one verified pass — the sanctioned append-only break.)
  { "minP",          SETTING_FLOAT,  &gSettings.llmMinP,          0, 0.0f, nullptr,    0,    1, "Min-P (0=off)",        nullptr, false, nullptr, "llmminp"   },  // idx 11
  { "kvPrecision",   SETTING_INT,    &gSettings.llmKvPrecision,   1, 0,    nullptr,    0,    2, "KV Cache (0=FP32,1=FP16,2=INT8, reload to apply)", "0:FP32,1:FP16,2:INT8", false, nullptr, "llmkvprec" },  // idx 12 (default FP16: 2x ctx, ~lossless)
  { "noRepeatNgram", SETTING_INT,    &gSettings.llmNoRepeatNgram, 0, 0,    nullptr,    0,    8, "No-repeat n-gram (0=off)", nullptr, false, nullptr, "llmnorepeatngram" },  // idx 13 (shipped off per user; 3 typical)
  { "confThreshold", SETTING_FLOAT,  &gSettings.llmConfThreshold, 0, -1.0f, nullptr,  -8,    0, "Confidence gate mean-logprob (0=off)", nullptr, false, nullptr, "llmconfthreshold" },  // idx 14
  { "contentBoost",  SETTING_FLOAT,  &gSettings.llmContentBoost,  0, 1.5f, nullptr,    0,    4, "Content boost (0=off)", nullptr, false, nullptr, "llmcontentboost" },  // idx 15 (on-topic logit bonus; co-tune w/ repPenalty)
  { "domainGate",    SETTING_BOOL,   &gSettings.llmDomainGate,    1, 0,    nullptr,    0,    1, "Domain gate (refuse off-topic)", nullptr, false, nullptr, "llmdomaingate" },  // idx 16 (refuse prompts outside the .bin's embedded domain vocab)
  { "profile",       SETTING_BOOL,   &gSettings.llmProfile,       0, 0,    nullptr,    0,    1, "Profiler (per-section fwd timing)", nullptr, false, nullptr, "llmprofile" },  // idx 17 (diagnostic; dumps a breakdown after each generation)
};

extern const SettingsModule llmSettingsModule = {
  "llm", "apps.llm", llmSettingEntries,
  sizeof(llmSettingEntries) / sizeof(llmSettingEntries[0]),
  nullptr,
  "On-device LLM generation defaults"
};

// Setting command handlers (one per entry, via index)
#define LLM_SETTING_CMD(funcName, idx) \
  static const char* funcName(const String& a) { \
    return handleSettingCommand(&llmSettingEntries[idx], a); \
  }

LLM_SETTING_CMD(cmd_llm_temperature,   0)
LLM_SETTING_CMD(cmd_llm_topp,          1)
LLM_SETTING_CMD(cmd_llm_maxtokens,     2)
LLM_SETTING_CMD(cmd_llm_sentencelimit, 3)
LLM_SETTING_CMD(cmd_llm_hardcap,       4)
LLM_SETTING_CMD(cmd_llm_reppenalty,    5)
LLM_SETTING_CMD(cmd_llm_repwindow,     6)
LLM_SETTING_CMD(cmd_llm_maxcontext,    7)
LLM_SETTING_CMD(cmd_llm_defaultmodel,  8)
LLM_SETTING_CMD(cmd_llm_minp,          9)
LLM_SETTING_CMD(cmd_llm_kvprec,       10)
LLM_SETTING_CMD(cmd_llm_autostart,    11)
LLM_SETTING_CMD(cmd_llm_norepeatngram, 12)
LLM_SETTING_CMD(cmd_llm_confthreshold, 13)
LLM_SETTING_CMD(cmd_llm_contentboost, 14)
LLM_SETTING_CMD(cmd_llm_domaingate,   15)
LLM_SETTING_CMD(cmd_llm_profile,      16)

const CommandEntry llmCommands[] = {
  { "llmstatus",        "Show LLM engine status (add 'json' for JSON output)",               false, cmd_llm_status },
  { "llmload",          "Load model [model.bin]",               true,  cmd_llm_load,         "Usage: llmload [filename.bin]" },
  { "llmunload",        "Unload model and free PSRAM",          true,  cmd_llm_unload },
  { "llmautostart",     "Auto-load default model at boot (0|1)", true,  cmd_llm_autostart,    "Usage: llmautostart <0|1>" },
  { "llmmodels",        "List available model files (add 'json' for JSON output)",           false, cmd_llm_models },
  { "llmgenerate",      "Ask the loaded model a question",      false, cmd_llm_generate,     "Usage: llmgenerate <question>  |  llmgenerate do: <intent>\nThe question is wrapped in the model's Q:/A: format for you, same as the web chat. Prefix 'do:' to ask for a command instead of an answer. A prompt that already starts with 'Q:' is sent as-is." },
  { "llmresult",        "Poll streamed generation (JSON)",      false, cmd_llm_result,       "Usage: llmresult json <offset>" },
  { "llmstop",          "Stop in-progress generation",          false, cmd_llm_stop },
  { "llmcorrupttest",   "Debug: force corruption-recovery test", true, cmd_llm_corrupt_test },
  { "llmclear",         "Reset the LLM conversation",           false, cmd_llm_clear },
  { "llmretry",         "Regenerate the last reply (JSON)",     false, cmd_llm_retry },
  { "llmturns",         "Read a conversation turn (JSON)",      false, cmd_llm_turns,        "Usage: llmturns json <index>" },
  { "llmmenu",          "Guided-input menu status/listing (add 'json')",  false, cmd_llm_menu, "Usage: llmmenu  |  llmmenu json  |  llmmenu json tpl <g> <off>  |  llmmenu json ent <g> <off>\nLists this model's guided question templates + entity rosters (empty if the model ships none)." },
  { "llmask",           "Ask a guided question by index",       false, cmd_llm_ask,          "Usage: llmask <group> <template> [entity]  |  llmask json <gen> <g> <t> [e]  |  llmask repeat\nComposes the question on-device from 'llmmenu' indices and streams the answer (read it with 'llmresult json 0')." },
  { "llmtemperature",   "Set default sampling temperature",     true,  cmd_llm_temperature,  "Usage: llmtemperature <0.0-2.0>" },
  { "llmtopp",          "Set default Top-P threshold",          true,  cmd_llm_topp,         "Usage: llmtopp <0.0-1.0>" },
  { "llmmaxtokens",     "Set default max tokens per reply",     true,  cmd_llm_maxtokens,    "Usage: llmmaxtokens <1-512>" },
  { "llmsentencelimit", "Set default sentence stop limit",      true,  cmd_llm_sentencelimit,"Usage: llmsentencelimit <0-20>" },
  { "llmhardcap",       "Set default hard token cap",           true,  cmd_llm_hardcap,      "Usage: llmhardcap <0-512>" },
  { "llmreppenalty",    "Set default repetition penalty",       true,  cmd_llm_reppenalty,   "Usage: llmreppenalty <1.0-3.0>" },
  { "llmrepwindow",     "Set default rep-penalty look-back",    true,  cmd_llm_repwindow,    "Usage: llmrepwindow <1-32>" },
  { "llmmaxcontext",    "Set KV cache context window (0=auto)", true,  cmd_llm_maxcontext,   "Usage: llmmaxcontext <0-4096>" },
  { "llmdefaultmodel",  "Set default model filename",           true,  cmd_llm_defaultmodel, "Usage: llmdefaultmodel <filename.bin>" },
  { "llmminp",          "Set min-p sampling floor (0=off)",     true,  cmd_llm_minp,         "Usage: llmminp <0.0-1.0>" },
  { "llmkvprec",        "KV cache precision (0=FP32,1=FP16,2=INT8)", true,  cmd_llm_kvprec,       "Usage: llmkvprec <0..2>  (0=FP32,1=FP16,2=INT8; reload model to apply)" },
  { "llmnorepeatngram", "Ban repeating generated n-grams (0=off)", true, cmd_llm_norepeatngram, "Usage: llmnorepeatngram <0-8>  (default 0=off; 3 breaks verbatim phrase loops)" },
  { "llmconfthreshold", "Low-confidence hedge threshold (0=off)",  true, cmd_llm_confthreshold, "Usage: llmconfthreshold <-8.0-0>  (mean logprob; default -1.0, 0 disables)" },
  { "llmcontentboost",  "On-topic logit bonus (0=off)",         true,  cmd_llm_contentboost, "Usage: llmcontentboost <0.0-4.0>  (default 1.5; higher = stickier to prompt words)" },
  { "llmdomaingate",    "Refuse prompts outside the model's domain (0|1)", true, cmd_llm_domaingate, "Usage: llmdomaingate <0|1>  (only enforced when the model .bin carries a domain vocab)" },
  { "llmprofile",       "Per-section forward-pass timing breakdown (0|1)",  true, cmd_llm_profile,    "Usage: llmprofile <0|1>  (diagnostic; splits qkv/attn/ffn/cls after each generation. Turn OFF other debugllm* flags for clean numbers)" },
};
const size_t llmCommandsCount = sizeof(llmCommands) / sizeof(llmCommands[0]);

#endif // ENABLE_ONDEVICE_LLM
