#ifndef G2_PAGE_POWER_H
#define G2_PAGE_POWER_H

// =============================================================================
// G2 glasses — "Power" page
// =============================================================================
// Two actions, both destructive enough to warrant a confirmation step:
//
//   Level 1 — actions:
//     <- Main Menu
//     Restart
//     Power Off
//
//   Level 2 — confirm prompt for the chosen action:
//     <- Cancel
//     Confirm Restart       (or "Confirm Power Off")
//
// Restart calls esp_restart() — clean reboot, BLE drops naturally.
// Power Off calls esp_deep_sleep_start() with no wake source configured,
// which is the closest thing the ESP32 has to "off" (~10 µA quiescent).
// The chip can be brought back by pressing the reset button.
//
// We intentionally don't try to keep BLE alive across either action —
// dropping the link is the correct behaviour for both restart and
// power-off, and the EvenCore lens UI handles the disconnect gracefully.

#include "System_BuildConfig.h"
#include <Arduino.h>

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

// CLI-direct text dump. Mostly here to satisfy the G2PageModule contract;
// the page is interactive-only in practice.
void g2BuildPowerInfo(char* out, size_t cap);

// Render the Level-1 action list. Called when the user taps "Power" from
// the main hijack menu. Sets the page-mode tracker so taps route here.
void g2ShowPowerMenu();

// Tap dispatch from handleHijackMenuTap when gHijackPage == POWER.
// Routes based on internal level state (action list vs. confirm prompt).
void g2PowerHandleTap(uint32_t idx);

#else
inline void g2BuildPowerInfo(char* out, size_t cap) {
  if (out && cap > 0) out[0] = '\0';
}
inline void g2ShowPowerMenu() {}
inline void g2PowerHandleTap(uint32_t idx) { (void)idx; }
#endif

#endif  // G2_PAGE_POWER_H
