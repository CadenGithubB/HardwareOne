#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Small, allocation-free image transform used beneath the camera HAL. Source
// and destination are packed little-endian RGB565; their allocations must not
// overlap. Preserve aspect ratio by cropping the centre, then bilinear resize.
// Enlarging a delivered frame never adds detail to the physical source image.
// A mount rotation (0, 90, 180 or 270 degrees counter-clockwise) turns the
// dw x dh result so a sideways module delivers an upright frame; mirror and
// flip then act on that upright frame. A quarter turn writes dh x dw pixels.
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
constexpr bool validRotation(unsigned rotation) { return rotation % 90 == 0 && rotation < 360; }
// A quarter turn swaps the delivered width and height.
constexpr bool transposes(unsigned rotation) { return rotation == 90 || rotation == 270; }
inline uint16_t read(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
inline uint32_t mix(uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                    uint32_t fx, uint32_t fy) {
  return ((a * (256 - fx) + b * fx) * (256 - fy) +
          (c * (256 - fx) + d * fx) * fy + 32768) >> 16;
}
// One source axis of a bilinear sample: the two source indices and the Q8
// weight of the second, for output index i along an axis of the dw x dh frame.
struct Axis { uint32_t i0, i1, f; };
inline Axis axisFor(int64_t step, int64_t max, uint32_t origin, uint32_t extent,
                    bool reflect, uint32_t i) {
  int64_t p = step * i + step / 2 - 32768;
  if (p < 0) p = 0;
  if (p > max) p = max;
  if (reflect) p = max - p;
  const uint32_t i0 = origin + uint32_t(p >> 16);
  return {i0, i0 + (i0 + 1 < origin + extent ? 1 : 0), uint32_t(p >> 8) & 255};
}
inline uint16_t sample(const uint8_t* src, uint32_t sw, const Axis& x, const Axis& y) {
  const uint16_t a = read(src + (size_t(y.i0) * sw + x.i0) * 2);
  const uint16_t b = read(src + (size_t(y.i0) * sw + x.i1) * 2);
  const uint16_t c = read(src + (size_t(y.i1) * sw + x.i0) * 2);
  const uint16_t d = read(src + (size_t(y.i1) * sw + x.i1) * 2);
  return uint16_t(mix(a >> 11, b >> 11, c >> 11, d >> 11, x.f, y.f) << 11) |
      uint16_t(mix((a >> 5) & 63, (b >> 5) & 63, (c >> 5) & 63, (d >> 5) & 63, x.f, y.f) << 5) |
      uint16_t(mix(a & 31, b & 31, c & 31, d & 31, x.f, y.f));
}
inline bool resize(const uint8_t* src, size_t srcBytes, uint32_t sw, uint32_t sh,
                   uint8_t* dst, size_t dstBytes, uint32_t dw, uint32_t dh,
                   bool mirror = false, bool flip = false, unsigned rotation = 0) {
  Crop crop{};
  if (!validRotation(rotation) || !src || !dst || !cropFor(sw, sh, dw, dh, crop) ||
      srcBytes < size_t(sw) * sh * 2 || dstBytes < size_t(dw) * dh * 2) return false;
  const size_t inputBytes = size_t(sw) * sh * 2, outputBytes = size_t(dw) * dh * 2;
  const uintptr_t s = reinterpret_cast<uintptr_t>(src), d = reinterpret_cast<uintptr_t>(dst);
  if ((s <= d && d - s < inputBytes) || (d <= s && s - d < outputBytes)) return false;
  // Turning the frame before the caller's mirror/flip equals sampling the
  // unturned dw x dh frame with reflected axes: a quarter turn moves each
  // reflection onto the other axis, and a half turn reflects both.
  const bool transpose = transposes(rotation);
  const bool reflectX = transpose ? flip != (rotation == 90) : mirror != (rotation == 180);
  const bool reflectY = transpose ? mirror != (rotation == 270) : flip != (rotation == 180);
  if (!reflectX && !reflectY && !transpose && crop.width == dw && crop.height == dh) {
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
  auto column = [&](uint32_t i) { return axisFor(stepX, maxX, crop.x, crop.width, reflectX, i); };
  auto row = [&](uint32_t i) { return axisFor(stepY, maxY, crop.y, crop.height, reflectY, i); };
  auto write = [&](size_t index, uint16_t pixel) {
    dst[index * 2] = uint8_t(pixel); dst[index * 2 + 1] = uint8_t(pixel >> 8);
  };
  if (!transpose) {
    for (uint32_t y = 0; y < dh; ++y) {
      const Axis ay = row(y);
      for (uint32_t x = 0; x < dw; ++x) write(size_t(y) * dw + x, sample(src, sw, column(x), ay));
    }
    return true;
  }
  // The turned frame is dh wide and dw tall: its row y reads one source
  // column and its column x reads one source row. Walk it in tiles so a tile's
  // column reads stay within a few cache lines instead of striding the whole
  // source once per pixel.
  constexpr uint32_t kTile = 32;
  for (uint32_t ty = 0; ty < dw; ty += kTile) {
    const uint32_t yEnd = ty + kTile < dw ? ty + kTile : dw;
    for (uint32_t tx = 0; tx < dh; tx += kTile) {
      const uint32_t xEnd = tx + kTile < dh ? tx + kTile : dh;
      for (uint32_t y = ty; y < yEnd; ++y) {
        const Axis ax = column(y);
        for (uint32_t x = tx; x < xEnd; ++x) write(size_t(y) * dh + x, sample(src, sw, ax, row(x)));
      }
    }
  }
  return true;
}
}  // namespace camera_rgb565
