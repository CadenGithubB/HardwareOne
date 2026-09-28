#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// ESP-SR 2.5.5's srmodel_load() takes no length and trusts packed offsets.
// Validate file metadata before calling it. This is structural validation, not
// authentication or validation of the neural-network coefficients themselves.
namespace ESPSRModelPack {
constexpr size_t kMaxFileBytes = 16u * 1024u * 1024u;
constexpr uint32_t kMaxModels = 16;
constexpr uint32_t kMaxFilesPerModel = 32;

inline uint32_t u32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
         (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline bool reject(const char** error, const char* message) {
  if (error) *error = message;
  return false;
}
inline bool nameValid(const uint8_t* name) {
  return name[0] != 0 && memchr(name, 0, 32) != nullptr;
}

inline bool validate(const void* buffer, size_t bytes, const char** error = nullptr) {
  if (error) *error = nullptr;
  if (!buffer || bytes < 4 || bytes > kMaxFileBytes)
    return reject(error, "model bundle size is invalid");
  const auto* data = static_cast<const uint8_t*>(buffer);
  const uint32_t models = u32(data);
  if (!models || models > kMaxModels)
    return reject(error, "model count is invalid");
  size_t cursor = 4;
  size_t modelNames[kMaxModels]{};
  // Pass one: prove all table reads are bounded before examining payloads.
  for (uint32_t m = 0; m < models; ++m) {
    if (bytes - cursor < 36)
      return reject(error, "model table is truncated");
    if (!nameValid(data + cursor))
      return reject(error, "model name is empty or unterminated");
    for (uint32_t prior = 0; prior < m; ++prior)
      if (strcmp(reinterpret_cast<const char*>(data + cursor),
                 reinterpret_cast<const char*>(data + modelNames[prior])) == 0)
        return reject(error, "duplicate model name");
    modelNames[m] = cursor;
    const uint32_t files = u32(data + cursor + 32);
    cursor += 36;
    if (!files || files > kMaxFilesPerModel)
      return reject(error, "model file count is invalid");
    if (files > (bytes - cursor) / 40)
      return reject(error, "file table is truncated");
    bool hasInfo = false;
    for (uint32_t f = 0; f < files; ++f) {
      const size_t at = cursor + size_t(f) * 40;
      if (!nameValid(data + at))
        return reject(error, "file name is empty or unterminated");
      const char* name = reinterpret_cast<const char*>(data + at);
      for (uint32_t prior = 0; prior < f; ++prior)
        if (strcmp(name, reinterpret_cast<const char*>(data + cursor + size_t(prior) * 40)) == 0)
          return reject(error, "duplicate model file name");
      if (strcmp(name, "_MODEL_INFO_") == 0) hasInfo = true;
      const uint32_t offset = u32(data + at + 32);
      const uint32_t length = u32(data + at + 36);
      // Vendor uses signed int for both fields. The bundle cap also bounds
      // its allocations and leaves get_model_info(size + 1) representable.
      if (!length || offset > bytes || length > bytes - offset)
        return reject(error, "model payload is outside the bundle");
    }
    // The auxiliary FST table is packed as a model but has no metadata file.
    if (!hasInfo && strcmp(reinterpret_cast<const char*>(data + modelNames[m]), "fst") != 0)
      return reject(error, "model information is missing");
    cursor += size_t(files) * 40;
  }
  const size_t headerEnd = cursor;
  size_t payloadEnd = headerEnd;
  cursor = 4;
  // The official packer appends each payload in table order with no padding.
  // Requiring that layout rejects header references, overlaps and hidden tails.
  for (uint32_t m = 0; m < models; ++m) {
    const uint32_t files = u32(data + cursor + 32);
    cursor += 36;
    for (uint32_t f = 0; f < files; ++f, cursor += 40) {
      const uint32_t offset = u32(data + cursor + 32);
      const uint32_t length = u32(data + cursor + 36);
      if (offset != payloadEnd)
        return reject(error, "model payload layout is inconsistent");
      payloadEnd += length;  // Bounds established above; offset == payloadEnd.
    }
  }
  if (payloadEnd != bytes)
    return reject(error, "model bundle has trailing data");
  return true;
}
}  // namespace ESPSRModelPack
