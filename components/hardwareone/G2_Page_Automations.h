#ifndef G2_PAGE_AUTOMATIONS_H
#define G2_PAGE_AUTOMATIONS_H

// =============================================================================
// G2 glasses — "Automations" App page
// =============================================================================
// A tap-navigated list of the device's saved automations (read from
// /system/automations.json) with a per-automation action sub-page:
//
//   LIST     "<- Apps" + one row per automation ("[on]/[off] <name>")
//     DETAIL "<- <name>" / Run now / Enable|Disable
//
// State-mutating taps (run / enable / disable) route through
// g2SubmitHijackCommand so they run on cmd_exec_task with the glasses user's
// auth identity — the same path the ESP-NOW App and Network pages use. Auth
// denials (non-admin / non-creator) come back as clean "Error:" results and
// leave the on/off badge unchanged. Reached from the Apps launcher; hidden
// from the main hijack menu.

#include "System_BuildConfig.h"

// Unconditional — declarations reference size_t / uint32_t regardless of the
// BT / G2 gate (mirrors G2_Page_ESPNow.h).
#include <stddef.h>
#include <stdint.h>

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

// CLI direct-invocation stub required by the page registry's buildText slot.
void g2BuildAutomationsInfo(char* out, size_t cap);

// Render the automations list. Called from the Apps launcher tap dispatch.
void g2ShowAutomationsMenu();

// Tap dispatch from handleHijackMenuTap when gHijackPage == AUTOMATIONS.
void g2AutomationsHandleTap(uint32_t idx);

#else  // stubs when BLE / G2 disabled

inline void g2BuildAutomationsInfo(char* out, size_t cap) {
  if (out && cap > 0) out[0] = '\0';
}
inline void g2ShowAutomationsMenu() {}
inline void g2AutomationsHandleTap(uint32_t /*idx*/) {}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#endif  // G2_PAGE_AUTOMATIONS_H
