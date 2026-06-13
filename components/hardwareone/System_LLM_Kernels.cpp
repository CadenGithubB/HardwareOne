/**
 * System_LLM_Kernels.cpp - Math kernels for the on-device LLM forward pass.
 *
 * Extracted verbatim from System_LLM.cpp (no behavioral change). Pure functions
 * over explicit buffers — no engine global state — forming the compute seam for
 * a future target-specific or SIMD weight-matmul backend.
 *
 * Forward pass based on Andrej Karpathy's llama2.c. Weights stay quantized in
 * PSRAM; INT8/INT4 are dequantized inline during the matmul (no FP32 expansion).
 */
#include "System_BuildConfig.h"
#if ENABLE_ONDEVICE_LLM

#include "System_LLM_Kernels.h"
#include <cmath>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dsps_dotprod.h"   // dsps_dotprod_f32 — esp-dsp S3 SIMD

// Inside a single matmul, yield to IDLE/cmd tasks every N output rows so one
// large op (e.g. the classifier, dim×vocab) can't starve IDLE1 past the 5 s
// task-WDT window. Only matmuls with d > this threshold yield, so the many
// small per-layer matmuls (d == dim) complete in one shot and pay no overhead.
static constexpr int LLM_MATMUL_YIELD_ROWS = 512;

// ============================================================================
// Normalization
// ============================================================================

void rmsnorm(float* o, const float* x, const float* weight, int size) {
  float ss = 0.0f;
  for (int j = 0; j < size; j++) ss += x[j] * x[j];
  ss /= size;
  ss += 1e-5f;
  ss = 1.0f / sqrtf(ss);
  for (int j = 0; j < size; j++) o[j] = weight[j] * (ss * x[j]);
}

// LayerNorm with optional bias (bias may be NULL for v1 models)
void layernorm(float* o, const float* x, const float* weight, const float* bias, int size) {
  float mean = 0.0f;
  for (int j = 0; j < size; j++) mean += x[j];
  mean /= size;
  float var = 0.0f;
  for (int j = 0; j < size; j++) { float d = x[j] - mean; var += d * d; }
  var /= size;
  float s = 1.0f / sqrtf(var + 1e-5f);
  if (bias) {
    for (int j = 0; j < size; j++) o[j] = weight[j] * ((x[j] - mean) * s) + bias[j];
  } else {
    for (int j = 0; j < size; j++) o[j] = weight[j] * ((x[j] - mean) * s);
  }
}

void softmax(float* x, int size) {
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

// ============================================================================
// Matrix-vector products (quant-aware)
// ============================================================================

static void matmul(float* xout, const float* x, const float* w, int n, int d) {
  // w(d,n) @ x(n,) -> xout(d,)
  // Use esp-dsp dot product for SIMD acceleration on S3
  for (int i = 0; i < d; i++) {
    float val = 0.0f;
    dsps_dotprod_f32(x, w + i * n, &val, n);
    xout[i] = val;
  }
}

// Fused INT8 dequantize + matmul: w(d,n) @ x(n,) -> xout(d,)
// Scales are stored in flat (row-major) quantization order: the scale for
// element at flat index k is scales[k / group_size].
//
// Fast path (common case, n % group_size == 0): precomputes per-row scale
// pointer and iterates over groups with a scalar multiply pulled out of the
// inner loop, avoiding an integer division per element.
static void matmul_q8(float* xout, const float* x, const int8_t* w,
                      const float* scales, int group_size, int n, int d) {
  const int n_groups = (n + group_size - 1) / group_size;
  const bool aligned = (n_groups * group_size == n);  // n % group_size == 0

  if (aligned) {
    // Fast path: group boundaries fall on exact element boundaries.
    // The scale for row i, group g is at scales[i * n_groups + g].
    for (int i = 0; i < d; i++) {
      if (d > LLM_MATMUL_YIELD_ROWS && i > 0 && (i % LLM_MATMUL_YIELD_ROWS) == 0) vTaskDelay(1);
      const int8_t* row      = w      + (size_t)i * n;
      const float*  row_sc   = scales + (size_t)i * n_groups;
      float val = 0.0f;
      for (int g = 0; g < n_groups; g++) {
        const int8_t* rg    = row + g * group_size;
        const float*  xg    = x   + g * group_size;
        // Accumulate int8·activation products in float, then apply the per-group
        // scale once — one fewer multiply per element than scaling inline.
        float gsum = 0.0f;
        for (int j = 0; j < group_size; j++) {
          gsum += (float)rg[j] * xg[j];
        }
        val += gsum * row_sc[g];
      }
      xout[i] = val;
    }
  } else {
    // General fallback (unusual: n not a multiple of group_size)
    for (int i = 0; i < d; i++) {
      if (d > LLM_MATMUL_YIELD_ROWS && i > 0 && (i % LLM_MATMUL_YIELD_ROWS) == 0) vTaskDelay(1);
      const int8_t* row      = w + (size_t)i * n;
      const size_t  row_base = (size_t)i * n;
      float val = 0.0f;
      for (int j = 0; j < n; j++) {
        val += ((float)row[j] * scales[(row_base + j) / group_size]) * x[j];
      }
      xout[i] = val;
    }
  }
}

// Fused INT4 dequantize + matmul: w_packed(d, ceil(n/2) bytes) @ x(n,) -> xout(d,)
// Nibble packing: low nibble (bits 3:0) = even index, high nibble (bits 7:4) = odd index.
// Signed 4-bit range [-8, 7].  Scales are identical layout to INT8 (per-group FP32).
static void matmul_q4(float* xout, const float* x, const uint8_t* w_packed,
                      const float* scales, int group_size, int n, int d) {
  const int n_groups        = (n + group_size - 1) / group_size;
  const int row_packed_bytes = (n + 1) / 2;

  for (int i = 0; i < d; i++) {
    if (d > LLM_MATMUL_YIELD_ROWS && i > 0 && (i % LLM_MATMUL_YIELD_ROWS) == 0) vTaskDelay(1);
    const uint8_t* row    = w_packed + (size_t)i * row_packed_bytes;
    const float*   row_sc = scales   + (size_t)i * n_groups;
    float val = 0.0f;

    for (int g = 0; g < n_groups; g++) {
      const float  sc    = row_sc[g];
      const int    start = g * group_size;
      const int    end   = (start + group_size > n) ? n : start + group_size;
      float gsum = 0.0f;
      for (int j = start; j < end; j++) {
        uint8_t byte = row[j >> 1];
        int8_t  w_val = (j & 1) ? (int8_t)(byte) >> 4         // high nibble (odd)
                                : (int8_t)((byte) << 4) >> 4;  // low nibble (even)
        gsum += (float)w_val * x[j];
      }
      val += gsum * sc;
    }
    xout[i] = val;
  }
}

// Dispatch: calls matmul (FP32), matmul_q4 (INT4 packed), or matmul_q8 (INT8).
void wmatmul(float* xout, const float* x,
             const float* fp, const int8_t* i8, const float* sc,
             const uint8_t* q4, const float* q4_sc,
             int gs, int n, int d) {
  if (fp)       matmul(xout, x, fp, n, d);
  else if (q4)  matmul_q4(xout, x, q4, q4_sc, gs, n, d);
  else          matmul_q8(xout, x, i8, sc, gs, n, d);
}

#endif // ENABLE_ONDEVICE_LLM
