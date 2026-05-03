// =============================================================================
// G2 glasses — "Network" page implementation
// =============================================================================
// See header for the contract. Top-level Network chooser drills into one
// of three subsystem submenus (WiFi / ESP-NOW / Bluetooth) plus several
// list sub-pages. Tap-only — anything in the OLED that needs text input
// (e.g. wifiadd <ssid> <password>, espnowsetname) is omitted.

#include "G2_Page_Network.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "G2_Glasses.h"
#include "G2_Ring.h"   // g2RingConnect/Disconnect/IsConnected
#include "Bluetooth.h"
#include "G2_Page_TextEntry.h"   // on-glasses keyboard for "Set Name"
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
extern httpd_handle_t server;
extern bool gServerIsHttps;
extern const char* cmd_httpstart(const String&);
extern const char* cmd_httpstop(const String&);
#endif

// -----------------------------------------------------------------------------
// Local state
// -----------------------------------------------------------------------------

enum NetworkSubMode : uint8_t {
  NET_SUB_MAIN          = 0,  // top-level chooser (WiFi / ESP-NOW / Bluetooth)
  NET_SUB_WIFI          = 1,  // WiFi action menu
  NET_SUB_WIFI_SCAN     = 2,  // WiFi scan results
  NET_SUB_WIFI_SAVED    = 3,  // WiFi saved-network list (tap = forget)
  NET_SUB_WIFI_STATUS   = 4,  // WiFi detailed status dump (info only)
  NET_SUB_ESPNOW        = 5,  // ESP-NOW action menu
  NET_SUB_ESPNOW_DEVS   = 6,  // ESP-NOW paired-device list (info only)
  NET_SUB_BLUETOOTH     = 7,  // Bluetooth action menu
};
static NetworkSubMode gNetSub = NET_SUB_MAIN;

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
  gNetSub = NET_SUB_MAIN;
  const char* items[] = {
    "<- Main Menu",   // 0 — exits Network back to the hijack root
    "WiFi >>",        // 1
    "ESP-NOW >>",     // 2
    "Bluetooth >>",   // 3
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
  gNetSub = NET_SUB_WIFI;
  static char wifiLine[64];
  static char httpLine[24];
  static char autoLine[32];   // "Auto Start: ON/OFF" — boot-time auto-connect

#if ENABLE_WIFI
  bool connected = WiFi.isConnected();
  const bool pending = !connected && gWifiPendingDeadlineMs > 0 &&
                       (int32_t)(millis() - gWifiPendingDeadlineMs) < 0;
  if (connected) {
    String ssid = WiFi.SSID();
    // Truncate long SSIDs so the "WiFi: ON | …" prefix still fits the
    // ~32-char visible row width.
    String shown = (ssid.length() > 14) ? (ssid.substring(0, 14) + "~") : ssid;
    snprintf(wifiLine, sizeof(wifiLine), "WiFi: ON | %s", shown.c_str());
  } else if (pending) {
    snprintf(wifiLine, sizeof(wifiLine), "WiFi: Pending...");
  } else {
    snprintf(wifiLine, sizeof(wifiLine), "WiFi: OFF");
  }
#else
  bool connected = false;
  snprintf(wifiLine, sizeof(wifiLine), "WiFi: not compiled");
#endif

#if ENABLE_HTTP_SERVER
  bool httpRunning = (server != nullptr);
  if (httpRunning) {
    snprintf(httpLine, sizeof(httpLine), "Stop %s",
             gServerIsHttps ? "HTTPS" : "HTTP");
  } else {
    snprintf(httpLine, sizeof(httpLine), "Start HTTP");
  }
#else
  snprintf(httpLine, sizeof(httpLine), "HTTP: n/a");
#endif

  snprintf(autoLine, sizeof(autoLine), "Auto Start: %s",
           gSettings.wifiAutoReconnect ? "ON" : "OFF");

  // Item indices match dispatch in handleWiFiTap. Top WiFi line is now
  // the on/off toggle; "Connect Best" / "Disconnect" were removed since
  // tapping the toggle does the same thing.
  const char* items[] = {
    "<- Network",      // 0 — back to Network top-level chooser
    wifiLine,          // 1 (toggle: tap = connect best / disconnect)
    "Status",          // 2 (drill into detailed info dump)
    "Scan Networks",   // 3
    "List Saved",      // 4
    httpLine,          // 5
    autoLine,          // 6 (toggle wifiAutoReconnect — boot-time)
  };
  g2ShowListPage(items, sizeof(items) / sizeof(items[0]));
  DEBUG_G2F("[G2] WiFi submenu shown (connected=%d, auto=%d)",
            connected ? 1 : 0, gSettings.wifiAutoReconnect ? 1 : 0);
}

// Detailed WiFi status — shown when the user taps "Status" on the WiFi
// submenu. List-style page so the rows are individually visible without
// the firmware's TEXT-view pagination juggling. All info, no actions —
// every tap returns to the WiFi submenu.
static void showWiFiStatusPage() {
  gNetSub = NET_SUB_WIFI_STATUS;
#if ENABLE_WIFI
  // 12 row buffers is comfortable headroom — current layout uses up to 10.
  static char rows[12][48];
  const char* ptrs[12];
  size_t n = 0;

  strcpy(rows[n], "<- WiFi"); ptrs[n] = rows[n]; n++;
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

// Render scan results as a tappable list. Items:
//   0: "<- WiFi" (returns to WiFi submenu)
//   1..N: SSID lines with RSSI suffix
static void showScanResults() {
  gNetSub = NET_SUB_WIFI_SCAN;
  static char rows[1 + 8][48];
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
  gNetSub = NET_SUB_WIFI_SAVED;
#if ENABLE_WIFI
  static char rows[1 + MAX_WIFI_NETWORKS][48];
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

// -----------------------------------------------------------------------------
// ESP-NOW submenu
// -----------------------------------------------------------------------------

static void showEspNowMenu() {
  gNetSub = NET_SUB_ESPNOW;
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
           gSettings.espnowenabled ? "ON" : "OFF");
  bool running = (gEspNow && gEspNow->initialized);
  if (running) {
    snprintf(stateLine, sizeof(stateLine), "ESP-NOW: ON");
    snprintf(modeLine, sizeof(modeLine), "Mode: %s", getEspNowModeString());
    snprintf(devsLine, sizeof(devsLine), "Peers: %d",
             gEspNow->deviceCount);
    const char* items[] = {
      "<- Network",      // 0 — back to Network top-level chooser
      stateLine,         // 1 (toggle)
      modeLine,          // 2 (info)
      devsLine,          // 3 (info)
      "View Devices",    // 4
      nameLine,          // 5 (tap → on-glasses text entry)
      autoLine,          // 6 (toggle espnowenabled — boot-time)
    };
    g2ShowListPage(items, sizeof(items) / sizeof(items[0]));
  } else {
    snprintf(stateLine, sizeof(stateLine), "ESP-NOW: OFF");
    // Only show toggle + View Devices + name editor when off; no point
    // listing Mode/Peers lines for a subsystem that isn't running.
    const char* items[] = {
      "<- Network",      // 0 — back to Network top-level chooser
      stateLine,         // 1 (toggle — tap to start)
      "View Devices",    // 2
      nameLine,          // 3 (tap → on-glasses text entry)
      autoLine,          // 4 (toggle espnowenabled — boot-time)
    };
    g2ShowListPage(items, sizeof(items) / sizeof(items[0]));
  }
#else
  static const char* na[] = { "<- Network", "ESP-NOW: not compiled" };
  g2ShowListPage(na, 2);
#endif
  DEBUG_G2F("[G2] ESP-NOW submenu shown (auto=%d)",
            gSettings.espnowenabled ? 1 : 0);
}

static void showEspNowDevices() {
  gNetSub = NET_SUB_ESPNOW_DEVS;
#if ENABLE_ESPNOW
  static char rows[1 + 8][48];
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
  static const char* na[] = { "<- Network", "(ESP-NOW not compiled)" };
  g2ShowListPage(na, 2);
#endif
}

// -----------------------------------------------------------------------------
// Bluetooth submenu
// -----------------------------------------------------------------------------

// R1 Ring connect — relocated here from the Test Suite so it sits next
// to the rest of the BLE-peripheral controls. Identical heap-low guard
// and disconnect-then-connect sequence as the original; the Arduino BLE
// stack leaks ~30 KB per server↔client reconnect cycle, so a long
// session can run DRAM low enough that spawning the ring connect task
// AND the page-swap worker back-to-back triggers an xTaskCreate
// failure followed seconds later by a BLE-stack assert-and-reboot
// (observed 2026-04-25 at heap ~2.5 KB). 16 KB is "enough for both
// tasks plus pb buffer plus a margin"; below that we abort with a
// clear log and recovery instructions.
static void triggerRingReconnect() {
  const uint32_t freeNow = ESP.getFreeHeap();
  if (freeNow < 16 * 1024) {
    DEBUG_G2F("[G2] Reconnect Ring: ABORTED — DRAM free %u B < 16 KB safety "
              "threshold. Reboot or disconnect/reconnect via web UI to "
              "recover heap (Arduino BLE leak per reconnect cycle is the "
              "usual cause).",
              (unsigned)freeNow);
    return;
  }

  // Force-disconnect first to give the BLE stack a clean slate.
  const bool wasUp = g2RingIsConnected();
  if (wasUp) {
    DEBUG_G2F("[G2] Reconnect Ring: dropping current connection first "
              "(heap=%u)", (unsigned)freeNow);
    g2RingDisconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
  } else {
    DEBUG_G2F("[G2] Reconnect Ring: ring already disconnected, "
              "skipping disconnect step (heap=%u)", (unsigned)freeNow);
  }

  if (g2RingConnect()) {
    DEBUG_G2F("[G2] Reconnect Ring: connect task started — "
              "watch ringstatus for completion");
  } else {
    DEBUG_G2F("[G2] Reconnect Ring: g2RingConnect() returned false — "
              "run a Temples Scan first to populate ring advertisement");
  }
  // Caller is responsible for re-rendering the menu; this helper just
  // kicks off the ring work.
}

// Force-disconnect the ring without re-connecting. Mirror of
// triggerRingReconnect's first half. No heap guard — disconnect doesn't
// allocate; only connect does.
static void triggerRingDisconnect() {
  if (!g2RingIsConnected()) {
    DEBUG_G2F("[G2] Disconnect Ring: already disconnected — no-op");
    return;
  }
  DEBUG_G2F("[G2] Disconnect Ring: dropping connection (heap=%u)",
            (unsigned)ESP.getFreeHeap());
  g2RingDisconnect();
}

static void showBluetoothMenu() {
  gNetSub = NET_SUB_BLUETOOTH;
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
           gSettings.bluetoothAutoStart ? "ON" : "OFF");
  // Per-mode "connected" semantics: server cares about phone connection,
  // client cares about whether the temple BLE links are live.
  const bool connected = isClient ? isG2Connected() : isBLEConnected();
  if (active) {
    snprintf(stateLine, sizeof(stateLine), "BLE: ON (%s)",
             bleSubsystemStateString());
    snprintf(modeLine, sizeof(modeLine), "Mode: %s",
             isClient ? "client (G2)" : "server");
    snprintf(connLine, sizeof(connLine), "Conn: %s",
             connected ? "yes" : "no");
    if (isClient) {
      // Client mode: no advertising row (server isn't running). Tapping
      // the toggle disconnects the client; "Disconnect G2" does the
      // same so we don't double up. R1 Ring rows appear after the G2
      // controls — ring is a separate BLE-central peer that lives on
      // the same controller and is most often paired with the glasses.
      // "Disconnect Ring" only renders when actually connected so the
      // menu doesn't carry a no-op row.
      static char ringLine[40];
      const bool ringUp = g2RingIsConnected();
      snprintf(ringLine, sizeof(ringLine), "Ring: %s",
               ringUp ? "connected" : "disconnected");
      if (ringUp) {
        const char* items[] = {
          "<- Network",      // 0
          stateLine,         // 1 (toggle = stop client)
          modeLine,          // 2 (info)
          connLine,          // 3 (info)
          "Disconnect G2",   // 4
          ringLine,          // 5 (info)
          "Reconnect Ring",  // 6
          "Disconnect Ring", // 7
          autoLine,          // 8 (toggle bluetoothAutoStart)
        };
        g2ShowListPage(items, sizeof(items) / sizeof(items[0]));
      } else {
        const char* items[] = {
          "<- Network",      // 0
          stateLine,         // 1 (toggle = stop client)
          modeLine,          // 2 (info)
          connLine,          // 3 (info)
          "Disconnect G2",   // 4
          ringLine,          // 5 (info)
          "Connect Ring",    // 6
          autoLine,          // 7 (toggle bluetoothAutoStart)
        };
        g2ShowListPage(items, sizeof(items) / sizeof(items[0]));
      }
    } else {
      // Server mode (legacy layout).
      const char* items[] = {
        "<- Network",      // 0
        stateLine,         // 1 (toggle = stop server)
        modeLine,          // 2 (info)
        connLine,          // 3 (info)
        "Toggle Adv",      // 4
        "Disconnect",      // 5
        autoLine,          // 6 (toggle bluetoothAutoStart)
      };
      g2ShowListPage(items, sizeof(items) / sizeof(items[0]));
    }
  } else {
    // Subsystem fully inactive — surface mode so the user knows whether
    // the toggle starts the server or the G2 client.
    snprintf(stateLine, sizeof(stateLine), "BLE: OFF");
    snprintf(modeLine, sizeof(modeLine), "Mode: %s",
             isClient ? "client (G2)" : "server");
    const char* items[] = {
      "<- Network",      // 0
      stateLine,         // 1 (toggle — starts whichever mode is selected)
      modeLine,          // 2 (info)
      autoLine,          // 3 (toggle bluetoothAutoStart)
    };
    g2ShowListPage(items, sizeof(items) / sizeof(items[0]));
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
// firmware that breathing room, then attempt the front-pane card.
// If CTRL{ENTER} still fails the back-pane menu just stays put for
// the scan window — same fallback as before. See
// docs/G2_PROTOCOL.md "Ways the pipeline can fail visibly" for the
// errorCode=7 finding.

#if ENABLE_WIFI
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

  // 2. Show the "Scanning..." card on the front pane. The card has
  //    its own ~10 s firmware auto-dismiss timer as a safety net if
  //    the explicit g2HideEvenAICard at the end of the flow doesn't
  //    take. Failure is non-fatal — if CTRL{ENTER} returns
  //    errorCode=7, the card just doesn't appear and the back-pane
  //    Network menu stays visible during the scan.
  g2ShowEvenAIReplyNoAsk("Scanning networks...");

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
  vTaskDelete(nullptr);
}

static void spawnNetworkScanWorker() {
  // 4 KB stack: WiFi.scanNetworks itself uses ~1 KB, EvenAI builders
  // need ~400 B for the pb buffer, plus FreeRTOS overhead. Same budget
  // as the AI test worker.
  if (xTaskCreate(networkScanWorker, "g2_net_scan", 4096, nullptr,
                  /*prio*/ 5, nullptr) != pdPASS) {
    DEBUG_G2F("[G2] Network: scan worker xTaskCreate failed — "
              "running scan inline (BLE will stall briefly)");
    runNetworkScanFlow();  // safe — no vTaskDelete in the body
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

static void handleMainTap(uint32_t idx) {
  switch (idx) {
    case 0: backToHijackMain(); break;        // <- Back
    case 1: showWiFiMenu();      break;       // WiFi >>
    case 2: showEspNowMenu();    break;       // ESP-NOW >>
    case 3: showBluetoothMenu(); break;       // Bluetooth >>
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

static void handleWiFiTap(uint32_t idx) {
  switch (idx) {
    case 0:  // <- Back to top-level
      g2ShowNetworkMenu();
      break;

    case 1: {  // WiFi: ON|… / WiFi: OFF — toggle
#if ENABLE_WIFI
      bool connected = WiFi.isConnected();
      if (connected) {
        DEBUG_G2F("[G2] WiFi toggle: disconnect");
        WiFi.disconnect(false);
        BROADCAST_PRINTF("[G2] WiFi: disconnect requested from glasses");
        // Disconnect is synchronous from the user's perspective —
        // clear any leftover pending state and re-render OFF.
        gWifiPendingDeadlineMs = 0;
        showWiFiMenu();
      } else {
        DEBUG_G2F("[G2] WiFi toggle: connect best");
        // Arm the "Pending..." overlay BEFORE we trigger the connect
        // and BEFORE the immediate re-render — showWiFiMenu() reads
        // gWifiPendingDeadlineMs to decide whether to show the
        // overlay text. 10 s window covers DHCP + association on a
        // healthy AP; the watchdog clears the flag the moment the
        // link comes up.
        gWifiPendingDeadlineMs = millis() + 10000;
        showWiFiMenu();   // immediate "WiFi: Pending..." render

        // Yield the radio briefly so the page-swap worker can flush
        // the Pending REBUILD to the BLE TX before WiFi.begin() takes
        // over coexistence. Without this, the user sees a ~2-3 s lag
        // before "Pending..." appears: the BLE envelope IS queued
        // synchronously, but the radio's actual transmit window is
        // starved by WiFi's PHY mode-switch + association cycle that
        // immediately follows. 150 ms is empirically enough for a
        // single REBUILD envelope (~50 ms on the wire) plus a small
        // margin. Cost: BLE notify task blocked for 150 ms; pending
        // notifications queue and dispatch late but are not dropped.
        // (Spawning a worker for the WiFi side was rejected — DRAM
        // is tight and the existing ring-connect path already hits
        // a heap-low cascade at ~16 KB free.)
        vTaskDelay(pdMS_TO_TICKS(150));

        bool ok = connectToBestWiFiNetwork();
        BROADCAST_PRINTF("[G2] WiFi: Connect Best → %s",
                         ok ? "started" : "no saved networks / failed");
        // Spawn the watchdog only if one isn't already running. A
        // second tap before the previous watchdog finished re-arms
        // the deadline — that watchdog observes the new deadline
        // and keeps polling.
        if (!gWifiPendingTaskActive) {
          gWifiPendingTaskActive = true;
          if (xTaskCreate(wifiPendingWatchdogTask, "g2_wifi_pending",
                          4096, nullptr, /*prio*/ 5, nullptr) != pdPASS) {
            DEBUG_G2F("[G2] WiFi pending: xTaskCreate failed — "
                      "menu won't auto-refresh on connect");
            gWifiPendingTaskActive = false;
            gWifiPendingDeadlineMs = 0;
            showWiFiMenu();   // drop the Pending overlay
          }
        }
      }
#endif
      break;
    }

    case 2:  // Status — drill into detailed info
      showWiFiStatusPage();
      break;

    case 3: {  // Scan Networks
#if ENABLE_WIFI
      DEBUG_G2F("[G2] WiFi: scan triggered from glasses");
      spawnNetworkScanWorker();
#endif
      break;
    }

    case 4: {  // List Saved
      showWiFiSavedList();
      break;
    }

    case 5: {  // HTTP toggle (Start / Stop)
#if ENABLE_HTTP_SERVER
      bool wasRunning = (server != nullptr);
      const char* result = wasRunning
          ? cmd_httpstop(String(""))
          : cmd_httpstart(String(""));
      BROADCAST_PRINTF("[G2] WiFi: HTTP %s → %s",
                       wasRunning ? "stop" : "start",
                       result ? result : "(no result)");
      // Re-render so the toggle label flips immediately.
      showWiFiMenu();
#else
      DEBUG_G2F("[G2] WiFi: HTTP not compiled in");
#endif
      break;
    }

    case 6: {  // Auto Start toggle — persists to NVS via setSetting
      const bool prev = gSettings.wifiAutoReconnect;
      setSetting(gSettings.wifiAutoReconnect, !prev);
      BROADCAST_PRINTF("[G2] WiFi: Auto Start %s -> %s (from glasses)",
                       prev ? "ON" : "OFF",
                       gSettings.wifiAutoReconnect ? "ON" : "OFF");
      showWiFiMenu();
      break;
    }

    default:
      DEBUG_G2F("[G2] WiFi: unknown idx=%u", (unsigned)idx);
      break;
  }
}

// Status page is info-only — every tap returns to the WiFi submenu.
static void handleWiFiStatusTap(uint32_t /*idx*/) {
  showWiFiMenu();
}

static void handleWiFiScanTap(uint32_t idx) {
  if (idx == 0) { showWiFiMenu(); return; }
  // Tapping an AP — without keyboard we can't enter a password, just log
  // the SSID for the operator. Open APs could be auto-connected via a
  // future enhancement.
  size_t apIdx = idx - 1;
  if (apIdx < gScanCacheCount) {
    const CachedAp& ap = gScanCache[apIdx];
    DEBUG_G2F("[G2] WiFi: SSID '%s' tapped — secured=%d. G2 has no "
              "keyboard; use the web UI or CLI 'wifiadd' to save creds.",
              ap.ssid.c_str(), ap.secured ? 1 : 0);
    BROADCAST_PRINTF("[G2] WiFi: tapped '%s' — use web UI to add",
                     ap.ssid.c_str());
  }
}

static void handleWiFiSavedTap(uint32_t idx) {
  if (idx == 0) { showWiFiMenu(); return; }
#if ENABLE_WIFI
  size_t savedIdx = idx - 1;
  if ((int)savedIdx < gWifiNetworkCount) {
    String ssid = gWifiNetworks[savedIdx].ssid;
    DEBUG_G2F("[G2] WiFi: Forget '%s'", ssid.c_str());
    extern const char* cmd_wifirm(const String& originalCmd);
    String cmd = String("wifirm ") + ssid;
    const char* result = cmd_wifirm(cmd);
    BROADCAST_PRINTF("[G2] WiFi: Forgot '%s' → %s",
                     ssid.c_str(), result ? result : "(no result)");
    // Re-render the (now shorter) saved list.
    showWiFiSavedList();
  }
#endif
}

#if ENABLE_ESPNOW
// onCommit for the ESPNow name editor — stores the new name via setSetting
// (persists to NVS) and re-renders the ESPNow submenu so the "Name:" row
// reflects the new value. Empty input is rejected to avoid clobbering with
// blank — Cancel covers the "I changed my mind" case.
static void espnowNameCommit(const char* text) {
  if (!text || text[0] == '\0') {
    DEBUG_G2F("[G2] ESP-NOW Set Name: empty input — keeping previous");
  } else {
    String old = gSettings.espnowDeviceName;
    setSetting(gSettings.espnowDeviceName, String(text));
    BROADCAST_PRINTF("[G2] ESP-NOW name: '%s' -> '%s'",
                     old.c_str(), text);
  }
  showEspNowMenu();
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
  if (idx == 0) { g2ShowNetworkMenu(); return; }

  if (idx == 1) {  // ESP-NOW state line — toggle
#if ENABLE_ESPNOW
    extern const char* cmd_espnow_init(const String&);
    extern const char* cmd_espnow_deinit(const String&);
    const char* result = running
        ? cmd_espnow_deinit(String(""))
        : cmd_espnow_init(String(""));
    BROADCAST_PRINTF("[G2] ESP-NOW: %s → %s",
                     running ? "stop" : "start",
                     result ? result : "(no result)");
    showEspNowMenu();
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
    const bool prev = gSettings.espnowenabled;
    setSetting(gSettings.espnowenabled, !prev);
    BROADCAST_PRINTF("[G2] ESP-NOW: Auto Start %s -> %s (from glasses)",
                     prev ? "ON" : "OFF",
                     gSettings.espnowenabled ? "ON" : "OFF");
    showEspNowMenu();
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

// Toggle the persisted Bluetooth auto-start flag. Hoisted out so every
// branch of handleBluetoothTap can call it without copy-pasting the
// setSetting + log + redraw triple.
static void bluetoothToggleAutoStart() {
  const bool prev = gSettings.bluetoothAutoStart;
  setSetting(gSettings.bluetoothAutoStart, !prev);
  BROADCAST_PRINTF("[G2] Bluetooth: Auto Start %s -> %s (from glasses)",
                   prev ? "ON" : "OFF",
                   gSettings.bluetoothAutoStart ? "ON" : "OFF");
  showBluetoothMenu();
}

static void handleBluetoothTap(uint32_t idx) {
  // Layout (kept in sync with showBluetoothMenu — "Auto Start" is the
  // last row in every branch, so its idx varies):
  //   active + server:               0=Back 1=toggle 2=Mode 3=Conn
  //                                  4=ToggleAdv 5=Disconnect 6=AutoStart
  //   active + client (ring up):     0..7 (see below) 8=AutoStart
  //   active + client (ring down):   0..6 7=AutoStart
  //   inactive:                      0=Back 1=toggle 2=Mode 3=AutoStart
  const bool active   = bleSubsystemActive();
  const bool isClient = (gSettings.bleMode == BLE_MODE_G2_CLIENT);
  if (idx == 0) { g2ShowNetworkMenu(); return; }

  if (idx == 1) {  // toggle — mode-aware
    if (active) {
      // Stop whichever subsystem is up. In client mode this also yanks
      // the menu out from under the user (they were viewing it on the
      // lens), but that's the explicit action they asked for.
      if (isClient) {
        deinitG2Client();
        BROADCAST_PRINTF("[G2] Bluetooth: G2 client stopped (from glasses)");
      } else {
        deinitBluetooth();
        BROADCAST_PRINTF("[G2] Bluetooth: server stopped");
      }
    } else {
      bool ok = isClient ? initG2Client() : initBluetooth();
      BROADCAST_PRINTF("[G2] Bluetooth: %s start → %s",
                       isClient ? "client" : "server",
                       ok ? "ok" : "failed");
    }
    showBluetoothMenu();
    return;
  }

  if (!active) {
    // Layout: 0=Back 1=toggle 2=Mode 3=AutoStart
    if (idx == 2) {
      DEBUG_G2F("[G2] Bluetooth: mode info row (use blemode CLI to change)");
    } else if (idx == 3) {
      bluetoothToggleAutoStart();
    } else {
      DEBUG_G2F("[G2] Bluetooth: idx=%u while OFF (only toggle valid)",
                (unsigned)idx);
    }
    return;
  }

  if (isClient) {
    // Client-mode layout (kept in sync with showBluetoothMenu):
    //   ringUp:  0=Back 1=toggle 2=Mode 3=Conn 4=DisconnectG2
    //            5=Ring(info) 6=ReconnectRing 7=DisconnectRing 8=AutoStart
    //   ringDown: 0=Back 1=toggle 2=Mode 3=Conn 4=DisconnectG2
    //             5=Ring(info) 6=ConnectRing 7=AutoStart
    const bool ringUp = g2RingIsConnected();
    const uint32_t kAutoIdx = ringUp ? 8 : 7;
    if (idx == kAutoIdx) {
      bluetoothToggleAutoStart();
      return;
    }
    if (idx == 2 || idx == 3 || idx == 5) {
      DEBUG_G2F("[G2] Bluetooth: client info row %u (no action)", (unsigned)idx);
      return;
    }
    if (idx == 4) {
      deinitG2Client();
      BROADCAST_PRINTF("[G2] Bluetooth: G2 disconnect requested from glasses");
      showBluetoothMenu();
      return;
    }
    if (idx == 6) {
      // "Connect Ring" (ringDown) and "Reconnect Ring" (ringUp) both
      // route to triggerRingReconnect — the helper handles the
      // disconnect-first half when needed and skips it otherwise.
      // Skip the menu redraw on purpose: re-rendering here means a
      // SHUTDOWN+CREATE-list page swap (4 KB worker stack, 8 KB pb
      // buffer) RIGHT after we already spent some heap on the ring
      // connect task — the back-to-back allocation pattern that
      // motivated the heap-low guard inside triggerRingReconnect.
      // The lens still shows the actions menu (we never tore it
      // down), and ring outcome surfaces in serial + the web UI's
      // ring-status panel.
      BROADCAST_PRINTF("[G2] Bluetooth: %s Ring requested from glasses",
                       g2RingIsConnected() ? "Reconnect" : "Connect");
      triggerRingReconnect();
      return;
    }
    if (idx == 7) {
      BROADCAST_PRINTF("[G2] Bluetooth: Disconnect Ring requested from glasses");
      triggerRingDisconnect();
      showBluetoothMenu();   // disconnect is synchronous; safe to redraw
      return;
    }
    DEBUG_G2F("[G2] Bluetooth (client): unknown idx=%u", (unsigned)idx);
    return;
  }

  // Server-mode layout (legacy).
  switch (idx) {
    case 2: case 3:  // info rows
      DEBUG_G2F("[G2] Bluetooth: info row %u (no action)", (unsigned)idx);
      break;

    case 4: {  // Toggle Adv — best-effort: start; if already advertising
               // the call returns false and we treat it as "stop".
      bool started = startBLEAdvertising();
      if (!started) {
        stopBLEAdvertising();
        BROADCAST_PRINTF("[G2] Bluetooth: advertising stopped");
      } else {
        BROADCAST_PRINTF("[G2] Bluetooth: advertising started");
      }
      showBluetoothMenu();
      break;
    }

    case 5: {  // Disconnect
      if (isBLEConnected()) {
        disconnectBLE();
        BROADCAST_PRINTF("[G2] Bluetooth: disconnect requested from glasses");
      } else {
        DEBUG_G2F("[G2] Bluetooth: not connected, no-op");
      }
      showBluetoothMenu();
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

void g2NetworkHandleTap(uint32_t idx) {
  switch (gNetSub) {
    case NET_SUB_MAIN:        handleMainTap(idx);        break;
    case NET_SUB_WIFI:        handleWiFiTap(idx);        break;
    case NET_SUB_WIFI_SCAN:   handleWiFiScanTap(idx);    break;
    case NET_SUB_WIFI_SAVED:  handleWiFiSavedTap(idx);   break;
    case NET_SUB_WIFI_STATUS: handleWiFiStatusTap(idx);  break;
    case NET_SUB_ESPNOW:      handleEspNowTap(idx);      break;
    case NET_SUB_ESPNOW_DEVS: handleEspNowDevsTap(idx);  break;
    case NET_SUB_BLUETOOTH:   handleBluetoothTap(idx);   break;
  }
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
