#pragma once

// Geometry shared by monochrome layouts. No driver dependency: host tests can
// exercise the same capacity calculation used by drawing and navigation.
namespace oled_layout {
constexpr int kHeaderHeight = 10;
constexpr int kFooterHeight = 10;
constexpr int kContentGap = 1;
constexpr int kOledContentHeight = 64 - kHeaderHeight - kFooterHeight - kContentGap;

// baselineRows preserves a screen's established 64px layout, including any
// deliberate spare space. Taller canvases use all remaining complete rows.
// reservedHeight accounts for status/title rows and any top/bottom padding.
constexpr int visibleRows(int contentHeight, int rowHeight,
                          int reservedHeight = 0, int baselineRows = 0) {
  if (rowHeight <= 0) return 1;
  if (baselineRows > 0 && contentHeight == kOledContentHeight) return baselineRows;
  const int rows = (contentHeight - reservedHeight) / rowHeight;
  return rows > 0 ? rows : 1;
}
}
