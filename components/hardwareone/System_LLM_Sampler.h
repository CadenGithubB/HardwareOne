/**
 * System_LLM_Sampler.h - Token sampling for the LLM engine.
 *
 * Extracted from System_LLM.cpp with no behavioral change. Operates in-place on
 * a logits buffer (temperature scaling + softmax applied internally) and uses
 * the shared gLLM.sampleIndices scratch for top-p. Pure given that scratch.
 */
#pragma once

// Categorical / top-p (nucleus) sampling. Returns the chosen token id.
// `logits` is modified in place. temperature==0 → greedy argmax.
int sample(float* logits, int vocab_size, float temperature, float topp);

// Mirostat v2 adaptive-surprise sampling. `mu` is persistent state across the
// tokens of one generation (init to 2*tau). `logits` modified in place.
int sample_mirostat2(float* logits, int n, float temperature, float tau, float eta, float* mu);
