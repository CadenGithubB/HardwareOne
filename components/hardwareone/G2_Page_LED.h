#ifndef G2_PAGE_LED_H
#define G2_PAGE_LED_H

// =============================================================================
// G2 glasses — "LED" sub-page
// =============================================================================
// Reached by drilling from Sensors → LED (the LED is enumerated as a device
// row per the LED-as-device decision — same placement as the OLED's
// Sensors → LED screen). Hidden from the top-level hijack menu (registered
// with hijackLabel=nullptr); navigated to programmatically, mirroring the
// Camera Settings sub-page pattern.
//
// Levels (file-static sub-state, one hijack page):
//   ROOT     <- Sensors / Color > / Effect > / Brightness: NN% / Off
//   COLORS   paged list of the 76 palette names ("off" + colorTable[])
//   EFFECTS  shared ledEffectNames[] + "off"
//
// All actions dispatch CLI commands via g2SubmitHijackCommand (paired-user
// auth + [CMD] audit): ledcolor / ledeffect / ledbrightness / ledclear.
// Effects are non-blocking (ledEffectStart engine) so taps return instantly.
// The effect list, brightness ladder, and color table are shared with the
// OLED LED screen via System_NeoPixel.h — only the render/tap glue is
// lens-specific.

#include "System_BuildConfig.h"
#include <stddef.h>
#include <stdint.h>

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

// CLI direct-invocation snapshot (page registry's buildText slot).
void g2BuildLedInfo(char* out, size_t cap);

// Render the LED root menu. Called from the Sensors list tap dispatch.
void g2ShowLedMenu();

// Tap dispatch from handleHijackMenuTap when gHijackPage == LED.
void g2LedHandleTap(uint32_t idx);

#else  // stubs when BLE / G2 disabled

inline void g2BuildLedInfo(char* out, size_t cap) {
  if (out && cap > 0) out[0] = '\0';
}
inline void g2ShowLedMenu() {}
inline void g2LedHandleTap(uint32_t /*idx*/) {}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#endif  // G2_PAGE_LED_H
