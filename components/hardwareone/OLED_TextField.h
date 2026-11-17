// =============================================================================
// OLED_TextField — bounded one-row text fields for list rows
// =============================================================================
// The shared list renderers and custom row drawers put every label through
// oledTextFieldDraw() instead of print(): a label is confined to its own
// rectangle, intersected with the content area, so it can never run onto the
// next row, across a pane separator, over a fixed value column or under the
// scrollbar. Text that fits is drawn as it always was (same glyphs, same
// pixels). Text that does not fit ends in a "..." ellipsis, except the
// selected row's label, which reveals itself by scrolling left: it shows its
// beginning, waits a second, moves at 20 px/s, pauses at the end and repeats.
// Only the fields a frame actually draws animate; an idle screen requests no
// frames. Policy, timing and identity rules live in OLED_TextFieldCore.h.
#pragma once

#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <stdint.h>

class Adafruit_GFX;

// Draws `text` as one row of glyphs inside [x, x+w) at row `y` (8 px high).
// `fg` is the glyph color; `bg == fg` draws glyph pixels only, any other `bg`
// fills each glyph cell first (inverse rows). `animate` marks the selected
// row's label, the only field that may scroll; `owner` names the list or
// screen it belongs to so a routine list rebuild keeps the scroll's progress
// while a different selection, label or width restarts it.
void oledTextFieldDraw(Adafruit_GFX* display, int x, int y, int w, const char* text,
                       uint16_t fg, uint16_t bg, bool animate, const void* owner);

// Frame bracket called by the display renderer around the page's drawing.
void oledTextFieldFrameBegin();
void oledTextFieldFrameEnd();
// Dirty-poll question: an animated field drawn last frame is due to move.
bool oledTextFieldNeedsFrame();
// A confirmation owns the screen: freeze fields where they stand.
void oledTextFieldSetPaused(bool paused);
// Display-identity change: nothing in flight carries over.
void oledTextFieldReset();

#endif  // ENABLE_OLED_DISPLAY
