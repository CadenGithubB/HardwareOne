#ifndef G2_PAGE_NETWORK_H
#define G2_PAGE_NETWORK_H

// =============================================================================
// G2 glasses — "Network" page
// =============================================================================
// Top-level chooser that drills into three subsystem submenus mirroring
// what the OLED Network mode exposes, minus anything that requires
// text input (e.g. adding a new network with a password). Tap-only.
//
// Sub-mode tree:
//   MAIN              [WiFi >>] [ESP-NOW >>] [Bluetooth >>]
//     WIFI            status info / Connect Best / Disconnect /
//                     Scan / List Saved / HTTP toggle
//       WIFI_SCAN     scan-results list (tap = log SSID; can't enter pwd)
//       WIFI_SAVED    saved-network list (tap = forget that network)
//     ESP-NOW          status info / Mode / Start-Stop / View Devices
//       ESPNOW_DEVS   paired-device list (info only)
//     BLUETOOTH       BLE on-off, Auto Start, ring controls; client mode
//                     also has G2 >> (AutoConnect, reconnect, disconnect)
//     BLUETOOTH_G2    G2 client-only controls (drill from Bluetooth)
//
// The page-mode tracker in G2_Glasses.h tracks which top-level
// page we're on; the local state struct here tracks the sub-mode
// within Network.

#include "System_BuildConfig.h"
#include <Arduino.h>

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

// Build the static info-dump variant (text page). Used by the CLI
// `g2network` command for direct invocation, and as the default render
// when entering the Network page from the hijack menu.
void g2BuildNetworkInfo(char* out, size_t cap);
bool g2ShowNetworkPage();

// Render the top-level Network chooser. Called when the user taps
// "Network" from the main hijack menu. The chooser drills into one of
// three subsystem submenus (WiFi / ESP-NOW / Bluetooth).
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
