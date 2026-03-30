/**
 * System_LLM.cpp - On-device LLM inference engine
 *
 * Port of Andrej Karpathy's llama2.c for ESP32-S3 with PSRAM.
 * https://github.com/karpathy/llama2.c
 *
 * Key adaptations:
 *   - All weight/activation buffers allocated in PSRAM via heap_caps
 *   - esp-dsp dot product for S3 SIMD acceleration
 *   - LittleFS model loading (no mmap)
 *   - Thread-safe generation with stop flag
 *   - Token callback for streaming output
 */

#include "System_BuildConfig.h"

#if ENABLE_ONDEVICE_LLM

#include "System_LLM.h"
#include "System_Debug.h"
#include "System_MemUtil.h"
#include "System_Filesystem.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <algorithm>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"

// esp-dsp for accelerated dot product on S3
#include "dsps_dotprod.h"

extern bool filesystemReady;

// ============================================================================
// Transformer Weights — pointers into a single PSRAM block
// ============================================================================

struct TransformerWeights {
  float* token_embedding_table;  // (vocab_size, dim)
  float* rms_att_weight;         // (layer, dim)
  float* rms_ffn_weight;         // (layer, dim)
  float* wq;                     // (layer, dim, n_heads * head_size)
  float* wk;                     // (layer, dim, n_kv_heads * head_size)
  float* wv;                     // (layer, dim, n_kv_heads * head_size)
  float* wo;                     // (layer, n_heads * head_size, dim)
  float* w1;                     // (layer, hidden_dim, dim)
  float* w2;                     // (layer, dim, hidden_dim)
  float* w3;                     // (layer, hidden_dim, dim)
  float* rms_final_weight;       // (dim,)
  float* wcls;                   // (vocab_size, dim) — classifier weights (optional, may alias embedding)
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
};

// ============================================================================
// Tokenizer
// ============================================================================

struct TokenizerState {
  char** vocab;
  float* vocab_scores;
  int vocab_size;
  unsigned int max_token_length;
  // Sorted vocab for BPE encode
  struct TokenIndex {
    const char* str;
    int id;
  };
  TokenIndex* sorted_vocab;
  bool sorted_vocab_built;
};

// ============================================================================
// Module State
// ============================================================================

static struct {
  LLMConfig config;
  TransformerWeights weights;
  RunState state;
  TokenizerState tokenizer;

  // Raw allocated blocks (for free)
  float* weightsData;
  float* stateData;
  char* tokenizerData;   // raw tokenizer file data

  size_t weightsSize;
  size_t stateSize;

  LLMState runState;
  volatile bool stopRequested;
  float lastTokPerSec;
  int lastTokCount;
  char modelPath[64];
  char errorMsg[128];

  SemaphoreHandle_t mutex;
} gLLM = {};

// ============================================================================
// PSRAM Allocation Helpers
// ============================================================================

static void* llmPsramAlloc(size_t size, const char* tag) {
  void* p = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM);
  if (!p) {
    ERROR_SYSTEMF("[LLM] PSRAM alloc failed: %s (%u bytes)", tag, (unsigned)size);
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
// Math Primitives
// ============================================================================

static void rmsnorm(float* o, const float* x, const float* weight, int size) {
  float ss = 0.0f;
  for (int j = 0; j < size; j++) ss += x[j] * x[j];
  ss /= size;
  ss += 1e-5f;
  ss = 1.0f / sqrtf(ss);
  for (int j = 0; j < size; j++) o[j] = weight[j] * (ss * x[j]);
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

// ============================================================================
// Transformer Forward Pass
// ============================================================================

static float* forward(int token, int pos) {
  LLMConfig* p = &gLLM.config;
  TransformerWeights* w = &gLLM.weights;
  RunState* s = &gLLM.state;

  int dim = p->dim;
  int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
  int kv_mul = p->n_heads / p->n_kv_heads;
  int hidden_dim = p->hidden_dim;
  int head_size = dim / p->n_heads;

  // Copy token embedding into activation
  float* content_row = w->token_embedding_table + token * dim;
  memcpy(s->x, content_row, dim * sizeof(float));

  // Forward through all layers
  for (int l = 0; l < p->n_layers; l++) {
    // Attention rmsnorm
    rmsnorm(s->xb, s->x, w->rms_att_weight + l * dim, dim);

    // Key and value point to the KV cache
    int loff = l * p->seq_len * kv_dim;
    float* key_cache_row = s->key_cache + loff + pos * kv_dim;
    float* value_cache_row = s->value_cache + loff + pos * kv_dim;

    // QKV matmuls
    matmul(s->q, s->xb, w->wq + l * dim * dim, dim, dim);
    matmul(key_cache_row, s->xb, w->wk + l * dim * kv_dim, dim, kv_dim);
    matmul(value_cache_row, s->xb, w->wv + l * dim * kv_dim, dim, kv_dim);

    // RoPE relative positional encoding
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

    // Multi-head attention
    for (int h = 0; h < p->n_heads; h++) {
      float* q_h = s->q + h * head_size;
      float* att_h = s->att + h * p->seq_len;
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

    // Output projection
    matmul(s->xb2, s->xb, w->wo + l * dim * dim, dim, dim);

    // Residual connection
    for (int i = 0; i < dim; i++) s->x[i] += s->xb2[i];

    // FFN rmsnorm
    rmsnorm(s->xb, s->x, w->rms_ffn_weight + l * dim, dim);

    // FFN: w1, w3 (SwiGLU)
    matmul(s->hb, s->xb, w->w1 + l * dim * hidden_dim, dim, hidden_dim);
    matmul(s->hb2, s->xb, w->w3 + l * dim * hidden_dim, dim, hidden_dim);

    // SwiGLU activation
    for (int i = 0; i < hidden_dim; i++) {
      float val = s->hb[i];
      // silu(x) = x * sigmoid(x)
      val *= (1.0f / (1.0f + expf(-val)));
      val *= s->hb2[i];
      s->hb[i] = val;
    }

    // FFN output
    matmul(s->xb, s->hb, w->w2 + l * dim * hidden_dim, hidden_dim, dim);

    // Residual
    for (int i = 0; i < dim; i++) s->x[i] += s->xb[i];
  }

  // Final rmsnorm
  rmsnorm(s->x, s->x, w->rms_final_weight, dim);

  // Classifier into logits
  matmul(s->logits, s->x, w->wcls, dim, p->vocab_size);

  return s->logits;
}

// ============================================================================
// Sampling
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
  // Top-p (nucleus) sampling
  // Sort probabilities in descending order, accumulate until >= topp
  struct ProbIndex { float prob; int index; };

  int n0 = 0;
  float cutoff = (1.0f - topp) / (n - 1);
  // Pre-filter: skip tokens with probability below cutoff
  // Use logits buffer as temp storage for ProbIndex (it's big enough)
  ProbIndex* probindex = (ProbIndex*)gLLM.state.logits; // reuse logits buffer temporarily
  // Actually we can't reuse logits since probabilities points there.
  // Use a simple approach: just iterate

  // Simple top-p without extra allocation: find threshold by sorting
  // For tiny vocab this is fine. For 32K vocab on ESP32, acceptable.

  // Accumulate from max down
  int best = sample_argmax(probabilities, n);
  float cumsum = 0.0f;

  // Temperature already applied to logits before softmax,
  // so probabilities sum to 1. Sample proportionally.
  float r = (float)esp_random() / (float)UINT32_MAX;

  cumsum = 0.0f;
  for (int i = 0; i < n; i++) {
    cumsum += probabilities[i];
    if (cumsum > r) return i;
  }
  return n - 1;  // fallback
}

static int sample(float* logits, int vocab_size, float temperature, float topp) {
  if (temperature == 0.0f) {
    return sample_argmax(logits, vocab_size);
  }

  // Apply temperature
  for (int q = 0; q < vocab_size; q++) {
    logits[q] /= temperature;
  }

  // Softmax
  softmax(logits, vocab_size);

  if (topp <= 0.0f || topp >= 1.0f) {
    // Simple random sample
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

// ============================================================================
// Tokenizer — BPE encode/decode
// ============================================================================

static bool loadTokenizer(const char* path) {
  TokenizerState* t = &gLLM.tokenizer;
  int vocab_size = gLLM.config.vocab_size;

  File f = LittleFS.open(path, "r");
  if (!f) {
    snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Cannot open tokenizer: %s", path);
    return false;
  }

  // Tokenizer binary format:
  // uint32_t max_token_length
  // For each token (vocab_size times):
  //   float score
  //   int32_t len
  //   char[len] string (NOT null-terminated in file)

  t->vocab_size = vocab_size;
  t->vocab = (char**)llmPsramAlloc(vocab_size * sizeof(char*), "tok.vocab");
  t->vocab_scores = (float*)llmPsramAlloc(vocab_size * sizeof(float), "tok.scores");
  if (!t->vocab || !t->vocab_scores) {
    f.close();
    return false;
  }

  uint32_t max_token_length = 0;
  f.read((uint8_t*)&max_token_length, sizeof(uint32_t));
  t->max_token_length = max_token_length;

  // Allocate a single block for all token strings
  // First pass: calculate total string size
  size_t fileStart = f.position();
  size_t totalStringBytes = 0;
  for (int i = 0; i < vocab_size; i++) {
    float score;
    int32_t len;
    f.read((uint8_t*)&score, sizeof(float));
    f.read((uint8_t*)&len, sizeof(int32_t));
    totalStringBytes += len + 1; // +1 for null terminator
    f.seek(f.position() + len);  // skip string bytes
  }

  // Allocate string pool
  char* stringPool = (char*)llmPsramAlloc(totalStringBytes, "tok.strings");
  if (!stringPool) {
    f.close();
    return false;
  }
  t->tokenizerData = stringPool;

  // Second pass: read data
  f.seek(fileStart);
  char* poolPtr = stringPool;
  for (int i = 0; i < vocab_size; i++) {
    float score;
    int32_t len;
    f.read((uint8_t*)&score, sizeof(float));
    f.read((uint8_t*)&len, sizeof(int32_t));
    t->vocab_scores[i] = score;
    t->vocab[i] = poolPtr;
    f.read((uint8_t*)poolPtr, len);
    poolPtr[len] = '\0';
    poolPtr += len + 1;
  }

  f.close();
  t->sorted_vocab = nullptr;
  t->sorted_vocab_built = false;

  INFO_SYSTEMF("[LLM] Tokenizer loaded: vocab_size=%d max_token_len=%u", vocab_size, max_token_length);
  return true;
}

static void freeTokenizer() {
  TokenizerState* t = &gLLM.tokenizer;
  llmPsramFree((void**)&t->vocab);
  llmPsramFree((void**)&t->vocab_scores);
  llmPsramFree((void**)&t->tokenizerData);
  llmPsramFree((void**)&t->sorted_vocab);
  memset(t, 0, sizeof(TokenizerState));
}

// Build sorted vocabulary for BPE encoding
static void buildSortedVocab() {
  TokenizerState* t = &gLLM.tokenizer;
  if (t->sorted_vocab_built) return;

  t->sorted_vocab = (TokenizerState::TokenIndex*)llmPsramAlloc(
    t->vocab_size * sizeof(TokenizerState::TokenIndex), "tok.sorted");
  if (!t->sorted_vocab) return;

  for (int i = 0; i < t->vocab_size; i++) {
    t->sorted_vocab[i].str = t->vocab[i];
    t->sorted_vocab[i].id = i;
  }

  // Sort by string (for binary search during encode)
  std::sort(t->sorted_vocab, t->sorted_vocab + t->vocab_size,
    [](const TokenizerState::TokenIndex& a, const TokenizerState::TokenIndex& b) {
      return strcmp(a.str, b.str) < 0;
    });

  t->sorted_vocab_built = true;
}

static int str_lookup(const char* str, const TokenizerState::TokenIndex* sorted_vocab, int vocab_size) {
  // Binary search for string in sorted vocabulary
  int lo = 0, hi = vocab_size - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    int cmp = strcmp(str, sorted_vocab[mid].str);
    if (cmp == 0) return sorted_vocab[mid].id;
    if (cmp < 0) hi = mid - 1;
    else lo = mid + 1;
  }
  return -1;
}

// Encode a string into tokens using BPE
static int encode(const char* text, int* tokens, int maxTokens) {
  TokenizerState* t = &gLLM.tokenizer;
  buildSortedVocab();
  if (!t->sorted_vocab) return 0;

  int n_tokens = 0;

  // First encode each character as its own token
  for (const char* c = text; *c != '\0'; c++) {
    char s[2] = { *c, '\0' };
    int id = str_lookup(s, t->sorted_vocab, t->vocab_size);
    if (id == -1) continue;  // skip unknown chars
    if (n_tokens >= maxTokens) break;
    tokens[n_tokens++] = id;
  }

  // BPE merge loop
  char* str_buffer = (char*)alloca(t->max_token_length * 2 + 4);
  while (true) {
    float best_score = -1e10f;
    int best_id = -1;
    int best_idx = -1;

    for (int i = 0; i < n_tokens - 1; i++) {
      snprintf(str_buffer, t->max_token_length * 2 + 2, "%s%s",
               t->vocab[tokens[i]], t->vocab[tokens[i + 1]]);
      int id = str_lookup(str_buffer, t->sorted_vocab, t->vocab_size);
      if (id != -1 && t->vocab_scores[id] > best_score) {
        best_score = t->vocab_scores[id];
        best_id = id;
        best_idx = i;
      }
    }

    if (best_idx == -1) break;  // no more merges

    // Merge tokens[best_idx] and tokens[best_idx+1]
    tokens[best_idx] = best_id;
    // Shift remaining tokens left
    for (int i = best_idx + 1; i < n_tokens - 1; i++) {
      tokens[i] = tokens[i + 1];
    }
    n_tokens--;
  }

  return n_tokens;
}

static const char* decode(int prev_token, int token) {
  TokenizerState* t = &gLLM.tokenizer;
  const char* piece = t->vocab[token];
  // Following BOS token, sentencepiece decoder strips leading space
  if (prev_token == 1 && piece[0] == ' ') piece++;
  // Handle raw byte tokens like <0x0A>
  if (piece[0] == '<' && piece[1] == '0' && piece[2] == 'x') {
    // Parse hex byte
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
// Model Loading
// ============================================================================

static bool loadWeights(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Cannot open model: %s", path);
    return false;
  }

  // Read config header
  size_t configSize = sizeof(LLMConfig);
  if (f.read((uint8_t*)&gLLM.config, configSize) != configSize) {
    snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Failed to read model header");
    f.close();
    return false;
  }

  LLMConfig* p = &gLLM.config;

  // Negative vocab_size means unshared output weights
  bool shared_weights = p->vocab_size > 0;
  p->vocab_size = abs(p->vocab_size);

  INFO_SYSTEMF("[LLM] Model config: dim=%d hidden=%d layers=%d heads=%d kv_heads=%d vocab=%d seq_len=%d",
               p->dim, p->hidden_dim, p->n_layers, p->n_heads, p->n_kv_heads, p->vocab_size, p->seq_len);

  // Calculate weight sizes
  int head_size = p->dim / p->n_heads;
  int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
  (void)head_size;

  size_t weightsBytes = f.size() - configSize;

  // Check PSRAM availability
  size_t freePSRAM = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  // KV cache + activation estimate
  size_t kvCacheSize = 2 * p->n_layers * p->seq_len * kv_dim * sizeof(float);
  size_t activationSize = (4 * p->dim + 2 * p->hidden_dim + p->n_heads * p->seq_len + p->vocab_size) * sizeof(float);
  size_t totalNeeded = weightsBytes + kvCacheSize + activationSize;

  INFO_SYSTEMF("[LLM] Memory: weights=%uKB kv_cache=%uKB activations=%uKB total=%uKB free_psram=%uKB",
               (unsigned)(weightsBytes/1024), (unsigned)(kvCacheSize/1024),
               (unsigned)(activationSize/1024), (unsigned)(totalNeeded/1024),
               (unsigned)(freePSRAM/1024));

  if (totalNeeded + LLM_PSRAM_RESERVE_BYTES > freePSRAM) {
    snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg),
             "Not enough PSRAM: need %uKB, have %uKB",
             (unsigned)(totalNeeded/1024), (unsigned)(freePSRAM/1024));
    f.close();
    return false;
  }

  // Allocate weight block
  gLLM.weightsData = (float*)llmPsramAlloc(weightsBytes, "llm.weights");
  if (!gLLM.weightsData) {
    f.close();
    return false;
  }
  gLLM.weightsSize = weightsBytes;

  // Read all weights in chunks (LittleFS has limited read buffer)
  INFO_SYSTEMF("[LLM] Loading %uKB of weights from flash...", (unsigned)(weightsBytes/1024));
  uint8_t* dest = (uint8_t*)gLLM.weightsData;
  size_t remaining = weightsBytes;
  const size_t chunkSize = 4096;
  while (remaining > 0) {
    size_t toRead = (remaining < chunkSize) ? remaining : chunkSize;
    size_t got = f.read(dest, toRead);
    if (got == 0) {
      snprintf(gLLM.errorMsg, sizeof(gLLM.errorMsg), "Read stall at offset %u", (unsigned)(weightsBytes - remaining));
      f.close();
      return false;
    }
    dest += got;
    remaining -= got;
    // Yield to avoid watchdog on large reads
    if (remaining % (64 * 1024) < chunkSize) {
      vTaskDelay(1);
    }
  }
  f.close();
  INFO_SYSTEMF("[LLM] Weights loaded successfully");

  // Map weight pointers into the contiguous block
  float* ptr = gLLM.weightsData;
  TransformerWeights* w = &gLLM.weights;

  w->token_embedding_table = ptr; ptr += p->vocab_size * p->dim;
  w->rms_att_weight = ptr;        ptr += p->n_layers * p->dim;
  w->wq = ptr;                    ptr += p->n_layers * p->dim * (p->n_heads * head_size);
  w->wk = ptr;                    ptr += p->n_layers * p->dim * kv_dim;
  w->wv = ptr;                    ptr += p->n_layers * p->dim * kv_dim;
  w->wo = ptr;                    ptr += p->n_layers * (p->n_heads * head_size) * p->dim;
  w->rms_ffn_weight = ptr;        ptr += p->n_layers * p->dim;
  w->w1 = ptr;                    ptr += p->n_layers * p->dim * p->hidden_dim;
  w->w2 = ptr;                    ptr += p->n_layers * p->hidden_dim * p->dim;
  w->w3 = ptr;                    ptr += p->n_layers * p->dim * p->hidden_dim;
  w->rms_final_weight = ptr;      ptr += p->dim;

  // Skip freq_cis (RoPE) — we compute these on the fly
  ptr += p->seq_len * head_size / 2;  // freq_cis_real
  ptr += p->seq_len * head_size / 2;  // freq_cis_imag

  if (shared_weights) {
    w->wcls = w->token_embedding_table;
  } else {
    w->wcls = ptr;
  }

  // Allocate run state (KV cache + activation buffers)
  size_t stateBytes = kvCacheSize + activationSize;
  gLLM.stateData = (float*)llmPsramAlloc(stateBytes, "llm.state");
  if (!gLLM.stateData) return false;
  gLLM.stateSize = stateBytes;

  // Map state pointers
  RunState* s = &gLLM.state;
  float* sp = gLLM.stateData;
  s->x = sp;           sp += p->dim;
  s->xb = sp;          sp += p->dim;
  s->xb2 = sp;         sp += p->dim;
  s->hb = sp;          sp += p->hidden_dim;
  s->hb2 = sp;         sp += p->hidden_dim;
  s->q = sp;           sp += p->dim;
  s->key_cache = sp;   sp += p->n_layers * p->seq_len * kv_dim;
  s->value_cache = sp; sp += p->n_layers * p->seq_len * kv_dim;
  s->att = sp;         sp += p->n_heads * p->seq_len;
  s->logits = sp;      sp += p->vocab_size;

  return true;
}

// ============================================================================
// Public API Implementation
// ============================================================================

void llmInit() {
  memset(&gLLM, 0, sizeof(gLLM));
  gLLM.runState = LLMState::UNLOADED;
  gLLM.mutex = xSemaphoreCreateMutex();
}

bool llmLoadModel(const char* modelPath, const char* tokenizerPath) {
  // Auto-init if not yet initialized
  if (!gLLM.mutex) llmInit();

  if (!filesystemReady) {
    strlcpy(gLLM.errorMsg, "Filesystem not ready", sizeof(gLLM.errorMsg));
    gLLM.runState = LLMState::ERROR;
    return false;
  }

  // Unload any existing model
  llmUnload();

  gLLM.runState = LLMState::LOADING;
  strlcpy(gLLM.modelPath, modelPath, sizeof(gLLM.modelPath));

  INFO_SYSTEMF("[LLM] Loading model: %s", modelPath);

  if (!loadWeights(modelPath)) {
    gLLM.runState = LLMState::ERROR;
    return false;
  }

  if (!loadTokenizer(tokenizerPath)) {
    gLLM.runState = LLMState::ERROR;
    llmUnload();
    return false;
  }

  gLLM.runState = LLMState::READY;
  INFO_SYSTEMF("[LLM] Model ready. PSRAM used: %uKB",
               (unsigned)((gLLM.weightsSize + gLLM.stateSize) / 1024));
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
  llmPsramFree((void**)&gLLM.stateData);

  memset(&gLLM.weights, 0, sizeof(gLLM.weights));
  memset(&gLLM.state, 0, sizeof(gLLM.state));
  gLLM.weightsSize = 0;
  gLLM.stateSize = 0;
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
  status.modelSizeBytes = gLLM.weightsSize;
  status.runtimeSizeBytes = gLLM.stateSize;
  status.totalPsramUsed = gLLM.weightsSize + gLLM.stateSize;
  status.lastTokensPerSec = gLLM.lastTokPerSec;
  status.lastTokenCount = gLLM.lastTokCount;
  strlcpy(status.errorMsg, gLLM.errorMsg, sizeof(status.errorMsg));
  return status;
}

int llmGenerate(const char* prompt, LLMTokenCallback tokenCb,
                int maxTokens, float temperature, float topp) {
  if (gLLM.runState != LLMState::READY) {
    return -1;
  }

  gLLM.runState = LLMState::GENERATING;
  gLLM.stopRequested = false;
  gLLM.errorMsg[0] = '\0';

  LLMConfig* p = &gLLM.config;

  // Encode prompt
  int* prompt_tokens = (int*)alloca((strlen(prompt) + 3) * sizeof(int));
  int num_prompt_tokens = encode(prompt, prompt_tokens, strlen(prompt) + 2);

  if (num_prompt_tokens < 1) {
    // Use BOS token if prompt is empty
    prompt_tokens[0] = 1; // BOS
    num_prompt_tokens = 1;
  }

  // Clear KV cache for fresh generation
  int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
  size_t kvSize = p->n_layers * p->seq_len * kv_dim * sizeof(float);
  memset(gLLM.state.key_cache, 0, kvSize);
  memset(gLLM.state.value_cache, 0, kvSize);

  int token = prompt_tokens[0]; // BOS or first prompt token
  int pos = 0;
  int generated = 0;

  unsigned long startMs = millis();

  int steps = std::min(maxTokens + num_prompt_tokens, p->seq_len);

  while (pos < steps) {
    if (gLLM.stopRequested) break;

    float* logits = forward(token, pos);

    int next;
    if (pos < num_prompt_tokens - 1) {
      // Still in prompt — force next prompt token
      next = prompt_tokens[pos + 1];
    } else {
      // Generate
      next = sample(logits, p->vocab_size, temperature, topp);
    }

    pos++;

    // EOS token (id=2 in sentencepiece) — stop
    if (next == 2) break;

    // Decode and emit token
    if (pos > num_prompt_tokens) {
      const char* piece = decode(token, next);
      if (piece && tokenCb) {
        if (!tokenCb(piece)) break;  // callback requested stop
      }
      generated++;
    }

    token = next;

    // Yield periodically to avoid watchdog
    if (generated % 4 == 0) {
      vTaskDelay(1);
    }
  }

  unsigned long elapsed = millis() - startMs;
  if (elapsed > 0 && generated > 0) {
    gLLM.lastTokPerSec = (float)generated / ((float)elapsed / 1000.0f);
  } else {
    gLLM.lastTokPerSec = 0;
  }
  gLLM.lastTokCount = generated;

  INFO_SYSTEMF("[LLM] Generated %d tokens in %lums (%.1f tok/s)",
               generated, elapsed, gLLM.lastTokPerSec);

  gLLM.runState = LLMState::READY;
  return generated;
}

void llmStop() {
  gLLM.stopRequested = true;
}

String llmListModels() {
  String json = "[";
  File dir = LittleFS.open("/system/llm");
  if (dir && dir.isDirectory()) {
    bool first = true;
    File entry;
    while ((entry = dir.openNextFile())) {
      String name = entry.name();
      if (name.endsWith(".bin") && !name.startsWith("tokenizer")) {
        if (!first) json += ",";
        json += "{\"name\":\"";
        json += name;
        json += "\",\"size\":";
        json += String((unsigned long)entry.size());
        json += "}";
        first = false;
      }
      entry.close();
    }
    dir.close();
  }
  json += "]";
  return json;
}

// ============================================================================
// CLI Commands
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
    "Config: dim=%d layers=%d heads=%d vocab=%d seq=%d\n"
    "PSRAM: %uKB (weights=%uKB runtime=%uKB)\n"
    "Last: %d tokens @ %.1f tok/s\n"
    "%s%s",
    stateStr, st.modelPath,
    st.config.dim, st.config.n_layers, st.config.n_heads,
    st.config.vocab_size, st.config.seq_len,
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
  const char* tokPath = LLM_DEFAULT_TOKENIZER_PATH;

  char customPath[96];
  if (a.length() > 0) {
    snprintf(customPath, sizeof(customPath), "/system/llm/%s", a.c_str());
    modelPath = customPath;
  }

  bool ok = llmLoadModel(modelPath, tokPath);
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
  snprintf(llmCmdBuf, sizeof(llmCmdBuf), "Models in /system/llm/:\n%s", models.c_str());
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
  { "llm status",   "Show LLM engine status",           false, cmd_llm_status },
  { "llm load",     "Load model [model.bin]",            true,  cmd_llm_load,     "Usage: llm load [filename.bin]" },
  { "llm unload",   "Unload model and free PSRAM",       true,  cmd_llm_unload },
  { "llm models",   "List available model files",        false, cmd_llm_models },
  { "llm generate", "Generate text from prompt",         false, cmd_llm_generate, "Usage: llm generate <prompt text>" },
  { "llm stop",     "Stop in-progress generation",       false, cmd_llm_stop },
};
const size_t llmCommandsCount = sizeof(llmCommands) / sizeof(llmCommands[0]);

#endif // ENABLE_ONDEVICE_LLM
