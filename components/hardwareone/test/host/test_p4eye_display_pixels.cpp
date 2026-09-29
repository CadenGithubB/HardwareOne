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

void checkBlackBorders(const uint16_t* panel) {
  for (int y = 0; y < 240; ++y) {
    for (int x = 0; x < 240; ++x) {
      if (x < 24 || x >= 216 || y < 72 || y >= 168) {
        assert(panel[y * 240 + x] == kBlack);
      }
    }
  }
}
}

int main() {
  std::array<uint8_t, 1024> canvas{};
  GuardedFrame guarded{};
  guarded.front() = 0x1234;
  guarded.back() = 0xabcd;
  auto* panel = guarded.data() + 1;

  // Four logical corner pixels. The first pixel expands to a 2x2 block; the
  // last row/column occupy one pixel at 1.5x. All four corners remain visible
  // inside the centered 192x96 image, with no cropping or OLED page inversion.
  canvas[0] = 1;
  canvas[127] = 1;
  canvas[7 * 128] = 0x80;
  canvas[1023] = 0x80;
  rasterize(canvas.data(), panel, 0, 240, false);
  assert(panel[72 * 240 + 24] == kWhite);
  assert(panel[73 * 240 + 25] == kWhite);
  assert(panel[72 * 240 + 26] == kBlack);
  assert(panel[74 * 240 + 24] == kBlack);
  assert(panel[72 * 240 + 214] == kBlack);
  assert(panel[72 * 240 + 215] == kWhite);
  assert(panel[73 * 240 + 215] == kWhite);
  assert(panel[166 * 240 + 24] == kBlack);
  assert(panel[167 * 240 + 24] == kWhite);
  assert(panel[167 * 240 + 25] == kWhite);
  assert(panel[167 * 240 + 215] == kWhite);
  checkBlackBorders(panel);
  checkGuards(guarded);

  // The first byte of the next SSD1306 page means y=8, not a row-major byte.
  canvas.fill(0);
  canvas[128] = 1;
  rasterize(canvas.data(), panel, 0, 240, false);
  assert(panel[83 * 240 + 24] == kBlack);
  assert(panel[84 * 240 + 24] == kWhite);
  assert(panel[85 * 240 + 25] == kWhite);
  assert(panel[86 * 240 + 24] == kBlack);

  // Inversion covers only the image; all four borders stay dark. Update only
  // y=72..167 in the same 8-row DMA stripes used by display().
  GuardedFrame stripedGuarded{};
  stripedGuarded.front() = 0x1234;
  stripedGuarded.back() = 0xabcd;
  auto* stripes = stripedGuarded.data() + 1;
  rasterize(canvas.data(), panel, 0, 240, true);
  for (int y = 72; y < 168; y += 8) {
    rasterize(canvas.data(), stripes + y * 240, y, 8, true);
  }
  for (int i = 0; i < 240 * 240; ++i) assert(panel[i] == stripes[i]);
  assert(panel[72 * 240 + 24] == kWhite);
  assert(panel[84 * 240 + 24] == kBlack);
  checkBlackBorders(panel);
  checkGuards(guarded);
  checkGuards(stripedGuarded);

  // A full white source occupies exactly 192x96 pixels and leaves symmetric
  // 24-pixel side borders and 72-pixel top/bottom borders.
  canvas.fill(0xff);
  rasterize(canvas.data(), panel, 0, 240, false);
  int whitePixels = 0;
  for (int y = 0; y < 240; ++y) {
    for (int x = 0; x < 240; ++x) {
      const bool inImage = x >= 24 && x < 216 && y >= 72 && y < 168;
      assert(panel[y * 240 + x] == (inImage ? kWhite : kBlack));
      whitePixels += panel[y * 240 + x] == kWhite;
    }
  }
  assert(whitePixels == 192 * 96);

  // Every 2x2 source square becomes a 3x3 panel square. This pattern checks
  // equal scaling on both axes, the alternating 2/1 pixel expansion, and
  // strictly black/white output for mixed content, including inverted frames.
  canvas.fill(0);
  for (int y = 0; y < 64; ++y) {
    for (int x = 0; x < 128; ++x) {
      if (((x / 2 + y / 2) & 1) == 0) {
        canvas[(y / 8) * 128 + x] |= static_cast<uint8_t>(1u << (y & 7));
      }
    }
  }
  for (bool inverted : {false, true}) {
    rasterize(canvas.data(), panel, 0, 240, inverted);
    for (int y = 0; y < 96; ++y) {
      for (int x = 0; x < 192; ++x) {
        const bool white = (((x / 3 + y / 3) & 1) == 0) != inverted;
        assert(panel[(y + 72) * 240 + x + 24] == (white ? kWhite : kBlack));
      }
    }
    checkBlackBorders(panel);
    checkGuards(guarded);
  }
  std::puts("P4-EYE display pixel tests passed");
}
