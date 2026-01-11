#ifndef OLED_FOOTER_H
#define OLED_FOOTER_H

#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

// Draw the persistent button hint footer for the current mode/state
// This should be called after all mode content is rendered but before display()
void drawOLEDFooter();

#endif // ENABLE_OLED_DISPLAY

#endif // OLED_FOOTER_H
