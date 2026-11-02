#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <algorithm>

// Presentation only: bounded RGB888 -> aspect-fit, one-bit MSB-first rows.
// No camera settings, driver buffers, display hardware or temporal flashing.
namespace camera_mono {
struct Tone { int low = 0, high = 255; bool initialized = false; };
struct Image { unsigned x = 0, y = 0, width = 0, height = 0, whitePixels = 0; };
inline bool render(const uint8_t* rgb, unsigned width, unsigned height,
                   size_t stride, size_t size, uint8_t* shades, size_t shadeCapacity,
                   uint8_t* bits, size_t bitCapacity, unsigned outWidth, unsigned outHeight,
                   Tone& tone, int contrast, bool dither, Image& image) {
  if (!rgb || !shades || !bits || !width || !height || width > 1600 || height > 1600 ||
      !outWidth || !outHeight || outWidth > 256 || outHeight > 256 ||
      stride < size_t(width) * 3 || size < size_t(width) * 3 ||
      size_t(height - 1) > (size - size_t(width) * 3) / stride ||
      size_t(outWidth) * outHeight > shadeCapacity ||
      size_t((outWidth + 7) / 8) * outHeight > bitCapacity || contrast < -2 || contrast > 2)
    return false;
  image = {};
  if (uint64_t(outWidth) * height <= uint64_t(outHeight) * width) {
    image.width = outWidth;
    image.height = std::max(1u, unsigned(uint64_t(height) * outWidth / width));
  } else {
    image.height = outHeight;
    image.width = std::max(1u, unsigned(uint64_t(width) * outHeight / height));
  }
  image.x = (outWidth - image.width) / 2;
  image.y = (outHeight - image.height) / 2;
  memset(shades, 0, size_t(outWidth) * outHeight);
  memset(bits, 0, size_t((outWidth + 7) / 8) * outHeight);
  uint32_t histogram[256]{};
  for (unsigned y = 0; y < image.height; ++y) {
    const unsigned top = uint64_t(y) * height / image.height;
    const unsigned bottom = std::min(height, std::max(top + 1, unsigned(uint64_t(y + 1) * height / image.height)));
    for (unsigned x = 0; x < image.width; ++x) {
      const unsigned left = uint64_t(x) * width / image.width;
      const unsigned right = std::min(width, std::max(left + 1, unsigned(uint64_t(x + 1) * width / image.width)));
      uint64_t sum = 0;
      unsigned count = 0;
      for (unsigned sy = top; sy < bottom; ++sy) {
        const auto* row = rgb + size_t(sy) * stride;
        for (unsigned sx = left; sx < right; ++sx) {
          const auto* p = row + sx * 3;
          sum += (77u * p[0] + 150u * p[1] + 29u * p[2] + 128u) >> 8;
          ++count;
        }
      }
      const auto gray = uint8_t(sum / count);
      shades[size_t(y + image.y) * outWidth + x + image.x] = gray;
      ++histogram[gray];
    }
  }
  const unsigned tail = std::max(1u, image.width * image.height / 100u);
  unsigned seen = 0;
  int low = 0, high = 255;
  for (; low < 255; ++low) if ((seen += histogram[low]) >= tail) break;
  seen = 0;
  for (; high > 0; --high) if ((seen += histogram[high]) >= tail) break;
  // Stretch useful scene contrast, without amplifying a flat/noisy dark field.
  if (high - low < 32) { low = 0; high = 255; }
  if (high - low < 96) {
    const int middle = (low + high) / 2;
    low = std::max(0, middle - 48);
    high = std::min(255, low + 96);
    low = std::max(0, high - 96);
  }
  if (!tone.initialized) { tone.low = low; tone.high = high; tone.initialized = true; }
  else { tone.low = (tone.low * 3 + low) / 4; tone.high = (tone.high * 3 + high) / 4; }
  static constexpr uint8_t bayer[16] = {0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
  const unsigned rowBytes = (outWidth + 7) / 8;
  for (unsigned y = image.y; y < image.y + image.height; ++y) {
    for (unsigned x = image.x; x < image.x + image.width; ++x) {
      int gray = (int(shades[size_t(y) * outWidth + x]) - tone.low) * 255 /
                 std::max(1, tone.high - tone.low);
      gray = 128 + (gray - 128) * (256 + contrast * 64) / 256;
      gray = std::max(0, std::min(255, gray));
      const int threshold = dither ? bayer[(y & 3) * 4 + (x & 3)] * 16 + 8 : 128;
      if (gray >= threshold) {
        bits[size_t(y) * rowBytes + x / 8] |= uint8_t(0x80u >> (x & 7));
        ++image.whitePixels;
      }
    }
  }
  return true;
}
} // namespace camera_mono
