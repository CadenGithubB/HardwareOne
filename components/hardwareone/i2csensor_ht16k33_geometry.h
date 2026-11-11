#pragma once
#include <stdint.h>

// Map the user's canvas to HT16K33 RAM (eight rows of sixteen bits).
// In square mode rotation stays within the chosen physical half.
inline bool ht16k33MapPixel(int x, int y, int rotation, bool square,
                           int panel, int& column, int& row) {
  const int width = square ? 8 : ((rotation & 1) ? 16 : 8);
  const int height = square ? 8 : ((rotation & 1) ? 8 : 16);
  if (rotation < 0 || rotation > 3 || panel < 0 || panel > 1 ||
      x < 0 || y < 0 || x >= width || y >= height) return false;
  const int longSide = square ? 8 : 16;
  switch (rotation) {
    case 0: column = y; row = 7 - x; break;
    case 1: column = x; row = y; break;
    case 2: column = longSide - 1 - y; row = x; break;
    default: column = longSide - 1 - x; row = 7 - y; break;
  }
  if (square) column += panel * 8;
  return true;
}
