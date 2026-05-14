#ifndef G2_PAGE_ESPNOW_H
#define G2_PAGE_ESPNOW_H

// =============================================================================
// G2 glasses — "ESP-NOW App" page
// =============================================================================
// Top-level hijack page that surfaces ESP-NOW *actions* (send / broadcast /
// ping / per-peer detail / stats). Distinct from Network → ESP-NOW, which
// remains the home for settings/info: ON/OFF toggle, device name, Auto Start,
// paired-device info list.
//
// Sub-mode tree:
//   MAIN              [State row][Peers >>][Broadcast >>][Stats >>]
//     PEERS           one row per gEspNow->devices[i]
//       PEER_DETAIL   Ping / Send "Hi" / Send… / Forget
//     BCAST           Canned: "Here" / "OK" / "Help" / Type message…
//     STATS           mode / peers / TX/RX / channel / MAC
//
// All state-mutating actions route through g2SubmitHijackCommand so they
// run on cmd_exec_task with the glasses user's auth identity (same path
// the Network page uses). Ping is the one exception — it uses the
// espnowAppPing* API in System_ESPNow.h to time the HEARTBEAT+ACK
// round-trip directly, since no CLI command for "ping with RTT" exists.

#include "System_BuildConfig.h"

// Unconditional — both the active and stub branches reference size_t /
// uint8_t / uint32_t in their declarations, so the integer-typedef headers
// must be visible regardless of the BT / G2 gate.
#include <stddef.h>
#include <stdint.h>

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include <Arduino.h>

// CLI direct invocation: dumps a one-screen status snapshot (mode, peers,
// TX/RX counts, MAC, channel) into `out`. Same shape as other pages'
// buildText hooks so the page registry can wire it as the no-hijack render.
void g2BuildESPNowAppInfo(char* out, size_t cap);

// Render the top-level chooser. Called from the hijack tap dispatcher when
// the user lands on the ESP-NOW App page from the main menu.
void g2ShowESPNowAppMenu();

// Tap dispatch from handleHijackMenuTap when gHijackPage == ESPNOW_APP.
// Caller passes the index of the tapped row in the current list.
void g2ESPNowAppHandleTap(uint32_t idx);

// Push-kick: called from the espnow_task RX drain after a new TEXT message
// is stored in PeerMessageHistory. No-op unless the user is currently on
// one of the ESP-NOW App page's message-displaying sub-modes (merged Inbox
// or matching per-peer Inbox); in that case it enqueues a Redraw via the
// lens applier so the list picks up the new entry within one tick of the
// applier worker.
//
// Safe to call from any task — only touches read-only globals and the lens
// job queue (which is itself thread-safe).
void g2ESPNowAppOnRxText(const uint8_t* senderMac);

#else  // stubs when BLE / G2 disabled

inline void g2BuildESPNowAppInfo(char* out, size_t cap) {
  if (out && cap > 0) out[0] = '\0';
}
inline void g2ShowESPNowAppMenu() {}
inline void g2ESPNowAppHandleTap(uint32_t /*idx*/) {}
inline void g2ESPNowAppOnRxText(const uint8_t* /*senderMac*/) {}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#endif  // G2_PAGE_ESPNOW_H
