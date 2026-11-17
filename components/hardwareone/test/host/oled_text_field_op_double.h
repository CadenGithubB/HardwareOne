// Text-field double for harnesses whose canvas records draw operations rather
// than pixels (the logging list checks). It applies the shipping policy from
// OLED_TextFieldCore.h to decide what a field shows (the text when it fits, the
// ellipsis form when static, the first whole cells of a selected label at the
// marquee's start) and prints that through the canvas, so op-level bounds
// checks see the glyphs the production primitive would leave inside the field.
// Pixel clipping itself is covered by test_oled_text_field.py.
// A function template: it draws through whichever canvas double the harness
// declares (setCursor/print), declared before the production code is spliced.
#pragma once

#include <cstring>

#include "OLED_TextFieldCore.h"

template <class Display>
inline void oledTextFieldDraw(Display* display, int x, int y, int w, const char* text,
                              uint16_t, uint16_t, bool animate, const void*) {
  if (!display || w <= 0) return;
  char label[oled_text::kLabelMax];
  const size_t n = oled_text::flatten(text, label, sizeof(label));
  if (n == 0) return;
  char shown[oled_text::kLabelMax];
  const int cells = oled_text::glyphsThatFit(w);
  if (oled_text::textWidthPx(n) <= w) {
    memcpy(shown, label, n + 1);
  } else if (!animate) {
    oled_text::ellipsize(label, n, cells, shown, sizeof(shown));
  } else {
    const size_t keep = static_cast<size_t>(cells) < n ? static_cast<size_t>(cells) : n;
    memcpy(shown, label, keep);
    shown[keep] = '\0';
  }
  display->setCursor(x, y);
  display->print(shown);
}
