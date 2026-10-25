#include "../../HAL_Display_P4EyePixels.h"
#include <array>
#include <cassert>
#include <cstdio>

using namespace p4eye_display;

namespace {
constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kWhite = 0xffff;
using GuardedFrame = std::array<uint16_t, 240 * 240 + 2>;
void checkGuards(const GuardedFrame& frame) {
  assert(frame.front() == 0x1234);
  assert(frame.back() == 0xabcd);
}
}

int main() {
  static_assert(kCanvasWidth == 128 && kCanvasHeight == 128);
  static_assert(kImageWidth == 240 && kImageHeight == 240);
  std::array<uint8_t, kCanvasWidth * ((kCanvasHeight + 7) / 8)> canvas{};
  GuardedFrame guarded{};
  guarded.front() = 0x1234;
  guarded.back() = 0xabcd;
  auto* panel = guarded.data() + 1;

  // All four logical corners reach the physical edges. The first source pixel
  // expands to 2x2; the last to 1x1. No cropping or page-order inversion.
  canvas[0] = 1;
  canvas[127] = 1;
  canvas[15 * 128] = 0x80;
  canvas.back() = 0x80;
  rasterize(canvas.data(), panel, 0, 240, false);
  assert(panel[0] == kWhite && panel[241] == kWhite);
  assert(panel[2] == kBlack && panel[2 * 240] == kBlack);
  assert(panel[238] == kBlack && panel[239] == kWhite);
  assert(panel[238 * 240] == kBlack && panel[239 * 240] == kWhite);
  assert(panel[240 * 240 - 1] == kWhite);
  checkGuards(guarded);

  // The first byte of page 8 is logical y=64, in the NEW lower half.
  canvas.fill(0);
  canvas[8 * 128] = 1;
  rasterize(canvas.data(), panel, 0, 240, false);
  assert(panel[119 * 240] == kBlack);
  assert(panel[120 * 240] == kWhite && panel[121 * 240 + 1] == kWhite);
  assert(panel[122 * 240] == kBlack);

  // DMA stripes must produce exactly the same frame, including inversion.
  GuardedFrame striped{};
  striped.front() = 0x1234;
  striped.back() = 0xabcd;
  rasterize(canvas.data(), panel, 0, 240, true);
  for (int y = 0; y < 240; y += kStripRows) {
    rasterize(canvas.data(), striped.data() + 1 + y * 240, y, kStripRows, true);
  }
  for (int i = 0; i < 240 * 240; ++i) assert(panel[i] == striped[i + 1]);
  assert(panel[120 * 240] == kBlack);
  checkGuards(guarded);
  checkGuards(striped);

  // Full white fills every physical pixel. A missing canvas stays dark,
  // including when inverted (failed initialization must not dereference it).
  canvas.fill(0xff);
  rasterize(canvas.data(), panel, 0, 240, false);
  for (int i = 0; i < 240 * 240; ++i) assert(panel[i] == kWhite);
  rasterize(nullptr, panel, 0, 240, true);
  for (int i = 0; i < 240 * 240; ++i) assert(panel[i] == kBlack);

  // Eight source pixels map to fifteen panel pixels in BOTH axes. This checks
  // the 1.875x pattern, monochrome output, and all sixteen packed source pages.
  canvas.fill(0);
  for (int y = 0; y < 128; ++y) {
    for (int x = 0; x < 128; ++x) {
      if (((x / 8 + y / 8) & 1) == 0)
        canvas[(y / 8) * 128 + x] |= static_cast<uint8_t>(1u << (y & 7));
    }
  }
  for (bool inverted : {false, true}) {
    rasterize(canvas.data(), panel, 0, 240, inverted);
    for (int y = 0; y < 240; ++y) {
      for (int x = 0; x < 240; ++x) {
        const bool white = (((x / 15 + y / 15) & 1) == 0) != inverted;
        assert(panel[y * 240 + x] == (white ? kWhite : kBlack));
      }
    }
    checkGuards(guarded);
  }
  std::puts("P4-EYE display pixel tests passed");
}
