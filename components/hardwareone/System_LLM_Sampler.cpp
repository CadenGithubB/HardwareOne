/**
 * System_LLM_Sampler.cpp - Token sampling for the LLM engine.
 *
 * Extracted verbatim from System_LLM.cpp (no behavioral change): argmax, top-p
 * (nucleus), temperature/categorical, and Mirostat v2. Operates in place on the
 * logits buffer and reuses gLLM.sampleIndices to avoid per-token allocation.
 */
#include "System_BuildConfig.h"
#if ENABLE_ONDEVICE_LLM

#include "System_LLM_Sampler.h"
#include "System_LLM_Internal.h"   // gLLM.sampleIndices / sampleIndicesSize
#include "System_LLM_Kernels.h"    // softmax
#include "System_Debug.h"          // DEBUG_LLM_GENERATEF
#include <cmath>
#include <cstdint>
#include "esp_random.h"

// Clamp logits before temperature scaling to prevent saturation from INT8
// accumulation errors or extreme activations compounding into ±Inf after div.
static constexpr float LOGIT_CLAMP_MAX =  50.0f;
static constexpr float LOGIT_CLAMP_MIN = -50.0f;

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
  // Uses pre-allocated gLLM.sampleIndices buffer (allocated once at model load)
  // to avoid malloc/free churn every token, which fragments the heap.

  int* indices = gLLM.sampleIndices;
  if (!indices || gLLM.sampleIndicesSize < n) {
    // Fallback: plain categorical sampling (no allocation needed)
    DEBUG_LLM_GENERATEF("[LLM] sample_topp: no index buffer, falling back to categorical");
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

  return result;
}

int sample(float* logits, int vocab_size, float temperature, float topp) {
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
int sample_mirostat2(float* logits, int n, float temperature, float tau, float eta, float* mu) {
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

#endif // ENABLE_ONDEVICE_LLM
