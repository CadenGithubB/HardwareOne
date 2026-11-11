#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Small, allocation-free image transform used beneath the camera HAL. Source
// and destination are packed little-endian RGB565; their allocations must not
// overlap. Preserve aspect ratio by cropping the centre, then bilinear resize.
// Enlarging a delivered frame never adds detail to the physical source image.
namespace camera_rgb565 {
struct Crop { uint32_t x, y, width, height; };
inline bool cropFor(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh, Crop& c) {
  if (!sw || !sh || !dw || !dh || sw > 4096 || sh > 4096 || dw > 4096 || dh > 4096) return false;
  c = {0, 0, sw, sh};
  if (uint64_t(sw) * dh > uint64_t(sh) * dw) c.width = uint32_t(uint64_t(sh) * dw / dh);
  else c.height = uint32_t(uint64_t(sw) * dh / dw);
  if (!c.width || !c.height) return false;
  c.x = (sw - c.width) / 2; c.y = (sh - c.height) / 2;
  return true;
}
inline int qualityToJpeg(int quality) {
  return quality < 0 || quality > 63 ? 0 : 100 - (quality * 99 + 31) / 63;
}
inline uint16_t read(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
inline uint32_t mix(uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                    uint32_t fx, uint32_t fy) {
  return ((a * (256 - fx) + b * fx) * (256 - fy) +
          (c * (256 - fx) + d * fx) * fy + 32768) >> 16;
}
inline bool resize(const uint8_t* src, size_t srcBytes, uint32_t sw, uint32_t sh,
                   uint8_t* dst, size_t dstBytes, uint32_t dw, uint32_t dh,
                   bool mirror = false, bool flip = false) {
  Crop crop{};
  if (!src || !dst || !cropFor(sw, sh, dw, dh, crop) ||
      srcBytes < size_t(sw) * sh * 2 || dstBytes < size_t(dw) * dh * 2) return false;
  const size_t inputBytes = size_t(sw) * sh * 2, outputBytes = size_t(dw) * dh * 2;
  const uintptr_t s = reinterpret_cast<uintptr_t>(src), d = reinterpret_cast<uintptr_t>(dst);
  if ((s <= d && d - s < inputBytes) || (d <= s && s - d < outputBytes)) return false;
  if (!mirror && !flip && crop.width == dw && crop.height == dh) {
    for (uint32_t y = 0; y < dh; ++y) {
      memcpy(dst + size_t(y) * dw * 2,
             src + (size_t(y + crop.y) * sw + crop.x) * 2, size_t(dw) * 2);
    }
    return true;
  }
  // Pixel-centre sampling. Q16 coordinates avoid floating point and allow
  // cheap incremental coordinates in the inner loop; interpolate at Q8.
  const int64_t stepX = (int64_t(crop.width) << 16) / dw;
  const int64_t stepY = (int64_t(crop.height) << 16) / dh;
  const int64_t maxX = int64_t(crop.width - 1) << 16;
  const int64_t maxY = int64_t(crop.height - 1) << 16;
  for (uint32_t y = 0; y < dh; ++y) {
    int64_t py = stepY * y + stepY / 2 - 32768;
    if (py < 0) py = 0;
    if (py > maxY) py = maxY;
    if (flip) py = maxY - py;
    const uint32_t y0 = crop.y + uint32_t(py >> 16);
    const uint32_t y1 = y0 + (y0 + 1 < crop.y + crop.height ? 1 : 0);
    const uint32_t fy = uint32_t(py >> 8) & 255;
    for (uint32_t x = 0; x < dw; ++x) {
      int64_t px = stepX * x + stepX / 2 - 32768;
      if (px < 0) px = 0;
      if (px > maxX) px = maxX;
      if (mirror) px = maxX - px;
      const uint32_t x0 = crop.x + uint32_t(px >> 16);
      const uint32_t x1 = x0 + (x0 + 1 < crop.x + crop.width ? 1 : 0);
      const uint32_t fx = uint32_t(px >> 8) & 255;
      const uint16_t a = read(src + (size_t(y0) * sw + x0) * 2);
      const uint16_t b = read(src + (size_t(y0) * sw + x1) * 2);
      const uint16_t c = read(src + (size_t(y1) * sw + x0) * 2);
      const uint16_t d = read(src + (size_t(y1) * sw + x1) * 2);
      const uint16_t pixel = uint16_t(mix(a >> 11, b >> 11, c >> 11, d >> 11, fx, fy) << 11) |
          uint16_t(mix((a >> 5) & 63, (b >> 5) & 63, (c >> 5) & 63, (d >> 5) & 63, fx, fy) << 5) |
          uint16_t(mix(a & 31, b & 31, c & 31, d & 31, fx, fy));
      const size_t offset = (size_t(y) * dw + x) * 2;
      dst[offset] = uint8_t(pixel); dst[offset + 1] = uint8_t(pixel >> 8);
    }
  }
  return true;
}
}  // namespace camera_rgb565
