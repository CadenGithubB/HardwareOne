// Host double of the vendored Adafruit_GFX surface that OLED_TextField.cpp
// draws through: the pure-virtual drawPixel, width()/height(), and the classic
// 6x8 drawChar at size 1 exactly as Adafruit_GFX.cpp ships it (edge clipping,
// the cp437 remap above 175, transparent or opaque cells, the opaque sixth
// column). Requires glcdfont.c (the shipping font table) to be included first.
// Shared by the layout, health, logging-UI and text-field harnesses so every
// spliced list renderer draws labels through the production primitive.
#pragma once

#include <cassert>
#include <cstdint>

class Adafruit_GFX {
 public:
  Adafruit_GFX(int16_t w, int16_t h) : _width(w), _height(h) {}
  virtual ~Adafruit_GFX() {}
  virtual void drawPixel(int16_t x, int16_t y, uint16_t color) = 0;
  virtual void startWrite() {}
  virtual void endWrite() {}
  virtual void writePixel(int16_t x, int16_t y, uint16_t color) { drawPixel(x, y, color); }
  virtual void writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) {
    for (int16_t i = 0; i < h; ++i) writePixel(x, y + i, color);
  }
  int16_t width() const { return _width; }
  int16_t height() const { return _height; }
  // Adafruit_GFX::drawChar, classic font branch, size 1.
  void drawChar(int16_t x, int16_t y, unsigned char c, uint16_t color, uint16_t bg, uint8_t size) {
    assert(size == 1);
    if ((x >= _width) || (y >= _height) || ((x + 6 - 1) < 0) || ((y + 8 - 1) < 0)) return;
    if (c >= 176) c++;
    startWrite();
    for (int8_t i = 0; i < 5; i++) {
      uint8_t line = font[c * 5 + i];
      for (int8_t j = 0; j < 8; j++, line >>= 1) {
        if (line & 1) writePixel(x + i, y + j, color);
        else if (bg != color) writePixel(x + i, y + j, bg);
      }
    }
    if (bg != color) writeFastVLine(x + 5, y, 8, bg);
    endWrite();
  }

 protected:
  int16_t _width, _height;
};
