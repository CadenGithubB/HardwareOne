/**
 * System_LLM_Kernels.h - Math kernels for the on-device LLM forward pass.
 *
 * Extracted from System_LLM.cpp with no behavioral change. These are pure
 * functions over explicit buffers — they hold no engine global state — so they
 * form the compute "seam": the single place a target-specific (S3/P4) or SIMD
 * weight-matmul backend would be swapped in.
 */
#pragma once

#include <cstddef>
#include <cstdint>

// Number of quant scale groups for n_elements at the given group_size.
// Inline so both the kernels and the model loader's memory-layout math share
// one definition.
static inline size_t scaleCount(size_t n_elements, int group_size) {
  return ((size_t)n_elements + group_size - 1) / group_size;
}

// Normalization (RMSNorm for Llama, LayerNorm — optional bias — for GPT-2).
void rmsnorm(float* o, const float* x, const float* weight, int size);
void layernorm(float* o, const float* x, const float* weight, const float* bias, int size);

// In-place softmax over the first `size` elements.
void softmax(float* x, int size);

// Quant-aware matrix-vector dispatch: w(d,n) @ x(n,) -> xout(d,).
// Exactly one weight representation is non-null and selects the path:
//   fp  -> FP32        i8/sc -> INT8 (per-group scales)   q4/q4_sc -> INT4 packed
// gs = quant group_size. Large matmuls (d > threshold) yield internally so a
// single op can't starve IDLE1 past the task-WDT window.
void wmatmul(float* xout, const float* x,
             const float* fp, const int8_t* i8, const float* sc,
             const uint8_t* q4, const float* q4_sc,
             int gs, int n, int d);

// A weight matrix packaged for linear(): exactly one of {fp, i8, q4} is set,
// with offsets already resolved. Built once at model load (buildLayerTensors)
// so forward() doesn't recompute per-layer offsets on every token.
struct QuantTensor {
  const float*   fp;     // FP32 weights, or null
  const int8_t*  i8;     // INT8 data, or null
  const float*   sc;     // INT8 per-group scales, or null
  const uint8_t* q4;     // INT4 packed data, or null
  const float*   q4_sc;  // INT4 per-group scales, or null
  int gs;                // quant group_size
  int n, d;              // w(d,n) @ x(n,) -> out(d,)
};

// Matrix-vector product with a prepackaged weight. Identical to the wmatmul
// call it forwards to — same dispatch, offsets resolved at load instead of
// per token.
inline void linear(float* out, const float* x, const QuantTensor& w) {
  wmatmul(out, x, w.fp, w.i8, w.sc, w.q4, w.q4_sc, w.gs, w.n, w.d);
}
