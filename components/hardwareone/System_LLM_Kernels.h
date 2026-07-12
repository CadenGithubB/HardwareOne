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
#include <cstring>

// ---------------------------------------------------------------------------
// IEEE-754 half <-> float conversion (software; Xtensa LX7 has no FP16 ALU).
// Used by the FP16 KV cache: pack on write, unpack on read. Round-to-nearest;
// KV magnitudes are small and well inside FP16 range, so this is lossless in
// practice for the values it stores. Inline + branch-light for the hot path.
// ---------------------------------------------------------------------------
static inline uint16_t f32_to_f16(float f) {
  uint32_t x; memcpy(&x, &f, sizeof(x));
  uint32_t sign = (x >> 16) & 0x8000u;
  int32_t  exp  = (int32_t)((x >> 23) & 0xff) - 127 + 15;
  uint32_t mant = x & 0x7fffffu;
  if (exp >= 0x1f) return (uint16_t)(sign | 0x7c00u);          // overflow -> inf
  if (exp <= 0) {                                              // subnormal / zero
    if (exp < -10) return (uint16_t)sign;
    mant |= 0x800000u;
    uint32_t shift = (uint32_t)(14 - exp);
    uint32_t m = mant >> shift;
    if ((mant >> (shift - 1)) & 1u) m += 1u;                   // round to nearest
    return (uint16_t)(sign | m);
  }
  uint16_t h = (uint16_t)(sign | ((uint32_t)exp << 10) | (mant >> 13));
  if (mant & 0x1000u) h = (uint16_t)(h + 1);                   // round to nearest
  return h;
}

static inline float f16_to_f32(uint16_t h) {
  uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
  uint32_t exp  = (h >> 10) & 0x1fu;
  uint32_t mant = h & 0x3ffu;
  uint32_t f;
  if (exp == 0) {
    if (mant == 0) { f = sign; }                               // zero
    else {                                                     // subnormal
      exp = 1;
      while (!(mant & 0x400u)) { mant <<= 1; exp++; }
      mant &= 0x3ffu;
      f = sign | ((uint32_t)(127 - 15 - exp + 1) << 23) | (mant << 13);
    }
  } else if (exp == 0x1f) {
    f = sign | 0x7f800000u | (mant << 13);                     // inf / nan
  } else {
    f = sign | ((exp - 15 + 127) << 23) | (mant << 13);
  }
  float out; memcpy(&out, &f, sizeof(out));
  return out;
}

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

// In-place vector add: dst[i] += src[i]. A natural spot for SIMD (dsps) later.
// dst/src are distinct at every call site (verified) — hence __restrict.
void vecAddInPlace(float* __restrict dst, const float* __restrict src, int n);

// Quant-aware matrix-vector dispatch: w(d,n) @ x(n,) -> xout(d,).
// Exactly one weight representation is non-null and selects the path:
//   fp  -> FP32        i8/sc -> INT8 (per-group scales)   q4/q4_sc -> INT4 packed
// gs = quant group_size. Large matmuls (d > threshold) yield internally so a
// single op can't starve IDLE1 past the task-WDT window.
void wmatmul(float* __restrict xout, const float* __restrict x,
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
inline void linear(float* __restrict out, const float* __restrict x, const QuantTensor& w) {
  wmatmul(out, x, w.fp, w.i8, w.sc, w.q4, w.q4_sc, w.gs, w.n, w.d);
}
