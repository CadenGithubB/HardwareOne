// =============================================================================
// G2 glasses — "Network" page implementation
// =============================================================================
// See header for the contract. The page has two sub-modes:
//   * MENU         — action list (Back / Connect Best / Disconnect / Scan / Forget)
//   * SCAN_RESULTS — list of SSIDs found in the most recent scan; tap to
//                    initiate connect (will fail without password since
//                    we have no keyboard input — but it's a clean no-op
//                    that logs the SSID for visibility)
//
// State is intentionally minimal — we don't try to manage the WiFi
// state machine ourselves, just call into the existing WiFi APIs the
// CLI commands use.

#include "G2_Page_Network.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "Optional_EvenG2.h"
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

// -----------------------------------------------------------------------------
// Local state
// -----------------------------------------------------------------------------

enum NetworkSubMode : uint8_t {
  NET_SUB_MENU         = 0,
  NET_SUB_SCAN_RESULTS = 1,
};
static NetworkSubMode gNetSub = NET_SUB_MENU;

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
// Action menu (list-mode, tappable)
// -----------------------------------------------------------------------------

void g2ShowNetworkMenu() {
  gNetSub = NET_SUB_MENU;
  // First two items are status-info rows (no-op taps); the rest are
  // actions. Keeping it short — 6 items is comfortable in the firmware
  // list widget without forcing the user to scroll a lot.
  static char wifiLine[40];
  static char espLine[40];
#if ENABLE_WIFI
  if (WiFi.isConnected()) {
    String ssid = WiFi.SSID();
    snprintf(wifiLine, sizeof(wifiLine), "WiFi: %s",
             ssid.length() > 12 ? (ssid.substring(0, 12) + "~").c_str()
                                : ssid.c_str());
  } else {
    snprintf(wifiLine, sizeof(wifiLine), "WiFi: offline");
  }
#else
  snprintf(wifiLine, sizeof(wifiLine), "WiFi: n/a");
#endif

#if ENABLE_ESPNOW
  if (gEspNow && gEspNow->initialized) {
    snprintf(espLine, sizeof(espLine), "ESPNow %s %dp",
             getEspNowModeString(), gEspNow->peerHistoryCount);
  } else {
    snprintf(espLine, sizeof(espLine), "ESPNow off");
  }
#else
  snprintf(espLine, sizeof(espLine), "ESPNow: n/a");
#endif

  // Indices match the dispatch in g2NetworkHandleTap.
  const char* items[] = {
    "<- Back",        // 0
    wifiLine,         // 1 (info, no-op)
    espLine,          // 2 (info, no-op)
    "Connect Best",   // 3
    "Disconnect",     // 4
    "Scan Networks",  // 5
    "Forget Current", // 6
  };
  if (g2ShowListPage(items, sizeof(items) / sizeof(items[0]))) {
    g2SetHijackPage(G2_HIJACK_PAGE_NETWORK);
    DEBUG_G2F("[G2] Network menu shown (sub=MENU)");
  } else {
    DEBUG_G2F("[G2] Network menu show FAILED");
  }
}

// Render scan results as a tappable list. Items are:
//   0: "<- Back" (returns to MENU sub-mode, not to top-level)
//   1..N: SSID lines with RSSI suffix
static void showScanResults() {
  gNetSub = NET_SUB_SCAN_RESULTS;
  // Build storage for the items we'll point to.
  static char rows[1 + 8][48];  // [0] back, [1..8] APs
  strcpy(rows[0], "<- Back");
  const char* ptrs[1 + 8];
  ptrs[0] = rows[0];
  size_t n = 1;
  for (size_t i = 0; i < gScanCacheCount && i < 8; i++) {
    snprintf(rows[1 + i], sizeof(rows[1 + i]),
             "%s %s %ddBm",
             gScanCache[i].secured ? "L" : " ",   // L = locked / secured
             gScanCache[i].ssid.length() > 0
                 ? gScanCache[i].ssid.c_str() : "<hidden>",
             gScanCache[i].rssi);
    ptrs[n++] = rows[1 + i];
  }
  if (n == 1) {
    // No APs found — render a single info line.
    static const char* empty[] = { "<- Back", "(no networks found)" };
    g2ShowListPage(empty, 2);
  } else {
    g2ShowListPage(ptrs, n);
  }
  DEBUG_G2F("[G2] Network scan results (%u APs)",
            (unsigned)gScanCacheCount);
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
// Tap dispatch
// -----------------------------------------------------------------------------

void g2NetworkHandleTap(uint32_t idx) {
  if (gNetSub == NET_SUB_SCAN_RESULTS) {
    if (idx == 0) {
      // Back to action menu.
      g2ShowNetworkMenu();
      return;
    }
    // Tapped an SSID. Without keyboard we can't enter a password, so
    // the most we can do is log it and bail. If it's an open AP, a
    // future enhancement could try connect-without-password.
    size_t apIdx = idx - 1;
    if (apIdx < gScanCacheCount) {
      const CachedAp& ap = gScanCache[apIdx];
      DEBUG_G2F("[G2] Network: SSID '%s' tapped — secured=%d. No keyboard "
                "input on G2; cannot enter password. Use the web UI or "
                "CLI 'wifi add' to save credentials.",
                ap.ssid.c_str(), ap.secured ? 1 : 0);
      BROADCAST_PRINTF("[G2] Tapped '%s' on glasses — use web UI to add",
                       ap.ssid.c_str());
    }
    return;
  }

  // MAIN sub-mode: dispatch by item index (matches g2ShowNetworkMenu).
  switch (idx) {
    case 0:  // <- Back to top-level menu
      g2SetHijackPage(G2_HIJACK_PAGE_MAIN);
      // The main hijack menu items are owned by hijackWorkerTask — we
      // just need to REBUILD with them. Re-sourcing the array here would
      // duplicate state, so instead we ask the hijack code to redraw.
      extern void g2RedrawHijackMainMenu();
      g2RedrawHijackMainMenu();
      break;

    case 1: case 2:
      // Info rows — no-op.
      DEBUG_G2F("[G2] Network: tapped info row %u (no action)", (unsigned)idx);
      break;

    case 3: {  // Connect Best
#if ENABLE_WIFI
      DEBUG_G2F("[G2] Network: Connect Best — calling connectToBestWiFiNetwork");
      // Defer to the existing helper used by the CLI `wificonnect` path.
      // Walks the saved-credentials list and starts a connect attempt to
      // whichever AP is best on RSSI; we don't block on the result.
      extern bool connectToBestWiFiNetwork();
      bool ok = connectToBestWiFiNetwork();
      BROADCAST_PRINTF("[G2] Network: Connect Best → %s",
                       ok ? "started" : "no saved networks / failed");
#else
      DEBUG_G2F("[G2] Network: WiFi not compiled in");
#endif
      break;
    }

    case 4: {  // Disconnect
#if ENABLE_WIFI
      DEBUG_G2F("[G2] Network: Disconnect");
      // disconnect(false) drops the AP association but keeps the station
      // driver running. disconnect(true) sets WIFI_OFF, which leaves
      // WiFi.status() reporting WL_STOPPED (254) — a later Connect Best
      // then spins its 12 s retry budget three times before the driver's
      // deinit/reinit hits a malloc-buffer-fail under BLE heap pressure
      // (observed 2026-04-26).
      WiFi.disconnect(false);
      BROADCAST_PRINTF("[G2] Network: WiFi disconnect requested from glasses");
#endif
      break;
    }

    case 5: {  // Scan Networks
#if ENABLE_WIFI
      DEBUG_G2F("[G2] Network: scan triggered from glasses");
      // Spawn a worker so the BLE notify task that ran this tap can
      // return immediately. The worker shows a "Scanning..." card on
      // the front pane, runs the 2-3 s blocking scan, dismisses the
      // card, and then swaps the back pane to the results list.
      spawnNetworkScanWorker();
#endif
      break;
    }

    case 6: {  // Forget Current
#if ENABLE_WIFI
      if (WiFi.isConnected()) {
        String ssid = WiFi.SSID();
        DEBUG_G2F("[G2] Network: Forget Current — removing '%s'",
                  ssid.c_str());
        // Defer to the CLI handler that removes a saved network. It
        // expects the original `wifirm <ssid>` form so we re-synthesize
        // it here. Result string is logged for visibility.
        extern const char* cmd_wifirm(const String& originalCmd);
        String cmd = String("wifirm ") + ssid;
        const char* result = cmd_wifirm(cmd);
        WiFi.disconnect(true);
        BROADCAST_PRINTF("[G2] Network: Forgot '%s' → %s",
                         ssid.c_str(), result ? result : "(no result)");
      } else {
        DEBUG_G2F("[G2] Network: Forget Current — not connected, no-op");
      }
#endif
      break;
    }

    default:
      DEBUG_G2F("[G2] Network: unknown idx=%u", (unsigned)idx);
      break;
  }
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
