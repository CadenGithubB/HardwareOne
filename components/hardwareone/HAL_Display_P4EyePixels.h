#pragma once
#include <stdint.h>

namespace p4eye_display {
constexpr int kWidth = 240;
constexpr int kHeight = 240;
constexpr int kCanvasWidth = 128;
constexpr int kCanvasHeight = 64;
constexpr int kImageWidth = 192;   // 1.5x, retaining the monochrome OLED layout.
constexpr int kImageHeight = 96;
constexpr int kImageLeft = (kWidth - kImageWidth) / 2;
constexpr int kImageTop = (kHeight - kImageHeight) / 2;
constexpr int kStripRows = 8;

// Nearest-neighbor scaling preserves aspect ratio without introducing gray.
// The centered 1.5x image leaves black borders on all four sides. White and
// black RGB565 are byte-order invariant; no SPI endian conversion is needed.
inline void rasterize(const uint8_t* canvas, uint16_t* output,
                      int startY, int rows, bool inverted) {
  for (int row = 0; row < rows; ++row) {
    const int y = startY + row - kImageTop;
    for (int x = 0; x < kWidth; ++x) {
      bool lit = false;
      const int imageX = x - kImageLeft;
      if (canvas && imageX >= 0 && imageX < kImageWidth && y >= 0 && y < kImageHeight) {
        const int sourceY = y * kCanvasHeight / kImageHeight;
        const int sourceX = imageX * kCanvasWidth / kImageWidth;
        lit = ((canvas[(sourceY / 8) * kCanvasWidth + sourceX] >>
                (sourceY & 7)) & 1) != 0;
        lit = lit != inverted;
      }
      output[row * kWidth + x] = lit ? 0xffff : 0;
    }
  }
}
}
