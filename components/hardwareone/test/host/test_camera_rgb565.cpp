#include "CameraRgb565.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>
using namespace camera_rgb565;
static void put(std::vector<uint8_t>& p, size_t i, uint16_t value) {
  p.at(i * 2) = value & 255; p.at(i * 2 + 1) = value >> 8;
}
int main() {
  Crop c{};
  assert(cropFor(1280, 720, 640, 480, c));
  assert(c.x == 160 && c.y == 0 && c.width == 960 && c.height == 720);
  assert(cropFor(1280, 720, 240, 240, c));
  assert(c.x == 280 && c.y == 0 && c.width == 720 && c.height == 720);
  assert(!cropFor(0, 720, 1, 1, c));
  assert(!cropFor(4097, 1, 1, 1, c));
  assert(qualityToJpeg(-1) == 0 && qualityToJpeg(64) == 0);
  assert(qualityToJpeg(0) == 100 && qualityToJpeg(63) == 1 && qualityToJpeg(12) == 81);
  for (int q = 0; q < 63; ++q) assert(qualityToJpeg(q) > qualityToJpeg(q + 1));
  std::vector<uint8_t> src(4 * 3 * 2), dst(src.size() + 8, 0xa5);
  for (size_t i = 0; i < 12; ++i) put(src, i, uint16_t(0x0821 * i));
  assert(resize(src.data(), src.size(), 4, 3, dst.data(), dst.size(), 4, 3));
  for (size_t i = 0; i < src.size(); ++i) assert(src[i] == dst[i]);
  for (size_t i = src.size(); i < dst.size(); ++i) assert(dst[i] == 0xa5);
  for (unsigned mode = 0; mode < 4; ++mode) {
    assert(resize(src.data(), src.size(), 4, 3, dst.data(), dst.size(), 4, 3, mode & 1, mode & 2));
    for (size_t y = 0; y < 3; ++y) for (size_t x = 0; x < 4; ++x) {
      const size_t sx = mode & 1 ? 3 - x : x, sy = mode & 2 ? 2 - y : y;
      assert(read(dst.data() + (y * 4 + x) * 2) == read(src.data() + (sy * 4 + sx) * 2));
    }
  }
  // A mount turn rotates the 4x3 result counter-clockwise first; the caller's
  // mirror and flip then act on that upright frame. Expected pixels come from
  // an independent two-step model: turn, then reflect the turned frame.
  for (unsigned rotation = 0; rotation < 360; rotation += 90) {
    const bool turned = rotation == 90 || rotation == 270;
    const size_t fw = turned ? 3 : 4, fh = turned ? 4 : 3;
    auto turnedPixel = [&](size_t x, size_t y) {  // source pixel shown at (x, y) of the turned frame
      if (rotation == 90) return read(src.data() + (x * 4 + (3 - y)) * 2);
      if (rotation == 180) return read(src.data() + ((2 - y) * 4 + (3 - x)) * 2);
      if (rotation == 270) return read(src.data() + ((2 - x) * 4 + y) * 2);
      return read(src.data() + (y * 4 + x) * 2);
    };
    for (unsigned mode = 0; mode < 4; ++mode) {
      std::fill(dst.begin(), dst.end(), 0xa5);
      assert(resize(src.data(), src.size(), 4, 3, dst.data(), dst.size(), 4, 3, mode & 1, mode & 2, rotation));
      for (size_t y = 0; y < fh; ++y) for (size_t x = 0; x < fw; ++x) {
        const size_t tx = mode & 1 ? fw - 1 - x : x, ty = mode & 2 ? fh - 1 - y : y;
        assert(read(dst.data() + (y * fw + x) * 2) == turnedPixel(tx, ty));
      }
      for (size_t i = src.size(); i < dst.size(); ++i) assert(dst[i] == 0xa5);
    }
  }
  assert(!resize(src.data(), src.size(), 4, 3, dst.data(), dst.size(), 4, 3, false, false, 45));
  assert(!resize(src.data(), src.size(), 4, 3, dst.data(), dst.size(), 4, 3, false, false, 360));
  // Tile edges: a 1:1 turn of a frame larger than one tile and not a multiple
  // of the tile width must place every coordinate-coded pixel.
  {
    const uint32_t w = 50, h = 37;
    std::vector<uint8_t> coded(w * h * 2), turned(w * h * 2 + 4, 0x3c);
    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) put(coded, y * w + x, uint16_t(y * 64 + x));
    assert(resize(coded.data(), coded.size(), w, h, turned.data(), turned.size() - 4, w, h, false, false, 90));
    for (uint32_t y = 0; y < w; ++y) for (uint32_t x = 0; x < h; ++x)
      assert(read(turned.data() + (y * h + x) * 2) == uint16_t(x * 64 + (w - 1 - y)));
    for (size_t i = w * h * 2; i < turned.size(); ++i) assert(turned[i] == 0x3c);
    assert(resize(coded.data(), coded.size(), w, h, turned.data(), turned.size() - 4, w, h, false, false, 270));
    for (uint32_t y = 0; y < w; ++y) for (uint32_t x = 0; x < h; ++x)
      assert(read(turned.data() + (y * h + x) * 2) == uint16_t((h - 1 - x) * 64 + y));
  }
  // A scaled quarter turn: the right half of a wide scene becomes the top half.
  {
    std::vector<uint8_t> halves(16 * 8 * 2), tall(8 * 4 * 2);
    for (size_t y = 0; y < 8; ++y) for (size_t x = 0; x < 16; ++x) put(halves, y * 16 + x, x < 8 ? 0xf800 : 0x001f);
    assert(resize(halves.data(), halves.size(), 16, 8, tall.data(), tall.size(), 8, 4, false, false, 90));
    for (size_t y = 0; y < 8; ++y) for (size_t x = 0; x < 4; ++x)
      assert(read(tall.data() + (y * 4 + x) * 2) == (y < 4 ? 0x001f : 0xf800));
    assert(resize(halves.data(), halves.size(), 16, 8, tall.data(), tall.size(), 8, 4, false, true, 90));
    for (size_t y = 0; y < 8; ++y) for (size_t x = 0; x < 4; ++x)
      assert(read(tall.data() + (y * 4 + x) * 2) == (y < 4 ? 0xf800 : 0x001f));
  }
  assert(!resize(src.data(), src.size() - 1, 4, 3, dst.data(), dst.size(), 4, 3));
  assert(!resize(src.data(), src.size(), 4, 3, dst.data(), src.size() - 1, 4, 3));
  assert(!resize(src.data(), src.size(), 4, 3, src.data(), src.size(), 4, 3));
  assert(!resize(src.data(), src.size(), 4, 3, src.data() + 2, src.size(), 4, 3));
  assert(!resize(nullptr, src.size(), 4, 3, dst.data(), dst.size(), 4, 3));
  // Centre crop must ignore the outside columns rather than stretch a wide image.
  std::vector<uint8_t> bars(8 * 4 * 2), square(4 * 4 * 2);
  for (size_t y = 0; y < 4; ++y) for (size_t x = 0; x < 8; ++x)
    put(bars, y * 8 + x, x < 2 ? 0xf800 : x >= 6 ? 0x001f : 0x07e0);
  assert(resize(bars.data(), bars.size(), 8, 4, square.data(), square.size(), 4, 4));
  for (size_t i = 0; i < 16; ++i) assert(read(square.data() + i * 2) == 0x07e0);
  // Bilinear averaging on a 2x2 source, including all three RGB565 channels.
  std::vector<uint8_t> corners(8), average(2);
  put(corners, 0, 0xf800); put(corners, 1, 0x07e0); put(corners, 2, 0x001f); put(corners, 3, 0xffff);
  assert(resize(corners.data(), corners.size(), 2, 2, average.data(), average.size(), 1, 1));
  assert(read(average.data()) == uint16_t((16 << 11) | (32 << 5) | 16));
  // Every deliverable camera shape must preserve a constant colour, including
  // awkward MCU-edge dimensions; canaries catch source/destination overruns.
  const unsigned shapes[][2] = {{96,96},{160,120},{176,144},{240,176},{240,240},
                                {320,240},{400,296},{640,480},{800,600},{1280,720}};
  std::vector<uint8_t> native(1280 * 720 * 2);
  for (size_t i = 0; i < native.size() / 2; ++i) put(native, i, 0x1234);
  for (const auto& shape : shapes) {
    const size_t n = size_t(shape[0]) * shape[1] * 2;
    std::vector<uint8_t> output(n + 16, 0x5a);
    for (unsigned rotation = 0; rotation < 360; rotation += 90) {
      assert(resize(native.data(), native.size(), 1280, 720, output.data() + 8, n, shape[0], shape[1],
                    false, false, rotation));
      for (size_t i = 0; i < n / 2; ++i) assert(read(output.data() + 8 + i * 2) == 0x1234);
      for (size_t i = 0; i < 8; ++i) assert(output[i] == 0x5a && output[n + 8 + i] == 0x5a);
    }
  }
  puts("Camera RGB565 crop/resize, orientation, bounded writes and quality semantics passed");
}
