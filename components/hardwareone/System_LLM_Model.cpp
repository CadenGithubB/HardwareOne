/**
 * System_LLM_Model.cpp - LLM1 model loading (header parse, validation, memory
 * layout, weight reading, run-state allocation). Extracted verbatim from
 * System_LLM.cpp (no behavioral change).
 */
#include "System_BuildConfig.h"
#if ENABLE_ONDEVICE_LLM

#include "System_LLM_Model.h"
#include "System_LLM_Internal.h"
#include "System_LLM_Kernels.h"     // scaleCount
#include "System_LLM_Tokenizer.h"   // loadTokenizerFromFile
#include "System_Debug.h"
#include "System_Filesystem.h"
#include "System_VFS.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include "esp_heap_caps.h"

// File I/O chunk size for reading tensors from flash (loader-local).
static constexpr size_t READ_CHUNK_SIZE = 4096;

// ============================================================================
// 10. Model Loading (LLM1 format)
// ============================================================================

static bool llmValidationErr(char* err, size_t errLen, const char* msg) {
  if (err && errLen) snprintf(err, errLen, "%s", msg);
  return false;
}

static bool validateLlmConfig(const LLMConfig* p, char* err, size_t errLen) {
  if (p->dim < 32 || p->dim > 4096) {
    return llmValidationErr(err, errLen, "Invalid model: dim out of range");
  }
  if (p->hidden_dim < 32 || p->hidden_dim > 16384) {
    return llmValidationErr(err, errLen, "Invalid model: hidden_dim out of range");
  }
  if (p->n_layers < 1 || p->n_layers > 128) {
    return llmValidationErr(err, errLen, "Invalid model: n_layers out of range");
  }
  if (p->n_heads < 1 || p->n_heads > 128 || p->n_kv_heads < 1 || p->n_kv_heads > 128) {
    return llmValidationErr(err, errLen, "Invalid model: head counts out of range");
  }
  if (p->dim % p->n_heads != 0) {
    return llmValidationErr(err, errLen, "Invalid model: dim not divisible by n_heads");
  }
  if (p->n_heads % p->n_kv_heads != 0) {
    return llmValidationErr(err, errLen, "Invalid model: n_heads not divisible by n_kv_heads");
  }
  if (p->vocab_size < 64 || p->vocab_size > 131072) {
    return llmValidationErr(err, errLen, "Invalid model: vocab_size out of range");
  }
  if (p->seq_len < 1 || p->seq_len > 8192) {
    return llmValidationErr(err, errLen, "Invalid model: seq_len out of range");
  }
  if (p->quant_type > 2) {
    return llmValidationErr(err, errLen, "Invalid model: unknown quant_type (expected 0, 1, or 2)");
  }
  if ((p->quant_type == 1 || p->quant_type == 2) && p->group_size == 0) {
    return llmValidationErr(err, errLen, "Invalid model: quantized model with group_size=0");
  }
  if (p->quant_type == 2 && p->file_version < 3) {
    return llmValidationErr(err, errLen, "Invalid model: INT4_MIXED requires file version >= 3");
  }
  if (p->quant_type == 2 && (p->n_q8_start + p->n_q8_end) > p->n_layers) {
    return llmValidationErr(err, errLen, "Invalid model: n_q8_start + n_q8_end > n_layers");
  }
  if (p->arch_type > 1) {
    return llmValidationErr(err, errLen, "Unsupported model: unknown arch_type (expected 0=Llama or 1=GPT-2)");
  }
  if (p->arch_type == 1 && p->n_kv_heads != p->n_heads) {
    return llmValidationErr(err, errLen, "Invalid GPT-2 model: n_kv_heads must equal n_heads (no GQA)");
  }
  return true;
}

// Read chunked data from LittleFS into a buffer, yielding periodically.
static bool readChunked(File& f, uint8_t* dest, size_t bytes) {
  const size_t chunkSize = READ_CHUNK_SIZE;
  size_t remaining = bytes;
  while (remaining > 0) {
    size_t toRead = (remaining < chunkSize) ? remaining : chunkSize;
    size_t got = f.read(dest, toRead);
    if (got == 0) return false;
    dest += got;
    remaining -= got;
    if (remaining % (64 * 1024) < chunkSize) vTaskDelay(1);
  }
  return true;
}

// Read a single tensor from file into an FP32 destination buffer.
// If the file stores INT8, dequantizes on the fly.
// force_fp32: norm tensors are always FP32 in file regardless of quant_type.
static bool readTensor(File& f, float* dest, uint32_t expected_elements,
                       uint8_t quant_type, uint16_t group_size, bool force_fp32) {
  uint32_t n_elements = 0;
  if (f.read((uint8_t*)&n_elements, 4) != 4) return false;

  if (n_elements != expected_elements) {
    ERROR_LLMF("Tensor size mismatch: got %u, expected %u at offset %u",
               n_elements, expected_elements, (unsigned)(f.position() - 4));
    return false;
  }

  if (quant_type == 0 || force_fp32) {
    // FP32: read directly
    return readChunked(f, (uint8_t*)dest, n_elements * sizeof(float));
  }

  // INT8: read scales, then dequantize int8 values
  uint32_t n_groups = (n_elements + group_size - 1) / group_size;

  // Read scales into temp buffer (heap, not stack — could be large)
  float* scales = (float*)malloc(n_groups * sizeof(float));
  if (!scales) return false;
  if (!readChunked(f, (uint8_t*)scales, n_groups * sizeof(float))) {
    free(scales);
    return false;
  }

  // Read and dequantize one group at a time
  int8_t* tmp = (int8_t*)malloc(group_size);
  if (!tmp) { free(scales); return false; }

  for (uint32_t g = 0; g < n_groups; g++) {
    uint32_t start = g * (uint32_t)group_size;
    uint32_t count = ((n_elements - start) < group_size) ? (n_elements - start) : group_size;
    if (f.read((uint8_t*)tmp, count) != count) {
      free(scales); free(tmp);
      return false;
    }
    float scale = scales[g];
    for (uint32_t i = 0; i < count; i++) {
      dest[start + i] = (float)tmp[i] * scale;
    }
    if (g % 64 == 0) vTaskDelay(1);
  }

  free(scales);
  free(tmp);
  return true;
}

// Read an INT8 tensor directly into pre-allocated int8 data and float scale buffers.
// For use when quant_type==1 and the tensor is NOT a norm (i.e., not force_fp32).
static bool readTensorQ8(File& f, int8_t* dest_data, float* dest_scales,
                          uint32_t expected_elements, uint16_t group_size) {
  uint32_t n_elements = 0;
  if (f.read((uint8_t*)&n_elements, 4) != 4) return false;
  if (n_elements != expected_elements) {
    ERROR_LLMF("Tensor Q8 size mismatch: got %u, expected %u at offset %u",
               n_elements, expected_elements, (unsigned)(f.position() - 4));
    return false;
  }
  uint32_t n_groups = (n_elements + group_size - 1) / group_size;
  if (!readChunked(f, (uint8_t*)dest_scales, n_groups * sizeof(float))) return false;
  if (!readChunked(f, (uint8_t*)dest_data, n_elements)) return false;
  return true;
}

// Read an INT4 nibble-packed tensor directly into pre-allocated packed uint8 + float scale buffers.
// Nibble packing: low nibble = even index, high nibble = odd index.  Signed [-8, 7].
static bool readTensorQ4(File& f, uint8_t* dest_packed, float* dest_scales,
                          uint32_t expected_elements, uint16_t group_size) {
  uint32_t n_elements = 0;
  if (f.read((uint8_t*)&n_elements, 4) != 4) return false;
  if (n_elements != expected_elements) {
    ERROR_LLMF("Tensor Q4 size mismatch: got %u, expected %u at offset %u",
               n_elements, expected_elements, (unsigned)(f.position() - 4));
    return false;
  }
  uint32_t n_groups      = (n_elements + group_size - 1) / group_size;
  uint32_t packed_bytes  = (n_elements + 1) / 2;
  if (!readChunked(f, (uint8_t*)dest_scales, n_groups * sizeof(float))) return false;
  if (!readChunked(f, dest_packed, packed_bytes)) return false;
  return true;
}

// Compute the file size of a tensor block in the LLM1 file.
// tensorQt: per-tensor quant (0=FP32, 1=INT8, 2=INT4).  For VERSION<=2 pass
// the global quant_type; for VERSION=3 pass the per-tensor quant.
static size_t tensorFileSize(uint32_t n_elements, uint8_t tensorQt, uint16_t group_size, bool is_norm) {
  size_t sz = 4; // uint32 element count prefix
  if (tensorQt == 0 || is_norm) {
    sz += (size_t)n_elements * 4;                             // FP32
  } else if (tensorQt == 2) {
    uint32_t n_groups = (n_elements + group_size - 1) / group_size;
    sz += (size_t)n_groups * 4 + ((size_t)n_elements + 1) / 2; // scales + packed nibbles
  } else {
    uint32_t n_groups = (n_elements + group_size - 1) / group_size;
    sz += (size_t)n_groups * 4 + (size_t)n_elements;          // scales + INT8 data
  }
  return sz;
}

// Shared context passed between loadWeights helper functions
struct LoadContext {
  int D, H, L, V, kv_dim;
  uint8_t qt;
  uint16_t gs;
  bool isGPT2, hasNormBias, v3;
  bool shared_weights;  // set by prescanTiedWeights
  int pfx;  // per-tensor prefix byte count (1 for v3, 0 otherwise)
  uint32_t infoLen;  // header offset 24: byte length of the optional info block (0 = none)
  size_t weightsBytes, weightsQ8Bytes;
  size_t weightsQ4Bytes, weightsQ4ScBytes, mixedMetaBytes;
  size_t kvCacheSize, hotSize, coldSize;
};

// Parse the 64-byte LLM1 header and validate the config.
// On success, gLLM.config is populated and ctx derived fields are set.
static bool parseModelHeader(File& f, LoadContext& ctx) {
  uint8_t hdr[64];
  if (f.read(hdr, 64) != 64) {
    setLlmError("Failed to read LLM1 header");
    return false;
  }

  uint32_t magic = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                   ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];
  if (magic != LLM1_MAGIC) {
    setLlmError(
             "Not an LLM1 file (magic=0x%08lX, expected 0x4C4C4D31)", (unsigned long)magic);
    return false;
  }

  uint8_t version = hdr[4];
  if (version < 1 || version > 3) {
    setLlmError("Unsupported LLM1 version: %u (supported: 1-3)", version);
    return false;
  }

  LLMConfig* p = &gLLM.config;
  p->file_version = version;
  p->quant_type = hdr[5];
  memcpy(&p->group_size, &hdr[6], 2);
  uint16_t tmp16;
  memcpy(&tmp16, &hdr[8], 2);  p->dim = tmp16;
  memcpy(&tmp16, &hdr[10], 2); p->hidden_dim = tmp16;
  p->n_layers = hdr[12];
  p->n_heads = hdr[13];
  p->n_kv_heads = hdr[14];
  uint32_t tmp32;
  memcpy(&tmp32, &hdr[15], 4); p->vocab_size = (int)tmp32;
  memcpy(&tmp16, &hdr[19], 2); p->seq_len = tmp16;
  p->arch_type = hdr[21];
  p->n_q8_start = (version >= 3) ? hdr[22] : 0;
  p->n_q8_end   = (version >= 3) ? hdr[23] : 0;
  // Optional info block length lives in the free header pad (offset 24), so it is
  // version-independent: v3 uses bytes 22-23, but 24+ stay reserved. 0 = no block.
  // Existing files zero-fill this region, so they parse as "no info block".
  memcpy(&ctx.infoLen, &hdr[24], 4);

  if (!validateLlmConfig(p, gLLM.errorMsg, sizeof(gLLM.errorMsg)))
    return false;

  // Populate derived context fields
  ctx.D = p->dim;  ctx.H = p->hidden_dim;  ctx.L = p->n_layers;  ctx.V = p->vocab_size;
  ctx.kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
  ctx.qt = p->quant_type;  ctx.gs = p->group_size;
  ctx.isGPT2 = (p->arch_type == 1);
  ctx.hasNormBias = (version >= 2 && ctx.isGPT2);  // GPT-2 LayerNorm biases present in all v2+ models
  ctx.v3 = (version >= 3);
  ctx.pfx = ctx.v3 ? 1 : 0;

  return true;
}

// Pre-scan past all tensor data to read the tied-weights flag, then seek back.
// Sets ctx.shared_weights and returns the tensor data start position.
static bool prescanTiedWeights(File& f, LoadContext& ctx) {
  LLMConfig* p = &gLLM.config;
  const int D = ctx.D, H = ctx.H, L = ctx.L, V = ctx.V;
  const uint8_t qt = ctx.qt;
  const uint16_t gs = ctx.gs;
  const int kv_dim = ctx.kv_dim;
  const int pfx = ctx.pfx;

  size_t tensorDataStart = f.position();

  DEBUG_LLM_LOADF("[LLM] LLM1 v%d model: dim=%d hidden=%d layers=%d heads=%d kv_heads=%d vocab=%d seq=%d quant=%s",
                  p->file_version, D, H, L, p->n_heads, p->n_kv_heads,
                  V, p->seq_len, qt == 2 ? "MIXED(Q4/Q8)" : (qt == 1 ? "INT8" : "FP32"));
  if (qt == 2) {
    DEBUG_LLM_LOADF("[LLM] Mixed policy: first %d + last %d layers INT8, middle %d layers INT4",
                    p->n_q8_start, p->n_q8_end, L - p->n_q8_start - p->n_q8_end);
  }

  size_t tensorDataSize = 0;

  if (qt == 2) {
    const int nQ8s = p->n_q8_start, nQ8e = p->n_q8_end;
    tensorDataSize += pfx + tensorFileSize(V * D, 1, gs, false);
    if (ctx.isGPT2) tensorDataSize += pfx + tensorFileSize(p->seq_len * D, 0, gs, true);
    for (int l = 0; l < L; l++) {
      uint8_t layerQt = (l < nQ8s || l >= L - nQ8e) ? 1 : 2;
      int gateElements = ctx.isGPT2 ? 1 : H * D;
      tensorDataSize += pfx + tensorFileSize(D, 0, gs, true);
      if (ctx.hasNormBias) tensorDataSize += pfx + tensorFileSize(D, 0, gs, true);
      tensorDataSize += pfx + tensorFileSize(D * D, layerQt, gs, false);
      tensorDataSize += pfx + tensorFileSize(D * kv_dim, layerQt, gs, false);
      tensorDataSize += pfx + tensorFileSize(D * kv_dim, layerQt, gs, false);
      tensorDataSize += pfx + tensorFileSize(D * D, layerQt, gs, false);
      tensorDataSize += pfx + tensorFileSize(D, 0, gs, true);
      if (ctx.hasNormBias) tensorDataSize += pfx + tensorFileSize(D, 0, gs, true);
      tensorDataSize += pfx + tensorFileSize(gateElements, ctx.isGPT2 ? 0 : layerQt, gs, ctx.isGPT2);
      tensorDataSize += pfx + tensorFileSize(H * D, layerQt, gs, false);
      tensorDataSize += pfx + tensorFileSize(D * H, layerQt, gs, false);
    }
    tensorDataSize += pfx + tensorFileSize(D, 0, gs, true);
    if (ctx.hasNormBias) tensorDataSize += pfx + tensorFileSize(D, 0, gs, true);
  } else {
    tensorDataSize += tensorFileSize(V * D, qt, gs, false);
    if (ctx.isGPT2) tensorDataSize += tensorFileSize(p->seq_len * D, qt, gs, true);
    int gateElements = ctx.isGPT2 ? 1 : H * D;
    for (int l = 0; l < L; l++) {
      tensorDataSize += tensorFileSize(D, qt, gs, true);
      if (ctx.hasNormBias) tensorDataSize += tensorFileSize(D, qt, gs, true);
      tensorDataSize += tensorFileSize(D * D, qt, gs, false);
      tensorDataSize += tensorFileSize(D * kv_dim, qt, gs, false);
      tensorDataSize += tensorFileSize(D * kv_dim, qt, gs, false);
      tensorDataSize += tensorFileSize(D * D, qt, gs, false);
      tensorDataSize += tensorFileSize(D, qt, gs, true);
      if (ctx.hasNormBias) tensorDataSize += tensorFileSize(D, qt, gs, true);
      tensorDataSize += tensorFileSize(gateElements, qt, gs, ctx.isGPT2);
      tensorDataSize += tensorFileSize(H * D, qt, gs, false);
      tensorDataSize += tensorFileSize(D * H, qt, gs, false);
    }
    tensorDataSize += tensorFileSize(D, qt, gs, true);
    if (ctx.hasNormBias) tensorDataSize += tensorFileSize(D, qt, gs, true);
  }

  f.seek(tensorDataStart + tensorDataSize);
  uint8_t tied_flag = 1;
  f.read(&tied_flag, 1);
  ctx.shared_weights = (tied_flag != 0);
  DEBUG_LLM_LOADF("[LLM] Weights tied=%d", ctx.shared_weights);

  f.seek(tensorDataStart);
  return true;
}

// Compute PSRAM memory requirements for all weight blocks and run state.
// Populates ctx size fields and checks PSRAM budget. Returns false if OOM.
static bool computeMemoryLayout(LoadContext& ctx) {
  LLMConfig* p = &gLLM.config;
  const int D = ctx.D, H = ctx.H, L = ctx.L, V = ctx.V;
  const uint8_t qt = ctx.qt;
  const uint16_t gs = ctx.gs;
  const int kv_dim = ctx.kv_dim;

  // Start with model's seq_len, capped by user request if given
  int seq_ctx = p->seq_len;
  if (gLLM.requestedMaxCtx > 0 && seq_ctx > gLLM.requestedMaxCtx) seq_ctx = gLLM.requestedMaxCtx;

  ctx.weightsBytes = 0;
  ctx.weightsQ8Bytes = 0;
  ctx.weightsQ4Bytes = 0;
  ctx.weightsQ4ScBytes = 0;
  ctx.mixedMetaBytes = 0;

  if (qt == 0) {
    size_t w1Floats = ctx.isGPT2 ? (size_t)L : (size_t)L * H * D;
    size_t weightsFloats = (size_t)V * D
      + (ctx.isGPT2 ? (size_t)p->seq_len * D : 0)
      + (size_t)L * D
      + (size_t)L * D * D
      + (size_t)L * D * kv_dim
      + (size_t)L * D * kv_dim
      + (size_t)L * D * D
      + (size_t)L * D
      + w1Floats
      + (size_t)L * D * H
      + (size_t)L * H * D
      + (size_t)D
      + (ctx.hasNormBias ? (size_t)L * D * 2 + D : 0);
    if (!ctx.shared_weights) weightsFloats += (size_t)V * D;
    ctx.weightsBytes = weightsFloats * sizeof(float);
  } else if (qt == 1) {
    size_t normsFloats = (size_t)L * D + (size_t)L * D + D
      + (ctx.isGPT2 ? (size_t)p->seq_len * D + L : 0)
      + (ctx.hasNormBias ? (size_t)L * D * 2 + D : 0);
    size_t scalesFloats = scaleCount((size_t)V * D, gs)
      + (size_t)L * scaleCount((size_t)D * D, gs)
      + (size_t)L * scaleCount((size_t)D * kv_dim, gs)
      + (size_t)L * scaleCount((size_t)D * kv_dim, gs)
      + (size_t)L * scaleCount((size_t)D * D, gs)
      + (ctx.isGPT2 ? 0 : (size_t)L * scaleCount((size_t)H * D, gs))
      + (size_t)L * scaleCount((size_t)D * H, gs)
      + (size_t)L * scaleCount((size_t)H * D, gs);
    if (!ctx.shared_weights) scalesFloats += scaleCount((size_t)V * D, gs);
    ctx.weightsBytes = (normsFloats + scalesFloats) * sizeof(float);
    ctx.weightsQ8Bytes = (size_t)V * D
      + (size_t)L * D * D
      + (size_t)L * D * kv_dim
      + (size_t)L * D * kv_dim
      + (size_t)L * D * D
      + (ctx.isGPT2 ? 0 : (size_t)L * H * D)
      + (size_t)L * D * H
      + (size_t)L * H * D;
    if (!ctx.shared_weights) ctx.weightsQ8Bytes += (size_t)V * D;
  }

  if (qt == 2) {
    const int nQ8s = p->n_q8_start, nQ8e = p->n_q8_end;
    const int nQ8 = nQ8s + nQ8e, nQ4 = L - nQ8;

    size_t normsFloats = (size_t)L * D + (size_t)L * D + D
      + (ctx.isGPT2 ? (size_t)p->seq_len * D + L : 0)
      + (ctx.hasNormBias ? (size_t)L * D * 2 + D : 0);

    size_t scalesFloats = scaleCount((size_t)V * D, gs)
      + (size_t)L * scaleCount((size_t)D * D, gs)
      + (size_t)L * scaleCount((size_t)D * kv_dim, gs)
      + (size_t)L * scaleCount((size_t)D * kv_dim, gs)
      + (size_t)L * scaleCount((size_t)D * D, gs)
      + (ctx.isGPT2 ? 0 : (size_t)L * scaleCount((size_t)H * D, gs))
      + (size_t)L * scaleCount((size_t)D * H, gs)
      + (size_t)L * scaleCount((size_t)H * D, gs);
    if (!ctx.shared_weights) scalesFloats += scaleCount((size_t)V * D, gs);

    size_t q8ScalesFloats = scaleCount((size_t)V * D, gs)
      + (size_t)nQ8 * scaleCount((size_t)D * D, gs)
      + (size_t)nQ8 * scaleCount((size_t)D * kv_dim, gs)
      + (size_t)nQ8 * scaleCount((size_t)D * kv_dim, gs)
      + (size_t)nQ8 * scaleCount((size_t)D * D, gs)
      + (ctx.isGPT2 ? 0 : (size_t)nQ8 * scaleCount((size_t)H * D, gs))
      + (size_t)nQ8 * scaleCount((size_t)D * H, gs)
      + (size_t)nQ8 * scaleCount((size_t)H * D, gs);
    if (!ctx.shared_weights) q8ScalesFloats += scaleCount((size_t)V * D, gs);

    size_t q4ScalesFloats = scalesFloats - q8ScalesFloats;

    ctx.weightsBytes = (normsFloats + q8ScalesFloats) * sizeof(float);

    size_t perLayerQ8Data = (size_t)D * D + (size_t)D * kv_dim + (size_t)D * kv_dim + (size_t)D * D
      + (ctx.isGPT2 ? 0 : (size_t)H * D) + (size_t)D * H + (size_t)H * D;
    ctx.weightsQ8Bytes = (size_t)V * D + (size_t)nQ8 * perLayerQ8Data;
    if (!ctx.shared_weights) ctx.weightsQ8Bytes += (size_t)V * D;

    size_t perLayerQ4Packed = ((size_t)D * D + 1) / 2 + ((size_t)D * kv_dim + 1) / 2
      + ((size_t)D * kv_dim + 1) / 2 + ((size_t)D * D + 1) / 2
      + (ctx.isGPT2 ? 0 : ((size_t)H * D + 1) / 2)
      + ((size_t)D * H + 1) / 2 + ((size_t)H * D + 1) / 2;
    ctx.weightsQ4Bytes = (size_t)nQ4 * perLayerQ4Packed;
    ctx.weightsQ4ScBytes = q4ScalesFloats * sizeof(float);
    ctx.mixedMetaBytes = (size_t)L * sizeof(uint8_t) + (size_t)L * sizeof(TransformerWeights::Q4LayerOffsets);

    DEBUG_LLM_MEMORYF("[LLM] Mixed Q4/Q8: nQ8=%d nQ4=%d", nQ8, nQ4);
  }

  // Fixed-size memory (weights, not context-dependent)
  size_t fixedBytes = ctx.weightsBytes + ctx.weightsQ8Bytes + ctx.weightsQ4Bytes
                    + ctx.weightsQ4ScBytes + ctx.mixedMetaBytes;
  // Hot activations (x/xb/xb2/q/hb/hb2) plus RoPE scratch:
  // rope_cos[D/2] + rope_sin[D/2] = D floats, rope_inv[head_size].
  int headSize = (p->n_heads > 0) ? D / p->n_heads : D;
  ctx.hotSize = (4 * D + 2 * H) * sizeof(float) + ((size_t)D + headSize) * sizeof(float);
  // Non-FP32 KV needs hot scratch: k_tmp[kv_dim] + v_tmp[kv_dim] + kv_deq[headSize].
  if (gLLM.kvPrecision != KV_FP32)
    ctx.hotSize += ((size_t)2 * kv_dim + headSize) * sizeof(float);
  fixedBytes += ctx.hotSize;

  size_t freePSRAM = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  // Use largest contiguous block as budget basis — total free can be misleading
  // because the cold state (KV cache + logits) is one big contiguous allocation.
  // After weights fragment PSRAM, the largest block shrinks significantly.
  // We use the smaller of (total free) and (weights + largest_block) to be safe:
  // weights can be multiple smaller allocs, but cold state must be contiguous.
  size_t budget = (freePSRAM > LLM_PSRAM_RESERVE_BYTES) ? freePSRAM - LLM_PSRAM_RESERVE_BYTES : 0;
  DEBUG_LLM_MEMORYF("[LLM] PSRAM: total_free=%uKB largest_block=%uKB",
                    (unsigned)(freePSRAM/1024), (unsigned)(largestBlock/1024));

  // Check if even weights alone exceed budget (ctx-independent)
  // coldSize has a fixed V component too
  size_t fixedCold = (size_t)V * sizeof(float);
  if (fixedBytes + fixedCold > budget) {
    setLlmError(
             "Weights too large for PSRAM: need %uKB, have %uKB (short %uKB)",
             (unsigned)((fixedBytes + fixedCold)/1024), (unsigned)(budget/1024),
             (unsigned)((fixedBytes + fixedCold - budget)/1024));
    return false;
  }

  // Auto-fit: reduce context until total fits in PSRAM
  // Per-context-slot cost: KV cache + attention scores
  size_t perCtxSlot = llmKvCacheBytes(gLLM.kvPrecision, L, 1, kv_dim, p->n_kv_heads)  // KV per position
                    + p->n_heads * sizeof(float);        // attention scores per position
  size_t remaining = budget - fixedBytes - fixedCold;
  int maxFitCtx = (int)(remaining / perCtxSlot);
  if (maxFitCtx < 1) maxFitCtx = 1;
  if (seq_ctx > maxFitCtx) {
    DEBUG_LLM_MEMORYF("[LLM] Auto-fit: ctx %d -> %d (PSRAM budget %uKB, weights %uKB, per-slot %u bytes)",
                      seq_ctx, maxFitCtx, (unsigned)(budget/1024), (unsigned)(fixedBytes/1024),
                      (unsigned)perCtxSlot);
    seq_ctx = maxFitCtx;
  }

  gLLM.seq_ctx = seq_ctx;
  if (seq_ctx < p->seq_len) {
    DEBUG_LLM_MEMORYF("[LLM] Context: model seq_len=%d -> runtime ctx=%d", p->seq_len, seq_ctx);
  }

  ctx.kvCacheSize = llmKvCacheBytes(gLLM.kvPrecision, L, seq_ctx, kv_dim, p->n_kv_heads);
  ctx.coldSize = (p->n_heads * seq_ctx + V) * sizeof(float);
  size_t totalNeeded = fixedBytes + ctx.kvCacheSize + ctx.coldSize;

  if (qt == 2) {
    DEBUG_LLM_MEMORYF("[LLM] Memory (MIXED): fp=%uKB q8=%uKB q4=%uKB q4sc=%uKB kv=%uKB act=%uKB total=%uKB free=%uKB",
                      (unsigned)(ctx.weightsBytes/1024), (unsigned)(ctx.weightsQ8Bytes/1024),
                      (unsigned)(ctx.weightsQ4Bytes/1024), (unsigned)(ctx.weightsQ4ScBytes/1024),
                      (unsigned)(ctx.kvCacheSize/1024), (unsigned)((ctx.hotSize + ctx.coldSize)/1024),
                      (unsigned)(totalNeeded/1024), (unsigned)(freePSRAM/1024));
  } else if (qt == 1) {
    DEBUG_LLM_MEMORYF("[LLM] Memory (INT8): fp_block=%uKB q8_block=%uKB kv=%uKB act=%uKB total=%uKB free=%uKB ctx=%d",
                      (unsigned)(ctx.weightsBytes/1024), (unsigned)(ctx.weightsQ8Bytes/1024),
                      (unsigned)(ctx.kvCacheSize/1024), (unsigned)((ctx.hotSize + ctx.coldSize)/1024),
                      (unsigned)(totalNeeded/1024), (unsigned)(freePSRAM/1024), seq_ctx);
  } else {
    DEBUG_LLM_MEMORYF("[LLM] Memory (FP32): weights=%uKB kv=%uKB act=%uKB total=%uKB free=%uKB ctx=%d",
                      (unsigned)(ctx.weightsBytes/1024), (unsigned)(ctx.kvCacheSize/1024),
                      (unsigned)((ctx.hotSize + ctx.coldSize)/1024), (unsigned)(totalNeeded/1024),
                      (unsigned)(freePSRAM/1024), seq_ctx);
  }

  // Final sanity check (should not fail after auto-fit, but be safe)
  if (totalNeeded + LLM_PSRAM_RESERVE_BYTES > freePSRAM) {
    setLlmError(
             "Not enough PSRAM: need %uKB + %uKB reserve = %uKB, have %uKB (short %uKB)",
             (unsigned)(totalNeeded/1024), (unsigned)(LLM_PSRAM_RESERVE_BYTES/1024),
             (unsigned)((totalNeeded + LLM_PSRAM_RESERVE_BYTES)/1024),
             (unsigned)(freePSRAM/1024),
             (unsigned)((totalNeeded + LLM_PSRAM_RESERVE_BYTES - freePSRAM)/1024));
    return false;
  }
  return true;
}

// Log min/max/mean for key tensors — cross-reference with converter PACK_SUMMARY.
static void spotCheckWeights(const LoadContext& ctx) {
  const TransformerWeights* w = &gLLM.weights;
  const LLMConfig* p = &gLLM.config;
  const int D = ctx.D, V = ctx.V;
  const uint16_t gs = ctx.gs;
  const bool isQ8  = (ctx.qt == 1);
  const bool isMix = (ctx.qt == 2);

  auto spotCheck = [](const float* data, int n, const char* name) {
    if (!data || n <= 0) return;
    float vmin = data[0], vmax = data[0], vsum = 0.f;
    int nans = 0;
    for (int i = 0; i < n; i++) {
      float v = data[i];
      if (isnan(v) || isinf(v)) { nans++; continue; }
      if (v < vmin) vmin = v;
      if (v > vmax) vmax = v;
      vsum += v;
    }
    int valid = n - nans;
    float mean = valid > 0 ? vsum / valid : 0.f;
    DEBUG_LLM_LOADF("[LLM] SPOT %s (%d): min=%.6f max=%.6f mean=%.6f nan=%d",
                    name, n, vmin, vmax, mean, nans);
  };

  auto spotCheckQ8 = [&](const int8_t* data, const float* scales, int n, int group_sz, const char* name) {
    if (!data || !scales || n <= 0) return;
    int sample_n = (n < 512) ? n : 512;
    float vmin = 1e30f, vmax = -1e30f, vsum = 0.f;
    int8_t imin = data[0], imax = data[0];
    for (int i = 0; i < sample_n; i++) {
      int8_t raw = data[i];
      if (raw < imin) imin = raw;
      if (raw > imax) imax = raw;
      float sc = scales[i / group_sz];
      float deq = (float)raw * sc;
      if (deq < vmin) vmin = deq;
      if (deq > vmax) vmax = deq;
      vsum += deq;
    }
    float sc0 = scales[0];
    float scLast = scales[(sample_n - 1) / group_sz];
    DEBUG_LLM_LOADF("[LLM] SPOT %s (%d, sampled %d): i8=[%d,%d] sc=[%.6f,%.6f] deq=[%.6f,%.6f] mean=%.6f",
                    name, n, sample_n, imin, imax, sc0, scLast, vmin, vmax, vsum / sample_n);
  };

  if (isQ8 || isMix) spotCheckQ8(w->emb_i8, w->emb_sc, V * D, gs, "embedding");
  else               spotCheck(w->token_embedding_table, V * D, "embedding");

  if (ctx.isGPT2 && w->pos_embedding_table)
    spotCheck(w->pos_embedding_table, p->seq_len * D, "pos_embedding");

  spotCheck(w->rms_att_weight, D, "L0_attn_norm");
  if (ctx.hasNormBias) spotCheck(w->rms_att_bias, D, "L0_attn_norm_bias");
  spotCheck(w->rms_ffn_weight, D, "L0_ffn_norm");
  if (ctx.hasNormBias) spotCheck(w->rms_ffn_bias, D, "L0_ffn_norm_bias");

  if (isQ8 || isMix)  spotCheckQ8(w->wq_i8, w->wq_sc, D * D, gs, "L0_wq");
  else if (w->wq)     spotCheck(w->wq, D * D, "L0_wq");

  spotCheck(w->rms_final_weight, D, "final_norm");
  if (ctx.hasNormBias) spotCheck(w->rms_final_bias, D, "final_norm_bias");

  if ((isQ8 || isMix) && w->wcls_i8) spotCheckQ8(w->wcls_i8, w->wcls_sc, V * D, gs, "lm_head");
  else if (w->wcls)                   spotCheck(w->wcls, V * D, "lm_head");

  DEBUG_LLM_LOADF("[LLM] ═══ Compare SPOT values above with converter PACK_SUMMARY ═══");
}

// Re-derive every RunState pointer from the two surviving base blocks
// (stateHotData = internal-RAM hot scratch, stateData = PSRAM KV/logits) using
// only gLLM.config + gLLM.seq_ctx. Idempotent: the loader calls it to bind
// pointers after allocation, and forward()'s corruption guard calls it again to
// recover a zeroed RunState pointer without a full reload. Returns false only if
// a base block is gone or the config is unusable (then a reload is the only fix).
bool llmBindRunState() {
  if (!gLLM.stateHotData || !gLLM.stateData) return false;
  const LLMConfig* p = &gLLM.config;
  const int D = p->dim, H = p->hidden_dim, L = p->n_layers, V = p->vocab_size;
  const int n_heads = p->n_heads;
  const int seq_ctx = gLLM.seq_ctx;
  if (D <= 0 || H <= 0 || L <= 0 || V <= 0 || n_heads <= 0 || p->n_kv_heads <= 0 || seq_ctx <= 0)
    return false;
  const int kv_dim   = (p->dim * p->n_kv_heads) / p->n_heads;
  const int headSize = D / n_heads;

  RunState* s = &gLLM.state;
  float* hp = gLLM.stateHotData;
  s->x   = hp;  hp += D;
  s->xb  = hp;  hp += D;
  s->xb2 = hp;  hp += D;
  s->q   = hp;  hp += D;
  s->hb  = hp;  hp += H;
  s->hb2 = hp;  hp += H;

  // RoPE scratch, carved from the same internal-RAM hot block.
  s->rope_cos = hp;  hp += D / 2;
  s->rope_sin = hp;  hp += D / 2;
  s->rope_inv = hp;  hp += headSize;
  // Precompute inverse frequencies once (constant across tokens and layers).
  for (int hd = 0; hd < headSize; hd++) {
    s->rope_inv[hd] = 1.0f / powf(10000.0f, (float)hd / (float)headSize);
  }

  // Non-FP32 KV scratch (hot block): linear() emits FP32 into k_tmp/v_tmp before
  // packing, kv_deq dequantizes a head-slice on read. Only carved when needed, so
  // FP32 mode keeps its exact prior DRAM footprint (these stay null there).
  if (gLLM.kvPrecision != KV_FP32) {
    s->k_tmp  = hp;  hp += kv_dim;
    s->v_tmp  = hp;  hp += kv_dim;
    s->kv_deq = hp;  hp += headSize;
  } else {
    s->k_tmp = s->v_tmp = s->kv_deq = nullptr;
  }

  // Cold block (PSRAM): KV cache (precision-dependent) followed by att + logits
  // (always FP32). FP16 KV is 4*kvCount bytes; INT8 KV is int8 data (aligned up
  // to 4) + FP32 per-head scales — both keep att 4-byte aligned. Layout here must
  // match llmKvCacheBytes() exactly.
  const size_t kvCount = (size_t)L * seq_ctx * kv_dim;
  // Clear every alternate pointer; the active branch fills only its own set.
  s->key_cache = nullptr;     s->value_cache = nullptr;
  s->key_cache_f16 = nullptr; s->value_cache_f16 = nullptr;
  s->key_cache_q8 = nullptr;  s->value_cache_q8 = nullptr;
  s->key_scales = nullptr;    s->value_scales = nullptr;
  if (gLLM.kvPrecision == KV_FP16) {
    uint16_t* kp = (uint16_t*)gLLM.stateData;
    s->key_cache_f16   = kp;  kp += kvCount;
    s->value_cache_f16 = kp;  kp += kvCount;
    float* cp = (float*)kp;
    s->att    = cp;  cp += n_heads * seq_ctx;
    s->logits = cp;  cp += V;
  } else if (gLLM.kvPrecision == KV_INT8) {
    const int nkv = p->n_kv_heads;
    int8_t* bp = (int8_t*)gLLM.stateData;
    s->key_cache_q8   = bp;  bp += kvCount;
    s->value_cache_q8 = bp;  bp += kvCount;
    uintptr_t a = ((uintptr_t)bp + 3u) & ~(uintptr_t)3u;  // align to FP32 scales
    float* cp = (float*)a;
    const size_t scaleCount = (size_t)L * seq_ctx * (size_t)nkv;
    s->key_scales   = cp;  cp += scaleCount;
    s->value_scales = cp;  cp += scaleCount;
    s->att    = cp;  cp += n_heads * seq_ctx;
    s->logits = cp;  cp += V;
  } else {
    float* cp = gLLM.stateData;
    s->key_cache   = cp;  cp += kvCount;
    s->value_cache = cp;  cp += kvCount;
    s->att    = cp;  cp += n_heads * seq_ctx;
    s->logits = cp;  cp += V;
  }
  return true;
}

// Allocate hot (internal RAM) and cold (PSRAM) activation buffers.
// ctx is non-const: fragmentation recovery may reduce kvCacheSize/coldSize.
static bool allocateRunState(LoadContext& ctx) {
  const LLMConfig* p = &gLLM.config;
  const int L = ctx.L, V = ctx.V;   // D/H/seq_ctx now bound inside llmBindRunState()
  const int kv_dim = ctx.kv_dim;
  int seq_ctx = gLLM.seq_ctx;

  gLLM.stateHotData = (float*)heap_caps_calloc(1, ctx.hotSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!gLLM.stateHotData) {
    DEBUG_LLM_MEMORYF("[LLM] Internal RAM alloc failed (%uB), falling back to PSRAM for hot state", (unsigned)ctx.hotSize);
    gLLM.stateHotData = (float*)llmPsramAlloc(ctx.hotSize, "llm.state.hot");
    if (!gLLM.stateHotData) return false;
  }
  gLLM.stateHotSize = ctx.hotSize;

  // Cold state (KV cache + att + logits) is one large contiguous PSRAM allocation.
  // After weight loading fragments PSRAM, the largest free block may be smaller
  // than total free bytes. Retry with reduced context if allocation fails.
  size_t coldStateBytes = ctx.kvCacheSize + ctx.coldSize;
  gLLM.stateData = (float*)llmPsramAlloc(coldStateBytes, "llm.state.cold");
  if (!gLLM.stateData) {
    // Fragmentation: largest contiguous block is too small. Shrink context to fit.
    size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t freePSRAM = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    DEBUG_LLM_MEMORYF("[LLM] Cold state alloc failed: need %uKB, PSRAM free=%uKB largest=%uKB — retrying with reduced ctx",
                      (unsigned)(coldStateBytes/1024), (unsigned)(freePSRAM/1024), (unsigned)(largestBlock/1024));
    // Recompute: how many context slots fit in largest contiguous block?
    size_t fixedCold = (size_t)V * sizeof(float);  // logits array (always needed)
    if (largestBlock <= fixedCold) {
      ERROR_LLMF("Cannot fit even logits in PSRAM (need %uKB, largest=%uKB)",
                 (unsigned)(fixedCold/1024), (unsigned)(largestBlock/1024));
      heap_caps_free(gLLM.stateHotData);
      gLLM.stateHotData = nullptr;
      return false;
    }
    size_t perCtxSlot = llmKvCacheBytes(gLLM.kvPrecision, L, 1, kv_dim, p->n_kv_heads) + p->n_heads * sizeof(float);
    int newCtx = (int)((largestBlock - fixedCold) / perCtxSlot);
    if (newCtx < 1) newCtx = 1;
    if (newCtx >= seq_ctx) newCtx = seq_ctx - 1;  // must shrink at least 1
    if (newCtx < 1) {
      heap_caps_free(gLLM.stateHotData);
      gLLM.stateHotData = nullptr;
      return false;
    }
    DEBUG_LLM_MEMORYF("[LLM] Fragmentation recovery: ctx %d -> %d", seq_ctx, newCtx);
    seq_ctx = newCtx;
    gLLM.seq_ctx = seq_ctx;
    ctx.kvCacheSize = llmKvCacheBytes(gLLM.kvPrecision, L, seq_ctx, kv_dim, p->n_kv_heads);
    ctx.coldSize = (p->n_heads * seq_ctx + V) * sizeof(float);
    coldStateBytes = ctx.kvCacheSize + ctx.coldSize;
    gLLM.stateData = (float*)llmPsramAlloc(coldStateBytes, "llm.state.cold.retry");
    if (!gLLM.stateData) {
      ERROR_LLMF("Cold state retry also failed (%uKB)", (unsigned)(coldStateBytes/1024));
      heap_caps_free(gLLM.stateHotData);
      gLLM.stateHotData = nullptr;
      return false;
    }
  }
  gLLM.stateSize = coldStateBytes;

  // Bind RunState pointers into the freshly allocated base blocks. Shared with
  // the corruption-recovery path (llmBindRunState is the single source of truth).
  if (!llmBindRunState()) return false;

  // Pre-allocate repetition penalty ring buffer (reused per generation call)
  int repSize = LLM_DEFAULT_REP_WINDOW;
  if (repSize > 256) repSize = 256;
  if (repSize > 0) {
    gLLM.repBuf = (int*)malloc(repSize * sizeof(int));
    gLLM.repBufSize = gLLM.repBuf ? repSize : 0;
  }

  // Pre-allocate the top-p sampling index buffer in PSRAM (reused every token
  // instead of malloc/free per token). It's only touched by sample_topp's
  // partial sort — a tiny fraction of per-token time, and the nucleus is usually
  // 1 token on this model — so keeping ~V*4 bytes (~13KB) out of internal DRAM
  // relieves heap pressure during generation at no measurable speed cost.
  gLLM.sampleIndices = (int*)llmPsramAlloc((size_t)V * sizeof(int), "llm.sampleidx");
  gLLM.sampleIndicesSize = gLLM.sampleIndices ? V : 0;

  // Pre-allocate the no-repeat n-gram history (every sampled token of one
  // generation; generation is bounded by seq_ctx). PSRAM — zero DRAM cost.
  // On alloc failure the blocker silently disables (llmGenerate null-checks).
  gLLM.genHist = (int*)llmPsramAlloc((size_t)seq_ctx * sizeof(int), "llm.ngramhist");
  gLLM.genHistSize = gLLM.genHist ? seq_ctx : 0;

  return true;
}

static inline QuantTensor mkTensor(const float* fp, const int8_t* i8, const float* sc,
                                   const uint8_t* q4, const float* q4_sc,
                                   int gs, int n, int d) {
  QuantTensor t; t.fp=fp; t.i8=i8; t.sc=sc; t.q4=q4; t.q4_sc=q4_sc; t.gs=gs; t.n=n; t.d=d;
  return t;
}

// Prepackage per-layer attention + FFN weights (and the classifier) into
// QuantTensors with offsets resolved once, so forward() indexes instead of
// recomputing pointers every token. The offset math mirrors exactly what
// forward() used to do inline — stored pointer values are identical, so output
// is byte-for-byte unchanged. Call after all raw weight pointers are set.
static bool buildLayerTensors() {
  const LLMConfig* p   = &gLLM.config;
  TransformerWeights* w = &gLLM.weights;
  const int dim        = p->dim;
  const int kv_dim     = (p->dim * p->n_kv_heads) / p->n_heads;
  const int hidden_dim = p->hidden_dim;
  const int gs         = p->group_size;
  const int L          = p->n_layers;

  w->layerT = (TransformerWeights::LayerTensors*)llmPsramAlloc(
                (size_t)L * sizeof(TransformerWeights::LayerTensors), "llm.layerT");
  if (!w->layerT) return false;

  int q8_li = 0;  // Q8 layer index (= l for pure INT8; sparse for mixed Q4/Q8)
  for (int l = 0; l < L; l++) {
    const bool isQ4 = (w->layer_quant && w->layer_quant[l] == 2);

    const size_t fp_lD2  = (size_t)l * dim * dim;
    const size_t fp_lDkv = (size_t)l * dim * kv_dim;
    const size_t fp_lDH  = (size_t)l * (size_t)dim * hidden_dim;
    const size_t q8_lD2  = (size_t)q8_li * dim * dim;
    const size_t q8_lDkv = (size_t)q8_li * dim * kv_dim;
    const size_t q8_lDH  = (size_t)q8_li * (size_t)dim * hidden_dim;
    const size_t scWQ  = gs ? q8_lD2  / gs : 0;
    const size_t scWKV = gs ? q8_lDkv / gs : 0;
    const size_t scWDH = gs ? q8_lDH  / gs : 0;

    const TransformerWeights::Q4LayerOffsets* off = isQ4 ? &w->q4_offsets[l] : nullptr;
    TransformerWeights::LayerTensors& T = w->layerT[l];

    T.wq = mkTensor(w->wq ? w->wq + fp_lD2 : nullptr,
                    (!isQ4 && w->wq_i8) ? w->wq_i8 + q8_lD2 : nullptr,
                    (!isQ4 && w->wq_sc) ? w->wq_sc + scWQ  : nullptr,
                    isQ4 ? w->q4_data + off->wq_data : nullptr,
                    isQ4 ? w->q4_scales + off->wq_sc : nullptr, gs, dim, dim);
    T.wk = mkTensor(w->wk ? w->wk + fp_lDkv : nullptr,
                    (!isQ4 && w->wk_i8) ? w->wk_i8 + q8_lDkv : nullptr,
                    (!isQ4 && w->wk_sc) ? w->wk_sc + scWKV  : nullptr,
                    isQ4 ? w->q4_data + off->wk_data : nullptr,
                    isQ4 ? w->q4_scales + off->wk_sc : nullptr, gs, dim, kv_dim);
    T.wv = mkTensor(w->wv ? w->wv + fp_lDkv : nullptr,
                    (!isQ4 && w->wv_i8) ? w->wv_i8 + q8_lDkv : nullptr,
                    (!isQ4 && w->wv_sc) ? w->wv_sc + scWKV  : nullptr,
                    isQ4 ? w->q4_data + off->wv_data : nullptr,
                    isQ4 ? w->q4_scales + off->wv_sc : nullptr, gs, dim, kv_dim);
    T.wo = mkTensor(w->wo ? w->wo + fp_lD2 : nullptr,
                    (!isQ4 && w->wo_i8) ? w->wo_i8 + q8_lD2 : nullptr,
                    (!isQ4 && w->wo_sc) ? w->wo_sc + scWQ  : nullptr,
                    isQ4 ? w->q4_data + off->wo_data : nullptr,
                    isQ4 ? w->q4_scales + off->wo_sc : nullptr, gs, dim, dim);
    T.w1 = mkTensor(w->w1 ? w->w1 + fp_lDH : nullptr,
                    (!isQ4 && w->w1_i8) ? w->w1_i8 + q8_lDH : nullptr,
                    (!isQ4 && w->w1_sc) ? w->w1_sc + scWDH : nullptr,
                    isQ4 ? w->q4_data + off->w1_data : nullptr,
                    isQ4 ? w->q4_scales + off->w1_sc : nullptr, gs, dim, hidden_dim);
    T.w2 = mkTensor(w->w2 ? w->w2 + fp_lDH : nullptr,
                    (!isQ4 && w->w2_i8) ? w->w2_i8 + q8_lDH : nullptr,
                    (!isQ4 && w->w2_sc) ? w->w2_sc + scWDH : nullptr,
                    isQ4 ? w->q4_data + off->w2_data : nullptr,
                    isQ4 ? w->q4_scales + off->w2_sc : nullptr, gs, hidden_dim, dim);
    T.w3 = mkTensor(w->w3 ? w->w3 + fp_lDH : nullptr,
                    (!isQ4 && w->w3_i8) ? w->w3_i8 + q8_lDH : nullptr,
                    (!isQ4 && w->w3_sc) ? w->w3_sc + scWDH : nullptr,
                    isQ4 ? w->q4_data + off->w3_data : nullptr,
                    isQ4 ? w->q4_scales + off->w3_sc : nullptr, gs, dim, hidden_dim);

    if (!isQ4) q8_li++;
  }

  // Classifier — always FP32 or INT8, never Q4 — (n=dim, d=vocab_size).
  w->clsT = mkTensor(w->wcls, w->wcls_i8, w->wcls_sc, nullptr, nullptr,
                     gs, dim, p->vocab_size);

  DEBUG_LLM_LOADF("[LLM] Built %d layer tensor sets + classifier for linear()", L);
  return true;
}

// Validate a MENU section payload (already read into `b`, `n` bytes) strictly per
// LLM_GUIDED_MENU_SPEC §4 and fill `outGroups`/`outCount`. ONE walk: every
// length-prefixed item must land inside the payload, all counts within caps, and
// <=1 slot byte (0x1F) per template. Returns true only for a fully valid menu.
// menu_ver!=1 or group_count outside 1..8 → false (the section is skipped, which
// is a first-class "no menu" state, not a load error). Descriptor offsets are
// byte offsets from the start of `b`.
static bool validateMenuBlob(const uint8_t* b, size_t n,
                             LLMMenuGroupDesc* outGroups, uint8_t* outCount) {
  if (n < 4) return false;                        // menu_ver + group_count + reserved
  if (b[0] != 1) return false;                    // menu_ver != 1 → skip section
  uint8_t groupCount = b[1];
  if (groupCount < 1 || groupCount > 8) return false;
  // b[2..3] = reserved u16 (must be 0 per spec; ignored on read).
  size_t pos = 4;
  for (uint8_t g = 0; g < groupCount; g++) {
    if (pos + 1 > n) return false;
    uint8_t flags = b[pos++];
    if (pos + 1 > n) return false;
    uint8_t nameLen = b[pos++];
    if (nameLen > 32) return false;
    if (pos + nameLen > n) return false;
    uint32_t nameOff = (uint32_t)pos;
    pos += nameLen;
    if (pos + 2 > n) return false;
    uint16_t tplCount = (uint16_t)(b[pos] | (b[pos + 1] << 8)); pos += 2;
    if (tplCount > 64) return false;
    if (pos + 2 > n) return false;
    uint16_t entCount = (uint16_t)(b[pos] | (b[pos + 1] << 8)); pos += 2;
    if (entCount > 1024) return false;
    uint32_t tplOff = (uint32_t)pos;
    for (uint16_t t = 0; t < tplCount; t++) {
      if (pos + 1 > n) return false;
      uint8_t len = b[pos++];
      if (len > 120) return false;
      if (pos + len > n) return false;
      uint8_t slots = 0;
      for (uint8_t k = 0; k < len; k++) if (b[pos + k] == 0x1F) slots++;
      if (slots > 1) return false;                // <=1 slot per template
      pos += len;
    }
    uint32_t entOff = (uint32_t)pos;
    for (uint16_t e = 0; e < entCount; e++) {
      if (pos + 1 > n) return false;
      uint8_t len = b[pos++];
      if (len > 48) return false;
      if (pos + len > n) return false;
      pos += len;
    }
    outGroups[g].nameOff  = nameOff;
    outGroups[g].tplOff   = tplOff;
    outGroups[g].entOff   = entOff;
    outGroups[g].tplCount = tplCount;
    outGroups[g].entCount = entCount;
    outGroups[g].flags    = flags;
    outGroups[g].nameLen  = nameLen;
  }
  *outCount = groupCount;
  return true;
}

// MENU section (id 4): read the payload into a PSRAM blob, validate strictly, and
// publish it (transferring ownership) or free it. The menu was already cleared by
// loadInfoBlockFromFile, so any failure path simply leaves guided input absent.
static void parseMenuSection(File& f, size_t payloadStart, uint32_t slen) {
  // Encoded MENU section is capped at 32768 B (spec §2); anything else is malformed.
  if (slen < 4 || slen > 32768) {
    DEBUG_LLM_LOADF("[LLM] menu: section size %u out of range — guided input disabled",
                    (unsigned)slen);
    return;
  }
  uint8_t* blob = (uint8_t*)llmPsramAlloc(slen, "llm.menu");
  if (!blob) {
    DEBUG_LLM_LOADF("[LLM] menu: OOM allocating %u B — guided input disabled", (unsigned)slen);
    return;
  }
  f.seek(payloadStart);
  if (f.read(blob, slen) != (int)slen) {
    DEBUG_LLM_LOADF("[LLM] menu: short read — guided input disabled");
    llmPsramFree((void**)&blob);
    return;
  }
  LLMMenuGroupDesc groups[8];
  uint8_t count = 0;
  if (!validateMenuBlob(blob, slen, groups, &count)) {
    DEBUG_LLM_LOADF("[LLM] menu: malformed MENU section — guided input disabled");
    llmPsramFree((void**)&blob);
    return;
  }
  llmMenuPublish(blob, slen, groups, count);      // takes ownership of blob
  DEBUG_LLM_LOADF("[LLM] menu: %u group(s) published (%u B)", (unsigned)count, (unsigned)slen);
}

// Read the optional info block (v2 TLV: description + icon + domain gate + guided
// menu) that sits between the 64-byte header and the tokenizer when the header's
// info_len (offset 24) is non-zero. Best effort: a malformed/oversized field is
// skipped rather than failing the load. ALWAYS leaves the file cursor at
// infoStart + infoLen — i.e. the first byte of the tokenizer block — so downstream
// reads (which key off the live f.position()) stay correct regardless of any parse
// drift. infoLen==0 is a no-op, leaving the cursor at the header end.
//
// A legacy v1 block (no 0x4932 sentinel) can't be read positionally by this
// parser — its metadata is wiped and the cursor seeks past (the model still
// loads). See LLM_GUIDED_MENU_SPEC §3.
static void loadInfoBlockFromFile(File& f, uint32_t infoLen) {
  gLLM.modelDesc[0]  = '\0';
  gLLM.modelHasIcon  = false;
  gLLM.modelIconW    = 0;
  gLLM.modelIconH    = 0;
  gLLM.modelRefusal[0] = '\0';
  if (gLLM.domainVocab) llmPsramFree((void**)&gLLM.domainVocab);
  gLLM.domainVocabCount = 0;
  gLLM.domainVocabBytes = 0;
  // Menu is published/cleared through the locked helpers so a reader on another
  // task never observes a torn state. Clear first; a valid MENU section republishes.
  llmMenuClear();

  if (infoLen == 0) return;

  const size_t infoStart = f.position();
  const size_t infoEnd   = infoStart + infoLen;

  // Info block v2 opens with a u16LE sentinel = 0x4932. A legacy v1 block starts
  // with desc_len (<=255, high byte 0), so any value >=0x0100 is unambiguously v2.
  uint16_t sentinel = 0;
  if (f.read((uint8_t*)&sentinel, 2) != 2 || sentinel != 0x4932) {
    f.seek(infoEnd);   // legacy/unreadable — metadata already wiped, model still loads
    return;
  }

  uint8_t sectionCount = 0;
  if (f.read(&sectionCount, 1) != 1) { f.seek(infoEnd); return; }

  // TLV walk. Inner section encodings are preserved from v1 so the field parsers
  // below match the pre-TLV layout (icon fmt/w/h/len; domain refusal + vocab).
  for (uint8_t s = 0; s < sectionCount; s++) {
    if (f.position() + 5 > infoEnd) break;         // no room for id(1)+len(4)
    uint8_t  sid  = 0;
    uint32_t slen = 0;
    if (f.read(&sid, 1) != 1) break;
    if (f.read((uint8_t*)&slen, 4) != 4) break;
    const size_t payloadStart = f.position();
    if (slen > infoEnd - payloadStart) break;      // declared length overruns the block

    switch (sid) {
      case 1: {  // DESC — raw UTF-8, whole payload (converter caps at 255 B)
        size_t copyLen = (slen < sizeof(gLLM.modelDesc)) ? slen : sizeof(gLLM.modelDesc) - 1;
        if (copyLen > 0) f.read((uint8_t*)gLLM.modelDesc, copyLen);
        gLLM.modelDesc[copyLen] = '\0';
        break;
      }
      case 2: {  // ICON — fmt(u8)+w(u8)+h(u8)+u16 len+packed 1bpp bitmap (MSB-first)
        uint8_t iconFmt = 0, iconW = 0, iconH = 0;
        uint16_t iconLen = 0;
        if (f.read(&iconFmt, 1) == 1 && f.read(&iconW, 1) == 1 &&
            f.read(&iconH, 1) == 1 && f.read((uint8_t*)&iconLen, 2) == 2) {
          const size_t expect = (size_t)((iconW + 7) / 8) * iconH;
          if (iconFmt == 1 && iconW > 0 && iconW <= 32 && iconH > 0 && iconH <= 32 &&
              iconLen == expect && iconLen <= sizeof(gLLM.modelIcon) &&
              (size_t)f.position() + iconLen <= payloadStart + slen) {
            if (f.read(gLLM.modelIcon, iconLen) == (int)iconLen) {
              gLLM.modelIconW   = iconW;
              gLLM.modelIconH   = iconH;
              gLLM.modelHasIcon = true;
            }
          }
        }
        break;
      }
      case 3: {  // DOMAIN — u16 refusal_len + refusal + u16 vocab_count + [u8 len][word]...
        const size_t domEnd = payloadStart + slen;
        uint16_t refLen = 0;
        if (f.read((uint8_t*)&refLen, 2) == 2 && refLen > 0 &&
            f.position() + refLen <= domEnd) {
          size_t copyLen = (refLen < sizeof(gLLM.modelRefusal)) ? refLen
                                                                : sizeof(gLLM.modelRefusal) - 1;
          if (copyLen > 0) f.read((uint8_t*)gLLM.modelRefusal, copyLen);
          gLLM.modelRefusal[copyLen] = '\0';
          f.seek(payloadStart + 2 + refLen);   // realign past any truncated refusal tail
        }
        if (f.position() + 2 <= domEnd) {
          uint16_t vcount = 0;
          if (f.read((uint8_t*)&vcount, 2) == 2 && vcount > 0) {
            const size_t blobLen = domEnd - f.position();
            if (blobLen > 0) {
              gLLM.domainVocab = (uint8_t*)llmPsramAlloc(blobLen, "llm.domainvocab");
              if (gLLM.domainVocab && f.read(gLLM.domainVocab, blobLen) == (int)blobLen) {
                gLLM.domainVocabCount = vcount;
                gLLM.domainVocabBytes = blobLen;
              } else {
                if (gLLM.domainVocab) llmPsramFree((void**)&gLLM.domainVocab);
                gLLM.domainVocabCount = 0;
                gLLM.domainVocabBytes = 0;
              }
            }
          }
        }
        break;
      }
      case 4:    // MENU — validated + published into a PSRAM blob
        parseMenuSection(f, payloadStart, slen);
        break;
      default:   // unknown section id → skip
        break;
    }

    // Realign to the next section start regardless of what the parser above did.
    f.seek(payloadStart + slen);
  }

  // Land exactly at the tokenizer, whatever we did (or failed to do) above.
  f.seek(infoStart + infoLen);
}

bool loadWeights(const char* path) {
  File f = VFS::openGuarded(path, "r", VFS::systemAuth("llm.load_weights"));
  if (!f) {
    setLlmError("Cannot open model: %s", path);
    return false;
  }

  // ---- Parse header ----
  LoadContext ctx = {};
  if (!parseModelHeader(f, ctx)) { f.close(); return false; }

  LLMConfig* p = &gLLM.config;
  int kv_dim = ctx.kv_dim;

  // ---- Read optional info block (description + icon), leaving the cursor at the
  //      tokenizer. No-op (cursor stays at byte 64) when info_len is 0. ----
  loadInfoBlockFromFile(f, ctx.infoLen);

  // ---- Read embedded tokenizer ----
  if (!loadTokenizerFromFile(f)) {
    setLlmError("Failed to load embedded tokenizer");
    f.close();
    return false;
  }

  // ---- Pre-scan for tied weights and compute memory layout ----
  if (!prescanTiedWeights(f, ctx)) { f.close(); return false; }
  if (!computeMemoryLayout(ctx)) { f.close(); return false; }

  const int D = ctx.D, H = ctx.H, L = ctx.L, V = ctx.V;
  const uint8_t qt = ctx.qt;
  const uint16_t gs = ctx.gs;
  const bool isGPT2 = ctx.isGPT2;
  const bool hasNormBias = ctx.hasNormBias;
  const bool shared_weights = ctx.shared_weights;
  const bool v3 = ctx.v3;
  const size_t weightsBytes = ctx.weightsBytes;
  const size_t weightsQ8Bytes = ctx.weightsQ8Bytes;
  const size_t weightsQ4Bytes = ctx.weightsQ4Bytes;
  const size_t weightsQ4ScBytes = ctx.weightsQ4ScBytes;

  // ---- Allocate weight blocks and map pointers ----
  gLLM.weightsData = (float*)llmPsramAlloc(weightsBytes, "llm.weights");
  if (!gLLM.weightsData) { f.close(); return false; }
  gLLM.weightsSize = weightsBytes;

  TransformerWeights* w = &gLLM.weights;

  if (qt == 0) {
    // FP32 mode: single float block, same layout as before
    float* ptr = gLLM.weightsData;
    w->token_embedding_table = ptr; ptr += V * D;
    if (isGPT2) {
      w->pos_embedding_table = ptr; ptr += p->seq_len * D;
    } else {
      w->pos_embedding_table = nullptr;
    }
    w->rms_att_weight = ptr; ptr += L * D;
    w->wq = ptr;             ptr += L * D * D;
    w->wk = ptr;             ptr += L * D * kv_dim;
    w->wv = ptr;             ptr += L * D * kv_dim;
    w->wo = ptr;             ptr += L * D * D;
    w->rms_ffn_weight = ptr; ptr += L * D;
    w->w1 = ptr;             ptr += isGPT2 ? L : L * H * D;
    w->w2 = ptr;             ptr += L * D * H;
    w->w3 = ptr;             ptr += L * H * D;
    w->rms_final_weight = ptr; ptr += D;
    if (hasNormBias) {
      w->rms_att_bias   = ptr; ptr += L * D;
      w->rms_ffn_bias   = ptr; ptr += L * D;
      w->rms_final_bias = ptr; ptr += D;
    } else {
      w->rms_att_bias = w->rms_ffn_bias = w->rms_final_bias = nullptr;
    }
    if (!shared_weights) w->wcls = ptr;
  } else if (qt == 1) {
    // INT8 mode: float block = norms + scales; separate int8 data block
    gLLM.weightsQ8Data = (int8_t*)llmPsramAlloc(weightsQ8Bytes, "llm.q8");
    if (!gLLM.weightsQ8Data) { f.close(); return false; }
    gLLM.weightsQ8Size = weightsQ8Bytes;

    // -- FP32 pointer: norms first --
    float* fp = gLLM.weightsData;
    w->token_embedding_table = nullptr;         // INT8 — use emb_i8/emb_sc
    w->rms_att_weight  = fp; fp += L * D;
    w->rms_ffn_weight  = fp; fp += L * D;
    w->rms_final_weight = fp; fp += D;
    if (isGPT2) {
      w->pos_embedding_table = fp; fp += p->seq_len * D;
      w->w1 = fp; fp += L;           // dummy FP32 per layer (not used in fwd; must be read)
    } else {
      w->pos_embedding_table = nullptr;
      w->w1 = nullptr;               // Llama: w1 is INT8
    }
    if (hasNormBias) {
      w->rms_att_bias   = fp; fp += L * D;
      w->rms_ffn_bias   = fp; fp += L * D;
      w->rms_final_bias = fp; fp += D;
    } else {
      w->rms_att_bias = w->rms_ffn_bias = w->rms_final_bias = nullptr;
    }
    // Null out FP32 matrix pointers (INT8 path uses _i8/_sc instead)
    w->wq = w->wk = w->wv = w->wo = nullptr;
    w->w2 = w->w3 = w->wcls = nullptr;

    // -- Scales (all contiguous in the float block after norms) --
    w->emb_sc  = fp; fp += scaleCount((size_t)V * D, gs);
    w->wq_sc   = fp; fp += L * scaleCount((size_t)D * D, gs);
    w->wk_sc   = fp; fp += L * scaleCount((size_t)D * kv_dim, gs);
    w->wv_sc   = fp; fp += L * scaleCount((size_t)D * kv_dim, gs);
    w->wo_sc   = fp; fp += L * scaleCount((size_t)D * D, gs);
    if (!isGPT2) {
      w->w1_sc = fp; fp += L * scaleCount((size_t)H * D, gs);
    } else {
      w->w1_sc = nullptr;
    }
    w->w2_sc   = fp; fp += L * scaleCount((size_t)D * H, gs);
    w->w3_sc   = fp; fp += L * scaleCount((size_t)H * D, gs);
    if (!shared_weights) {
      w->wcls_sc = fp; // last field — no advance needed
    }

    // -- INT8 data pointers --
    int8_t* q8 = gLLM.weightsQ8Data;
    w->emb_i8  = q8; q8 += (size_t)V * D;
    w->wq_i8   = q8; q8 += (size_t)L * D * D;
    w->wk_i8   = q8; q8 += (size_t)L * D * kv_dim;
    w->wv_i8   = q8; q8 += (size_t)L * D * kv_dim;
    w->wo_i8   = q8; q8 += (size_t)L * D * D;
    if (!isGPT2) {
      w->w1_i8 = q8; q8 += (size_t)L * H * D;
    } else {
      w->w1_i8 = nullptr;
    }
    w->w2_i8   = q8; q8 += (size_t)L * D * H;
    w->w3_i8   = q8; q8 += (size_t)L * H * D;
    if (!shared_weights) {
      w->wcls_i8 = q8; // last field — no advance needed
    }
  } else {
    // ── INT4_MIXED mode (qt==2) ──
    // Allocate Q8 data block (embedding + INT8 layers)
    gLLM.weightsQ8Data = (int8_t*)llmPsramAlloc(weightsQ8Bytes, "llm.q8");
    if (!gLLM.weightsQ8Data) { f.close(); return false; }
    gLLM.weightsQ8Size = weightsQ8Bytes;

    // Allocate Q4 packed data block
    w->q4_data = (uint8_t*)llmPsramAlloc(weightsQ4Bytes, "llm.q4");
    if (!w->q4_data) { f.close(); return false; }
    w->q4_data_size = weightsQ4Bytes;

    // Allocate Q4 scales block
    w->q4_scales = (float*)llmPsramAlloc(weightsQ4ScBytes, "llm.q4sc");
    if (!w->q4_scales) { f.close(); return false; }
    w->q4_scales_size = weightsQ4ScBytes;

    // Allocate per-layer metadata
    w->layer_quant = (uint8_t*)llmPsramAlloc(L, "llm.lq");
    if (!w->layer_quant) { f.close(); return false; }
    w->q4_offsets = (TransformerWeights::Q4LayerOffsets*)llmPsramAlloc(
        L * sizeof(TransformerWeights::Q4LayerOffsets), "llm.q4off");
    if (!w->q4_offsets) { f.close(); return false; }
    memset(w->q4_offsets, 0, L * sizeof(TransformerWeights::Q4LayerOffsets));

    // Fill layer_quant array
    const int nQ8s = p->n_q8_start;
    const int nQ8e = p->n_q8_end;
    for (int l = 0; l < L; l++) {
      w->layer_quant[l] = (l < nQ8s || l >= L - nQ8e) ? 1 : 2;
    }

    // -- FP32 block: norms + Q8 scales --
    float* fp = gLLM.weightsData;
    w->token_embedding_table = nullptr;
    w->rms_att_weight  = fp; fp += L * D;
    w->rms_ffn_weight  = fp; fp += L * D;
    w->rms_final_weight = fp; fp += D;
    if (isGPT2) {
      w->pos_embedding_table = fp; fp += p->seq_len * D;
      w->w1 = fp; fp += L;
    } else {
      w->pos_embedding_table = nullptr;
      w->w1 = nullptr;
    }
    if (hasNormBias) {
      w->rms_att_bias   = fp; fp += L * D;
      w->rms_ffn_bias   = fp; fp += L * D;
      w->rms_final_bias = fp; fp += D;
    } else {
      w->rms_att_bias = w->rms_ffn_bias = w->rms_final_bias = nullptr;
    }
    w->wq = w->wk = w->wv = w->wo = nullptr;
    w->w2 = w->w3 = w->wcls = nullptr;

    // Q8 layer scales in the float block (only for INT8 layers)
    const int nQ8 = nQ8s + nQ8e;
    w->emb_sc  = fp; fp += scaleCount((size_t)V * D, gs);
    w->wq_sc   = fp; fp += nQ8 * scaleCount((size_t)D * D, gs);
    w->wk_sc   = fp; fp += nQ8 * scaleCount((size_t)D * kv_dim, gs);
    w->wv_sc   = fp; fp += nQ8 * scaleCount((size_t)D * kv_dim, gs);
    w->wo_sc   = fp; fp += nQ8 * scaleCount((size_t)D * D, gs);
    if (!isGPT2) {
      w->w1_sc = fp; fp += nQ8 * scaleCount((size_t)H * D, gs);
    } else {
      w->w1_sc = nullptr;
    }
    w->w2_sc   = fp; fp += nQ8 * scaleCount((size_t)D * H, gs);
    w->w3_sc   = fp; fp += nQ8 * scaleCount((size_t)H * D, gs);
    if (!shared_weights) {
      w->wcls_sc = fp;
    }

    // Q8 data block: embedding first, then INT8-layer weights (contiguous by tensor type)
    int8_t* q8 = gLLM.weightsQ8Data;
    w->emb_i8 = q8; q8 += (size_t)V * D;
    w->wq_i8  = q8; q8 += (size_t)nQ8 * D * D;
    w->wk_i8  = q8; q8 += (size_t)nQ8 * D * kv_dim;
    w->wv_i8  = q8; q8 += (size_t)nQ8 * D * kv_dim;
    w->wo_i8  = q8; q8 += (size_t)nQ8 * D * D;
    if (!isGPT2) {
      w->w1_i8 = q8; q8 += (size_t)nQ8 * H * D;
    } else {
      w->w1_i8 = nullptr;
    }
    w->w2_i8  = q8; q8 += (size_t)nQ8 * D * H;
    w->w3_i8  = q8; q8 += (size_t)nQ8 * H * D;
    if (!shared_weights) {
      w->wcls_i8 = q8;
    }

    DEBUG_LLM_LOADF("[LLM] Mixed Q4/Q8 pointers mapped: Q8 layers=%d, Q4 layers=%d", nQ8, L - nQ8);
  }

  // ---- Read tensors from file ----
  const bool isQ8  = (qt == 1);
  const bool isMix = (qt == 2);
  DEBUG_LLM_LOADF("[LLM] Loading weights from flash (%s)...",
                  isMix ? "MIXED Q4/Q8" : (isQ8 ? "INT8 in PSRAM" : "FP32"));

  if (isQ8 || isMix) {
    size_t sc_emb  = scaleCount((size_t)V * D, gs);
    size_t sc_wq   = scaleCount((size_t)D * D, gs);
    size_t sc_wkv  = scaleCount((size_t)D * kv_dim, gs);
    size_t sc_wdh  = scaleCount((size_t)D * H, gs);
    DEBUG_LLM_LOADF("[LLM] Scale geometry: dim=%d kv_dim=%d hidden=%d gs=%d", D, kv_dim, H, gs);
    DEBUG_LLM_LOADF("[LLM]   emb: %u  wq/wo: %u  wk/wv: %u  w1/w2/w3: %u",
                    (unsigned)sc_emb, (unsigned)sc_wq, (unsigned)sc_wkv, (unsigned)sc_wdh);
  }

  // Helper: read and discard a VERSION=3 per-tensor quant prefix byte.
  // Returns the prefix value, or -1 on read error.
  auto readPrefix = [&](File& file) -> int {
    if (!v3) return -2; // no prefix for VERSION<=2
    uint8_t pfxByte = 0;
    if (file.read(&pfxByte, 1) != 1) return -1;
    return (int)pfxByte;
  };

  // 1. Embedding (always Q8 for quantized models)
  if (isMix || isQ8) {
    if (v3) readPrefix(f);  // skip prefix byte
    if (!readTensorQ8(f, w->emb_i8, w->emb_sc, V * D, gs)) {
      setLlmError("Failed reading embedding tensor (Q8)");
      f.close(); return false;
    }
  } else {
    if (!readTensor(f, w->token_embedding_table, V * D, qt, gs, false)) {
      setLlmError("Failed reading embedding tensor");
      f.close(); return false;
    }
  }

  // 1b. Positional embedding (GPT-2 only, always FP32)
  if (isGPT2) {
    if (v3) readPrefix(f);
    if (!readTensor(f, w->pos_embedding_table, p->seq_len * D, qt, gs, true)) {
      setLlmError("Failed reading positional embedding");
      f.close(); return false;
    }
    DEBUG_LLM_LOADF("[LLM] Positional embedding loaded (%d x %d)", p->seq_len, D);
  }

  // 2. Per-layer tensors
  // For mixed mode: Q8 layer data is contiguous (indexed by q8_li, not global l)
  // and Q4 data/scales are tracked with running cursors to build the offset table.
  int q8_li = 0;                // Q8 layer counter for mixed mode
  size_t q4_data_cursor = 0;    // running byte offset into w->q4_data
  size_t q4_sc_cursor = 0;      // running float offset into w->q4_scales

  for (int l = 0; l < L; l++) {
    const bool isQ4Layer = isMix && (w->layer_quant[l] == 2);

    // Norm tensors: always FP32, always indexed by global layer l
    if (v3) readPrefix(f);
    if (!readTensor(f, w->rms_att_weight + l*D, D, qt, gs, true)) {
      setLlmError("Failed reading attn_norm layer %d", l);
      f.close(); return false;
    }
    if (hasNormBias) {
      if (v3) readPrefix(f);
      if (!readTensor(f, w->rms_att_bias + l*D, D, qt, gs, true)) {
        setLlmError("Failed reading attn_norm_bias layer %d", l);
        f.close(); return false;
      }
    }

    // QKV + output projections
    bool layerOk;
    if (isQ4Layer) {
      // INT4 layer: read Q4 tensors, build offset table
      auto& off = w->q4_offsets[l];
      uint32_t nWQ = D * D, nWK = D * kv_dim, nWV = D * kv_dim, nWO = D * D;

      off.wq_data = q4_data_cursor;  off.wq_sc = q4_sc_cursor;
      if (v3) readPrefix(f);
      layerOk = readTensorQ4(f, w->q4_data + q4_data_cursor, w->q4_scales + q4_sc_cursor, nWQ, gs);
      q4_data_cursor += ((size_t)nWQ + 1) / 2;  q4_sc_cursor += scaleCount(nWQ, gs);

      off.wk_data = q4_data_cursor;  off.wk_sc = q4_sc_cursor;
      if (v3) readPrefix(f);
      layerOk = layerOk && readTensorQ4(f, w->q4_data + q4_data_cursor, w->q4_scales + q4_sc_cursor, nWK, gs);
      q4_data_cursor += ((size_t)nWK + 1) / 2;  q4_sc_cursor += scaleCount(nWK, gs);

      off.wv_data = q4_data_cursor;  off.wv_sc = q4_sc_cursor;
      if (v3) readPrefix(f);
      layerOk = layerOk && readTensorQ4(f, w->q4_data + q4_data_cursor, w->q4_scales + q4_sc_cursor, nWV, gs);
      q4_data_cursor += ((size_t)nWV + 1) / 2;  q4_sc_cursor += scaleCount(nWV, gs);

      off.wo_data = q4_data_cursor;  off.wo_sc = q4_sc_cursor;
      if (v3) readPrefix(f);
      layerOk = layerOk && readTensorQ4(f, w->q4_data + q4_data_cursor, w->q4_scales + q4_sc_cursor, nWO, gs);
      q4_data_cursor += ((size_t)nWO + 1) / 2;  q4_sc_cursor += scaleCount(nWO, gs);
    } else if (isQ8 || isMix) {
      // INT8 layer (pure INT8 or Q8 layer in mixed mode)
      size_t q8lD2  = (size_t)q8_li * D * D;
      size_t q8lDkv = (size_t)q8_li * D * kv_dim;
      size_t q8scWQ  = (size_t)q8_li * scaleCount((size_t)D * D, gs);
      size_t q8scWKV = (size_t)q8_li * scaleCount((size_t)D * kv_dim, gs);
      if (v3) readPrefix(f);
      layerOk = readTensorQ8(f, w->wq_i8+q8lD2,  w->wq_sc+q8scWQ,  D*D,      gs);
      if (v3) readPrefix(f);
      layerOk = layerOk && readTensorQ8(f, w->wk_i8+q8lDkv, w->wk_sc+q8scWKV, D*kv_dim, gs);
      if (v3) readPrefix(f);
      layerOk = layerOk && readTensorQ8(f, w->wv_i8+q8lDkv, w->wv_sc+q8scWKV, D*kv_dim, gs);
      if (v3) readPrefix(f);
      layerOk = layerOk && readTensorQ8(f, w->wo_i8+q8lD2,  w->wo_sc+q8scWQ,  D*D,      gs);
    } else {
      // FP32 mode
      size_t lD2  = (size_t)l * D * D;
      size_t lDkv = (size_t)l * D * kv_dim;
      layerOk = readTensor(f, w->wq+lD2,  D*D,      qt, gs, false)
             && readTensor(f, w->wk+lDkv, D*kv_dim, qt, gs, false)
             && readTensor(f, w->wv+lDkv, D*kv_dim, qt, gs, false)
             && readTensor(f, w->wo+lD2,  D*D,      qt, gs, false);
    }
    if (!layerOk) {
      setLlmError("Failed reading QKV/O layer %d", l);
      f.close(); return false;
    }

    // FFN norm (always FP32)
    if (v3) readPrefix(f);
    if (!readTensor(f, w->rms_ffn_weight + l*D, D, qt, gs, true)) {
      setLlmError("Failed reading ffn_norm layer %d", l);
      f.close(); return false;
    }
    if (hasNormBias) {
      if (v3) readPrefix(f);
      if (!readTensor(f, w->rms_ffn_bias + l*D, D, qt, gs, true)) {
        setLlmError("Failed reading ffn_norm_bias layer %d", l);
        f.close(); return false;
      }
    }

    // Gate (w1): GPT-2 always FP32 dummy; Llama FP32/Q8/Q4
    if (isGPT2) {
      if (v3) readPrefix(f);
      if (!readTensor(f, w->w1 + l, 1, qt, gs, true)) {
        setLlmError("Failed reading GPT-2 gate layer %d", l);
        f.close(); return false;
      }
    } else if (isQ4Layer) {
      auto& off = w->q4_offsets[l];
      off.w1_data = q4_data_cursor;  off.w1_sc = q4_sc_cursor;
      if (v3) readPrefix(f);
      if (!readTensorQ4(f, w->q4_data + q4_data_cursor, w->q4_scales + q4_sc_cursor, H*D, gs)) {
        setLlmError("Failed reading gate(Q4) layer %d", l);
        f.close(); return false;
      }
      q4_data_cursor += ((size_t)H * D + 1) / 2;  q4_sc_cursor += scaleCount((size_t)H * D, gs);
    } else if (isQ8 || isMix) {
      size_t q8lDH  = (size_t)q8_li * (size_t)D * H;
      size_t q8scDH = (size_t)q8_li * scaleCount((size_t)H * D, gs);
      if (v3) readPrefix(f);
      if (!readTensorQ8(f, w->w1_i8+q8lDH, w->w1_sc+q8scDH, H*D, gs)) {
        setLlmError("Failed reading gate(Q8) layer %d", l);
        f.close(); return false;
      }
    } else {
      size_t lDH = (size_t)l * (size_t)D * H;
      if (!readTensor(f, w->w1+lDH, H*D, qt, gs, false)) {
        setLlmError("Failed reading gate layer %d", l);
        f.close(); return false;
      }
    }

    // Up (w3) + Down (w2)
    if (isQ4Layer) {
      auto& off = w->q4_offsets[l];
      off.w3_data = q4_data_cursor;  off.w3_sc = q4_sc_cursor;
      if (v3) readPrefix(f);
      if (!readTensorQ4(f, w->q4_data + q4_data_cursor, w->q4_scales + q4_sc_cursor, H*D, gs)) {
        setLlmError("Failed reading up(Q4) layer %d", l);
        f.close(); return false;
      }
      q4_data_cursor += ((size_t)H * D + 1) / 2;  q4_sc_cursor += scaleCount((size_t)H * D, gs);

      off.w2_data = q4_data_cursor;  off.w2_sc = q4_sc_cursor;
      if (v3) readPrefix(f);
      if (!readTensorQ4(f, w->q4_data + q4_data_cursor, w->q4_scales + q4_sc_cursor, D*H, gs)) {
        setLlmError("Failed reading down(Q4) layer %d", l);
        f.close(); return false;
      }
      q4_data_cursor += ((size_t)D * H + 1) / 2;  q4_sc_cursor += scaleCount((size_t)D * H, gs);
    } else if (isQ8 || isMix) {
      size_t q8lDH  = (size_t)q8_li * (size_t)D * H;
      size_t q8scDH = (size_t)q8_li * scaleCount((size_t)D * H, gs);
      if (v3) readPrefix(f);
      if (!readTensorQ8(f, w->w3_i8+q8lDH, w->w3_sc+q8scDH, H*D, gs)) {
        setLlmError("Failed reading up(Q8) layer %d", l);
        f.close(); return false;
      }
      if (v3) readPrefix(f);
      if (!readTensorQ8(f, w->w2_i8+q8lDH, w->w2_sc+q8scDH, D*H, gs)) {
        setLlmError("Failed reading down(Q8) layer %d", l);
        f.close(); return false;
      }
    } else {
      size_t lDH = (size_t)l * (size_t)D * H;
      if (!readTensor(f, w->w3+lDH, H*D, qt, gs, false)
       || !readTensor(f, w->w2+lDH, D*H, qt, gs, false)) {
        setLlmError("Failed reading FFN layer %d", l);
        f.close(); return false;
      }
    }

    if (!isQ4Layer) q8_li++;
    DEBUG_LLM_LOADF("[LLM] Layer %d/%d loaded (%s)", l + 1, L,
                    isQ4Layer ? "Q4" : (isQ8 || isMix ? "Q8" : "FP32"));
  }

  if (isMix) {
    DEBUG_LLM_LOADF("[LLM] Q4 cursors final: data=%u/%u scales=%u/%u",
                    (unsigned)q4_data_cursor, (unsigned)w->q4_data_size,
                    (unsigned)(q4_sc_cursor * sizeof(float)), (unsigned)w->q4_scales_size);
  }

  // 3. Final norm (always FP32)
  if (v3) readPrefix(f);
  if (!readTensor(f, w->rms_final_weight, D, qt, gs, true)) {
    setLlmError("Failed reading final norm");
    f.close(); return false;
  }
  if (hasNormBias) {
    if (v3) readPrefix(f);
    if (!readTensor(f, w->rms_final_bias, D, qt, gs, true)) {
      setLlmError("Failed reading final norm bias");
      f.close(); return false;
    }
  }

  // 4. Tied flag + optional LM head
  uint8_t tied_check = 1;
  f.read(&tied_check, 1);
  if (tied_check != 0) {
    // Tied: LM head shares embedding weights
    if (isQ8 || isMix) {
      w->wcls_i8 = w->emb_i8;
      w->wcls_sc = w->emb_sc;
    } else {
      w->wcls = w->token_embedding_table;
    }
  } else {
    if (shared_weights) {
      setLlmError("Tied flag mismatch");
      f.close(); return false;
    }
    // LM head is always Q8 (never Q4), even in mixed mode
    if (isQ8 || isMix) {
      if (v3) readPrefix(f);
      if (!readTensorQ8(f, w->wcls_i8, w->wcls_sc, V * D, gs)) {
        setLlmError("Failed reading LM head (Q8)");
        f.close(); return false;
      }
    } else {
      if (!readTensor(f, w->wcls, V * D, qt, gs, false)) {
        setLlmError("Failed reading LM head");
        f.close(); return false;
      }
    }
  }

  f.close();
  DEBUG_LLM_LOADF("[LLM] Weights loaded successfully (%s)",
                  isMix ? "MIXED Q4/Q8" : (isQ8 ? "INT8" : "FP32"));

  spotCheckWeights(ctx);

  // ---- Allocate run state ----
  if (!allocateRunState(ctx)) return false;

  // ---- Prepackage per-layer weights for linear() ----
  if (!buildLayerTensors()) return false;

  return true;
}


#endif // ENABLE_ONDEVICE_LLM
