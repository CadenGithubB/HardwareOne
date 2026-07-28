// =============================================================================
// G2 glasses — "Network" page implementation
// =============================================================================
// See header for the contract. Top-level Network chooser drills into one
// of three subsystem submenus (WiFi / ESP-NOW / Bluetooth) plus several
// list sub-pages. Tap-only — anything in the OLED that needs text input
// (e.g. wifiadd <ssid> <password>, espnowsetname) is omitted.

#include "WebServer_Handle.h"
#include "G2_Page_Network.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "G2_Glasses.h"
#include "System_TaskUtils.h"  // APP_CORE / PRO_CORE task-placement constants
#include "G2_Ring.h"   // g2RingIsConnected (R1 menu status)
#include "Bluetooth.h"
#include "BLE_Peers.h"       // gBlePeerData — G2 AutoReconnect in Bluetooth → G2
#include "System_Settings.h" // setSetting — peer autoReconnect + bleAutoStart
#include "G2_Page_TextEntry.h"   // on-glasses keyboard for "Set Name"
#include "G2_HijackCmd.h"        // g2BumpMenuGen() for setNetSub navigation tracking
#include <new>                   // std::nothrow — for LensUiJob/RedrawSpec allocation
#include "System_Debug.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if ENABLE_WIFI
#include <WiFi.h>
#include "System_WiFi.h"
#endif

#if ENABLE_ESPNOW
#include "System_ESPNow.h"
#endif

#if ENABLE_HTTP_SERVER
#include <esp_http_server.h>
extern bool gServerIsHttps;
#endif

// -----------------------------------------------------------------------------
// Local state
// -----------------------------------------------------------------------------

enum NetworkSubMode : uint8_t {
  NET_SUB_MAIN          = 0,  // top-level chooser (WiFi / HTTP(S) / ESP-NOW / Bluetooth)
  NET_SUB_WIFI          = 1,  // WiFi action menu
  NET_SUB_WIFI_SCAN     = 2,  // WiFi scan results
  NET_SUB_WIFI_SAVED    = 3,  // WiFi saved-network list (tap = forget)
  NET_SUB_WIFI_STATUS   = 4,  // WiFi detailed status dump (info only)
  NET_SUB_ESPNOW        = 5,  // ESP-NOW action menu
  NET_SUB_ESPNOW_DEVS   = 6,  // ESP-NOW paired-device list (info only)
  NET_SUB_BLUETOOTH     = 7,  // Bluetooth action menu
  NET_SUB_BLUETOOTH_G2  = 8,  // G2 client controls (under Bluetooth)
  NET_SUB_HTTP          = 9,  // HTTP(S) server controls (start/stop, HTTPS toggle, auto-start)
  NET_SUB_BLUETOOTH_R1  = 10, // R1 ring controls (under Bluetooth) — mirror of G2 submenu
  NET_SUB_WIFI_SAVED_ACT= 11, // per-saved-network action menu (Connect / Forget)
};
static NetworkSubMode gNetSub = NET_SUB_MAIN;
// Index (into gWifiNetworks) of the saved network the user drilled into from
// the saved list — consumed by the Connect/Forget action submenu.
static int gWifiSavedSel = -1;

// Single mutator so every gNetSub transition bumps the menu generation —
// in-flight hijack-command callbacks compare cookie.menuGen against the
// live value and drop redraws when the user has navigated away. See
// G2_HijackCmd.h staleness contract. Initializer above intentionally
// bypasses this helper (no transition, no in-flight cmds at boot).
static inline void setNetSub(NetworkSubMode m) {
  if (gNetSub == m) return;
  gNetSub = m;
  g2BumpMenuGen();
}

// Cache of the most recent scan results so tap-to-connect knows which
// SSID was at index N. Populated when the user taps "Scan Networks".
// Capped at 8 — anything beyond that and the user can re-scan after
// physically moving closer to fewer APs.
struct CachedAp { String ssid; int rssi; bool secured; };
static CachedAp gScanCache[8];
static size_t   gScanCacheCount = 0;

// "Pending..." overlay state for the WiFi toggle row.
//
// Rationale: WiFi.disconnect() returns immediately so the OFF render
// snaps in place, but connectToBestWiFiNetwork() is async — by the
// time it returns the kick is in flight but the connection isn't up
// yet. Without this overlay the user sees "WiFi: OFF" for 3-4 s after
// tapping (until DHCP completes and a later render picks it up),
// which feels like the tap was ignored.
//
// While `millis() < gWifiPendingDeadlineMs`, showWiFiMenu() renders
// the toggle row as "WiFi: Pending..." regardless of the live
// WiFi.isConnected() value, so the user gets immediate visual feedback.
// A short watchdog task polls WiFi.isConnected() every 500 ms and
// re-renders the menu the moment the state flips (or when the deadline
// expires, whichever comes first). Single-shot — re-tapping the toggle
// while pending arms a fresh deadline + watchdog.
static volatile uint32_t gWifiPendingDeadlineMs = 0;
static volatile bool     gWifiPendingTaskActive = false;

// Same "Pending..." treatment for the R1 ring connect (see showBluetoothR1Menu /
// handleBluetoothR1Tap). `ringconnect` returns OK the instant the connect is
// KICKED, but the scan+connect+auth finishes ~5-8 s later with no completion
// event — so the immediate menu re-render showed "disconnected" and never
// updated. While pending, showBluetoothR1Menu() renders "Ring: connecting..."
// and a watchdog polls g2RingIsConnected() until it flips (or the deadline).
static volatile uint32_t gRingPendingDeadlineMs = 0;
static volatile bool     gRingPendingTaskActive = false;

// -----------------------------------------------------------------------------
// Info text (used by the CLI direct-invocation path)
// -----------------------------------------------------------------------------

void g2BuildNetworkInfo(char* out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';

  String s;
  s.reserve(384);
  s += "Network\n";

#if ENABLE_WIFI
  {
    // RADIO power axis, then WiFi CONNECTION axis below.
    char rl[24];
    snprintf(rl, sizeof(rl), "Radio: %s\n", wifiRadioOn() ? "ON" : "OFF");
    s += rl;
  }
  if (WiFi.isConnected()) {
    char line[80];
    String ssid = WiFi.SSID();
    String ip   = WiFi.localIP().toString();
    long rssi   = WiFi.RSSI();
    int chan    = WiFi.channel();
    snprintf(line, sizeof(line), "WiFi: %s\n", ssid.c_str());           s += line;
    snprintf(line, sizeof(line), "IP %s ch%d\n", ip.c_str(), chan);     s += line;
    snprintf(line, sizeof(line), "RSSI %lddBm\n", rssi);                 s += line;
  } else {
    s += "WiFi: offline\n";
  }
#else
  s += "WiFi: not compiled in\n";
#endif

#if ENABLE_ESPNOW
  if (gEspNow && gEspNow->initialized) {
    char line[80];
    snprintf(line, sizeof(line), "ESPNow %s\n", getEspNowModeString());
    s += line;
    snprintf(line, sizeof(line), "Peers %d\n", gEspNow->peerHistoryCount);
    s += line;
  } else {
    s += "ESPNow: off\n";
  }
#endif

  // MAC tail — short identity anchor.
#if ENABLE_WIFI
  String mac = WiFi.macAddress();
  char tail[8] = {0};
  int ti = 0;
  for (int i = 9; i < (int)mac.length() && ti < 6; i++) {
    char c = mac[i];
    if (c != ':') tail[ti++] = c;
  }
  char macLine[24];
  snprintf(macLine, sizeof(macLine), "MAC ..%s", tail);
  s += macLine;
#endif

  strncpy(out, s.c_str(), cap - 1);
  out[cap - 1] = '\0';
}

bool g2ShowNetworkPage() {
  char buf[400];
  g2BuildNetworkInfo(buf, sizeof(buf));
  DEBUG_G2F("[G2] Network page (%u B):\n%s", (unsigned)strlen(buf), buf);
  return g2ShowText(buf);
}

// -----------------------------------------------------------------------------
// Top-level chooser (WiFi / ESP-NOW / Bluetooth)
// -----------------------------------------------------------------------------

void g2ShowNetworkMenu() {
  setNetSub(NET_SUB_MAIN);
  // Top-level structure groups by radio: WiFi (with HTTP(S) and ESP-NOW
  // nested under it as WiFi-radio-dependent services) and Bluetooth.
  // ESP-NOW technically only needs the WiFi radio on, not an STA
  // association, but UX-wise users treat WiFi as the master switch and
  // expect ESP-NOW to live under it.
  const char* items[] = {
    "<- Main Menu",   // 0 — exits Network back to the hijack root
    "WiFi >>",        // 1
    "Bluetooth >>",   // 2
  };
  if (g2ShowListPage(items, sizeof(items) / sizeof(items[0]))) {
    g2SetHijackPage(G2_HIJACK_PAGE_NETWORK);
    DEBUG_G2F("[G2] Network top-level chooser shown");
  } else {
    DEBUG_G2F("[G2] Network chooser show FAILED");
  }
}

// -----------------------------------------------------------------------------
// WiFi submenu
// -----------------------------------------------------------------------------

static void showWiFiMenu() {
  setNetSub(NET_SUB_WIFI);
  static char radioLine[24];  // "Radio: ON/OFF" — whole-radio power (airplane mode)
  static char wifiLine[64];
  static char autoLine[32];   // "Auto Start: ON/OFF" — boot-time auto-connect
  static char httpLine[40];
  static char espnowLine[40];

#if ENABLE_WIFI
  // Two separate axes shown as two rows: RADIO power vs WiFi CONNECTION.
  snprintf(radioLine, sizeof(radioLine), "Radio: %s", wifiRadioOn() ? "ON" : "OFF");
  bool connected = WiFi.isConnected();
  const bool pending = !connected && gWifiPendingDeadlineMs > 0 &&
                       (int32_t)(millis() - gWifiPendingDeadlineMs) < 0;
  if (connected) {
    String ssid = WiFi.SSID();
    // Truncate long SSIDs so the row still fits the ~32-char visible width.
    String shown = (ssid.length() > 16) ? (ssid.substring(0, 16) + "~") : ssid;
    snprintf(wifiLine, sizeof(wifiLine), "WiFi: %s", shown.c_str());
  } else if (pending) {
    snprintf(wifiLine, sizeof(wifiLine), "WiFi: Pending...");
  } else {
    snprintf(wifiLine, sizeof(wifiLine), "WiFi: Disconnected");
  }
#else
  bool connected = false;
  snprintf(radioLine, sizeof(radioLine), "Radio: n/a");
  snprintf(wifiLine, sizeof(wifiLine), "WiFi: not compiled");
#endif

  snprintf(autoLine, sizeof(autoLine), "Auto Start: %s",
           gSettings.wifiAutoStart ? "ON" : "OFF");

  // HTTP(S) and ESP-NOW nest under WiFi because they ride the radio. HTTP needs
  // an actual network connection (an IP) → tag on `connected`. ESP-NOW only needs
  // the radio powered → tag on wifiRadioOn(). The underlying tap still fires
  // (better to surface a real error than silently no-op).
#if ENABLE_HTTP_SERVER
  snprintf(httpLine, sizeof(httpLine), "HTTP(S) >>%s",
           connected ? "" : " (WiFi req.)");
#else
  snprintf(httpLine, sizeof(httpLine), "HTTP(S): n/a");
#endif
#if ENABLE_ESPNOW
  snprintf(espnowLine, sizeof(espnowLine), "ESP-NOW >>%s",
           wifiRadioOn() ? "" : " (radio off)");
#else
  snprintf(espnowLine, sizeof(espnowLine), "ESP-NOW: n/a");
#endif

  // Item indices MUST match the dispatch switch in handleWiFiTap.
  const char* items[] = {
    "<- Connect",      // 0 — back to Network top-level chooser
    radioLine,         // 1 (toggle: radio power on/off — airplane mode)
    wifiLine,          // 2 (toggle: tap = connect best / disconnect)
    "Disconnect",      // 3 — explicit disconnect (works while Pending too)
    "Status",          // 4 (drill into detailed info dump)
    "Scan Networks",   // 5
    "List Saved",      // 6
    autoLine,          // 7 (toggle wifiAutoStart — boot-time)
    httpLine,          // 8 — opens HTTP(S) submenu
    espnowLine,        // 9 — opens ESP-NOW submenu
  };
  g2ShowListPage(items, sizeof(items) / sizeof(items[0]));
  DEBUG_G2F("[G2] WiFi submenu shown (connected=%d, auto=%d)",
            connected ? 1 : 0, gSettings.wifiAutoStart ? 1 : 0);
}

// Detailed WiFi status — shown when the user taps "Status" on the WiFi
// submenu. List-style page so the rows are individually visible without
// the firmware's TEXT-view pagination juggling. All info, no actions —
// every tap returns to the WiFi submenu.
static void showWiFiStatusPage() {
  setNetSub(NET_SUB_WIFI_STATUS);
#if ENABLE_WIFI
  // 12 row buffers is comfortable headroom — current layout uses up to 10.
  static EXT_RAM_BSS_ATTR char rows[12][48];  // PSRAM: deep-copied by g2ShowListPage
  const char* ptrs[12];
  size_t n = 0;

  strcpy(rows[n], "<- WiFi"); ptrs[n] = rows[n]; n++;
  // Two separate axes: RADIO power, then WiFi connection State.
  snprintf(rows[n], sizeof(rows[n]), "Radio: %s", wifiRadioOn() ? "ON" : "OFF");
  ptrs[n] = rows[n]; n++;
  bool connected = WiFi.isConnected();
  snprintf(rows[n], sizeof(rows[n]), "State: %s",
           connected ? "connected" : "offline");
  ptrs[n] = rows[n]; n++;

  if (connected) {
    snprintf(rows[n], sizeof(rows[n]), "SSID: %s", WiFi.SSID().c_str());
    ptrs[n] = rows[n]; n++;
    snprintf(rows[n], sizeof(rows[n]), "BSSID: %s", WiFi.BSSIDstr().c_str());
    ptrs[n] = rows[n]; n++;
    snprintf(rows[n], sizeof(rows[n]), "IP: %s",
             WiFi.localIP().toString().c_str());
    ptrs[n] = rows[n]; n++;
    snprintf(rows[n], sizeof(rows[n]), "GW: %s",
             WiFi.gatewayIP().toString().c_str());
    ptrs[n] = rows[n]; n++;
    snprintf(rows[n], sizeof(rows[n]), "DNS: %s",
             WiFi.dnsIP().toString().c_str());
    ptrs[n] = rows[n]; n++;
    snprintf(rows[n], sizeof(rows[n]), "RSSI %lddBm  ch%ld",
             (long)WiFi.RSSI(), (long)WiFi.channel());
    ptrs[n] = rows[n]; n++;
    snprintf(rows[n], sizeof(rows[n]), "TX: %ddBm",
             (int)WiFi.getTxPower());
    ptrs[n] = rows[n]; n++;
  }
  snprintf(rows[n], sizeof(rows[n]), "MAC: %s", WiFi.macAddress().c_str());
  ptrs[n] = rows[n]; n++;

  g2ShowListPage(ptrs, n);
  DEBUG_G2F("[G2] WiFi status page shown (rows=%u)", (unsigned)n);
#else
  static const char* na[] = { "<- WiFi", "(WiFi not compiled)" };
  g2ShowListPage(na, 2);
#endif
}

// Fallback while WiFi.scanNetworks runs: back-pane list when the Even-AI
// front card could not be opened (e.g. CTRL ENTER error 7). Primary path
// is g2ShowEvenAIReplyNoAsk("Scanning networks...") in runNetworkScanFlow.
static void showWiFiScanningPlaceholder() {
  setNetSub(NET_SUB_WIFI_SCAN);
  gScanCacheCount = 0;
  static const char* rows[] = {
      "<- WiFi",
      "Scanning… (wait)",
  };
  g2ShowListPage(rows, sizeof(rows) / sizeof(rows[0]));
  DEBUG_G2F("[G2] WiFi: scan placeholder shown (back pane)");
}

// Render scan results as a tappable list. Items:
//   0: "<- WiFi" (returns to WiFi submenu)
//   1..N: SSID lines with RSSI suffix
static void showScanResults() {
  setNetSub(NET_SUB_WIFI_SCAN);
  static EXT_RAM_BSS_ATTR char rows[1 + 8][48];  // PSRAM: deep-copied by g2ShowListPage
  strcpy(rows[0], "<- WiFi");
  const char* ptrs[1 + 8];
  ptrs[0] = rows[0];
  size_t n = 1;
  for (size_t i = 0; i < gScanCacheCount && i < 8; i++) {
    snprintf(rows[1 + i], sizeof(rows[1 + i]),
             "%s %s %ddBm",
             gScanCache[i].secured ? "L" : " ",
             gScanCache[i].ssid.length() > 0
                 ? gScanCache[i].ssid.c_str() : "<hidden>",
             gScanCache[i].rssi);
    ptrs[n++] = rows[1 + i];
  }
  if (n == 1) {
    static const char* empty[] = { "<- WiFi", "(no networks found)" };
    g2ShowListPage(empty, 2);
  } else {
    g2ShowListPage(ptrs, n);
  }
  DEBUG_G2F("[G2] WiFi scan results (%u APs)",
            (unsigned)gScanCacheCount);
}

// Saved-network list. Tap to forget that entry (no password required —
// only the SSID is needed for cmd_wifirm).
static void showWiFiSavedList() {
  setNetSub(NET_SUB_WIFI_SAVED);
#if ENABLE_WIFI
  static EXT_RAM_BSS_ATTR char rows[1 + MAX_WIFI_NETWORKS][48];  // PSRAM: deep-copied by g2ShowListPage
  strcpy(rows[0], "<- WiFi");
  const char* ptrs[1 + MAX_WIFI_NETWORKS];
  ptrs[0] = rows[0];
  size_t n = 1;
  const int count = gWifiNetworkCount;
  for (int i = 0; i < count && i < MAX_WIFI_NETWORKS; i++) {
    snprintf(rows[1 + i], sizeof(rows[1 + i]),
             "[%d] %s%s",
             gWifiNetworks[i].priority,
             gWifiNetworks[i].ssid.c_str(),
             i == 0 ? " *" : "");
    ptrs[n++] = rows[1 + i];
  }
  if (n == 1) {
    static const char* empty[] = { "<- WiFi", "(no saved networks)" };
    g2ShowListPage(empty, 2);
  } else {
    g2ShowListPage(ptrs, n);
  }
  DEBUG_G2F("[G2] WiFi saved list (%d entries) — tap to forget",
            count);
#else
  static const char* na[] = { "<- WiFi", "(WiFi not compiled)" };
  g2ShowListPage(na, 2);
#endif
}

// Per-saved-network action menu — reached by tapping a network on the saved
// list. gWifiSavedSel holds its index into gWifiNetworks. Lets the user CONNECT
// to that saved network (password already stored) or FORGET it, instead of the
// old behaviour where a tap immediately forgot it.
static void showWiFiSavedActionMenu() {
  setNetSub(NET_SUB_WIFI_SAVED_ACT);
#if ENABLE_WIFI
  static char back[48];
  if (gWifiSavedSel >= 0 && gWifiSavedSel < gWifiNetworkCount) {
    snprintf(back, sizeof(back), "<- %s", gWifiNetworks[gWifiSavedSel].ssid.c_str());
  } else {
    strcpy(back, "<- Saved");
  }
  const char* items[] = { back, "Connect", "Forget" };
  g2ShowListPage(items, sizeof(items) / sizeof(items[0]));
#else
  static const char* na[] = { "<- Saved", "(WiFi not compiled)" };
  g2ShowListPage(na, 2);
#endif
}

// -----------------------------------------------------------------------------
// ESP-NOW submenu
// -----------------------------------------------------------------------------

static void showEspNowMenu() {
  setNetSub(NET_SUB_ESPNOW);
  static char stateLine[40];
  static char modeLine[40];
  static char devsLine[32];
  static char autoLine[32];   // "Auto Start: ON/OFF" — boot-time auto-init

#if ENABLE_ESPNOW
  static char nameLine[48];
  snprintf(nameLine, sizeof(nameLine), "Name: %s",
           gSettings.espnowDeviceName.length() > 0
               ? gSettings.espnowDeviceName.c_str()
               : "(unset)");
  snprintf(autoLine, sizeof(autoLine), "Auto Start: %s",
           gSettings.espnowEnabled ? "ON" : "OFF");
  bool running = (gEspNow && gEspNow->initialized);
  if (running) {
    snprintf(stateLine, sizeof(stateLine), "ESP-NOW: ON");
    snprintf(modeLine, sizeof(modeLine), "Mode: %s", getEspNowModeString());
    snprintf(devsLine, sizeof(devsLine), "Peers: %d",
             gEspNow->deviceCount);
    const char* items[] = {
      "<- WiFi",         // 0 — back to WiFi submenu (parent of ESP-NOW now)
      stateLine,         // 1 (toggle)
      modeLine,          // 2 (info)
      devsLine,          // 3 (info)
      "View Devices",    // 4
      nameLine,          // 5 (tap → on-glasses text entry)
      autoLine,          // 6 (toggle espnowEnabled — boot-time)
    };
    g2ShowListPage(items, sizeof(items) / sizeof(items[0]));
  } else {
    snprintf(stateLine, sizeof(stateLine), "ESP-NOW: OFF");
    // Only show toggle + View Devices + name editor when off; no point
    // listing Mode/Peers lines for a subsystem that isn't running.
    const char* items[] = {
      "<- WiFi",         // 0 — back to WiFi submenu (parent of ESP-NOW now)
      stateLine,         // 1 (toggle — tap to start)
      "View Devices",    // 2
      nameLine,          // 3 (tap → on-glasses text entry)
      autoLine,          // 4 (toggle espnowEnabled — boot-time)
    };
    g2ShowListPage(items, sizeof(items) / sizeof(items[0]));
  }
#else
  static const char* na[] = { "<- WiFi", "ESP-NOW: not compiled" };
  g2ShowListPage(na, 2);
#endif
  DEBUG_G2F("[G2] ESP-NOW submenu shown (auto=%d)",
            gSettings.espnowEnabled ? 1 : 0);
}

static void showEspNowDevices() {
  setNetSub(NET_SUB_ESPNOW_DEVS);
#if ENABLE_ESPNOW
  static EXT_RAM_BSS_ATTR char rows[1 + 8][48];  // PSRAM: deep-copied by g2ShowListPage
  strcpy(rows[0], "<- ESP-NOW");
  const char* ptrs[1 + 8];
  ptrs[0] = rows[0];
  size_t n = 1;
  if (gEspNow && gEspNow->initialized) {
    const int devCount = gEspNow->deviceCount;
    for (int i = 0; i < devCount && i < 8; i++) {
      const EspNowDevice& d = gEspNow->devices[i];
      const char* label = d.friendlyName.length() > 0
          ? d.friendlyName.c_str()
          : (d.name.length() > 0 ? d.name.c_str() : "(unnamed)");
      snprintf(rows[1 + i], sizeof(rows[1 + i]),
               "%s%s%s",
               label,
               d.room.length() > 0 ? " @ " : "",
               d.room.length() > 0 ? d.room.c_str() : "");
      ptrs[n++] = rows[1 + i];
    }
  }
  if (n == 1) {
    static const char* empty[] = { "<- ESP-NOW", "(no peers — pair via web)" };
    g2ShowListPage(empty, 2);
  } else {
    g2ShowListPage(ptrs, n);
  }
  DEBUG_G2F("[G2] ESP-NOW device list (%u entries)", (unsigned)(n - 1));
#else
  static const char* na[] = { "<- WiFi", "(ESP-NOW not compiled)" };
  g2ShowListPage(na, 2);
#endif
}

// -----------------------------------------------------------------------------
// Bluetooth submenu
// -----------------------------------------------------------------------------

// G2-specific rows (mode/conn, boot AutoReconnect, saved-MAC reconnect,
// disconnect) live here so the parent Bluetooth list stays short. Parent
// keeps BLE on/off, Auto Start, and R1 ring rows only.
static void showBluetoothG2Menu() {
  setNetSub(NET_SUB_BLUETOOTH_G2);
  static char connLine[40];
  static char autoReconnLine[44];

  // Mode row used to live here as info-only; promoted to the parent
  // Bluetooth menu as a toggle 2026-05-09 so the user can flip
  // server↔client without drilling through G2-specific controls. The
  // G2 submenu now stays focused on G2-specific actions only.
  const bool active = bleSubsystemActive();
  if (active) {
    snprintf(connLine, sizeof(connLine), "Conn: %s",
             isG2Connected() ? "yes" : "no");
  } else {
    snprintf(connLine, sizeof(connLine), "BLE: stopped");
  }
  snprintf(autoReconnLine, sizeof(autoReconnLine), "G2 AutoReconnect: %s",
           gBlePeerData[BLE_PEER_G2_GLASSES].autoReconnect ? "ON" : "OFF");

  if (active) {
    const char* items[] = {
      "<- Bluetooth",   // 0
      connLine,         // 1 (info)
      autoReconnLine,     // 2 (toggle boot auto-reconnect)
      "Reconnect G2",   // 3 (saved MACs — same as boot path)
      "Disconnect G2",  // 4
    };
    g2ShowListPage(items, sizeof(items) / sizeof(items[0]));
  } else {
    const char* items[] = {
      "<- Bluetooth",   // 0
      connLine,         // 1 (info — "BLE: stopped")
      autoReconnLine,     // 2 (toggle boot auto-reconnect)
    };
    g2ShowListPage(items, sizeof(items) / sizeof(items[0]));
  }
  DEBUG_G2F("[G2] Bluetooth → G2 submenu shown (active=%d)", active ? 1 : 0);
}

// R1 ring submenu — mirror of showBluetoothG2Menu. Connect/Disconnect/
// Reconnect Ring + R1 AutoReconnect toggle all live here so the parent
// Bluetooth menu stays short. Added 2026-05-09 alongside the parent-
// menu R1 >> drill-in.
static void showBluetoothR1Menu() {
  setNetSub(NET_SUB_BLUETOOTH_R1);
  static char connLine[40];
  static char autoReconnLine[44];

  const bool active = bleSubsystemActive();
  const bool ringUp = g2RingIsConnected();
  // "connecting..." while a fresh connect is in flight (async, no completion
  // event) so the user gets feedback instead of a stale "disconnected".
  const bool pending = !ringUp && gRingPendingDeadlineMs > 0 &&
                       (int32_t)(millis() - gRingPendingDeadlineMs) < 0;
  if (active) {
    snprintf(connLine, sizeof(connLine), "Ring: %s",
             pending ? "connecting..." : (ringUp ? "connected" : "disconnected"));
  } else {
    snprintf(connLine, sizeof(connLine), "BLE: stopped");
  }
  snprintf(autoReconnLine, sizeof(autoReconnLine), "R1 AutoReconnect: %s",
           gBlePeerData[BLE_PEER_R1_RING].autoReconnect ? "ON" : "OFF");

  if (active) {
    if (ringUp) {
      const char* items[] = {
        "<- Bluetooth",    // 0
        connLine,          // 1 (info)
        autoReconnLine,      // 2 (toggle boot auto-reconnect)
        "Reconnect Ring",  // 3
        "Disconnect Ring", // 4
      };
      g2ShowListPage(items, sizeof(items) / sizeof(items[0]));
    } else {
      const char* items[] = {
        "<- Bluetooth",    // 0
        connLine,          // 1 (info)
        autoReconnLine,      // 2 (toggle boot auto-reconnect)
        "Connect Ring",    // 3
      };
      g2ShowListPage(items, sizeof(items) / sizeof(items[0]));
    }
  } else {
    const char* items[] = {
      "<- Bluetooth",      // 0
      connLine,            // 1 (info — "BLE: stopped")
      autoReconnLine,        // 2 (toggle boot auto-reconnect)
    };
    g2ShowListPage(items, sizeof(items) / sizeof(items[0]));
  }
  DEBUG_G2F("[G2] Bluetooth → R1 submenu shown (active=%d ringUp=%d)",
            active ? 1 : 0, ringUp ? 1 : 0);
}

static void showBluetoothMenu() {
  setNetSub(NET_SUB_BLUETOOTH);
  static char stateLine[40];
  static char modeLine[40];
  static char connLine[40];
  static char autoLine[32];   // "Auto Start: ON/OFF" — boot-time auto-init

  // Aggregate-status pattern (mirrors the dashboard tile + ESPNow): the
  // BLE subsystem is "on" when EITHER the server OR the G2 client is
  // initialized, and the rendered state reflects whichever is active.
  // Without this, viewing the menu from the lens (which means G2 client
  // is connected) showed "BLE: OFF" — confusing and wrong.
  const bool active   = bleSubsystemActive();
  const bool isClient = (gSettings.bleMode == BLE_MODE_G2_CLIENT);
  const bool clientUp = isG2ClientInitialized();
  const bool serverUp = isBLERunning();
  snprintf(autoLine, sizeof(autoLine), "Auto Start: %s",
           gSettings.bleAutoStart ? "ON" : "OFF");
  // Per-mode "connected" semantics: server cares about phone connection,
  // client cares about whether the temple BLE links are live.
  const bool connected = isClient ? isG2Connected() : isBLEConnected();
  // Mode is now a toggle (was info-only inside the G2 submenu before
  // 2026-05-09). Lives between the BLE on/off row and the G2/R1
  // drill-ins so it's visible regardless of mode and immediately
  // adjacent to the BLE toggle that flipping it may also disrupt.
  // Tap dispatches `blemode <other>` via cmd_exec — that handler
  // tears down the active mode (deinitG2Client / deinitBluetooth)
  // before flipping the persisted setting, mirroring the destructive
  // semantics of the BLE on/off toggle on idx=1.
  snprintf(modeLine, sizeof(modeLine), "Mode: %s",
           isClient ? "client (G2)" : "server");
  if (active) {
    snprintf(stateLine, sizeof(stateLine), "BLE: ON (%s)",
             bleSubsystemStateString());
    snprintf(connLine, sizeof(connLine), "Conn: %s",
             connected ? "yes" : "no");
    if (isClient) {
      // Client mode: G2 + R1 controls each live under their own
      // submenu so the parent stays at BLE / Mode / G2 >> / R1 >> /
      // Auto Start. Ring action rows (Connect/Reconnect/Disconnect
      // Ring) moved into the R1 submenu 2026-05-09 alongside this
      // restructure.
      const char* items[] = {
        "<- Connect",      // 0
        stateLine,         // 1 (toggle = stop client)
        modeLine,          // 2 (toggle — server↔client)
        "G2 >>",           // 3
        "R1 >>",           // 4
        autoLine,          // 5 (toggle bleAutoStart)
      };
      g2ShowListPage(items, sizeof(items) / sizeof(items[0]));
    } else {
      // Server mode (legacy layout, plus Mode toggle slotted in).
      const char* items[] = {
        "<- Connect",      // 0
        stateLine,         // 1 (toggle = stop server)
        modeLine,          // 2 (toggle — server↔client)
        connLine,          // 3 (info)
        "Toggle Adv",      // 4
        "Disconnect",      // 5
        autoLine,          // 6 (toggle bleAutoStart)
      };
      g2ShowListPage(items, sizeof(items) / sizeof(items[0]));
    }
  } else {
    // Subsystem fully inactive — Mode toggle still useful so the user
    // can pre-select what the BLE-on toggle starts.
    snprintf(stateLine, sizeof(stateLine), "BLE: OFF");
    if (isClient) {
      const char* items[] = {
        "<- Connect",      // 0
        stateLine,         // 1 (toggle — starts G2 client)
        modeLine,          // 2 (toggle — server↔client)
        "G2 >>",           // 3 (AutoReconnect + reconnect when BLE up)
        "R1 >>",           // 4 (R1 AutoReconnect + connect when BLE up)
        autoLine,          // 5 (toggle bleAutoStart)
      };
      g2ShowListPage(items, sizeof(items) / sizeof(items[0]));
    } else {
      const char* items[] = {
        "<- Connect",      // 0
        stateLine,         // 1 (toggle — starts server)
        modeLine,          // 2 (toggle — server↔client)
        autoLine,          // 3 (toggle bleAutoStart)
      };
      g2ShowListPage(items, sizeof(items) / sizeof(items[0]));
    }
  }
  DEBUG_G2F("[G2] Bluetooth submenu shown (server=%d client=%d conn=%d mode=%s)",
            serverUp ? 1 : 0, clientUp ? 1 : 0, connected ? 1 : 0,
            isClient ? "client" : "server");
}

// -----------------------------------------------------------------------------
// Scan worker — runs the WiFi scan off the BLE notify task so heartbeats
// keep flowing during the 2-3 s scan window. Optionally shows a
// "Scanning networks..." card on the front pane via the Even-AI
// subsystem.
// -----------------------------------------------------------------------------
// Earlier observation: CTRL{ENTER} on sid=0x07 returns errorCode=7
// when called too quickly after a hijack-tap that drilled into a
// sub-page. The same call from the Test Suite's AI Panel Tests works
// fine. Best hypothesis: the firmware needs a brief settle window
// after the SHUTDOWN+CREATE that drilled into Network (or after the
// tap dispatch itself) before it'll honor a new app activation. We
// add a 400 ms delay at the start of the scan flow to give the
// firmware that breathing room, then hide+EvenAI front card. If
// CTRL{ENTER} still fails we show a small back-pane "Scanning…" list.
// See docs/G2_PROTOCOL.md "Ways the pipeline can fail visibly" for the
// errorCode=7 finding.

#if ENABLE_WIFI
// Only one scan flow at a time: each tap used to spawn another `g2_net_scan`
// task. Two workers interleave WiFi.scanNetworks (not safe concurrently),
// gScanCache String writes, and EvenAI ENTER/ANALYSE/REPLY/HIDE — that
// produced empty results, missing "Scanning..." cards, and (under load)
// heap asserts (tlsf double-free) in field logs.
static portMUX_TYPE s_netScanMux = portMUX_INITIALIZER_UNLOCKED;
static bool s_netScanRunning = false;

static void netScanMarkFinished(void) {
  portENTER_CRITICAL(&s_netScanMux);
  s_netScanRunning = false;
  portEXIT_CRITICAL(&s_netScanMux);
}

// Body of the scan flow — split out from the FreeRTOS task entry so the
// vTaskDelete is only called when we actually own the task. Keeps the
// inline fallback path safe (calling vTaskDelete on the BTC task that
// ran our tap handler would tear down BLE entirely).
static void runNetworkScanFlow() {
  // 1. Settle window. Empirically the firmware rejects CTRL{ENTER}
  //    when fired immediately after a tap that drilled into a hijack
  //    sub-page. 400 ms is long enough to clear that race in
  //    observation; shorter windows sometimes still hit errorCode=7.
  vTaskDelay(pdMS_TO_TICKS(400));

  // 2. Clear any stuck Even-AI card from a previous scan, then try the
  //    front-pane "Scanning..." card (primary UX). We intentionally do
  //    NOT call g2ShowListPage before this: enqueueing a back-pane
  //    SHUTDOWN+CREATE first races the Even-AI pipeline and often left
  //    users with only the small list and no front card.
  g2HideEvenAICard();
  vTaskDelay(pdMS_TO_TICKS(120));

  if (!g2ShowEvenAIReplyNoAsk("Scanning networks...")) {
    DEBUG_G2F("[G2] WiFi: Even-AI scanning card failed — back-pane "
              "placeholder");
    showWiFiScanningPlaceholder();
  }

  // 3. Run the synchronous scan. Hidden=true so we capture every SSID
  //    in range even if the AP isn't broadcasting; the user picks
  //    from the result list afterwards.
  int n = WiFi.scanNetworks(/*async*/ false, /*hidden*/ true);
  if (n < 0) n = 0;
  gScanCacheCount = 0;
  for (int i = 0; i < n && gScanCacheCount < 8; i++) {
    gScanCache[gScanCacheCount].ssid    = WiFi.SSID(i);
    gScanCache[gScanCacheCount].rssi    = WiFi.RSSI(i);
    gScanCache[gScanCacheCount].secured = (WiFi.encryptionType(i)
                                           != WIFI_AUTH_OPEN);
    gScanCacheCount++;
  }
  WiFi.scanDelete();
  DEBUG_G2F("[G2] Network: scan finished, cached %u APs",
            (unsigned)gScanCacheCount);

  // 4. Dismiss the front-pane card explicitly so it disappears the
  //    moment the back-pane swap happens, rather than lingering for
  //    its full firmware auto-dismiss window.
  g2HideEvenAICard();

  // 5. Swap the back pane to the results list.
  showScanResults();
}

static void networkScanWorker(void* /*arg*/) {
  runNetworkScanFlow();
  netScanMarkFinished();
  vTaskDelete(nullptr);
}

static void spawnNetworkScanWorker() {
  portENTER_CRITICAL(&s_netScanMux);
  if (s_netScanRunning) {
    portEXIT_CRITICAL(&s_netScanMux);
    DEBUG_G2F("[G2] WiFi: scan already in progress — ignoring duplicate tap");
    return;
  }
  s_netScanRunning = true;
  portEXIT_CRITICAL(&s_netScanMux);

  // 4 KB stack: WiFi.scanNetworks itself uses ~1 KB, EvenAI builders
  // need ~400 B for the pb buffer, plus FreeRTOS overhead. Same budget
  // as the AI test worker.
  if (xTaskCreatePinnedToCore(networkScanWorker, "g2_net_scan", 4096, nullptr,
                  /*prio*/ 5, nullptr, APP_CORE) != pdPASS) {
    DEBUG_G2F("[G2] Network: scan worker xTaskCreate failed — "
              "running scan inline (BLE will stall briefly)");
    runNetworkScanFlow();  // safe — no vTaskDelete in the body
    netScanMarkFinished();
  }
}
#endif  // ENABLE_WIFI

// -----------------------------------------------------------------------------
// Tap dispatch — branch on submode, then by item index within that mode.
// -----------------------------------------------------------------------------

static void backToHijackMain() {
  g2SetHijackPage(G2_HIJACK_PAGE_MAIN);
  extern void g2RedrawHijackMainMenu();
  g2RedrawHijackMainMenu();
}

// Forward decl — defined below alongside other section show fns. Kept
// here so handleWiFiTap can dispatch to it without reordering the file.
static void showHttpMenu();

static void handleMainTap(uint32_t idx) {
  switch (idx) {
    case 0: backToHijackMain(); break;        // <- Back
    case 1: showWiFiMenu();      break;       // WiFi >>
    case 2: showBluetoothMenu(); break;       // Bluetooth >>
    default:
      DEBUG_G2F("[G2] Network MAIN: unknown idx=%u", (unsigned)idx);
      break;
  }
}

#if ENABLE_WIFI
// Watchdog spawned by the WiFi-toggle ON branch. Polls WiFi.isConnected()
// every 500 ms for up to ~10 s and re-renders the menu the moment the
// connection comes up — replaces the "WiFi: Pending..." overlay with
// the live "WiFi: ON | <ssid>" text. If the deadline expires without a
// successful connect (no saved network, AP out of range, wrong creds),
// it re-renders one final time so the menu drops back to "WiFi: OFF"
// instead of getting stuck on "Pending...".
//
// Runs on its own task, not the BLE notify task, because the polling
// loop yields with vTaskDelay — calling g2ShowListPage from a task
// other than the original tap dispatcher is fine; the BLE write path
// is already mutex-guarded.
static void wifiPendingWatchdogTask(void* /*arg*/) {
  bool wasConnected = false;
  bool rerendered   = false;
  while (gWifiPendingDeadlineMs > 0 &&
         (int32_t)(millis() - gWifiPendingDeadlineMs) < 0) {
    vTaskDelay(pdMS_TO_TICKS(500));
    const bool nowConnected = WiFi.isConnected();
    if (nowConnected && !wasConnected) {
      gWifiPendingDeadlineMs = 0;
      // Only re-render if the user is still on the WiFi submenu —
      // otherwise we'd clobber whatever they navigated to next.
      if (gNetSub == NET_SUB_WIFI) {
        showWiFiMenu();
        rerendered = true;
      }
      break;
    }
    wasConnected = nowConnected;
  }
  // Final re-render when the deadline expired without a state change,
  // so the menu drops the "Pending..." text and reflects whatever
  // stuck (usually "OFF" if the connect failed).
  if (!rerendered && gNetSub == NET_SUB_WIFI) {
    gWifiPendingDeadlineMs = 0;
    showWiFiMenu();
  } else {
    gWifiPendingDeadlineMs = 0;
  }
  gWifiPendingTaskActive = false;
  vTaskDelete(nullptr);
}
#endif  // ENABLE_WIFI

// Generic helper used by every WiFi-tap cmd_exec callback below. Takes a
// render fn (showWiFiMenu / showWiFiSavedList / etc.), wraps it in a Redraw
// job, and enqueues — gen-guard in the lens applier handles the "user
// navigated away" case automatically. MUST be called from cmd_exec_task
// (i.e. from inside a g2SubmitHijackCommand callback). Does NOT touch the
// lens directly.
static void enqueueWifiRedrawFromCallback(const G2CmdCookie& cookie,
                                          void (*renderFn)(),
                                          const char* tag) {
  RedrawSpec* spec = new (std::nothrow) RedrawSpec{};
  if (!spec) {
    DEBUG_G2F("[G2] %s: RedrawSpec alloc failed — lens won't refresh", tag);
    return;
  }
  spec->render = renderFn;

  LensUiJob* job = new (std::nothrow) LensUiJob{};
  if (!job) {
    DEBUG_G2F("[G2] %s: LensUiJob alloc failed", tag);
    delete spec;
    return;
  }
  job->kind           = LensJobKind::Redraw;
  job->submitMenuGen  = cookie.menuGen;
  job->cmdSeq         = cookie.seq;
  job->targetPage     = cookie.targetPage;
  job->targetNetSub   = cookie.targetNetSub;
  job->payload.redraw = spec;

  if (!g2EnqueueLensJob(job)) {
    DEBUG_G2F("[G2] %s: lens job enqueue FAILED — lens won't refresh", tag);
    delete spec;
    delete job;
  }
}

// Step 4 + Group A: hijack-tap cmd_exec completion callbacks.
// All run on cmd_exec_task after the underlying CLI command finishes; each
// just enqueues a Redraw of the appropriate WiFi-related view. Naming
// reflects which view gets redrawn so the call site is self-documenting.

static void onWifiMenuRefreshDone(bool ok,
                                  const char* result,
                                  const G2CmdCookie& cookie,
                                  void* /*userData*/) {
  DEBUG_G2F("[G2] WiFi-menu refresh cmd done: ok=%d seq=%llu menuGen=%u result='%s'",
            (int)ok, (unsigned long long)cookie.seq,
            (unsigned)cookie.menuGen, result ? result : "");
  enqueueWifiRedrawFromCallback(cookie, &showWiFiMenu, "WiFi-menu refresh");
}

static void onWifiSavedListRefreshDone(bool ok,
                                       const char* result,
                                       const G2CmdCookie& cookie,
                                       void* /*userData*/) {
  DEBUG_G2F("[G2] WiFi-saved refresh cmd done: ok=%d seq=%llu menuGen=%u result='%s'",
            (int)ok, (unsigned long long)cookie.seq,
            (unsigned)cookie.menuGen, result ? result : "");
  enqueueWifiRedrawFromCallback(cookie, &showWiFiSavedList, "WiFi-saved refresh");
}

// ESP-NOW / Bluetooth submenu refresh callbacks. Identical pattern to the
// WiFi ones above — the "Wifi" in enqueueWifiRedrawFromCallback's name is
// historical; the helper itself is render-fn agnostic.
#if ENABLE_ESPNOW
static void onEspNowMenuRefreshDone(bool ok,
                                    const char* result,
                                    const G2CmdCookie& cookie,
                                    void* /*userData*/) {
  DEBUG_G2F("[G2] ESP-NOW-menu refresh cmd done: ok=%d seq=%llu menuGen=%u result='%s'",
            (int)ok, (unsigned long long)cookie.seq,
            (unsigned)cookie.menuGen, result ? result : "");
  enqueueWifiRedrawFromCallback(cookie, &showEspNowMenu, "ESP-NOW-menu refresh");
}

// Completion handler for the ESP-NOW *open* (start) toggle. Unlike the generic
// refresh above, this surfaces the failure reason on the lens: openespnow
// (cmd_espnow_init) can genuinely fail — most often "Insufficient memory for
// ESP-NOW (need ~40KB DRAM ...)" when WiFi + HTTP + G2 are all up, or a captured
// initEspNow() reason. The old path discarded `result` and just re-rendered the
// menu back to "OFF", so a failed start looked like the button did nothing.
EXT_RAM_BSS_ATTR static char sEspNowOpenMsg[128];
static void showEspNowOpenMsg() { g2ShowText(sEspNowOpenMsg); }

static void onEspNowOpenDone(bool ok,
                             const char* result,
                             const G2CmdCookie& cookie,
                             void* /*userData*/) {
  DEBUG_G2F("[G2] ESP-NOW open cmd done: ok=%d result='%s'",
            (int)ok, result ? result : "");
  if (gEspNow && gEspNow->initialized) {
    // Started successfully — re-render the menu (now shows "ESP-NOW: ON").
    enqueueWifiRedrawFromCallback(cookie, &showEspNowMenu, "ESP-NOW open ok");
    return;
  }
  // Still not running → the start failed. Show the captured reason on the lens
  // instead of silently snapping back to OFF.
  strncpy(sEspNowOpenMsg, (result && result[0]) ? result : "ESP-NOW failed to start",
          sizeof(sEspNowOpenMsg) - 1);
  sEspNowOpenMsg[sizeof(sEspNowOpenMsg) - 1] = '\0';
  enqueueWifiRedrawFromCallback(cookie, &showEspNowOpenMsg, "ESP-NOW open failed");
}
#endif

static void onBluetoothMenuRefreshDone(bool ok,
                                       const char* result,
                                       const G2CmdCookie& cookie,
                                       void* /*userData*/) {
  DEBUG_G2F("[G2] Bluetooth-menu refresh cmd done: ok=%d seq=%llu menuGen=%u result='%s'",
            (int)ok, (unsigned long long)cookie.seq,
            (unsigned)cookie.menuGen, result ? result : "");
  enqueueWifiRedrawFromCallback(cookie, &showBluetoothMenu, "Bluetooth-menu refresh");
}

static void onBluetoothG2MenuRefreshDone(bool ok,
                                         const char* result,
                                         const G2CmdCookie& cookie,
                                         void* /*userData*/) {
  DEBUG_G2F("[G2] Bluetooth-G2-menu refresh cmd done: ok=%d seq=%llu menuGen=%u result='%s'",
            (int)ok, (unsigned long long)cookie.seq,
            (unsigned)cookie.menuGen, result ? result : "");
  enqueueWifiRedrawFromCallback(cookie, &showBluetoothG2Menu, "Bluetooth-G2-menu refresh");
}

// Watchdog for the R1 ring connect — mirror of wifiPendingWatchdogTask. Polls
// g2RingIsConnected() every 500 ms and re-renders the R1 submenu the moment the
// ring actually connects (scan+connect+auth completes well after `ringconnect`
// returns OK), or once more when the deadline expires so "connecting..." drops
// back to "disconnected" instead of getting stuck. Runs on its own task (the
// poll yields with vTaskDelay); g2ShowListPage off the tap dispatcher is fine —
// the BLE write path is mutex-guarded (same rationale as the WiFi watchdog).
static void ringPendingWatchdogTask(void* /*arg*/) {
  bool rerendered = false;
  while (gRingPendingDeadlineMs > 0 &&
         (int32_t)(millis() - gRingPendingDeadlineMs) < 0) {
    vTaskDelay(pdMS_TO_TICKS(500));
    if (g2RingIsConnected()) {
      gRingPendingDeadlineMs = 0;
      // Only repaint if the user is still on the R1 submenu — otherwise we'd
      // clobber whatever they navigated to.
      if (gNetSub == NET_SUB_BLUETOOTH_R1) { showBluetoothR1Menu(); rerendered = true; }
      break;
    }
  }
  if (!rerendered && gNetSub == NET_SUB_BLUETOOTH_R1) {
    gRingPendingDeadlineMs = 0;
    showBluetoothR1Menu();   // deadline expired → drop "connecting..." overlay
  } else {
    gRingPendingDeadlineMs = 0;
  }
  gRingPendingTaskActive = false;
  vTaskDelete(nullptr);
}

// R1 ring submenu refresh — same pattern as the G2 callback above.
// Called by handleBluetoothR1Tap after R1 AutoReconnect cmd_exec completes
// so the row repaints with the new ON/OFF state. Signature matches the
// G2HijackCmdCallback typedef in G2_HijackCmd.h: (ok, result, cookie,
// userData).
static void onBluetoothR1MenuRefreshDone(bool ok,
                                         const char* result,
                                         const G2CmdCookie& cookie,
                                         void* /*userData*/) {
  DEBUG_G2F("[G2] Bluetooth-R1-menu refresh cmd done: ok=%d seq=%llu menuGen=%u result='%s'",
            (int)ok, (unsigned long long)cookie.seq,
            (unsigned)cookie.menuGen, result ? result : "");
  enqueueWifiRedrawFromCallback(cookie, &showBluetoothR1Menu, "Bluetooth-R1-menu refresh");
}

static void handleWiFiTap(uint32_t idx) {
  switch (idx) {
    case 0:  // <- Back to top-level
      g2ShowNetworkMenu();
      break;

    case 1: {  // Radio power toggle (airplane mode) — routed via cmd_exec.
#if ENABLE_WIFI
      G2CmdCookie cookie{};
      cookie.targetPage   = g2GetHijackPage();
      cookie.targetNetSub = (uint8_t)gNetSub;
      const char* rcmd = wifiRadioOn() ? "radiopower off" : "radiopower on";
      DEBUG_G2F("[G2] Radio toggle: %s via cmd_exec", rcmd);
      gWifiPendingDeadlineMs = 0;   // radio power is not the WiFi "Pending" overlay
      if (!g2SubmitHijackCommand(rcmd, cookie, onWifiMenuRefreshDone, nullptr)) {
        DEBUG_G2F("[G2] Radio toggle: submit FAILED — no inline mutate");
      }
#endif
      break;
    }

    case 2: {  // WiFi: connect best / disconnect — toggle. Group A: routed via cmd_exec.
#if ENABLE_WIFI
      bool connected = WiFi.isConnected();
      G2CmdCookie cookie{};
      cookie.targetPage   = g2GetHijackPage();
      cookie.targetNetSub = (uint8_t)gNetSub;

      if (connected) {
        // Disconnect path. Submit `closewifi`; callback re-renders the
        // menu showing OFF. Synchronous from the user's perspective —
        // closewifi returns immediately after kicking the disconnect,
        // and from then on WiFi.isConnected() is false so the redraw
        // shows the right state.
        DEBUG_G2F("[G2] WiFi toggle: closewifi via cmd_exec");
        gWifiPendingDeadlineMs = 0;   // clear stale pending overlay
        if (!g2SubmitHijackCommand("closewifi", cookie,
                                   onWifiMenuRefreshDone, nullptr)) {
          // Never mutate WiFi on the tap/BLE task — queue full / OOM /
          // blank pairedByUser. UI stays as-is; user can retry.
          DEBUG_G2F("[G2] WiFi toggle: closewifi submit FAILED — no inline mutate");
        }
      } else {
        // Connect path. Optimistic UI: arm the "Pending..." deadline +
        // render synchronously BEFORE the submit so the user sees
        // immediate feedback (callback can't fire until openwifi returns,
        // and even that's before actual WiFi link-up). 150 ms BLE-flush
        // breath stays as-is — the REBUILD must hit the air before WiFi
        // takes radio coexistence. Watchdog still spawns to detect the
        // actual link-up event (cmd_exec callback only confirms the kick,
        // not the result; replacing the watchdog with WiFi.onEvent is a
        // separate followup).
        DEBUG_G2F("[G2] WiFi toggle: openwifi via cmd_exec (Pending overlay armed)");
        gWifiPendingDeadlineMs = millis() + 10000;
        showWiFiMenu();   // immediate "WiFi: Pending..." render
        vTaskDelay(pdMS_TO_TICKS(150));

        if (!g2SubmitHijackCommand("openwifi", cookie,
                                   onWifiMenuRefreshDone, nullptr)) {
          DEBUG_G2F("[G2] WiFi toggle: openwifi submit FAILED — no inline mutate");
          gWifiPendingDeadlineMs = 0;
          showWiFiMenu();
        } else if (!gWifiPendingTaskActive) {
          // Watchdog only when the kick actually queued — only one alive
          // at a time; subsequent taps re-arm gWifiPendingDeadlineMs.
          gWifiPendingTaskActive = true;
          if (xTaskCreatePinnedToCore(wifiPendingWatchdogTask, "g2_wifi_pending",
                          4096, nullptr, /*prio*/ 5, nullptr, APP_CORE) != pdPASS) {
            DEBUG_G2F("[G2] WiFi pending: xTaskCreate failed — "
                      "menu won't auto-refresh on connect");
            gWifiPendingTaskActive = false;
            gWifiPendingDeadlineMs = 0;
            showWiFiMenu();
          }
        }
      }
#endif
      break;
    }

    case 3: {  // Disconnect from the AP but KEEP the radio on, so you can scan /
               // reconnect to another network without re-enabling WiFi. Routed
               // through the `wifidisconnect` command (cmd_exec) like every other
               // action — this is deliberately NOT `closewifi`, which also stops
               // the HTTP server and disables web output. Works in any state,
               // including while "Pending...".
#if ENABLE_WIFI
      G2CmdCookie cookie{};
      cookie.targetPage   = g2GetHijackPage();
      cookie.targetNetSub = (uint8_t)gNetSub;
      gWifiPendingDeadlineMs = 0;   // cancel any pending overlay
      DEBUG_G2F("[G2] WiFi: Disconnect (radio stays on) via cmd_exec");
      if (!g2SubmitHijackCommand("wifidisconnect", cookie, onWifiMenuRefreshDone, nullptr)) {
        DEBUG_G2F("[G2] WiFi: wifidisconnect submit FAILED — no inline mutate");
      }
#endif
      break;
    }

    case 4:  // Status — drill into detailed info
      showWiFiStatusPage();
      break;

    case 5: {  // Scan Networks
#if ENABLE_WIFI
      DEBUG_G2F("[G2] WiFi: scan triggered from glasses");
      spawnNetworkScanWorker();
#endif
      break;
    }

    case 6: {  // List Saved
      showWiFiSavedList();
      break;
    }

    case 7: {  // Auto Start toggle — first hijack tap routed through cmd_exec.
               // Submits the existing `wifiautoreconnect <0|1>` CLI command
               // via g2SubmitHijackCommand. The completion callback enqueues
               // a Redraw job that the lens applier guards by menuGen and
               // dispatches into showWiFiMenu(). See proposal §5.4 + step 4.
               //
               // (HTTP toggle moved to its own Network -> HTTP(S) section;
               // see showHttpMenu / handleHttpTap below.)
      const bool prev = gSettings.wifiAutoStart;
      G2CmdCookie cookie{};
      cookie.targetPage   = g2GetHijackPage();
      cookie.targetNetSub = (uint8_t)gNetSub;
      // cookie.seq + cookie.menuGen are stamped by g2SubmitHijackCommand.

      char line[40];
      snprintf(line, sizeof(line), "wifiautoreconnect %d", prev ? 0 : 1);

      if (g2SubmitHijackCommand(line, cookie, onWifiMenuRefreshDone, nullptr)) {
        BROADCAST_PRINTF("[G2] WiFi: Auto Start toggle %s→%s submitted via cmd_exec",
                         prev ? "ON" : "OFF", prev ? "OFF" : "ON");
      } else {
        DEBUG_G2F("[G2] WiFi: Auto Start submit FAILED — no inline mutate");
      }
      break;
    }

    case 8:  // HTTP(S) >> — opens the HTTP submenu (server start/stop, HTTPS
             //              mode, auto-start, URL display).
      showHttpMenu();
      break;

    case 9:  // ESP-NOW >> — opens the ESP-NOW submenu (peer list, role, etc.)
      showEspNowMenu();
      break;

    default:
      DEBUG_G2F("[G2] WiFi: unknown idx=%u", (unsigned)idx);
      break;
  }
}

// Status page is info-only — every tap returns to the WiFi submenu.
static void handleWiFiStatusTap(uint32_t /*idx*/) {
  showWiFiMenu();
}

// --- Join-a-scanned-AP flow -------------------------------------------------
// Tap a scanned network → (secured) on-lens char-picker keyboard for the
// password → `wifiadd "<ssid>" "<pass>" 1 0` through cmd_exec (auth + audit,
// wifiadd is admin-gated so a non-admin pairer sees the denial text) → the
// command's result text on the lens. Open APs skip the keyboard and save with
// a quoted empty password. Accepted v1 limits: keyboard maxLen 32 (< the
// 63-char PSK ceiling — longer PSKs need the web UI) and password renders
// unmasked on the wearer-private lens.
static char sWifiAddSsid[33];
EXT_RAM_BSS_ATTR static char sWifiAddMsg[128];
static void showWifiAddMsg() { g2ShowText(sWifiAddMsg); }

static void onWifiAddDone(bool ok, const char* result,
                          const G2CmdCookie& cookie, void* /*userData*/) {
  DEBUG_G2F("[G2] wifiadd done: ok=%d result='%s'", (int)ok, result ? result : "");
  // Show the command's own text — success ("Saved network 'X' ...") and
  // failures (admin denial, list full) both belong on the lens, not just in
  // the log (the old stub's silence is exactly what we're fixing).
  strncpy(sWifiAddMsg,
          (result && result[0]) ? result : (ok ? "Network saved" : "Add failed"),
          sizeof(sWifiAddMsg) - 1);
  sWifiAddMsg[sizeof(sWifiAddMsg) - 1] = '\0';
  enqueueWifiRedrawFromCallback(cookie, &showWifiAddMsg, "wifiadd done");
}

// Build + submit from the snapshot SSID and a password. Runs on the BLE
// notify task (keyboard commit) — String build + submit only, no inline
// mutate (wrong stack + skips auth). The keyboard charset cannot produce a
// double-quote, so the password is always safe to quote verbatim.
static void wifiAddSubmit(const char* pass) {
  String line = String("wifiadd \"") + sWifiAddSsid + "\" \"" + (pass ? pass : "") + "\" 1 0";
  G2CmdCookie cookie{};
  cookie.targetPage   = g2GetHijackPage();
  cookie.targetNetSub = (uint8_t)gNetSub;
  if (!g2SubmitHijackCommand(line.c_str(), cookie, onWifiAddDone, nullptr)) {
    DEBUG_G2F("[G2] wifiadd: submit FAILED — no inline mutate");
    showScanResults();
  }
}

static void wifiAddPassCommit(const char* text) {
  if (!text || text[0] == '\0') {
    DEBUG_G2F("[G2] wifiadd: empty password — treated as cancel");
    showScanResults();
    return;
  }
  wifiAddSubmit(text);
}

static void wifiAddPassCancel() {
  DEBUG_G2F("[G2] wifiadd: password entry cancelled");
  showScanResults();
}

static void handleWiFiScanTap(uint32_t idx) {
  if (idx == 0) { showWiFiMenu(); return; }
  size_t apIdx = idx - 1;
  if (apIdx >= gScanCacheCount) return;
  const CachedAp& ap = gScanCache[apIdx];
  // Hidden APs scan with an empty SSID — nothing to add. An SSID containing a
  // double-quote can't survive the quoted command line (CommandArgs has no
  // escape support) — refuse rather than mangle it.
  if (ap.ssid.length() == 0 || ap.ssid.indexOf('"') >= 0) {
    DEBUG_G2F("[G2] WiFi: tapped hidden/unquotable SSID — ignored");
    return;
  }
  // Snapshot at tap time: the keyboard overlay blocks page taps but NOT the
  // scan worker, so this gScanCache entry can be rewritten while the
  // keyboard is open.
  strncpy(sWifiAddSsid, ap.ssid.c_str(), sizeof(sWifiAddSsid) - 1);
  sWifiAddSsid[sizeof(sWifiAddSsid) - 1] = '\0';
  if (!ap.secured) {
    DEBUG_G2F("[G2] WiFi: open AP '%s' — saving without password", sWifiAddSsid);
    wifiAddSubmit("");
    return;
  }
  DEBUG_G2F("[G2] WiFi: secured AP '%s' — opening password entry", sWifiAddSsid);
  TextEntryConfig cfg = {};
  cfg.prompt   = "WiFi Password";
  cfg.initial  = "";
  cfg.maxLen   = 32;   // keyboard cap; >32-char PSKs need the web UI
  cfg.onCommit = wifiAddPassCommit;
  cfg.onCancel = wifiAddPassCancel;
  cfg.isSecret = true;   // PSK — keep out of debug logs
  if (!g2BeginTextEntry(cfg)) {
    DEBUG_G2F("[G2] WiFi: text-entry failed to start");
  }
}

// Tapping a saved network now opens an action menu (Connect / Forget) instead
// of immediately forgetting it.
static void handleWiFiSavedTap(uint32_t idx) {
  if (idx == 0) { showWiFiMenu(); return; }
#if ENABLE_WIFI
  size_t savedIdx = idx - 1;
  if ((int)savedIdx < gWifiNetworkCount) {
    gWifiSavedSel = (int)savedIdx;
    showWiFiSavedActionMenu();
  }
#endif
}

// Action menu for one saved network: 0 = back, 1 = Connect, 2 = Forget.
static void handleWiFiSavedActionTap(uint32_t idx) {
  if (idx == 0) { showWiFiSavedList(); return; }   // <- back to the saved list
#if ENABLE_WIFI
  if (gWifiSavedSel < 0 || gWifiSavedSel >= gWifiNetworkCount) {
    showWiFiSavedList();
    return;
  }
  String ssid = gWifiNetworks[gWifiSavedSel].ssid;
  G2CmdCookie cookie{};
  cookie.targetPage = g2GetHijackPage();

  if (idx == 1) {  // Connect — openwifi --index is 1-based into gWifiNetworks.
    char line[40];
    snprintf(line, sizeof(line), "openwifi --index %d", gWifiSavedSel + 1);
    DEBUG_G2F("[G2] WiFi: Connect saved '%s' (--index %d) via cmd_exec",
              ssid.c_str(), gWifiSavedSel + 1);
    cookie.targetNetSub   = NET_SUB_WIFI;     // land back on the WiFi menu
    gWifiPendingDeadlineMs = millis() + 10000;
    setNetSub(NET_SUB_WIFI);
    showWiFiMenu();                            // immediate "Pending..." feedback
    vTaskDelay(pdMS_TO_TICKS(150));
    if (!g2SubmitHijackCommand(line, cookie, onWifiMenuRefreshDone, nullptr)) {
      DEBUG_G2F("[G2] WiFi: Connect saved submit FAILED");
      gWifiPendingDeadlineMs = 0;
      showWiFiMenu();
    } else if (!gWifiPendingTaskActive) {
      gWifiPendingTaskActive = true;
      if (xTaskCreatePinnedToCore(wifiPendingWatchdogTask, "g2_wifi_pending",
                      4096, nullptr, /*prio*/ 5, nullptr, APP_CORE) != pdPASS) {
        gWifiPendingTaskActive = false;
        gWifiPendingDeadlineMs = 0;
      }
    }
  } else {  // idx == 2: Forget — wifirm <ssid>, return to the saved list.
    cookie.targetNetSub = NET_SUB_WIFI_SAVED;
    String line = String("wifirm ") + ssid;
    DEBUG_G2F("[G2] WiFi: Forget saved '%s' via cmd_exec", ssid.c_str());
    if (!g2SubmitHijackCommand(line.c_str(), cookie,
                               onWifiSavedListRefreshDone, nullptr)) {
      DEBUG_G2F("[G2] WiFi: Forget saved submit FAILED — no inline mutate");
    }
  }
#endif
}

#if ENABLE_ESPNOW
// onCommit for the ESPNow name editor — submits `espnowsetname "<name>"`
// through cmd_exec so the rename runs as the paired-by user and goes
// through the same auth+log path as the CLI form. Completion callback
// re-renders the ESPNow submenu so the "Name:" row reflects the new
// value. Empty input is rejected before submit (Cancel covers the
// "I changed my mind" case). Submit failure is a no-op — never setSetting
// on the tap/BLE task (wrong stack + skips auth).
static void espnowNameCommit(const char* text) {
  if (!text || text[0] == '\0') {
    DEBUG_G2F("[G2] ESP-NOW Set Name: empty input — keeping previous");
    showEspNowMenu();
    return;
  }
  // Quoted like the login/filerename submits: the keyboard has a Space row
  // but no double-quote, so wrapping keeps a spaced name as one arg (the
  // handler then rejects it with its charset error instead of mangling).
  String line = String("espnowsetname \"") + text + "\"";
  G2CmdCookie cookie{};
  cookie.targetPage   = g2GetHijackPage();
  cookie.targetNetSub = (uint8_t)gNetSub;
  if (!g2SubmitHijackCommand(line.c_str(), cookie,
                             onEspNowMenuRefreshDone, nullptr)) {
    DEBUG_G2F("[G2] ESP-NOW Set Name: submit FAILED — no inline mutate");
    showEspNowMenu();
  }
}

static void espnowNameCancel() {
  DEBUG_G2F("[G2] ESP-NOW Set Name: cancelled");
  showEspNowMenu();
}
#endif

static void handleEspNowTap(uint32_t idx) {
  // Layout depends on running state:
  //   ON:  0=Back 1=toggle 2=Mode 3=Peers 4=ViewDevices 5=Name
  //   OFF: 0=Back 1=toggle 2=ViewDevices 3=Name
#if ENABLE_ESPNOW
  bool running = (gEspNow && gEspNow->initialized);
#else
  bool running = false;
#endif
  // Back goes to the WiFi submenu now — ESP-NOW is nested under WiFi
  // since both ride the WiFi radio.
  if (idx == 0) { showWiFiMenu(); return; }

  if (idx == 1) {  // ESP-NOW state line — toggle via cmd_exec
#if ENABLE_ESPNOW
    const char* line = running ? "closeespnow" : "openespnow";
    // For the OPEN path, use a callback that surfaces the failure reason on the
    // lens (init can fail on memory/coexistence); CLOSE just re-renders.
    G2HijackCmdCallback cb = running ? onEspNowMenuRefreshDone : onEspNowOpenDone;
    G2CmdCookie cookie{};
    cookie.targetPage   = g2GetHijackPage();
    cookie.targetNetSub = (uint8_t)gNetSub;
    DEBUG_G2F("[G2] ESP-NOW toggle: %s via cmd_exec", line);
    if (!g2SubmitHijackCommand(line, cookie, cb, nullptr)) {
      DEBUG_G2F("[G2] ESP-NOW toggle: %s submit FAILED — no inline mutate", line);
    }
#else
    DEBUG_G2F("[G2] ESP-NOW: not compiled in");
#endif
    return;
  }

#if ENABLE_ESPNOW
  // Name editor — same row index regardless of running state, just shifts
  // position. Brings up the on-glasses keyboard pre-filled with the
  // current name. onCommit/onCancel both redraw the ESPNow submenu.
  const uint32_t kNameIdxOn  = 5;
  const uint32_t kNameIdxOff = 3;
  if ((running && idx == kNameIdxOn) || (!running && idx == kNameIdxOff)) {
    TextEntryConfig cfg = {};
    cfg.prompt   = "ESPNow Name";
    cfg.initial  = gSettings.espnowDeviceName.c_str();
    cfg.maxLen   = 24;   // ESPNow name is short — host-friendly bound
    cfg.onCommit = espnowNameCommit;
    cfg.onCancel = espnowNameCancel;
    if (!g2BeginTextEntry(cfg)) {
      DEBUG_G2F("[G2] ESP-NOW: text-entry failed to start");
    }
    return;
  }
#endif

  // Auto Start row idx depends on running state — see showEspNowMenu().
#if ENABLE_ESPNOW
  const uint32_t kAutoIdxOn  = 6;
  const uint32_t kAutoIdxOff = 4;
  if ((running && idx == kAutoIdxOn) || (!running && idx == kAutoIdxOff)) {
    const bool prev = gSettings.espnowEnabled;
    char line[40];
    snprintf(line, sizeof(line), "espnowenabled %d", prev ? 0 : 1);
    G2CmdCookie cookie{};
    cookie.targetPage   = g2GetHijackPage();
    cookie.targetNetSub = (uint8_t)gNetSub;
    if (g2SubmitHijackCommand(line, cookie, onEspNowMenuRefreshDone, nullptr)) {
      BROADCAST_PRINTF("[G2] ESP-NOW: Auto Start toggle %s→%s submitted via cmd_exec",
                       prev ? "ON" : "OFF", prev ? "OFF" : "ON");
    } else {
      DEBUG_G2F("[G2] ESP-NOW: Auto Start submit FAILED — no inline mutate");
    }
    return;
  }
#endif

  if (running) {
    if (idx == 2 || idx == 3) {  // info rows
      DEBUG_G2F("[G2] ESP-NOW: info row %u (no action)", (unsigned)idx);
      return;
    }
    if (idx == 4) { showEspNowDevices(); return; }
  } else {
    if (idx == 2) { showEspNowDevices(); return; }
  }
  DEBUG_G2F("[G2] ESP-NOW: unknown idx=%u (running=%d)",
            (unsigned)idx, running ? 1 : 0);
}

static void handleEspNowDevsTap(uint32_t idx) {
  if (idx == 0) { showEspNowMenu(); return; }
  // Info-only — pairing/unpairing requires MAC + name text input.
  DEBUG_G2F("[G2] ESP-NOW: device row %u tapped (info-only)", (unsigned)idx);
}

// Toggle the persisted Bluetooth auto-start flag via the `bleautostart`
// CLI command (routed through cmd_exec → runs as the paired-by user).
// Hoisted out so every branch of handleBluetoothTap can call it without
// copy-pasting the submit boilerplate. Submit failure is a no-op.
static void bluetoothToggleAutoStart() {
  const bool prev = gSettings.bleAutoStart;
  const char* arg = prev ? "off" : "on";
  char line[32];
  snprintf(line, sizeof(line), "bleautostart %s", arg);
  G2CmdCookie cookie{};
  cookie.targetPage   = g2GetHijackPage();
  cookie.targetNetSub = (uint8_t)gNetSub;
  if (g2SubmitHijackCommand(line, cookie, onBluetoothMenuRefreshDone, nullptr)) {
    BROADCAST_PRINTF("[G2] Bluetooth: Auto Start toggle %s→%s submitted via cmd_exec",
                     prev ? "ON" : "OFF", prev ? "OFF" : "ON");
  } else {
    DEBUG_G2F("[G2] Bluetooth: Auto Start submit FAILED — no inline mutate");
  }
}

static void handleBluetoothG2Tap(uint32_t idx) {
  // Layout (after 2026-05-09 Mode-row promotion to parent menu):
  //   active:    0=Back 1=Conn(info) 2=AutoReconnect 3=Reconnect 4=Disconnect
  //   inactive:  0=Back 1=BLE-stopped(info) 2=AutoReconnect
  const bool active = bleSubsystemActive();
  if (idx == 0) {
    showBluetoothMenu();
    return;
  }
  if (idx == 1) {
    DEBUG_G2F("[G2] Bluetooth → G2: info row %u (no action)", (unsigned)idx);
    return;
  }
  if (idx == 2) {
    // Route through cmd_exec → `bleautoreconnect g2-glasses on|off`. This
    // is intentional, not just for consistency: cmd_bleautoreconnect calls
    // bleStampPairedByIfBlank when flipping ON, capturing whoever owns
    // the BT subsystem. With auth.user = pairedByUser, that means the
    // glasses become "owned by themselves" the first time you flip this
    // ON from a freshly-paired set — which is fine, since the very first
    // flip would have come from a real admin via the web UI or CLI to
    // pair them in the first place.
    const bool prev = gBlePeerData[BLE_PEER_G2_GLASSES].autoReconnect;
    const char* arg = prev ? "off" : "on";
    char line[64];
    snprintf(line, sizeof(line), "bleautoreconnect g2-glasses %s", arg);
    G2CmdCookie cookie{};
    cookie.targetPage   = g2GetHijackPage();
    cookie.targetNetSub = (uint8_t)gNetSub;
    if (g2SubmitHijackCommand(line, cookie, onBluetoothG2MenuRefreshDone, nullptr)) {
      BROADCAST_PRINTF("[G2] G2 AutoReconnect %s→%s submitted via cmd_exec",
                       prev ? "ON" : "OFF", prev ? "OFF" : "ON");
    } else {
      DEBUG_G2F("[G2] G2 AutoReconnect submit FAILED — no inline mutate");
    }
    return;
  }
  if (!active) {
    DEBUG_G2F("[G2] Bluetooth → G2: idx=%u — start BLE first", (unsigned)idx);
    return;
  }
  if (idx == 3) {
    // Saved-MAC reconnect — same as boot path. Must go through cmd_exec
    // (auth + stack); never call g2ConnectSaved() on the tap task.
    G2CmdCookie cookie{};
    cookie.targetPage   = g2GetHijackPage();
    cookie.targetNetSub = (uint8_t)gNetSub;
    DEBUG_G2F("[G2] Reconnect G2 via cmd_exec (openg2 saved)");
    if (!g2SubmitHijackCommand("openg2 saved", cookie,
                               onBluetoothG2MenuRefreshDone, nullptr)) {
      DEBUG_G2F("[G2] Reconnect G2 submit FAILED — no inline mutate");
    }
    return;
  }
  if (idx == 4) {
    // Soft disconnect (keep GATT cache). Full teardown is g2deinit / closeg2 full.
    G2CmdCookie cookie{};
    cookie.targetPage   = g2GetHijackPage();
    cookie.targetNetSub = (uint8_t)gNetSub;
    DEBUG_G2F("[G2] Disconnect G2 via cmd_exec (closeg2)");
    if (!g2SubmitHijackCommand("closeg2", cookie,
                               onBluetoothG2MenuRefreshDone, nullptr)) {
      DEBUG_G2F("[G2] Disconnect G2 submit FAILED — no inline mutate");
    }
    return;
  }
  DEBUG_G2F("[G2] Bluetooth → G2: unknown idx=%u", (unsigned)idx);
}

// Forward decl — the R1 menu refresh callback lives in the same
// callbacks block as onBluetoothG2MenuRefreshDone. Declared here so
// handleBluetoothR1Tap below can pass it as the cmd_exec completion.
static void onBluetoothR1MenuRefreshDone(bool ok,
                                         const char* result,
                                         const G2CmdCookie& cookie,
                                         void* userData);

static void handleBluetoothR1Tap(uint32_t idx) {
  // Layout (mirror of handleBluetoothG2Tap):
  //   active+ringUp:    0=Back 1=Conn 2=AutoReconnect 3=Reconnect 4=Disconnect
  //   active+ringDown:  0=Back 1=Conn 2=AutoReconnect 3=Connect
  //   inactive:         0=Back 1=BLE-stopped 2=AutoReconnect
  const bool active = bleSubsystemActive();
  const bool ringUp = g2RingIsConnected();
  if (idx == 0) {
    showBluetoothMenu();
    return;
  }
  if (idx == 1) {
    DEBUG_G2F("[G2] Bluetooth → R1: info row %u (no action)", (unsigned)idx);
    return;
  }
  if (idx == 2) {
    // R1 AutoReconnect — same cmd_exec pattern as G2 AutoReconnect, just
    // targeting the r1-ring peer instead of g2-glasses.
    const bool prev = gBlePeerData[BLE_PEER_R1_RING].autoReconnect;
    const char* arg = prev ? "off" : "on";
    char line[64];
    snprintf(line, sizeof(line), "bleautoreconnect r1-ring %s", arg);
    G2CmdCookie cookie{};
    cookie.targetPage   = g2GetHijackPage();
    cookie.targetNetSub = (uint8_t)gNetSub;
    if (g2SubmitHijackCommand(line, cookie, onBluetoothR1MenuRefreshDone, nullptr)) {
      BROADCAST_PRINTF("[G2] R1 AutoReconnect %s→%s submitted via cmd_exec",
                       prev ? "ON" : "OFF", prev ? "OFF" : "ON");
    } else {
      DEBUG_G2F("[G2] R1 AutoReconnect submit FAILED — no inline mutate");
    }
    return;
  }
  if (!active) {
    DEBUG_G2F("[G2] Bluetooth → R1: idx=%u — start BLE first", (unsigned)idx);
    return;
  }
  if (idx == 3) {
    // ringUp → reconnect (drop + settle + connect on cmd_exec); else connect.
    // Heap guard + 500 ms settle live inside `ringconnect reconnect`.
    const char* line = ringUp ? "ringconnect reconnect" : "ringconnect";
    G2CmdCookie cookie{};
    cookie.targetPage   = g2GetHijackPage();
    cookie.targetNetSub = (uint8_t)gNetSub;
    BROADCAST_PRINTF("[G2] Bluetooth: %s Ring via cmd_exec",
                     ringUp ? "Reconnect" : "Connect");
    // Fresh connect: `ringconnect` returns OK on kick-off, but scan+connect+auth
    // finishes ~5-8 s later with no completion event. Arm the "connecting..."
    // overlay + watchdog so the menu flips to "connected" when it actually is,
    // instead of staying stuck on "disconnected". (Reconnect keeps ringUp true,
    // so its menu already reads right — no overlay needed.) Mirrors WiFi connect.
    if (!ringUp) {
      gRingPendingDeadlineMs = millis() + 12000;
      showBluetoothR1Menu();            // immediate "Ring: connecting..." render
      vTaskDelay(pdMS_TO_TICKS(150));   // let the REBUILD hit the air before the scan storms BLE
    }
    if (!g2SubmitHijackCommand(line, cookie,
                               onBluetoothR1MenuRefreshDone, nullptr)) {
      DEBUG_G2F("[G2] R1 %s submit FAILED — no inline mutate",
                ringUp ? "reconnect" : "connect");
      gRingPendingDeadlineMs = 0;
      if (!ringUp) showBluetoothR1Menu();
    } else if (!ringUp && !gRingPendingTaskActive) {
      gRingPendingTaskActive = true;
      if (xTaskCreatePinnedToCore(ringPendingWatchdogTask, "g2_ring_pending",
                      4096, nullptr, /*prio*/ 5, nullptr, APP_CORE) != pdPASS) {
        DEBUG_G2F("[G2] Ring pending: xTaskCreate failed — menu won't auto-refresh on connect");
        gRingPendingTaskActive = false;
        gRingPendingDeadlineMs = 0;
      }
    }
    return;
  }
  if (ringUp && idx == 4) {
    G2CmdCookie cookie{};
    cookie.targetPage   = g2GetHijackPage();
    cookie.targetNetSub = (uint8_t)gNetSub;
    BROADCAST_PRINTF("[G2] Bluetooth: Disconnect Ring via cmd_exec");
    if (!g2SubmitHijackCommand("ringdisconnect", cookie,
                               onBluetoothR1MenuRefreshDone, nullptr)) {
      DEBUG_G2F("[G2] R1 disconnect submit FAILED — no inline mutate");
    }
    return;
  }
  DEBUG_G2F("[G2] Bluetooth → R1: unknown idx=%u (ringUp=%d)",
            (unsigned)idx, ringUp ? 1 : 0);
}

static void handleBluetoothTap(uint32_t idx) {
  // Layout (keep in sync with showBluetoothMenu) — restructured 2026-05-09
  // to promote Mode out of the G2 submenu and consolidate ring controls
  // under a new R1 >> drill-in:
  //   active + server:           0=Back 1=BLE 2=Mode 3=Conn 4=ToggleAdv 5=Disconnect 6=Auto
  //   active + client:           0=Back 1=BLE 2=Mode 3=G2>> 4=R1>> 5=Auto
  //   inactive + server:         0=Back 1=BLE 2=Mode 3=Auto
  //   inactive + client:         0=Back 1=BLE 2=Mode 3=G2>> 4=R1>> 5=Auto
  const bool active   = bleSubsystemActive();
  const bool isClient = (gSettings.bleMode == BLE_MODE_G2_CLIENT);
  if (idx == 0) { g2ShowNetworkMenu(); return; }

  if (idx == 1) {  // toggle — mode-aware, routed via cmd_exec
    // Map (mode, active) → CLI command name:
    //   client + active   → closeg2   (yanks the lens out from under the user)
    //   client + !active  → openg2
    //   server + active   → closeble
    //   server + !active  → openble
    // Client+active is destructive to the lens redraw — the connection
    // we'd repaint on goes down. The completion callback will find the
    // lens disconnected and the redraw will skip silently in
    // g2EnqueueLensJob, which is fine.
    const char* line;
    if (active) line = isClient ? "closeg2" : "closeble";
    else        line = isClient ? "openg2"  : "openble";

    G2CmdCookie cookie{};
    cookie.targetPage   = g2GetHijackPage();
    cookie.targetNetSub = (uint8_t)gNetSub;
    DEBUG_G2F("[G2] Bluetooth toggle: %s via cmd_exec", line);
    if (!g2SubmitHijackCommand(line, cookie,
                               onBluetoothMenuRefreshDone, nullptr)) {
      DEBUG_G2F("[G2] Bluetooth toggle: %s submit FAILED — no inline mutate", line);
    }
    return;
  }

  if (idx == 2) {  // Mode toggle (server ↔ client) — routed via cmd_exec.
    // `blemode <other>` tears down whichever side is currently active
    // before flipping the persisted setting (see cmd_blemode in
    // Bluetooth.cpp). Destructive to the lens hijack when going from
    // client + active → server: the BLE link we render through is
    // about to drop. Same caveat as the BLE on/off toggle on idx=1;
    // the completion callback's redraw will silently no-op once the
    // link is gone.
    const char* arg = isClient ? "server" : "client";
    char line[24];
    snprintf(line, sizeof(line), "blemode %s", arg);
    G2CmdCookie cookie{};
    cookie.targetPage   = g2GetHijackPage();
    cookie.targetNetSub = (uint8_t)gNetSub;
    DEBUG_G2F("[G2] Bluetooth Mode toggle: %s via cmd_exec", line);
    if (!g2SubmitHijackCommand(line, cookie,
                               onBluetoothMenuRefreshDone, nullptr)) {
      DEBUG_G2F("[G2] Bluetooth Mode toggle: submit FAILED — no inline mutate");
    }
    return;
  }

  if (!active) {
    if (isClient) {
      // 0=Back 1=BLE 2=Mode 3=G2>> 4=R1>> 5=Auto
      if (idx == 3) { showBluetoothG2Menu(); return; }
      if (idx == 4) { showBluetoothR1Menu(); return; }
      if (idx == 5) { bluetoothToggleAutoStart(); return; }
    } else {
      // 0=Back 1=BLE 2=Mode 3=Auto
      if (idx == 3) { bluetoothToggleAutoStart(); return; }
    }
    DEBUG_G2F("[G2] Bluetooth: idx=%u while OFF (unknown)", (unsigned)idx);
    return;
  }

  if (isClient) {
    // 0=Back 1=BLE 2=Mode 3=G2>> 4=R1>> 5=Auto
    if (idx == 3) { showBluetoothG2Menu(); return; }
    if (idx == 4) { showBluetoothR1Menu(); return; }
    if (idx == 5) { bluetoothToggleAutoStart(); return; }
    DEBUG_G2F("[G2] Bluetooth (client): unknown idx=%u", (unsigned)idx);
    return;
  }

  // Server-mode layout — Mode (idx 2) is now a toggle handled before
  // this switch. Conn (idx 3) remains info-only.
  switch (idx) {
    case 3:  // Conn info row
      DEBUG_G2F("[G2] Bluetooth: info row %u (no action)", (unsigned)idx);
      break;

    case 4: {  // Toggle Adv — routed via cmd_exec (`bleadv toggle` was
               // extended in Bluetooth.cpp to support stop/toggle).
      G2CmdCookie cookie{};
      cookie.targetPage   = g2GetHijackPage();
      cookie.targetNetSub = (uint8_t)gNetSub;
      if (!g2SubmitHijackCommand("bleadv toggle", cookie,
                                 onBluetoothMenuRefreshDone, nullptr)) {
        DEBUG_G2F("[G2] Bluetooth: bleadv toggle submit FAILED — no inline mutate");
      }
      break;
    }

    case 5: {  // Disconnect — routed via cmd_exec (`bledisconnect`).
      if (!isBLEConnected()) {
        DEBUG_G2F("[G2] Bluetooth: not connected, no-op");
        showBluetoothMenu();
        break;
      }
      G2CmdCookie cookie{};
      cookie.targetPage   = g2GetHijackPage();
      cookie.targetNetSub = (uint8_t)gNetSub;
      if (!g2SubmitHijackCommand("bledisconnect", cookie,
                                 onBluetoothMenuRefreshDone, nullptr)) {
        DEBUG_G2F("[G2] Bluetooth: bledisconnect submit FAILED — no inline mutate");
      }
      break;
    }

    case 6:  // Auto Start (server-mode layout)
      bluetoothToggleAutoStart();
      break;

    default:
      DEBUG_G2F("[G2] Bluetooth: unknown idx=%u", (unsigned)idx);
      break;
  }
}

// -----------------------------------------------------------------------------
// HTTP(S) submenu — server start/stop, HTTPS-mode toggle, auto-start
// -----------------------------------------------------------------------------
//
// HTTP truly depends on WiFi being connected (no IP → no serving). The
// "(WiFi req.)" tag mirrors the one on the top-level HTTP(S) row so the
// reason for inaction is visible from inside the section too. The toggle
// itself still fires when tapped — the underlying cmd_httpstart returns a
// useful error in that case rather than silently no-op'ing, which means
// the failure shows up in the broadcast log instead of "I tapped and
// nothing happened."
//
// HTTPS mode is a build/setting concern: changing gSettings.httpsEnabled
// requires a reboot to take effect (cert load happens during init), so
// the row label notes that explicitly.

static void showHttpMenu() {
  setNetSub(NET_SUB_HTTP);
  static char serverLine[40];
  static char httpsLine[32];
  static char autoLine[32];
  static char ipLine[40];

#if ENABLE_HTTP_SERVER
  bool running = (server != nullptr);
  bool connected = false;
  #if ENABLE_WIFI
  connected = WiFi.isConnected();
  #endif

  if (running) {
    snprintf(serverLine, sizeof(serverLine), "Stop %s",
             gServerIsHttps ? "HTTPS" : "HTTP");
  } else if (!connected) {
    // Not running AND no WiFi → tapping won't accomplish anything until
    // WiFi comes up. Tag accordingly so the user knows why.
    snprintf(serverLine, sizeof(serverLine), "Start %s (WiFi req.)",
             gSettings.httpsEnabled ? "HTTPS" : "HTTP");
  } else {
    snprintf(serverLine, sizeof(serverLine), "Start %s",
             gSettings.httpsEnabled ? "HTTPS" : "HTTP");
  }

  // HTTPS toggle — value applies on next openhttp / next boot. The "(reboot
  // req.)" tag matches the cmd_httpsEnabled response wording.
  snprintf(httpsLine, sizeof(httpsLine), "HTTPS: %s (reboot req.)",
           gSettings.httpsEnabled ? "ON" : "OFF");

  snprintf(autoLine, sizeof(autoLine), "Auto Start: %s",
           gSettings.httpAutoStart ? "ON" : "OFF");

  // Show the URL when running so the wearer can see where to point the
  // browser without leaving the lens. Hidden when offline / not running.
  if (running && connected) {
  #if ENABLE_WIFI
    snprintf(ipLine, sizeof(ipLine), "URL: %s://%s",
             gServerIsHttps ? "https" : "http",
             WiFi.localIP().toString().c_str());
  #else
    snprintf(ipLine, sizeof(ipLine), "URL: %s://(?)",
             gServerIsHttps ? "https" : "http");
  #endif
  } else {
    ipLine[0] = '\0';
  }
#else
  snprintf(serverLine, sizeof(serverLine), "Server: not compiled");
  httpsLine[0] = '\0';
  autoLine[0] = '\0';
  ipLine[0] = '\0';
#endif

  // Variable row count — URL row only included when the server is up.
  // Item indices are dispatched by handleHttpTap below; keep them in sync
  // when adding rows.
  const char* items[8];
  size_t n = 0;
  items[n++] = "<- WiFi";     // 0 — HTTP(S) is nested under WiFi
  items[n++] = serverLine;    // 1 — start/stop toggle
#if ENABLE_HTTP_SERVER
  items[n++] = httpsLine;     // 2 — HTTPS mode toggle
  items[n++] = autoLine;      // 3 — Auto Start toggle
  if (ipLine[0]) {
    items[n++] = ipLine;      // 4 — info-only URL (when running)
  }
#endif

  g2ShowListPage(items, n);
  DEBUG_G2F("[G2] HTTP submenu shown (running=%d, https=%d, auto=%d)",
            (server != nullptr) ? 1 : 0,
            gSettings.httpsEnabled ? 1 : 0,
            gSettings.httpAutoStart ? 1 : 0);
}

// Completion callback for any cmd_exec submission inside the HTTP submenu.
// Re-renders the same submenu via the lens-applier Redraw path so the new
// state (running flag, label change) shows up as soon as cmd_exec finishes.
static void onHttpMenuRefreshDone(bool /*ok*/, const char* /*result*/,
                                  const G2CmdCookie& cookie, void* /*userData*/) {
  enqueueWifiRedrawFromCallback(cookie, &showHttpMenu, "HTTP-menu refresh");
}

static void handleHttpTap(uint32_t idx) {
  // Back goes to the WiFi submenu now — HTTP(S) is nested under WiFi
  // since the server can only run with a WiFi connection.
  if (idx == 0) { showWiFiMenu(); return; }

#if ENABLE_HTTP_SERVER
  G2CmdCookie cookie{};
  cookie.targetPage   = g2GetHijackPage();
  cookie.targetNetSub = (uint8_t)gNetSub;

  switch (idx) {
    case 1: {  // Start/Stop server toggle
      bool wasRunning = (server != nullptr);
      const char* line = wasRunning ? "closehttp" : "openhttp";
      DEBUG_G2F("[G2] HTTP toggle: %s via cmd_exec", line);
      if (!g2SubmitHijackCommand(line, cookie, onHttpMenuRefreshDone, nullptr)) {
        DEBUG_G2F("[G2] HTTP toggle: %s submit FAILED — no inline mutate", line);
      }
      break;
    }

    case 2: {  // HTTPS mode toggle (reboot required to take effect)
      const bool prev = gSettings.httpsEnabled;
      char line[40];
      snprintf(line, sizeof(line), "httpsEnabled %d", prev ? 0 : 1);
      if (g2SubmitHijackCommand(line, cookie, onHttpMenuRefreshDone, nullptr)) {
        BROADCAST_PRINTF("[G2] HTTPS mode toggle %s→%s submitted (reboot to apply)",
                         prev ? "ON" : "OFF", prev ? "OFF" : "ON");
      } else {
        DEBUG_G2F("[G2] HTTPS mode submit FAILED — no inline mutate");
      }
      break;
    }

    case 3: {  // HTTP Auto Start toggle (boot-time start)
      const bool prev = gSettings.httpAutoStart;
      char line[40];
      snprintf(line, sizeof(line), "httpAutoStart %d", prev ? 0 : 1);
      if (g2SubmitHijackCommand(line, cookie, onHttpMenuRefreshDone, nullptr)) {
        BROADCAST_PRINTF("[G2] HTTP Auto Start toggle %s→%s submitted",
                         prev ? "ON" : "OFF", prev ? "OFF" : "ON");
      } else {
        DEBUG_G2F("[G2] HTTP Auto Start submit FAILED — no inline mutate");
      }
      break;
    }

    case 4:  // URL row (info only)
      DEBUG_G2F("[G2] HTTP: URL row tapped (info-only)");
      break;

    default:
      DEBUG_G2F("[G2] HTTP: unknown idx=%u", (unsigned)idx);
      break;
  }
#else
  (void)idx;
  DEBUG_G2F("[G2] HTTP: server not compiled in");
#endif
}

void g2NetworkHandleTap(uint32_t idx) {
  switch (gNetSub) {
    case NET_SUB_MAIN:        handleMainTap(idx);        break;
    case NET_SUB_WIFI:        handleWiFiTap(idx);        break;
    case NET_SUB_WIFI_SCAN:   handleWiFiScanTap(idx);    break;
    case NET_SUB_WIFI_SAVED:  handleWiFiSavedTap(idx);   break;
    case NET_SUB_WIFI_SAVED_ACT: handleWiFiSavedActionTap(idx); break;
    case NET_SUB_WIFI_STATUS: handleWiFiStatusTap(idx);  break;
    case NET_SUB_ESPNOW:      handleEspNowTap(idx);      break;
    case NET_SUB_ESPNOW_DEVS: handleEspNowDevsTap(idx);  break;
    case NET_SUB_BLUETOOTH:   handleBluetoothTap(idx);   break;
    case NET_SUB_BLUETOOTH_G2: handleBluetoothG2Tap(idx); break;
    case NET_SUB_BLUETOOTH_R1: handleBluetoothR1Tap(idx); break;
    case NET_SUB_HTTP:        handleHttpTap(idx);        break;
  }
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
