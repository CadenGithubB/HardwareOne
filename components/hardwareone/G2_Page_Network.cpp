// =============================================================================
// G2 glasses — "Network" page implementation
// =============================================================================
// See header for the contract. Top-level Network chooser drills into one
// of three subsystem submenus (WiFi / ESP-NOW / Bluetooth) plus several
// list sub-pages. Tap-only — anything in the OLED that needs text input
// (e.g. wifiadd <ssid> <password>, espnowsetname) is omitted.

#include "G2_Page_Network.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "Optional_EvenG2.h"
#include "Optional_Bluetooth.h"
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
  NET_SUB_ESPNOW        = 4,  // ESP-NOW action menu
  NET_SUB_ESPNOW_DEVS   = 5,  // ESP-NOW paired-device list (info only)
  NET_SUB_BLUETOOTH     = 6,  // Bluetooth action menu
};
static NetworkSubMode gNetSub = NET_SUB_MAIN;

// Cache of the most recent scan results so tap-to-connect knows which
// SSID was at index N. Populated when the user taps "Scan Networks".
// Capped at 8 — anything beyond that and the user can re-scan after
// physically moving closer to fewer APs.
struct CachedAp { String ssid; int rssi; bool secured; };
static CachedAp gScanCache[8];
static size_t   gScanCacheCount = 0;

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
    "<- Back",        // 0
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
  static char wifiLine[48];
  static char rssiLine[32];
  static char httpLine[24];

#if ENABLE_WIFI
  bool connected = WiFi.isConnected();
  if (connected) {
    String ssid = WiFi.SSID();
    snprintf(wifiLine, sizeof(wifiLine), "WiFi: %s",
             ssid.length() > 14 ? (ssid.substring(0, 14) + "~").c_str()
                                : ssid.c_str());
    snprintf(rssiLine, sizeof(rssiLine), "RSSI %lddBm  ch%ld",
             (long)WiFi.RSSI(), (long)WiFi.channel());
  } else {
    snprintf(wifiLine, sizeof(wifiLine), "WiFi: offline");
    snprintf(rssiLine, sizeof(rssiLine), "(not connected)");
  }
#else
  bool connected = false;
  snprintf(wifiLine, sizeof(wifiLine), "WiFi: not compiled");
  snprintf(rssiLine, sizeof(rssiLine), "-");
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

  // Item indices match dispatch in handleWiFiTap.
  const char* items[] = {
    "<- Back",         // 0
    wifiLine,          // 1 (info)
    rssiLine,          // 2 (info)
    "Connect Best",    // 3
    "Disconnect",      // 4
    "Scan Networks",   // 5
    "List Saved",      // 6
    httpLine,          // 7
  };
  g2ShowListPage(items, sizeof(items) / sizeof(items[0]));
  DEBUG_G2F("[G2] WiFi submenu shown (connected=%d)", connected ? 1 : 0);
}

// Render scan results as a tappable list. Items:
//   0: "<- Back" (returns to WiFi submenu)
//   1..N: SSID lines with RSSI suffix
static void showScanResults() {
  gNetSub = NET_SUB_WIFI_SCAN;
  static char rows[1 + 8][48];
  strcpy(rows[0], "<- Back");
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
    static const char* empty[] = { "<- Back", "(no networks found)" };
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
  strcpy(rows[0], "<- Back");
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
    static const char* empty[] = { "<- Back", "(no saved networks)" };
    g2ShowListPage(empty, 2);
  } else {
    g2ShowListPage(ptrs, n);
  }
  DEBUG_G2F("[G2] WiFi saved list (%d entries) — tap to forget",
            count);
#else
  static const char* na[] = { "<- Back", "(WiFi not compiled)" };
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
  static char toggleLine[24];

#if ENABLE_ESPNOW
  bool running = (gEspNow && gEspNow->initialized);
  if (running) {
    snprintf(stateLine, sizeof(stateLine), "ESP-NOW: ON");
    snprintf(modeLine, sizeof(modeLine), "Mode: %s", getEspNowModeString());
    snprintf(devsLine, sizeof(devsLine), "Peers: %d",
             gEspNow->deviceCount);
    snprintf(toggleLine, sizeof(toggleLine), "Stop");
  } else {
    snprintf(stateLine, sizeof(stateLine), "ESP-NOW: OFF");
    snprintf(modeLine, sizeof(modeLine), "(not initialised)");
    snprintf(devsLine, sizeof(devsLine), "Peers: -");
    snprintf(toggleLine, sizeof(toggleLine), "Start");
  }
#else
  snprintf(stateLine, sizeof(stateLine), "ESP-NOW: not compiled");
  snprintf(modeLine, sizeof(modeLine), "-");
  snprintf(devsLine, sizeof(devsLine), "-");
  snprintf(toggleLine, sizeof(toggleLine), "n/a");
#endif

  const char* items[] = {
    "<- Back",         // 0
    stateLine,         // 1 (info)
    modeLine,          // 2 (info)
    devsLine,          // 3 (info)
    toggleLine,        // 4 (Start/Stop toggle)
    "View Devices",    // 5
  };
  g2ShowListPage(items, sizeof(items) / sizeof(items[0]));
  DEBUG_G2F("[G2] ESP-NOW submenu shown");
}

static void showEspNowDevices() {
  gNetSub = NET_SUB_ESPNOW_DEVS;
#if ENABLE_ESPNOW
  static char rows[1 + 8][48];
  strcpy(rows[0], "<- Back");
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
    static const char* empty[] = { "<- Back", "(no peers — pair via web)" };
    g2ShowListPage(empty, 2);
  } else {
    g2ShowListPage(ptrs, n);
  }
  DEBUG_G2F("[G2] ESP-NOW device list (%u entries)", (unsigned)(n - 1));
#else
  static const char* na[] = { "<- Back", "(ESP-NOW not compiled)" };
  g2ShowListPage(na, 2);
#endif
}

// -----------------------------------------------------------------------------
// Bluetooth submenu
// -----------------------------------------------------------------------------

static void showBluetoothMenu() {
  gNetSub = NET_SUB_BLUETOOTH;
  static char stateLine[40];
  static char modeLine[40];
  static char connLine[40];
  static char toggleLine[24];

  bool running = isBLERunning();
  bool connected = isBLEConnected();
  if (running) {
    snprintf(stateLine, sizeof(stateLine), "BLE: ON (%s)", getBLEStateString());
    snprintf(toggleLine, sizeof(toggleLine), "Stop");
  } else {
    snprintf(stateLine, sizeof(stateLine), "BLE: OFF");
    snprintf(toggleLine, sizeof(toggleLine), "Start");
  }
  snprintf(modeLine, sizeof(modeLine), "Mode: %s", getBleModeString());
  snprintf(connLine, sizeof(connLine), "Conn: %s",
           connected ? "yes" : "no");

  const char* items[] = {
    "<- Back",         // 0
    stateLine,         // 1 (info)
    modeLine,          // 2 (info)
    connLine,          // 3 (info)
    toggleLine,        // 4 (Start/Stop)
    "Toggle Adv",      // 5
    "Disconnect",      // 6
  };
  g2ShowListPage(items, sizeof(items) / sizeof(items[0]));
  DEBUG_G2F("[G2] Bluetooth submenu shown (running=%d, conn=%d)",
            running ? 1 : 0, connected ? 1 : 0);
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

static void handleWiFiTap(uint32_t idx) {
  switch (idx) {
    case 0:  // <- Back to top-level
      g2ShowNetworkMenu();
      break;

    case 1: case 2:  // info rows — no-op
      DEBUG_G2F("[G2] WiFi: info row %u (no action)", (unsigned)idx);
      break;

    case 3: {  // Connect Best
#if ENABLE_WIFI
      DEBUG_G2F("[G2] WiFi: Connect Best");
      bool ok = connectToBestWiFiNetwork();
      BROADCAST_PRINTF("[G2] WiFi: Connect Best → %s",
                       ok ? "started" : "no saved networks / failed");
#endif
      break;
    }

    case 4: {  // Disconnect
#if ENABLE_WIFI
      DEBUG_G2F("[G2] WiFi: Disconnect");
      WiFi.disconnect(false);
      BROADCAST_PRINTF("[G2] WiFi: disconnect requested from glasses");
      // Refresh the menu so the SSID/RSSI lines update on next render.
      showWiFiMenu();
#endif
      break;
    }

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

    case 7: {  // HTTP toggle (Start / Stop)
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

    default:
      DEBUG_G2F("[G2] WiFi: unknown idx=%u", (unsigned)idx);
      break;
  }
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

static void handleEspNowTap(uint32_t idx) {
  switch (idx) {
    case 0:  // <- Back
      g2ShowNetworkMenu();
      break;

    case 1: case 2: case 3:  // info rows
      DEBUG_G2F("[G2] ESP-NOW: info row %u (no action)", (unsigned)idx);
      break;

    case 4: {  // Start / Stop toggle
#if ENABLE_ESPNOW
      bool running = (gEspNow && gEspNow->initialized);
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
      break;
    }

    case 5: {  // View Devices
      showEspNowDevices();
      break;
    }

    default:
      DEBUG_G2F("[G2] ESP-NOW: unknown idx=%u", (unsigned)idx);
      break;
  }
}

static void handleEspNowDevsTap(uint32_t idx) {
  if (idx == 0) { showEspNowMenu(); return; }
  // Info-only — pairing/unpairing requires MAC + name text input.
  DEBUG_G2F("[G2] ESP-NOW: device row %u tapped (info-only)", (unsigned)idx);
}

static void handleBluetoothTap(uint32_t idx) {
  switch (idx) {
    case 0:  // <- Back
      g2ShowNetworkMenu();
      break;

    case 1: case 2: case 3:  // info rows
      DEBUG_G2F("[G2] Bluetooth: info row %u (no action)", (unsigned)idx);
      break;

    case 4: {  // Start / Stop toggle
      bool running = isBLERunning();
      bool ok = running ? (deinitBluetooth(), true) : initBluetooth();
      BROADCAST_PRINTF("[G2] Bluetooth: %s → %s",
                       running ? "stop" : "start",
                       ok ? "ok" : "failed");
      showBluetoothMenu();
      break;
    }

    case 5: {  // Toggle Adv
      // No "is advertising right now" bit exposed cleanly — best-effort:
      // start if not connected, stop if currently advertising. The CLI
      // `bleadv` just calls startBLEAdvertising unconditionally; we
      // match that and let the user re-toggle to stop.
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

    case 6: {  // Disconnect
      if (isBLEConnected()) {
        disconnectBLE();
        BROADCAST_PRINTF("[G2] Bluetooth: disconnect requested from glasses");
      } else {
        DEBUG_G2F("[G2] Bluetooth: not connected, no-op");
      }
      showBluetoothMenu();
      break;
    }

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
    case NET_SUB_ESPNOW:      handleEspNowTap(idx);      break;
    case NET_SUB_ESPNOW_DEVS: handleEspNowDevsTap(idx);  break;
    case NET_SUB_BLUETOOTH:   handleBluetoothTap(idx);   break;
  }
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
