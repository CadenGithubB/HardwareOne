/**
 * System_LLM_Sampler.cpp - Token sampling for the LLM engine.
 *
 * Sampling primitives: argmax (greedy), top-p (nucleus), min-p, and the
 * temperature/categorical dispatcher. Operates in place on the logits buffer
 * and reuses gLLM.sampleIndices to avoid per-token allocation.
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

static int sample_topp(float* probabilities, int n, float topp, float* outChosenProb = nullptr) {
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
      if (cdf > r) { if (outChosenProb) *outChosenProb = probabilities[i]; return i; }
    }
    if (outChosenProb) *outChosenProb = probabilities[n - 1];
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
  float chosen_prob = probabilities[nucleus_n - 1];
  for (int i = 0; i < nucleus_n; i++) {
    cdf += probabilities[i];
    if (cdf > r) { result = indices[i]; result_rank = i; chosen_prob = probabilities[i]; break; }
  }
  if (outChosenProb) *outChosenProb = chosen_prob;

  DEBUG_LLM_GENERATEF("[LLM]   sampled tok=%d at rank=%d/%d (r=%.4f)",
                      result, result_rank, nucleus_n, r / cumsum);

  return result;
}

int sample(float* logits, int vocab_size, float temperature, float topp, float minp,
           float* outChosenProb) {
  if (outChosenProb) *outChosenProb = -1.0f;  // default: no signal
  if (temperature == 0.0f) {
    // Greedy: no softmax computed, so no confidence signal (left at -1.0f).
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

  // Min-p: when active (minp>0) keep only tokens with prob >= minp * p_max — a
  // relative floor that adapts to the model's confidence (tight when it's sure,
  // looser when it isn't). Replaces top-p for this token. logits[] holds
  // probabilities here. minp==0 falls through to the existing top-p path, so this
  // is a clean A/B toggle.
  if (minp > 0.0f) {
    float thresh = minp * max_prob;
    float mass = 0.0f;
    for (int q = 0; q < vocab_size; q++) {
      if (logits[q] < thresh) logits[q] = 0.0f; else mass += logits[q];
    }
    DEBUG_LLM_GENERATEF("[LLM] min-p: floor=%.4f (minp=%.2f x pmax=%.3f) kept_mass=%.3f",
                        thresh, minp, max_prob, mass);
    float r = ((float)esp_random() / (float)UINT32_MAX) * mass;
    float cdf = 0.0f;
    for (int q = 0; q < vocab_size; q++) {
      if (logits[q] > 0.0f) { cdf += logits[q]; if (cdf > r) { if (outChosenProb) *outChosenProb = logits[q]; return q; } }
    }
    if (outChosenProb) *outChosenProb = max_prob;  // float-rounding fallback
    return max_prob_id;  // float-rounding fallback: the most likely token
  }

  if (topp <= 0.0f || topp >= 1.0f) {
    // Simple random sample (no top-p filtering)
    //DEBUG_LLM_GENERATEF("[LLM] sample: categorical (topp=%.2f, no nucleus filter)", topp);
    float r = (float)esp_random() / (float)UINT32_MAX;
    float cdf = 0.0f;
    for (int i = 0; i < vocab_size; i++) {
      cdf += logits[i];
      if (cdf > r) { if (outChosenProb) *outChosenProb = logits[i]; return i; }
    }
    if (outChosenProb) *outChosenProb = logits[vocab_size - 1];
    return vocab_size - 1;
  }

  return sample_topp(logits, vocab_size, topp, outChosenProb);
}

#endif // ENABLE_ONDEVICE_LLM
