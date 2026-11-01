#pragma once
#include <stdint.h>
#include <stddef.h>

#define WAVESHARE_CANVAS_WIDTH 128
#define WAVESHARE_CANVAS_HEIGHT 88

namespace waveshare_display {
constexpr int kPanelWidth = 720, kPanelHeight = 1280;
constexpr int kCanvasWidth = WAVESHARE_CANVAS_WIDTH;
constexpr int kCanvasHeight = WAVESHARE_CANVAS_HEIGHT;
constexpr size_t kCanvasBytes = kCanvasWidth * kCanvasHeight / 8;
struct Point { int x, y; };
struct Viewport { int x, y, scale; };
constexpr Viewport viewport(bool landscape) {
  return landscape ? Viewport{0, 8, 8} : Viewport{40, 380, 5};
}
// Landscape surface is 1280x720. Native framebuffer/touch remain 720x1280.
constexpr Point toPanel(int x, int y, bool landscape) {
  return landscape ? Point{kPanelWidth - 1 - y, x} : Point{x, y};
}
constexpr Point fromPanel(int x, int y, bool landscape) {
  return landscape ? Point{y, kPanelWidth - 1 - x} : Point{x, y};
}
inline void rasterize(const uint8_t* canvas, uint16_t* pixels,
                      bool landscape, bool inverted) {
  const auto v = viewport(landscape);
  for (int y = 0; y < kCanvasHeight; ++y)
    for (int x = 0; x < kCanvasWidth; ++x) {
      const bool on = ((canvas[x + (y / 8) * kCanvasWidth] >> (y & 7)) & 1) != 0;
      const uint16_t color = on != inverted ? 0xffff : 0;
      for (int dy = 0; dy < v.scale; ++dy)
        for (int dx = 0; dx < v.scale; ++dx) {
          const auto p = toPanel(v.x + x*v.scale + dx, v.y + y*v.scale + dy, landscape);
          pixels[p.y*kPanelWidth + p.x] = color;
        }
    }
}
} // namespace waveshare_display
