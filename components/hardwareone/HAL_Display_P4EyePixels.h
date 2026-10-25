#pragma once
#include <stdint.h>

#define P4EYE_CANVAS_WIDTH 128
#define P4EYE_CANVAS_HEIGHT 128

namespace p4eye_display {
constexpr int kWidth = 240;
constexpr int kHeight = 240;
constexpr int kCanvasWidth = P4EYE_CANVAS_WIDTH;
constexpr int kCanvasHeight = P4EYE_CANVAS_HEIGHT;
constexpr int kImageWidth = kWidth;   // 1.875x on both axes; keep 128px text wrapping.
constexpr int kImageHeight = kHeight;
constexpr int kImageLeft = (kWidth - kImageWidth) / 2;
constexpr int kImageTop = (kHeight - kImageHeight) / 2;
constexpr int kStripRows = 8;

// Nearest-neighbor scaling preserves aspect ratio without introducing gray.
// The square canvas fills the panel. Fractional scaling duplicates source
// pixels unevenly (as the previous 1.5x path did). White and
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
