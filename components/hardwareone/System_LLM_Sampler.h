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
// If outChosenProb != nullptr, it receives the post-softmax probability of the
// chosen token (Phase 2 confidence signal), or -1.0f when no signal is available
// (temperature==0 greedy path computes no softmax).
int sample(float* logits, int vocab_size, float temperature, float topp, float minp,
           float* outChosenProb = nullptr);
