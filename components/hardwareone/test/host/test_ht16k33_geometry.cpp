#include "../../HT16K33_Geometry.h"
#include <assert.h>
#include <stdint.h>
#include <initializer_list>

int main() {
  for (bool square : {false, true}) {
    for (int panel = 0; panel < 2; ++panel) {
      for (int rotation = 0; rotation < 4; ++rotation) {
        const int width = square ? 8 : ((rotation & 1) ? 16 : 8);
        const int height = square ? 8 : ((rotation & 1) ? 8 : 16);
        uint16_t rows[8] = {};
        for (int y = -1; y <= height; ++y) {
          for (int x = -1; x <= width; ++x) {
            int column = -1, row = -1;
            const bool valid = ht16k33MapPixel(x, y, rotation, square, panel, column, row);
            assert(valid == (x >= 0 && x < width && y >= 0 && y < height));
            if (!valid) continue;
            assert(column >= 0 && column < 16 && row >= 0 && row < 8);
            if (square) assert(column / 8 == panel); // rotation cannot switch squares
            const uint16_t bit = uint16_t(1U << column);
            assert(!(rows[row] & bit)); // one-to-one pixel mapping
            rows[row] |= bit;
          }
        }
        for (auto row : rows) assert(row == (square ? (panel ? 0xff00 : 0x00ff) : 0xffff));
      }
    }
  }
  int column, row;
  assert(ht16k33MapPixel(0, 0, 1, false, 0, column, row) && column == 0 && row == 0);
  assert(ht16k33MapPixel(15, 7, 1, false, 0, column, row) && column == 15 && row == 7);
  assert(ht16k33MapPixel(0, 0, 3, true, 1, column, row) && column == 15 && row == 7);
  assert(!ht16k33MapPixel(0, 0, 4, true, 0, column, row));
  assert(!ht16k33MapPixel(0, 0, 1, true, 2, column, row));
}
