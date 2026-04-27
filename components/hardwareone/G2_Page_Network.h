#ifndef G2_PAGE_NETWORK_H
#define G2_PAGE_NETWORK_H

// =============================================================================
// G2 glasses — "Network" page
// =============================================================================
// Display + a few tap-driven actions. The OLED Network mode supports adding
// new networks via on-screen keyboard — that's lost on G2 (no keyboard
// input), but everything else maps:
//   • WiFi SSID / IP / RSSI / channel  → static text
//   • Mesh role + peer count           → static text
//   • "Connect best saved network"     → tap action
//   • "Disconnect WiFi"                → tap action
//   • "Scan for networks" + show list  → tap action; results render as a
//                                         REBUILD-list page (sub-state)
//   • "Forget current network"         → tap action with confirmation
//
// State machine: this page can be in two sub-modes — MAIN (action menu)
// and SCAN_RESULTS (list of nearby SSIDs). The page-mode tracker in
// Optional_EvenG2.h tracks which top-level page we're on; the local
// state struct here tracks which sub-mode within Network.

#include "System_BuildConfig.h"
#include <Arduino.h>

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

// Build the static info-dump variant (text page). Used by the CLI
// `g2network` command for direct invocation, and as the default render
// when entering the Network page from the hijack menu.
void g2BuildNetworkInfo(char* out, size_t cap);
bool g2ShowNetworkPage();

// Render the Network sub-menu as a tap-driven list. Called when the
// user taps "Network" from the main hijack menu. Items:
//   <- Back         (tap → go to main menu)
//   Connect Best
//   Disconnect
//   Scan Networks
//   Forget Current
//   (info row)      ← shows current WiFi state, no-op tap
void g2ShowNetworkMenu();

// Tap dispatch from handleHijackMenuTap when gHijackPage == NETWORK.
// Caller passes the index of the tapped item in the current list.
void g2NetworkHandleTap(uint32_t idx);

#else
inline void g2BuildNetworkInfo(char* out, size_t cap) {
  if (out && cap > 0) out[0] = '\0';
}
inline bool g2ShowNetworkPage() { return false; }
inline void g2ShowNetworkMenu() {}
inline void g2NetworkHandleTap(uint32_t idx) { (void)idx; }
#endif

#endif  // G2_PAGE_NETWORK_H
