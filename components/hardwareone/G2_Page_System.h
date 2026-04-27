#ifndef G2_PAGE_SYSTEM_H
#define G2_PAGE_SYSTEM_H

// =============================================================================
// G2 glasses — "System" page
// =============================================================================
// Pure display page (no taps, no state). Mirrors the OLED System mode's
// information set: battery, WiFi, heap, PSRAM, uptime, temperature.
// Audit confirmed every field maps cleanly without lossy compromises —
// the OLED mode has zero interactive elements, so there's nothing to
// reduce or skip in the port.

#include "System_BuildConfig.h"
#include <Arduino.h>

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

void g2BuildSystemPage(char* out, size_t cap);
bool g2ShowSystemPage();

#else
inline void g2BuildSystemPage(char* out, size_t cap) {
  if (out && cap > 0) out[0] = '\0';
}
inline bool g2ShowSystemPage() { return false; }
#endif

#endif  // G2_PAGE_SYSTEM_H
