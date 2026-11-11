/**
 * System_LLMModel.h - LLM1 model loading.
 *
 * Extracted from System_LLM.cpp with no behavioral change. Parses the LLM1
 * header, validates config, computes the PSRAM memory layout, reads weights
 * (FP32 / INT8 / INT4-mixed), loads the embedded tokenizer, and allocates run
 * state — all into the gLlm runtime singleton.
 */
#pragma once

// Load a model file (header + tokenizer + weights) at `path` into gLlm.
// Returns false on any error (sets gLlm.errorMsg / runState). Includes the
// tokenizer load and run-state allocation.
bool loadWeights(const char* path);
