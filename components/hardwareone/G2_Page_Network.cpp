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
#include "BLE_CentralCoordinator.h"  // bleCentralRingShouldYield (R1 menu status)
#include "BLE_LinkStatus.h"   // role / relay pipe / app console words (Bluetooth + Phone menus)
#include "System_Settings.h" // setSetting — peer autoReconnect + bleAutoStart
#include "G2_Page_TextEntry.h"   // on-glasses keyboard for "Set Name"
#include "G2_HijackCmd.h"        // g2BumpMenuGen() for setNetSub navigation tracking
#include <utility>               // move an admitted saved-network snapshot
#include "System_Debug.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if ENABLE_WIFI
#include <WiFi.h>
#include "System_WiFi.h"
#include "network/wifi/System_WiFiNetworks.h"
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
  NET_SUB_WIFI_SAVED    = 3,  // WiFi saved-network list (tap opens actions)
  NET_SUB_WIFI_STATUS   = 4,  // WiFi detailed status dump (info only)
  NET_SUB_ESPNOW        = 5,  // ESP-NOW action menu
  NET_SUB_ESPNOW_DEVS   = 6,  // ESP-NOW paired-device list (info only)
  NET_SUB_BLUETOOTH     = 7,  // Bluetooth action menu
  NET_SUB_BLUETOOTH_G2  = 8,  // G2 client controls (under Bluetooth)
  NET_SUB_HTTP          = 9,  // HTTP(S) server controls (start/stop, HTTPS toggle, auto-start)
  NET_SUB_BLUETOOTH_R1  = 10, // R1 ring controls (under Bluetooth) — mirror of G2 submenu
  NET_SUB_WIFI_SAVED_ACT= 11, // per-saved-network action menu (Connect / Forget)
  NET_SUB_BLUETOOTH_PHONE = 12, // phone relay pipe controls (under Bluetooth)
};
static NetworkSubMode gNetSub = NET_SUB_MAIN;
// Index into the admitted list snapshot, preserved through the action submenu.
// Commands use the selected entry's stable identity, never its live array index.
static int gWifiSavedSel = -1;
#if ENABLE_WIFI
EXT_RAM_BSS_ATTR static WifiNetworkSnapshot gWifiSavedSnapshot;  // PSRAM: presentation copy, no credentials
#endif

// Rows of the Network action menu the lens shows now (WiFi, ESP-NOW, HTTP(S),
// Bluetooth, G2, R1). Their layouts and toggle labels follow live state (radio,
// link, ESP-NOW, HTTP server, BLE role, ring, settings), which web, CLI, an
// automation or a link event can change between the render and the tap. So the
// renderer binds every row to its action, and for a toggle to the state it
// switches to as labelled, and the tap handler dispatches only from that
// binding instead of rebuilding the layout from live state.
struct NetRowBinding {
  uint8_t action;  // the submenu's own row-action enum
  bool    on;      // toggles: the state the tap switches to (true = on/open/start)
};
static constexpr size_t kNetRowMax = 10;
static NetRowBinding gNetRows[kNetRowMax] = {};
static volatile uint8_t gNetRowCount = 0;
static volatile uint8_t gNetRowSub = NET_SUB_MAIN;  // submenu that bound gNetRows

// Single mutator so every gNetSub transition bumps the menu generation —
// in-flight hijack-command callbacks compare cookie.menuGen against the
// live value and drop redraws when the user has navigated away. See
// G2_HijackCmd.h staleness contract. Initializer above intentionally
// bypasses this helper (no transition, no in-flight cmds at boot).
// A transition also drops the bound rows: they belong to the previous view, and
// the new one (or a view without bound rows) may still be refused by the lens.
static inline void setNetSub(NetworkSubMode m) {
  if (gNetSub == m) return;
  gNetSub = m;
  __atomic_store_n(&gNetRowCount, 0, __ATOMIC_RELEASE);
  g2BumpMenuGen();
}

// Labels plus bindings of one menu, collected while it is rendered.
struct NetRowList {
  const char*   items[kNetRowMax];
  NetRowBinding rows[kNetRowMax];
  size_t        n = 0;
  void add(const char* label, uint8_t action, bool on = false) {
    if (n >= kNetRowMax) return;
    items[n] = label;
    rows[n] = NetRowBinding{action, on};
    ++n;
  }
};

// Draws `list` and binds its rows to `sub` once the lens accepted the page.
// g2ShowListPage has claimed page-swap admission before returning true, so tap
// dispatch is fenced while the bindings are published. A refused render binds
// nothing: the rows still on the lens keep theirs.
static bool showNetRows(NetworkSubMode sub, const NetRowList& list) {
  if (!g2ShowListPage(list.items, list.n)) return false;
  for (size_t i = 0; i < list.n; ++i) {
    __atomic_store_n(&gNetRows[i].action, list.rows[i].action, __ATOMIC_RELAXED);
    __atomic_store_n(&gNetRows[i].on, list.rows[i].on, __ATOMIC_RELAXED);
  }
  __atomic_store_n(&gNetRowSub, (uint8_t)sub, __ATOMIC_RELAXED);
  __atomic_store_n(&gNetRowCount, (uint8_t)list.n, __ATOMIC_RELEASE);
  return true;
}

// The binding of row `idx` as `sub` drew it. False when the bound rows belong to
// another view (or none are bound) or the row does not exist: the tap is stale.
static bool netRowAt(NetworkSubMode sub, uint32_t idx, NetRowBinding* out) {
  const uint8_t count = __atomic_load_n(&gNetRowCount, __ATOMIC_ACQUIRE);
  const uint8_t owner = __atomic_load_n(&gNetRowSub, __ATOMIC_RELAXED);
  if (owner != (uint8_t)sub || idx >= count) {
    DEBUG_G2F("[G2] Network sub=%u: stale/unknown row idx=%u (bound sub=%u rows=%u)",
              (unsigned)sub, (unsigned)idx, (unsigned)owner, (unsigned)count);
    return false;
  }
  out->action = __atomic_load_n(&gNetRows[idx].action, __ATOMIC_RELAXED);
  out->on     = __atomic_load_n(&gNetRows[idx].on, __ATOMIC_RELAXED);
  return true;
}

// Row actions per submenu (WiFi's WifiMenuAction sits with showWiFiMenu).
// `on` in the binding gives a toggle's direction.
enum EspNowRowAction : uint8_t {
  ESPNOW_ROW_BACK = 0,    // -> WiFi submenu
  ESPNOW_ROW_INFO,        // Mode / Peers / not compiled: no action
  ESPNOW_ROW_POWER,       // on: openespnow (name keyboard first if unset); off: closeespnow
  ESPNOW_ROW_DEVICES,     // paired-device list
  ESPNOW_ROW_NAME,        // on-glasses keyboard for the device name
  ESPNOW_ROW_AUTO_START,  // espnowenabled 1|0
};
enum HttpRowAction : uint8_t {
  HTTP_ROW_BACK = 0,      // -> WiFi submenu
  HTTP_ROW_INFO,          // URL / not compiled: no action
  HTTP_ROW_SERVER,        // on: openhttp; off: closehttp
  HTTP_ROW_HTTPS,         // httpsEnabled 1|0 (reboot to apply)
  HTTP_ROW_AUTO_START,    // httpAutoStart 1|0
};
enum BluetoothRowAction : uint8_t {
  BT_ROW_BACK = 0,        // -> Network chooser
  BT_ROW_INFO,            // Conn / Saved: no action
  BT_ROW_CLIENT_POWER,    // on: openg2; off: closeg2 (glasses only; ring, phone, stack stay)
  BT_ROW_SERVER_POWER,    // on: openconsole; off: closeconsole
  BT_ROW_MODE,            // Role row: on: blemode client; off: blemode server
  BT_ROW_G2,              // G2 >> submenu
  BT_ROW_R1,              // R1 >> submenu
  BT_ROW_PHONE,           // Phone >> submenu (ENABLE_BLE_PHONE_PIPE)
  BT_ROW_TOGGLE_ADV,      // bleadv toggle
  BT_ROW_DISCONNECT,      // bledisconnect
  BT_ROW_AUTO_START,      // bleautostart on|off
};
enum G2RowAction : uint8_t {
  G2_ROW_BACK = 0,        // -> Bluetooth submenu
  G2_ROW_INFO,            // Conn / BLE stopped: no action
  G2_ROW_AUTO_RECONNECT,  // bleautoreconnect g2-glasses on|off
  G2_ROW_RECONNECT,       // openg2 saved
  G2_ROW_DISCONNECT,      // closeg2
};
enum R1RowAction : uint8_t {
  R1_ROW_BACK = 0,        // -> Bluetooth submenu
  R1_ROW_INFO,            // Ring / BLE stopped: no action
  R1_ROW_AUTO_RECONNECT,  // bleautoreconnect r1-ring on|off
  R1_ROW_CONNECT,         // openring, with the "connecting..." overlay
  R1_ROW_RECONNECT,       // openring reconnect
  R1_ROW_DISCONNECT,      // closering
};
enum PhoneRowAction : uint8_t {
  PHONE_ROW_BACK = 0,     // -> Bluetooth submenu
  PHONE_ROW_INFO,         // Pipe / Paired / Console: no action
  PHONE_ROW_AUTO_RECONNECT, // bleautoreconnect phone on|off
  PHONE_ROW_PIPE,         // on: openphone (open the pipe, connect the paired phone); off: closephone
};

// Cache of the most recent scan results so tap-to-connect knows which
// SSID was at index N. Populated when the user taps "Scan Networks".
// Capped at 8 — anything beyond that and the user can re-scan after
// physically moving closer to fewer APs.
struct CachedAp {
  char ssid[33];
  int8_t rssi;
  bool secured;
};
// The result list survives only until the user taps it. Keep its fixed payload
// in PSRAM so dense/long SSIDs never allocate or retain Arduino String buffers
// in internal heap after a scan.
EXT_RAM_BSS_ATTR static CachedAp gScanCache[8];
static size_t   gScanCacheCount = 0;
// Empty means the most recent scan completed successfully. A short failure
// message is kept separately so BUSY/driver failures are not presented as an
// empty RF environment.
static char     gScanFailure[32] = {};

// "Pending..." overlay state for the WiFi toggle row.
//
// Rationale: WiFi.disconnect() returns immediately, while openwifi runs the
// potentially long connection transaction on cmd_exec_task. Without this
// overlay, the user sees the prior state until that command completes, which
// feels like the tap was ignored.
//
// While `millis() < gWifiPendingDeadlineMs`, showWiFiMenu() renders
// the toggle row as "WiFi: Pending..." regardless of the live
// WiFi.isConnected() value, so the user gets immediate visual feedback.
// A short watchdog task polls WiFi.isConnected() every 500 ms and queues a
// redraw of the menu the moment the state flips (or when the deadline
// expires, whichever comes first). Single-shot — re-tapping the toggle
// while pending arms a fresh deadline + watchdog.
static volatile uint32_t gWifiPendingDeadlineMs = 0;
static volatile bool     gWifiPendingTaskActive = false;

static uint32_t wifiPendingOverlayDeadline() {
  return __atomic_load_n(&gWifiPendingDeadlineMs, __ATOMIC_ACQUIRE);
}

static void cancelWifiPendingOverlay() {
  __atomic_store_n(&gWifiPendingDeadlineMs, 0, __ATOMIC_RELEASE);
}

static uint32_t armWifiPendingOverlay() {
  uint32_t deadline = millis() + 10000u;
  if (deadline == 0) deadline = 1;  // zero is the inactive sentinel
  __atomic_store_n(&gWifiPendingDeadlineMs, deadline, __ATOMIC_RELEASE);
  return deadline;
}

// A slow openwifi can outlive the 10-second overlay and a later tap can arm a
// new attempt. Only the command that owns the current deadline may clear it;
// otherwise an older completion would erase the newer attempt's feedback.
static bool clearWifiPendingOverlayIfOwned(uint32_t deadline) {
  if (deadline == 0) return false;
  uint32_t expected = deadline;
  return __atomic_compare_exchange_n(&gWifiPendingDeadlineMs, &expected, 0,
                                     false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE);
}

static bool claimWifiPendingWatchdog() {
  bool expected = false;
  return __atomic_compare_exchange_n(&gWifiPendingTaskActive, &expected, true,
                                     false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE);
}

// Same "Pending..." treatment for the R1 ring connect (see showBluetoothR1Menu /
// handleBluetoothR1Tap). `openring` returns OK the instant the connect is
// KICKED, but the scan+connect+auth finishes ~5-8 s later with no completion
// event — so the immediate menu re-render showed "disconnected" and never
// updated. While pending, showBluetoothR1Menu() renders "Ring: connecting..."
// and a watchdog polls g2RingIsConnected() until it flips. The 12-second
// deadline is only the command-kick grace: once the unified BLE worker owns
// the job, the watchdog renews it while g2RingConnectInFlight() remains true,
// including while a real G2 recording delays the connect.
static volatile uint32_t gRingPendingDeadlineMs = 0;
static volatile bool     gRingPendingTaskActive = false;
static constexpr uint32_t RING_PENDING_HARD_TIMEOUT_MS = 230000;

// Menu generation of the last view drawn with the WiFi "Pending..." or the R1
// "connecting..." overlay. showWiFiMenu / showBluetoothR1Menu publish it, so it
// is the view the tap drew before arming the watchdog, or the same menu drawn
// again after the user came back to it while still pending.
static uint32_t gWifiPendingViewGen = 0;
static uint32_t gRingPendingViewGen = 0;

// The pending watchdogs run on their own tasks and never draw. When their state
// changes they queue a redraw of the view that last showed the overlay; the lens
// applier drops it if any navigation since then (another Network view, another
// hijack page such as a held note, a lens re-entry) bumped the menu generation.
static void queuePendingViewRedraw(uint32_t* viewGen, NetworkSubMode sub,
                                   void (*render)(), const char* tag) {
  G2CmdCookie cookie{};
  cookie.menuGen      = __atomic_load_n(viewGen, __ATOMIC_ACQUIRE);
  cookie.targetPage   = G2_HIJACK_PAGE_NETWORK;
  cookie.targetNetSub = (uint8_t)sub;
  (void)g2EnqueueRedraw(cookie, render, tag);
}

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

// The WiFi menu has a compact radio-off layout, so visible row indices are not
// stable. Keep the action bound to the exact accepted layout instead of
// reconstructing it from live WiFi state when a later tap arrives (the radio
// may have changed in between). Bound through the shared row table above.
enum WifiMenuAction : uint8_t {
  WIFI_MENU_BACK = 0,
  WIFI_MENU_TOGGLE_RADIO,       // on: radiopower on; off: radiopower off
  WIFI_MENU_TOGGLE_CONNECTION,  // on: openwifi (Pending overlay); off: closewifi
  WIFI_MENU_DISCONNECT,
  WIFI_MENU_STATUS,
  WIFI_MENU_SCAN,
  WIFI_MENU_SAVED,
  WIFI_MENU_AUTO_START,         // wifiautostart 1|0
  WIFI_MENU_HTTP,
  WIFI_MENU_ESPNOW,
};

static void showWiFiMenu() {
  setNetSub(NET_SUB_WIFI);
  const uint32_t viewGen = g2CurrentMenuGen();  // this view, before drawing
  EXT_RAM_BSS_ATTR static char radioLine[24];  // "Radio: ON/OFF" — whole-radio power (airplane mode)
  EXT_RAM_BSS_ATTR static char wifiLine[64];  // PSRAM: deep-copied by g2ShowListPage
  EXT_RAM_BSS_ATTR static char autoLine[32];   // "Auto Start: ON/OFF" — boot-time auto-connect
  EXT_RAM_BSS_ATTR static char httpLine[40];  // PSRAM: deep-copied by g2ShowListPage
  EXT_RAM_BSS_ATTR static char espnowLine[40];  // PSRAM: deep-copied by g2ShowListPage

#if ENABLE_WIFI
  // Two separate axes shown as two rows: RADIO power vs WiFi CONNECTION.
  const bool radioOn = wifiRadioOn();
  snprintf(radioLine, sizeof(radioLine), "Radio: %s", radioOn ? "ON" : "OFF");
  bool connected = WiFi.isConnected();
  const uint32_t pendingDeadline = wifiPendingOverlayDeadline();
  const bool pending = !connected && pendingDeadline > 0 &&
                       (int32_t)(millis() - pendingDeadline) < 0;
  if (connected) {
    String ssid = WiFi.SSID();
    // Truncate long SSIDs so the row still fits the ~32-char visible width.
    String shown = (ssid.length() > 16) ? (ssid.substring(0, 16) + "~") : ssid;
    snprintf(wifiLine, sizeof(wifiLine), "WiFi: %s", shown.c_str());
  } else if (pending) {
    snprintf(wifiLine, sizeof(wifiLine), "WiFi: Pending...");
  } else if (!gSettings.wifiEnabled) {
    snprintf(wifiLine, sizeof(wifiLine), "WiFi: Disabled in Settings");
  } else if (!radioOn) {
    snprintf(wifiLine, sizeof(wifiLine),
             "WiFi: Disabled (tap to enable)");
  } else {
    snprintf(wifiLine, sizeof(wifiLine), "WiFi: Disconnected");
  }
#else
  const bool radioOn = false;
  bool connected = false;
  const bool pending = false;
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
           radioOn ? "" : " (radio off)");
#else
  snprintf(espnowLine, sizeof(espnowLine), "ESP-NOW: n/a");
#endif

  NetRowList rows;
  rows.add("<- Connect", WIFI_MENU_BACK);
  rows.add(radioLine, WIFI_MENU_TOGGLE_RADIO, /*on=*/!radioOn);
  // A live link reads as the SSID and the tap disconnects; Pending, Disabled
  // and Disconnected all read as "connect" (Pending re-arms the attempt).
  rows.add(wifiLine, WIFI_MENU_TOGGLE_CONNECTION, /*on=*/!connected);

  // With the whole radio down neither action is useful. The WiFi row above is
  // the deliberate way back in: it runs openwifi, after which the refreshed
  // radio-on menu exposes Disconnect and Scan again. The persisted master
  // switch still gates Scan; Disconnect remains available for a live/pending
  // connection so the user can always stop it.
  const bool showDisconnect = connected || pending ||
                              (gSettings.wifiEnabled && radioOn);
  if (showDisconnect)
    rows.add("Disconnect", WIFI_MENU_DISCONNECT);

  rows.add("Status", WIFI_MENU_STATUS);
  if (gSettings.wifiEnabled && (radioOn || connected || pending))
    rows.add("Scan Networks", WIFI_MENU_SCAN);

  rows.add("List Saved", WIFI_MENU_SAVED);
  rows.add(autoLine, WIFI_MENU_AUTO_START, /*on=*/!gSettings.wifiAutoStart);
  rows.add(httpLine, WIFI_MENU_HTTP);
  rows.add(espnowLine, WIFI_MENU_ESPNOW);

  showNetRows(NET_SUB_WIFI, rows);
  if (pending) __atomic_store_n(&gWifiPendingViewGen, viewGen, __ATOMIC_RELEASE);
  DEBUG_G2F("[G2] WiFi submenu shown (radio=%d connected=%d rows=%u auto=%d)",
            radioOn ? 1 : 0, connected ? 1 : 0, (unsigned)rows.n,
            gSettings.wifiAutoStart ? 1 : 0);
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
    // Quarter-dBm: printing the raw value here used to read "TX: 84dBm".
    char txPower[24];
    if (wifiFormatTxPower(txPower, sizeof(txPower)) == ESP_OK) {
      snprintf(rows[n], sizeof(rows[n]), "TX: %s", txPower);
    } else {
      snprintf(rows[n], sizeof(rows[n]), "TX: unavailable");
    }
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

// Fallback while the coordinated WiFi scan runs: back-pane list when the Even-AI
// front card could not be opened (e.g. CTRL ENTER error 7). Primary path
// is g2ShowEvenAIReplyNoAsk("Scanning networks...") in runNetworkScanFlow,
// which empties the scan cache before queueing this view. Returns the menu
// generation of the scan view it draws.
static uint32_t showWiFiScanningPlaceholder() {
  setNetSub(NET_SUB_WIFI_SCAN);
  const uint32_t viewGen = g2CurrentMenuGen();
  static const char* rows[] = {
      "<- WiFi",
      "Scanning… (wait)",
  };
  g2ShowListPage(rows, sizeof(rows) / sizeof(rows[0]));
  DEBUG_G2F("[G2] WiFi: scan placeholder shown (back pane)");
  return viewGen;
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
             "%s %.32s %ddBm",
             gScanCache[i].secured ? "L" : " ",
             gScanCache[i].ssid[0] ? gScanCache[i].ssid : "<hidden>",
             gScanCache[i].rssi);
    ptrs[n++] = rows[1 + i];
  }
  if (n == 1) {
    const char* empty[] = {
      "<- WiFi",
      gScanFailure[0] ? gScanFailure : "(no networks found)",
    };
    g2ShowListPage(empty, 2);
  } else {
    g2ShowListPage(ptrs, n);
  }
  DEBUG_G2F("[G2] WiFi scan results (%u APs)",
            (unsigned)gScanCacheCount);
}

// Saved-network list. Retain the exact identities used for these rows until the
// next admitted list; store changes cannot retarget a later Connect/Forget tap.
static void showWiFiSavedList() {
#if ENABLE_WIFI
  WifiNetworkSnapshot snapshot;
  const bool available = wifiNetworkSnapshot(snapshot);
  static EXT_RAM_BSS_ATTR char rows[1 + MAX_WIFI_NETWORKS][48];  // PSRAM: deep-copied by g2ShowListPage
  strcpy(rows[0], "<- WiFi");
  const char* ptrs[1 + MAX_WIFI_NETWORKS];
  ptrs[0] = rows[0];
  size_t n = 1;
  const int count = snapshot.count;
  for (int i = 0; i < count && i < MAX_WIFI_NETWORKS; i++) {
    snprintf(rows[1 + i], sizeof(rows[1 + i]),
             "[%d] %s%s",
             snapshot.entries[i].priority,
             snapshot.entries[i].ssid.c_str(),
             i == 0 ? " *" : "");
    ptrs[n++] = rows[1 + i];
  }
  bool admitted;
  if (n == 1) {
    const char* empty[] = {
      "<- WiFi", available ? "(no saved networks)" : "(saved list unavailable)"
    };
    admitted = g2ShowListPage(empty, 2);
  } else {
    admitted = g2ShowListPage(ptrs, n);
  }
  if (!admitted) return;
  gWifiSavedSnapshot = std::move(snapshot);
  gWifiSavedSel = -1;
  setNetSub(NET_SUB_WIFI_SAVED);
  DEBUG_G2F("[G2] WiFi saved list (%d entries) — tap for actions",
            count);
#else
  static const char* na[] = { "<- WiFi", "(WiFi not compiled)" };
  if (g2ShowListPage(na, 2)) setNetSub(NET_SUB_WIFI_SAVED);
#endif
}

// Per-saved-network action menu — reached by tapping a network on the saved
// list. gWifiSavedSel holds its index into the retained snapshot. Lets the user CONNECT
// to that saved network (password already stored) or FORGET it, instead of the
// old behaviour where a tap immediately forgot it.
static void showWiFiSavedActionMenu() {
#if ENABLE_WIFI
  EXT_RAM_BSS_ATTR static char back[48];  // PSRAM: deep-copied by g2ShowListPage
  if (gWifiSavedSel >= 0 && gWifiSavedSel < gWifiSavedSnapshot.count) {
    snprintf(back, sizeof(back), "<- %s", gWifiSavedSnapshot.entries[gWifiSavedSel].ssid.c_str());
  } else {
    strcpy(back, "<- Saved");
  }
  const char* items[] = { back, "Connect", "Forget" };
  if (g2ShowListPage(items, sizeof(items) / sizeof(items[0])))
    setNetSub(NET_SUB_WIFI_SAVED_ACT);
#else
  static const char* na[] = { "<- Saved", "(WiFi not compiled)" };
  if (g2ShowListPage(na, 2)) setNetSub(NET_SUB_WIFI_SAVED_ACT);
#endif
}

// -----------------------------------------------------------------------------
// ESP-NOW submenu
// -----------------------------------------------------------------------------

static void showEspNowMenu() {
  setNetSub(NET_SUB_ESPNOW);
  EXT_RAM_BSS_ATTR static char stateLine[40];  // PSRAM: deep-copied by g2ShowListPage
  EXT_RAM_BSS_ATTR static char modeLine[40];  // PSRAM: deep-copied by g2ShowListPage
  EXT_RAM_BSS_ATTR static char devsLine[32];  // PSRAM: deep-copied by g2ShowListPage
  EXT_RAM_BSS_ATTR static char autoLine[32];   // "Auto Start: ON/OFF" — boot-time auto-init
  NetRowList rows;
  rows.add("<- WiFi", ESPNOW_ROW_BACK);  // WiFi submenu is ESP-NOW's parent

#if ENABLE_ESPNOW
  EXT_RAM_BSS_ATTR static char nameLine[48];  // PSRAM: deep-copied by g2ShowListPage
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
    rows.add(stateLine, ESPNOW_ROW_POWER, /*on=*/false);
    rows.add(modeLine, ESPNOW_ROW_INFO);
    rows.add(devsLine, ESPNOW_ROW_INFO);
  } else {
    // Only show toggle + View Devices + name editor when off; no point
    // listing Mode/Peers lines for a subsystem that isn't running.
    snprintf(stateLine, sizeof(stateLine), "ESP-NOW: OFF");
    rows.add(stateLine, ESPNOW_ROW_POWER, /*on=*/true);
  }
  rows.add("View Devices", ESPNOW_ROW_DEVICES);
  rows.add(nameLine, ESPNOW_ROW_NAME);  // tap → on-glasses text entry
  rows.add(autoLine, ESPNOW_ROW_AUTO_START, /*on=*/!gSettings.espnowEnabled);
#else
  rows.add("ESP-NOW: not compiled", ESPNOW_ROW_INFO);
#endif
  showNetRows(NET_SUB_ESPNOW, rows);
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

// The Bluetooth menu's layout: the central-host roles (the glasses, the pipe
// beside them, the phone pipe alone) get the client rows, the app console the
// server rows. The running role wins (hw1link::roleOf, where a live console
// server outranks the rest); with nothing running, the saved mode is the role
// the BLE row starts.
static bool bluetoothClientRoleForUi(const BleLinkStatus& link) {
  const hw1link::Role role = link.role == hw1link::Role::Off ? link.saved : link.role;
  return role != hw1link::Role::Console && role != hw1link::Role::Off;
}

// G2-specific rows (mode/conn, boot AutoReconnect, saved-MAC reconnect,
// disconnect) live here so the parent Bluetooth list stays short. Parent
// keeps BLE on/off, Auto Start, and R1 ring rows only.
static void showBluetoothG2Menu() {
  setNetSub(NET_SUB_BLUETOOTH_G2);
  EXT_RAM_BSS_ATTR static char connLine[40];  // PSRAM: deep-copied by g2ShowListPage
  EXT_RAM_BSS_ATTR static char autoReconnLine[44];  // PSRAM: deep-copied by g2ShowListPage

  // Mode row used to live here as info-only; promoted to the parent
  // Bluetooth menu as a toggle 2026-05-09 so the user can flip
  // server↔client without drilling through G2-specific controls. The
  // G2 submenu now stays focused on G2-specific actions only.
  const bool active = isG2ClientInitialized();
  if (active) {
    snprintf(connLine, sizeof(connLine), "Conn: %s",
             isG2Connected() ? "yes" : "no");
  } else {
    snprintf(connLine, sizeof(connLine), "BLE: stopped");
  }
  const bool autoReconnect = blePeerAutoReconnectEnabled(BLE_PEER_G2_GLASSES);
  snprintf(autoReconnLine, sizeof(autoReconnLine), "G2 AutoReconnect: %s",
           autoReconnect ? "ON" : "OFF");

  NetRowList rows;
  rows.add("<- Bluetooth", G2_ROW_BACK);
  rows.add(connLine, G2_ROW_INFO);  // "Conn: yes/no", or "BLE: stopped"
  rows.add(autoReconnLine, G2_ROW_AUTO_RECONNECT, /*on=*/!autoReconnect);  // boot auto-reconnect
  if (active) {
    rows.add("Reconnect G2", G2_ROW_RECONNECT);   // saved MACs — same as boot path
    rows.add("Disconnect G2", G2_ROW_DISCONNECT);
  }
  showNetRows(NET_SUB_BLUETOOTH_G2, rows);
  DEBUG_G2F("[G2] Bluetooth → G2 submenu shown (active=%d)", active ? 1 : 0);
}

// R1 ring submenu — mirror of showBluetoothG2Menu. Connect/Disconnect/
// Reconnect Ring + R1 AutoReconnect toggle all live here so the parent
// Bluetooth menu stays short. Added 2026-05-09 alongside the parent-
// menu R1 >> drill-in.
static void showBluetoothR1Menu() {
  setNetSub(NET_SUB_BLUETOOTH_R1);
  const uint32_t viewGen = g2CurrentMenuGen();  // this view, before drawing
  EXT_RAM_BSS_ATTR static char connLine[40];  // PSRAM: deep-copied by g2ShowListPage
  EXT_RAM_BSS_ATTR static char autoReconnLine[44];  // PSRAM: deep-copied by g2ShowListPage

  const bool active = isG2ClientInitialized();
  const bool ringUp = g2RingIsConnected();
  // "connecting..." while a fresh connect is in flight (async, no completion
  // event) so the user gets feedback instead of a stale "disconnected".
  const bool pending = !ringUp && gRingPendingDeadlineMs > 0 &&
                       (int32_t)(millis() - gRingPendingDeadlineMs) < 0;
  // On a radio with fewer than four links the enabled phone pipe holds the
  // ring's slot (BLE_CentralCoordinator.h), so say why it stays down.
  const bool paused = !ringUp && bleCentralRingShouldYield();
  if (active) {
    snprintf(connLine, sizeof(connLine), "Ring: %s",
             pending ? "connecting..."
                     : (ringUp ? "connected" : (paused ? "paused (phone pipe on)" : "disconnected")));
  } else {
    snprintf(connLine, sizeof(connLine), "BLE: stopped");
  }
  const bool autoReconnect = blePeerAutoReconnectEnabled(BLE_PEER_R1_RING);
  snprintf(autoReconnLine, sizeof(autoReconnLine), "R1 AutoReconnect: %s",
           autoReconnect ? "ON" : "OFF");

  NetRowList rows;
  rows.add("<- Bluetooth", R1_ROW_BACK);
  rows.add(connLine, R1_ROW_INFO);  // "Ring: ...", or "BLE: stopped"
  rows.add(autoReconnLine, R1_ROW_AUTO_RECONNECT, /*on=*/!autoReconnect);  // boot auto-reconnect
  if (active) {
    if (ringUp) {
      rows.add("Reconnect Ring", R1_ROW_RECONNECT);
      rows.add("Disconnect Ring", R1_ROW_DISCONNECT);
    } else {
      rows.add("Connect Ring", R1_ROW_CONNECT);
    }
  }
  showNetRows(NET_SUB_BLUETOOTH_R1, rows);
  if (pending) __atomic_store_n(&gRingPendingViewGen, viewGen, __ATOMIC_RELEASE);
  DEBUG_G2F("[G2] Bluetooth → R1 submenu shown (active=%d ringUp=%d)",
            active ? 1 : 0, ringUp ? 1 : 0);
}

#if ENABLE_BLE_PHONE_PIPE
// Phone relay pipe submenu — beside G2 and R1. The pipe joins the glasses'
// central host; pairing (`phone pair`) needs a local admin on the
// device's own screen or console, so the lens only says whether it is paired.
// The rows use the words every surface shares (BLE_LinkStatusCore.h): the pipe
// ("ready", "searching", "fail: not found", ...), its enrollment, and the app
// console, which rides the pipe's own link beside the glasses ("waiting" until
// the pipe is ready, then "signed out" / "signed in"; "no secret" without
// blesecret).
static void showBluetoothPhoneMenu() {
  setNetSub(NET_SUB_BLUETOOTH_PHONE);
  EXT_RAM_BSS_ATTR static char pipeRow[32];      // "Pipe: fail: not found" is the longest
  EXT_RAM_BSS_ATTR static char consoleRow[24];   // "Console: signed out" is the longest
  EXT_RAM_BSS_ATTR static char autoReconnLine[44];  // PSRAM: deep-copied by g2ShowListPage
  const BleLinkStatus link = bleLinkStatus();
  // A stopped pipe reads "off" or "no secret" (hw1link::pipeOf).
  const bool active = link.pipe.phase != hw1link::Pipe::Off &&
                      link.pipe.phase != hw1link::Pipe::NoSecret;
  char pipeWords[24];
  snprintf(pipeRow, sizeof(pipeRow), "Pipe: %s",
           hw1link::pipeLine(link.pipe, pipeWords, sizeof(pipeWords)));
  snprintf(consoleRow, sizeof(consoleRow), "Console: %s", hw1link::consoleText(link.console));
  snprintf(autoReconnLine, sizeof(autoReconnLine), "Phone AutoReconnect: %s",
           link.pipeAutoReconnect ? "ON" : "OFF");

  NetRowList rows;
  rows.add("<- Bluetooth", PHONE_ROW_BACK);
  rows.add(pipeRow, PHONE_ROW_INFO);
  rows.add(link.pipePaired ? "Paired: yes" : "Paired: no", PHONE_ROW_INFO);
  rows.add(consoleRow, PHONE_ROW_INFO);
  rows.add(autoReconnLine, PHONE_ROW_AUTO_RECONNECT, /*on=*/!link.pipeAutoReconnect);
  // One verb each way: `openphone` opens the pipe on the client stack and
  // connects the paired phone; `closephone` drops the link and stops the pipe.
  rows.add(active ? "Close Phone" : "Open Phone", PHONE_ROW_PIPE, /*on=*/!active);
  showNetRows(NET_SUB_BLUETOOTH_PHONE, rows);
  DEBUG_G2F("[G2] Bluetooth → Phone submenu shown (pipe=%s console=%s)",
            hw1link::pipeId(link.pipe.phase), hw1link::consoleId(link.console));
}
#endif

static void showBluetoothMenu() {
  setNetSub(NET_SUB_BLUETOOTH);
  EXT_RAM_BSS_ATTR static char stateLine[40];  // PSRAM: deep-copied by g2ShowListPage
  EXT_RAM_BSS_ATTR static char roleRow[24];    // "Role: Glasses+pipe" is the longest
  EXT_RAM_BSS_ATTR static char savedRow[24];   // "Saved: Glasses+pipe"
  EXT_RAM_BSS_ATTR static char consoleRow[24]; // "Console: signed out" (server layout) is the longest
  EXT_RAM_BSS_ATTR static char autoLine[32];   // "Auto Start: ON/OFF" — boot-time auto-init

  // Aggregate-status pattern (mirrors the dashboard tile + ESPNow): the
  // BLE subsystem is "on" when EITHER the server OR the G2 client is
  // initialized, and the rendered state reflects whichever is active.
  // Without this, viewing the menu from the lens (which means G2 client
  // is connected) showed "BLE: OFF" — confusing and wrong.
  const BleLinkStatus link = bleLinkStatus();
  const bool active   = bleSubsystemActive();
  const bool isClient = bluetoothClientRoleForUi(link);
  snprintf(autoLine, sizeof(autoLine), "Auto Start: %s",
           gSettings.bleAutoStart ? "ON" : "OFF");
  // The server layout's Console row is the app console in the shared words
  // (hw1link::consoleText: "waiting", "hidden", "signed out", "signed in"),
  // read from the snapshot, never from gBLEState. It replaced "Conn: yes/no".
  // In the client layout the console rides the phone pipe's link, so its row
  // is under Phone >>. The lens offers no switch to server mode (openconsole):
  // starting it would drop the glasses the lens runs on.
  // The Role row names what runs now in the words every surface shares
  // (hw1link::roleText: "App console", "Glasses", "Glasses+pipe", "Phone pipe",
  // "Off"); it replaced "Mode: server" / "Mode: client (G2)", which named
  // neither the phone pipe nor the app console. It is also the mode toggle
  // (was info-only inside the G2 submenu before 2026-05-09), between the BLE
  // on/off row and the G2/R1 drill-ins so it's visible regardless of mode and
  // immediately adjacent to the BLE toggle that flipping it may also disrupt.
  // Tap dispatches `blemode <other>` via cmd_exec — that handler
  // tears down the active mode (deinitG2Client / deinitBluetooth)
  // before flipping the persisted setting, mirroring the destructive
  // semantics of the BLE on/off toggle on idx=1. A saved mode that differs
  // (always, while nothing runs) gets its own "Saved: ..." row rather than
  // roleLine's "(saved: ...)" suffix, so each lens row stays short.
  snprintf(roleRow, sizeof(roleRow), "Role: %s", hw1link::roleText(link.role));
  const bool savedDiffers = hw1link::savedDiffers(link.role, link.saved);
  if (savedDiffers) {
    snprintf(savedRow, sizeof(savedRow), "Saved: %s", hw1link::roleText(link.saved));
  }
  if (active) {
    snprintf(stateLine, sizeof(stateLine), "BLE: ON (%s)",
             bleSubsystemStateString());
    snprintf(consoleRow, sizeof(consoleRow), "Console: %s",
             hw1link::consoleText(link.console));
  } else {
    // Subsystem fully inactive — Role toggle still useful so the user
    // can pre-select what the BLE-on toggle starts.
    snprintf(stateLine, sizeof(stateLine), "BLE: OFF");
  }

  NetRowList rows;
  rows.add("<- Connect", BT_ROW_BACK);
  // The BLE row starts or stops the role the menu shows.
  rows.add(stateLine, isClient ? BT_ROW_CLIENT_POWER : BT_ROW_SERVER_POWER,
           /*on=*/!active);
  rows.add(roleRow, BT_ROW_MODE, /*on (to client)=*/!isClient);
  if (savedDiffers) rows.add(savedRow, BT_ROW_INFO);
  if (isClient) {
    // Client mode: G2 + R1 controls each live under their own
    // submenu so the parent stays at BLE / Role / G2 >> / R1 >> /
    // Auto Start. Ring action rows (Connect/Reconnect/Disconnect
    // Ring) moved into the R1 submenu 2026-05-09 alongside this
    // restructure. With BLE off they still hold the AutoReconnect rows.
    rows.add("G2 >>", BT_ROW_G2);
    rows.add("R1 >>", BT_ROW_R1);
#if ENABLE_BLE_PHONE_PIPE
    rows.add("Phone >>", BT_ROW_PHONE);
#endif
  } else if (active) {
    // Server mode (legacy layout, plus Role toggle slotted in).
    rows.add(consoleRow, BT_ROW_INFO);
    rows.add("Toggle Adv", BT_ROW_TOGGLE_ADV);
    rows.add("Disconnect", BT_ROW_DISCONNECT);
  }
  rows.add(autoLine, BT_ROW_AUTO_START, /*on=*/!gSettings.bleAutoStart);
  showNetRows(NET_SUB_BLUETOOTH, rows);
  DEBUG_G2F("[G2] Bluetooth submenu shown (role=%s saved=%s console=%s layout=%s)",
            hw1link::roleId(link.role), hw1link::roleId(link.saved),
            hw1link::consoleId(link.console), isClient ? "client" : "server");
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
// task. Two workers used to interleave uncoordinated scans,
// gScanCache writes, and EvenAI ENTER/ANALYSE/REPLY/HIDE — that
// produced empty results, missing "Scanning..." cards, and (under load)
// heap asserts (tlsf double-free) in field logs.
static portMUX_TYPE s_netScanMux = portMUX_INITIALIZER_UNLOCKED;
static bool s_netScanRunning = false;
// The scan flow never draws from its task. It reports into one Network view:
// the WiFi menu it was started from, then the back-pane placeholder once the
// lens applier has drawn that. Its redraws are fenced on this view's menu
// generation, so a scan that ends after the user left draws nothing.
// Both fields are guarded by s_netScanMux.
static G2CmdCookie s_netScanView = {};
static bool s_netScanResultsReady = false;

static void netScanMarkFinished(void) {
  portENTER_CRITICAL(&s_netScanMux);
  s_netScanRunning = false;
  portEXIT_CRITICAL(&s_netScanMux);
}

static bool cacheG2ScanRecord(const WifiScanRecord& record,
                              uint16_t /*index*/, uint16_t /*total*/,
                              void* /*context*/) {
  if (gScanCacheCount >= 8) return false;
  CachedAp& cached = gScanCache[gScanCacheCount++];
  strlcpy(cached.ssid, record.ssid, sizeof(cached.ssid));
  cached.rssi = record.rssi;
  cached.secured = record.authMode != WIFI_AUTH_OPEN;
  return gScanCacheCount < 8;
}

// Lens-applier renderer for the placeholder, queued fenced on the WiFi menu
// view. Drawing it moves Network to the scan sub-page (a new menu generation),
// so the flow reports into that view from here on. If the worker already
// published its results, the redraw it queued against the WiFi menu view is
// stale now and will be dropped, so this queues the results redraw on the scan
// view instead. It cannot draw them in this lens job: the placeholder's page
// swap is still queued behind it, so a second list render here is refused.
static void showScanPlaceholderForFlow() {
  const uint32_t viewGen = showWiFiScanningPlaceholder();
  portENTER_CRITICAL(&s_netScanMux);
  s_netScanView.menuGen      = viewGen;
  s_netScanView.targetNetSub = (uint8_t)NET_SUB_WIFI_SCAN;
  const bool resultsReady = s_netScanResultsReady;
  const G2CmdCookie view = s_netScanView;
  portEXIT_CRITICAL(&s_netScanMux);
  if (resultsReady) (void)g2EnqueueRedraw(view, &showScanResults, "WiFi scan results");
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

  // Start from an empty cache, before any placeholder can be drawn, so its
  // "Scanning…" row never maps to an AP from an earlier scan.
  memset(gScanCache, 0, sizeof(gScanCache));
  gScanCacheCount = 0;
  gScanFailure[0] = '\0';

  // 2. Clear any stuck Even-AI card from a previous scan, then try the
  //    front-pane "Scanning..." card (primary UX). We intentionally do
  //    NOT call g2ShowListPage before this: enqueueing a back-pane
  //    SHUTDOWN+CREATE first races the Even-AI pipeline and often left
  //    users with only the small list and no front card. Skipped when the
  //    user already left the WiFi menu during the settle window.
  portENTER_CRITICAL(&s_netScanMux);
  const G2CmdCookie startView = s_netScanView;
  portEXIT_CRITICAL(&s_netScanMux);
  const bool cardAttempted = startView.menuGen == g2CurrentMenuGen();
  if (cardAttempted) {
    g2HideEvenAICard();
    vTaskDelay(pdMS_TO_TICKS(120));

    if (!g2ShowEvenAIReplyNoAsk("Scanning networks...")) {
      DEBUG_G2F("[G2] WiFi: Even-AI scanning card failed — back-pane "
                "placeholder");
      (void)g2EnqueueRedraw(startView, &showScanPlaceholderForFlow,
                            "WiFi scan placeholder");
    }
  } else {
    DEBUG_G2F("[G2] WiFi: WiFi menu left before the scan card — no card");
  }

  // 3. Run the coordinated synchronous scan. Hidden=true preserves the old
  //    raw-order behavior: hidden entries count toward the first-eight cap and
  //    appear as "<hidden>" in the result list. An OK coordinator result has
  //    released the driver's scan records; a cleanup failure is non-OK and the
  //    copied prefix is discarded below.
  const WifiScanResult result = wifiScanForEach(
      /*includeHidden=*/true, cacheG2ScanRecord, nullptr,
      /*acquireTimeoutMs=*/0);
  if (!result.ok()) {
    // A mid-drain driver failure is not a successful partial scan. Discard the
    // copied prefix so the page cannot present stale/incomplete AP choices.
    memset(gScanCache, 0, sizeof(gScanCache));
    gScanCacheCount = 0;
    const char* status = wifiScanStatusText(result.status);
    snprintf(gScanFailure, sizeof(gScanFailure), "(scan %s; retry)", status);
    DEBUG_G2F("[G2] Network: scan failed (%s, driver=%ld)", status,
              (long)result.driverError);
  }
  DEBUG_G2F("[G2] Network: scan finished, cached %u/%u APs",
            (unsigned)gScanCacheCount, (unsigned)result.found);

  // 4. Dismiss the front-pane card explicitly so it disappears the
  //    moment the back-pane swap happens, rather than lingering for
  //    its full firmware auto-dismiss window.
  if (cardAttempted) g2HideEvenAICard();

  // 5. Publish the results, then queue the back-pane swap to the results list
  //    on the view the flow last drew (see showScanPlaceholderForFlow).
  portENTER_CRITICAL(&s_netScanMux);
  s_netScanResultsReady = true;
  const G2CmdCookie resultView = s_netScanView;
  portEXIT_CRITICAL(&s_netScanMux);
  (void)g2EnqueueRedraw(resultView, &showScanResults, "WiFi scan results");
}

static void networkScanWorker(void* /*arg*/) {
  runNetworkScanFlow();
  netScanMarkFinished();
  vTaskDelete(nullptr);
}

static void spawnNetworkScanWorker() {
  // Runs on the tap dispatcher while the WiFi menu is the current view.
  G2CmdCookie startView{};
  startView.menuGen      = g2CurrentMenuGen();
  startView.targetPage   = g2GetHijackPage();
  startView.targetNetSub = (uint8_t)gNetSub;
  portENTER_CRITICAL(&s_netScanMux);
  if (s_netScanRunning) {
    portEXIT_CRITICAL(&s_netScanMux);
    DEBUG_G2F("[G2] WiFi: scan already in progress — ignoring duplicate tap");
    return;
  }
  s_netScanRunning = true;
  s_netScanView = startView;
  s_netScanResultsReady = false;
  portEXIT_CRITICAL(&s_netScanMux);

  // 4 KB stack: the blocking scan path uses ~1 KB, EvenAI builders
  // need ~400 B for the pb buffer, plus FreeRTOS overhead. Same budget
  // as the AI test worker.
  taskStackRecord("g2_net_scan", 4096);
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
// every 500 ms for up to ~10 s and, the moment the connection comes up,
// clears the overlay and queues a redraw of the menu — replacing the
// "WiFi: Pending..." overlay with the live SSID. If the deadline expires
// without a successful connect (no saved network, AP out of range, wrong
// creds), it does the same once so the menu shows the live Disabled or
// Disconnected state instead of getting stuck on "Pending...".
//
// Runs on its own task, not the BLE notify task, because the polling
// loop yields with vTaskDelay. It never draws: the redraw is fenced on the
// view that showed the overlay (queuePendingViewRedraw), so it is dropped
// once the user has navigated anywhere else.
static void wifiPendingWatchdogTask(void* /*arg*/) {
  for (;;) {
    // Monitor whichever attempt currently owns the overlay. A later tap may
    // replace the deadline while this task is asleep; ownership-CAS prevents
    // the older attempt from clearing or redrawing over the newer one.
    for (;;) {
      const uint32_t watchedDeadline = wifiPendingOverlayDeadline();
      if (watchedDeadline == 0) break;

      if ((int32_t)(millis() - watchedDeadline) >= 0) {
        if (!clearWifiPendingOverlayIfOwned(watchedDeadline)) continue;
        queuePendingViewRedraw(&gWifiPendingViewGen, NET_SUB_WIFI,
                               &showWiFiMenu, "WiFi pending");
        break;
      }

      vTaskDelay(pdMS_TO_TICKS(500));
      if (!WiFi.isConnected()) continue;
      if (!clearWifiPendingOverlayIfOwned(watchedDeadline)) continue;
      // Applied only while the WiFi menu that showed "Pending..." is still
      // the current view; otherwise it would clobber the user's next page.
      queuePendingViewRedraw(&gWifiPendingViewGen, NET_SUB_WIFI,
                             &showWiFiMenu, "WiFi pending");
      break;
    }

    // Publish inactivity, then close the handoff race: a tap that re-armed
    // while this task still looked active either starts a replacement task or
    // is reclaimed here. Exactly one watchdog remains responsible.
    __atomic_store_n(&gWifiPendingTaskActive, false, __ATOMIC_RELEASE);
    if (wifiPendingOverlayDeadline() == 0) break;
    if (!claimWifiPendingWatchdog()) break;
  }
  vTaskDelete(nullptr);
}
#endif  // ENABLE_WIFI

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
  (void)g2EnqueueRedraw(cookie, &showWiFiMenu, "WiFi-menu refresh");
}

// openwifi is a synchronous command on cmd_exec_task: by callback time it has
// either associated or finished failing. End the optimistic overlay here so a
// no-saved-network/auth failure immediately reveals the live radio state rather
// than leaving "Pending..." on the lens until the watchdog deadline.
static void onWifiConnectRefreshDone(bool ok,
                                     const char* result,
                                     const G2CmdCookie& cookie,
                                     void* userData) {
  const uint32_t pendingDeadline = (uint32_t)(uintptr_t)userData;
  const bool ownedPending = clearWifiPendingOverlayIfOwned(pendingDeadline);
  DEBUG_G2F("[G2] WiFi-connect refresh cmd done: ok=%d pending=%d seq=%llu menuGen=%u result='%s'",
            (int)ok, (int)ownedPending, (unsigned long long)cookie.seq,
            (unsigned)cookie.menuGen, result ? result : "");
  (void)g2EnqueueRedraw(cookie, &showWiFiMenu, "WiFi-connect refresh");
}

static void onWifiSavedListRefreshDone(bool ok,
                                       const char* result,
                                       const G2CmdCookie& cookie,
                                       void* /*userData*/) {
  DEBUG_G2F("[G2] WiFi-saved refresh cmd done: ok=%d seq=%llu menuGen=%u result='%s'",
            (int)ok, (unsigned long long)cookie.seq,
            (unsigned)cookie.menuGen, result ? result : "");
  (void)g2EnqueueRedraw(cookie, &showWiFiSavedList, "WiFi-saved refresh");
}

// ESP-NOW / Bluetooth submenu refresh callbacks use the same redraw bridge.
#if ENABLE_ESPNOW
static void onEspNowMenuRefreshDone(bool ok,
                                    const char* result,
                                    const G2CmdCookie& cookie,
                                    void* /*userData*/) {
  DEBUG_G2F("[G2] ESP-NOW-menu refresh cmd done: ok=%d seq=%llu menuGen=%u result='%s'",
            (int)ok, (unsigned long long)cookie.seq,
            (unsigned)cookie.menuGen, result ? result : "");
  (void)g2EnqueueRedraw(cookie, &showEspNowMenu, "ESP-NOW-menu refresh");
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
    (void)g2EnqueueRedraw(cookie, &showEspNowMenu, "ESP-NOW open ok");
    return;
  }
  // Still not running → the start failed. Show the captured reason on the lens
  // instead of silently snapping back to OFF.
  strncpy(sEspNowOpenMsg, (result && result[0]) ? result : "ESP-NOW failed to start",
          sizeof(sEspNowOpenMsg) - 1);
  sEspNowOpenMsg[sizeof(sEspNowOpenMsg) - 1] = '\0';
  (void)g2EnqueueRedraw(cookie, &showEspNowOpenMsg, "ESP-NOW open failed");
}

// After the first-time name is persisted, start ESP-NOW. If the user has
// already left this submenu, still issue openespnow (that was the tap) but
// skip the lens redraw so we don't clobber whatever they navigated to.
static void onEspNowNameThenOpenDone(bool ok,
                                     const char* result,
                                     const G2CmdCookie& cookie,
                                     void* /*userData*/) {
  DEBUG_G2F("[G2] ESP-NOW name-then-open: setname ok=%d result='%s' name='%s'",
            (int)ok, result ? result : "",
            gSettings.espnowDeviceName.c_str());
  if (gSettings.espnowDeviceName.length() == 0) {
    strncpy(sEspNowOpenMsg,
            (result && result[0]) ? result : "ESP-NOW needs a device name",
            sizeof(sEspNowOpenMsg) - 1);
    sEspNowOpenMsg[sizeof(sEspNowOpenMsg) - 1] = '\0';
    (void)g2EnqueueRedraw(cookie, &showEspNowOpenMsg,
                                  "ESP-NOW setname failed");
    return;
  }
  const bool stillHere =
      (g2GetHijackPage() == cookie.targetPage &&
       gNetSub == NET_SUB_ESPNOW);
  G2HijackCmdCallback cb = stillHere ? onEspNowOpenDone : nullptr;
  if (!g2SubmitHijackCommand("openespnow", cookie, cb, nullptr)) {
    DEBUG_G2F("[G2] ESP-NOW name-then-open: openespnow submit FAILED");
    if (stillHere) {
      (void)g2EnqueueRedraw(cookie, &showEspNowMenu,
                                    "ESP-NOW open submit failed");
    }
  }
}
#endif

static void onBluetoothMenuRefreshDone(bool ok,
                                       const char* result,
                                       const G2CmdCookie& cookie,
                                       void* /*userData*/) {
  DEBUG_G2F("[G2] Bluetooth-menu refresh cmd done: ok=%d seq=%llu menuGen=%u result='%s'",
            (int)ok, (unsigned long long)cookie.seq,
            (unsigned)cookie.menuGen, result ? result : "");
  (void)g2EnqueueRedraw(cookie, &showBluetoothMenu, "Bluetooth-menu refresh");
}

static void onBluetoothG2MenuRefreshDone(bool ok,
                                         const char* result,
                                         const G2CmdCookie& cookie,
                                         void* /*userData*/) {
  DEBUG_G2F("[G2] Bluetooth-G2-menu refresh cmd done: ok=%d seq=%llu menuGen=%u result='%s'",
            (int)ok, (unsigned long long)cookie.seq,
            (unsigned)cookie.menuGen, result ? result : "");
  (void)g2EnqueueRedraw(cookie, &showBluetoothG2Menu, "Bluetooth-G2-menu refresh");
}

// Watchdog for the R1 ring connect — mirror of wifiPendingWatchdogTask. Polls
// g2RingIsConnected() every 500 ms and queues a redraw of the R1 submenu the
// moment the ring actually connects. After the initial command-kick grace it
// follows the worker's in-flight flag, so a job queued behind a real recording
// does not falsely revert to "disconnected". A 230 s hard bound prevents a
// wedged worker from pinning the overlay forever. Runs on its own task (the
// poll yields with vTaskDelay) and never draws: like the WiFi watchdog, its
// redraw is fenced on the view that showed "connecting...".
static void ringPendingWatchdogTask(void* /*arg*/) {
  bool sawWorkerInFlight = false;
  const uint32_t watchdogStartedMs = millis();
  while ((uint32_t)(millis() - watchdogStartedMs) <
         RING_PENDING_HARD_TIMEOUT_MS) {
    vTaskDelay(pdMS_TO_TICKS(500));
    if (g2RingIsConnected()) break;
    if (g2RingConnectInFlight()) {
      sawWorkerInFlight = true;
      // Keep the renderer's simple, rollover-safe deadline predicate true
      // while the worker owns the job. Hard-timeout cleanup below writes zero.
      gRingPendingDeadlineMs = millis() + 1000u;
      continue;
    }
    if (sawWorkerInFlight) break;  // completed unsuccessfully; repaint now
    const bool kickoffPending = gRingPendingDeadlineMs > 0 &&
                                (int32_t)(millis() - gRingPendingDeadlineMs) < 0;
    if (!kickoffPending) break;
  }
  // Connected, failed or expired: drop the "connecting..." overlay, then
  // repaint the R1 submenu only if it is still the view that showed it —
  // otherwise we'd clobber whatever the user navigated to.
  gRingPendingDeadlineMs = 0;
  queuePendingViewRedraw(&gRingPendingViewGen, NET_SUB_BLUETOOTH_R1,
                         &showBluetoothR1Menu, "R1 pending");
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
  // A refused connect never goes in flight, so drop the "connecting..."
  // overlay now instead of after the watchdog's kick-off grace.
  if (!ok || (result && strncmp(result, "Error:", 6) == 0)) gRingPendingDeadlineMs = 0;
  (void)g2EnqueueRedraw(cookie, &showBluetoothR1Menu, "Bluetooth-R1-menu refresh");
}

#if ENABLE_BLE_PHONE_PIPE
// Phone submenu refresh — same pattern as the R1 callback above.
static void onBluetoothPhoneMenuRefreshDone(bool ok,
                                            const char* result,
                                            const G2CmdCookie& cookie,
                                            void* /*userData*/) {
  DEBUG_G2F("[G2] Bluetooth-Phone-menu refresh cmd done: ok=%d seq=%llu menuGen=%u result='%s'",
            (int)ok, (unsigned long long)cookie.seq,
            (unsigned)cookie.menuGen, result ? result : "");
  (void)g2EnqueueRedraw(cookie, &showBluetoothPhoneMenu, "Bluetooth-Phone-menu refresh");
}
#endif

static void handleWiFiTap(uint32_t idx) {
  NetRowBinding row;
  if (!netRowAt(NET_SUB_WIFI, idx, &row)) return;
  const WifiMenuAction action = (WifiMenuAction)row.action;

  switch (action) {
    case WIFI_MENU_BACK:  // <- Back to top-level
      g2ShowNetworkMenu();
      break;

    case WIFI_MENU_TOGGLE_RADIO: {  // Airplane-mode radio toggle via cmd_exec.
#if ENABLE_WIFI
      G2CmdCookie cookie{};
      cookie.targetPage   = g2GetHijackPage();
      cookie.targetNetSub = (uint8_t)gNetSub;
      const char* rcmd = row.on ? "radiopower on" : "radiopower off";
      DEBUG_G2F("[G2] Radio toggle: %s via cmd_exec", rcmd);
      cancelWifiPendingOverlay();   // radio power is not the WiFi "Pending" overlay
      if (!g2SubmitHijackCommand(rcmd, cookie, onWifiMenuRefreshDone, nullptr)) {
        DEBUG_G2F("[G2] Radio toggle: submit FAILED — no inline mutate");
      }
#endif
      break;
    }

    case WIFI_MENU_TOGGLE_CONNECTION: {  // Connect best / disconnect toggle.
#if ENABLE_WIFI
      // Direction as the row was labelled: the SSID row disconnects, every
      // other state (Pending, Disabled, Disconnected) connects.
      const bool connect = row.on;
      G2CmdCookie cookie{};
      cookie.targetPage   = g2GetHijackPage();
      cookie.targetNetSub = (uint8_t)gNetSub;

      // The persisted master switch is intentionally stronger than the
      // runtime radio toggle. Do not promise/animate a connection attempt that
      // ensureWiFiInitialized() must reject; the row already directs the user
      // to Settings in this state.
      if (connect && !gSettings.wifiEnabled) {
        DEBUG_G2F("[G2] WiFi toggle ignored: WiFi disabled in Settings");
        cancelWifiPendingOverlay();
        showWiFiMenu();
        break;
      }

      if (!connect) {
        // Disconnect path. Submit `closewifi`; callback re-renders the
        // menu showing OFF. Synchronous from the user's perspective —
        // closewifi returns immediately after kicking the disconnect,
        // and from then on WiFi.isConnected() is false so the redraw
        // shows the right state.
        DEBUG_G2F("[G2] WiFi toggle: closewifi via cmd_exec");
        cancelWifiPendingOverlay();   // clear stale pending overlay
        if (!g2SubmitHijackCommand("closewifi", cookie,
                                   onWifiMenuRefreshDone, nullptr)) {
          // Never mutate WiFi on the tap/BLE task — queue full / OOM /
          // blank pairedByUser. UI stays as-is; user can retry.
          DEBUG_G2F("[G2] WiFi toggle: closewifi submit FAILED — no inline mutate");
        }
      } else {
        // Connect path. Optimistic UI: arm the "Pending..." deadline +
        // render synchronously BEFORE the submit so the user sees
        // immediate feedback; the callback cannot fire until openwifi returns.
        // The 150 ms BLE-flush breath stays as-is — the REBUILD must hit the
        // air before WiFi takes radio coexistence. The watchdog can reveal a
        // successful link before openwifi finishes post-connect work; the
        // completion callback ends the overlay on final success or failure.
        DEBUG_G2F("[G2] WiFi toggle: openwifi via cmd_exec (Pending overlay armed)");
        const uint32_t pendingDeadline = armWifiPendingOverlay();
        showWiFiMenu();   // immediate "WiFi: Pending..." render
        vTaskDelay(pdMS_TO_TICKS(150));

        if (!g2SubmitHijackCommand("openwifi", cookie,
                                   onWifiConnectRefreshDone,
                                   (void*)(uintptr_t)pendingDeadline)) {
          DEBUG_G2F("[G2] WiFi toggle: openwifi submit FAILED — no inline mutate");
          clearWifiPendingOverlayIfOwned(pendingDeadline);
          showWiFiMenu();
        } else if (claimWifiPendingWatchdog()) {
          // Watchdog only when the kick actually queued — only one alive
          // at a time; subsequent taps re-arm gWifiPendingDeadlineMs.
          taskStackRecord("g2_wifi_pending", 4096);
          if (xTaskCreatePinnedToCore(wifiPendingWatchdogTask, "g2_wifi_pending",
                          4096, nullptr, /*prio*/ 5, nullptr, APP_CORE) != pdPASS) {
            DEBUG_G2F("[G2] WiFi pending: xTaskCreate failed — "
                      "menu won't auto-refresh on connect");
            __atomic_store_n(&gWifiPendingTaskActive, false, __ATOMIC_RELEASE);
            clearWifiPendingOverlayIfOwned(pendingDeadline);
            showWiFiMenu();
          }
        }
      }
#endif
      break;
    }

    case WIFI_MENU_DISCONNECT: {  // Disconnect AP but KEEP the radio on, so a
                                  // scan/reconnect needs no radio re-enable.
                                  // This deliberately is not `closewifi`, which
                                  // also stops HTTP and disables web output.
#if ENABLE_WIFI
      G2CmdCookie cookie{};
      cookie.targetPage   = g2GetHijackPage();
      cookie.targetNetSub = (uint8_t)gNetSub;
      cancelWifiPendingOverlay();   // cancel any pending overlay
      DEBUG_G2F("[G2] WiFi: Disconnect (radio stays on) via cmd_exec");
      if (!g2SubmitHijackCommand("wifidisconnect", cookie, onWifiMenuRefreshDone, nullptr)) {
        DEBUG_G2F("[G2] WiFi: wifidisconnect submit FAILED — no inline mutate");
      }
#endif
      break;
    }

    case WIFI_MENU_STATUS:  // Drill into detailed info.
      showWiFiStatusPage();
      break;

    case WIFI_MENU_SCAN: {
#if ENABLE_WIFI
      DEBUG_G2F("[G2] WiFi: scan triggered from glasses");
      spawnNetworkScanWorker();
#endif
      break;
    }

    case WIFI_MENU_SAVED: {
      showWiFiSavedList();
      break;
    }

    case WIFI_MENU_AUTO_START: {  // Boot-time Auto Start toggle via cmd_exec.
               // Submits the existing `wifiautostart <0|1>` CLI command
               // via g2SubmitHijackCommand. The completion callback enqueues
               // a Redraw job that the lens applier guards by menuGen and
               // dispatches into showWiFiMenu(). See proposal §5.4 + step 4.
               //
               // This row reads and displays gSettings.wifiAutoStart, so it
               // must write that same setting. It used to submit
               // `wifiautoreconnect`, which is the unrelated drop-recovery
               // hunt: the command succeeded, the row re-read wifiAutoStart,
               // and the toggle appeared to do nothing while silently
               // clearing the user's reconnect preference.
               //
               // (HTTP toggle moved to its own Network -> HTTP(S) section;
               // see showHttpMenu / handleHttpTap below.)
      const bool on = row.on;  // the state the row offered when drawn
      G2CmdCookie cookie{};
      cookie.targetPage   = g2GetHijackPage();
      cookie.targetNetSub = (uint8_t)gNetSub;
      // cookie.seq + cookie.menuGen are stamped by g2SubmitHijackCommand.

      char line[40];
      snprintf(line, sizeof(line), "wifiautostart %d", on ? 1 : 0);

      if (g2SubmitHijackCommand(line, cookie, onWifiMenuRefreshDone, nullptr)) {
        BROADCAST_PRINTF("[G2] WiFi: Auto Start toggle %s→%s submitted via cmd_exec",
                         on ? "OFF" : "ON", on ? "ON" : "OFF");
      } else {
        DEBUG_G2F("[G2] WiFi: Auto Start submit FAILED — no inline mutate");
      }
      break;
    }

    case WIFI_MENU_HTTP:  // Server start/stop, HTTPS mode, auto-start, URL.
      showHttpMenu();
      break;

    case WIFI_MENU_ESPNOW:  // Peer list, role, etc.
      showEspNowMenu();
      break;

    default:
      DEBUG_G2F("[G2] WiFi: unknown action=%u at idx=%u",
                (unsigned)action, (unsigned)idx);
      break;
  }
}

// Status page is info-only — every tap returns to the WiFi submenu.
static void handleWiFiStatusTap(uint32_t /*idx*/) {
  showWiFiMenu();
}

// --- Join-a-scanned-AP flow -------------------------------------------------
// Tap a scanned network → (secured) on-lens char-picker keyboard for the
// password → the owner's structured add (wifiAddCommandLine:
// `wifiadd "<ssid>" "<pass>" 1 0`) through cmd_exec (auth + audit,
// wifiadd is admin-gated so a non-admin pairer sees the denial text) → the
// command's result text on the lens. Open APs skip the keyboard and save with
// a quoted empty password. Accepted v1 limits: this field uses maxLen 32 (< the
// 64-char PSK ceiling — longer PSKs need the web UI) and password renders
// unmasked on the wearer-private lens.
static char sWifiAddSsid[33];
EXT_RAM_BSS_ATTR static char sWifiAddMsg[128];
static void showWifiAddMsg() { g2ShowText(sWifiAddMsg); }

// The saved-network owner decides what a field cannot carry on the wifiadd
// line (a `"` would end a quoted field early, so a scanned `x" "y` would save
// SSID x with password y; control characters too) and writes the line
// (wifiAddCommandLine). The page refuses such a field before anything is
// built and only words the refusal.
static void showWifiAddRefusal() {
  snprintf(sWifiAddMsg, sizeof(sWifiAddMsg), "%s",
           "Error: WiFi name and password cannot contain a double-quote (\") "
           "or control characters");
  showWifiAddMsg();
}

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
  (void)g2EnqueueRedraw(cookie, &showWifiAddMsg, "wifiadd done");
}

// Build + submit from the snapshot SSID and a password. Runs on the BLE
// notify task (keyboard commit) — String build + submit only, no inline
// mutate (wrong stack + skips auth). The keyboard charset already filters
// double-quotes and control bytes; the owner's structured add refuses a field
// the line cannot carry for every caller before the line is built.
static void wifiAddSubmit(const char* pass) {
  if (!pass) pass = "";
  String line;
  if (wifiAddCommandLine(String(sWifiAddSsid), String(pass), /*priority=*/1,
                         /*hidden=*/false, line) != WifiFieldProblem::None) {
    DEBUG_G2F("[G2] wifiadd: SSID or password has a quote/control byte — refused");
    showWifiAddRefusal();
    return;
  }
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
  // Hidden APs scan with an empty SSID — nothing to add.
  if (ap.ssid[0] == '\0') {
    DEBUG_G2F("[G2] WiFi: tapped hidden SSID — ignored");
    return;
  }
  // A scanned SSID with a double-quote or control byte can't survive the
  // quoted command line: say so on the lens instead of opening the keyboard.
  if (wifiNetworkFieldProblem(ap.ssid) != WifiFieldProblem::None) {
    DEBUG_G2F("[G2] WiFi: tapped unquotable SSID — refused");
    showWifiAddRefusal();
    return;
  }
  // Snapshot at tap time: the keyboard overlay blocks page taps but NOT the
  // scan worker, so this gScanCache entry can be rewritten while the
  // keyboard is open.
  strncpy(sWifiAddSsid, ap.ssid, sizeof(sWifiAddSsid) - 1);
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
  cfg.maxLen   = 32;   // field cap; >32-char PSKs still use the web UI
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
  if (savedIdx < static_cast<size_t>(gWifiSavedSnapshot.count)) {
    gWifiSavedSel = (int)savedIdx;
    showWiFiSavedActionMenu();
  }
#endif
}

// Action menu for one saved network: 0 = back, 1 = Connect, 2 = Forget.
static void handleWiFiSavedActionTap(uint32_t idx) {
  if (idx == 0) { showWiFiSavedList(); return; }   // <- back to the saved list
#if ENABLE_WIFI
  if (gWifiSavedSel < 0 || gWifiSavedSel >= gWifiSavedSnapshot.count) {
    showWiFiSavedList();
    return;
  }
  const SavedWifiNetwork& selected = gWifiSavedSnapshot.entries[gWifiSavedSel];
  G2CmdCookie cookie{};
  cookie.targetPage = g2GetHijackPage();

  if (idx == 1) {  // Resolve the displayed entry's identity on cmd_exec.
    char line[40];
    snprintf(line, sizeof(line), "openwifi --saved-id %lu", (unsigned long)selected.id);
    DEBUG_G2F("[G2] WiFi: Connect saved '%s' (--saved-id %lu) via cmd_exec",
              selected.ssid.c_str(), (unsigned long)selected.id);
    cookie.targetNetSub   = NET_SUB_WIFI;     // land back on the WiFi menu
    const uint32_t pendingDeadline = armWifiPendingOverlay();
    setNetSub(NET_SUB_WIFI);
    showWiFiMenu();                            // immediate "Pending..." feedback
    vTaskDelay(pdMS_TO_TICKS(150));
    if (!g2SubmitHijackCommand(line, cookie,
                               onWifiConnectRefreshDone,
                               (void*)(uintptr_t)pendingDeadline)) {
      DEBUG_G2F("[G2] WiFi: Connect saved submit FAILED");
      clearWifiPendingOverlayIfOwned(pendingDeadline);
      showWiFiMenu();
    } else if (claimWifiPendingWatchdog()) {
      taskStackRecord("g2_wifi_pending", 4096);
      if (xTaskCreatePinnedToCore(wifiPendingWatchdogTask, "g2_wifi_pending",
                      4096, nullptr, /*prio*/ 5, nullptr, APP_CORE) != pdPASS) {
        __atomic_store_n(&gWifiPendingTaskActive, false, __ATOMIC_RELEASE);
        clearWifiPendingOverlayIfOwned(pendingDeadline);
        showWiFiMenu();
      }
    }
  } else if (idx == 2) {  // Forget the same displayed entry, return to the list.
    cookie.targetNetSub = NET_SUB_WIFI_SAVED;
    char line[32];
    snprintf(line, sizeof(line), "wifirmid %lu", (unsigned long)selected.id);
    DEBUG_G2F("[G2] WiFi: Forget saved '%s' via cmd_exec", selected.ssid.c_str());
    if (!g2SubmitHijackCommand(line, cookie,
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

// Shared keyboard launch for the Name row and the unnamed OFF→ON tap.
  // maxLen 19 matches the V4 wire field (20 bytes including NUL).
static bool beginEspNowNameEntry(TextEntryCommitFn onCommit) {
  TextEntryConfig cfg = {};
  cfg.prompt   = "ESPNow Name";
  cfg.initial  = gSettings.espnowDeviceName.c_str();
  cfg.maxLen   = 19;
  cfg.onCommit = onCommit;
  cfg.onCancel = espnowNameCancel;
  if (!g2BeginTextEntry(cfg)) {
    DEBUG_G2F("[G2] ESP-NOW: text-entry failed to start");
    return false;
  }
  return true;
}

// Commit path used when the user tapped ESP-NOW: OFF with no name set:
// persist the name, then open ESP-NOW on that command's completion.
static void espnowNameThenOpenCommit(const char* text) {
  if (!text || text[0] == '\0') {
    DEBUG_G2F("[G2] ESP-NOW name-then-open: empty input — not starting");
    showEspNowMenu();
    return;
  }
  String line = String("espnowsetname \"") + text + "\"";
  G2CmdCookie cookie{};
  cookie.targetPage   = g2GetHijackPage();
  cookie.targetNetSub = (uint8_t)gNetSub;
  if (!g2SubmitHijackCommand(line.c_str(), cookie,
                             onEspNowNameThenOpenDone, nullptr)) {
    DEBUG_G2F("[G2] ESP-NOW name-then-open: setname submit FAILED");
    showEspNowMenu();
  }
}
#endif

static void handleEspNowTap(uint32_t idx) {
  // Rows and toggle directions are the ones showEspNowMenu bound: the ON and
  // OFF layouts differ, and ESP-NOW can start or stop between render and tap.
  NetRowBinding row;
  if (!netRowAt(NET_SUB_ESPNOW, idx, &row)) return;

  switch ((EspNowRowAction)row.action) {
    case ESPNOW_ROW_BACK:
      // Back goes to the WiFi submenu now — ESP-NOW is nested under WiFi
      // since both ride the WiFi radio.
      showWiFiMenu();
      return;

    case ESPNOW_ROW_POWER: {  // ESP-NOW state line — toggle via cmd_exec
#if ENABLE_ESPNOW
      if (row.on && gSettings.espnowDeviceName.length() == 0) {
        // Match OLED: ESP-NOW cannot start without a device name, so open
        // the on-glasses keyboard first and start the radio after commit.
        DEBUG_G2F("[G2] ESP-NOW toggle: name unset — opening keyboard");
        if (!beginEspNowNameEntry(espnowNameThenOpenCommit)) {
          showEspNowMenu();
        }
        return;
      }
      const char* line = row.on ? "openespnow" : "closeespnow";
      // For the OPEN path, use a callback that surfaces the failure reason on the
      // lens (init can fail on memory/coexistence); CLOSE just re-renders.
      G2HijackCmdCallback cb = row.on ? onEspNowOpenDone : onEspNowMenuRefreshDone;
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

    case ESPNOW_ROW_DEVICES:
      showEspNowDevices();
      return;

    case ESPNOW_ROW_NAME:
#if ENABLE_ESPNOW
      // Brings up the on-glasses keyboard pre-filled with the current name.
      // onCommit/onCancel both redraw the ESPNow submenu.
      (void)beginEspNowNameEntry(espnowNameCommit);
#endif
      return;

    case ESPNOW_ROW_AUTO_START: {
#if ENABLE_ESPNOW
      char line[40];
      snprintf(line, sizeof(line), "espnowenabled %d", row.on ? 1 : 0);
      G2CmdCookie cookie{};
      cookie.targetPage   = g2GetHijackPage();
      cookie.targetNetSub = (uint8_t)gNetSub;
      if (g2SubmitHijackCommand(line, cookie, onEspNowMenuRefreshDone, nullptr)) {
        BROADCAST_PRINTF("[G2] ESP-NOW: Auto Start toggle %s→%s submitted via cmd_exec",
                         row.on ? "OFF" : "ON", row.on ? "ON" : "OFF");
      } else {
        DEBUG_G2F("[G2] ESP-NOW: Auto Start submit FAILED — no inline mutate");
      }
#endif
      return;
    }

    case ESPNOW_ROW_INFO:
    default:
      DEBUG_G2F("[G2] ESP-NOW: info row %u (no action)", (unsigned)idx);
      return;
  }
}

static void handleEspNowDevsTap(uint32_t idx) {
  if (idx == 0) { showEspNowMenu(); return; }
  // Info-only — pairing/unpairing requires MAC + name text input.
  DEBUG_G2F("[G2] ESP-NOW: device row %u tapped (info-only)", (unsigned)idx);
}

// Toggle the persisted Bluetooth auto-start flag via the `bleautostart`
// CLI command (routed through cmd_exec → runs as the paired-by user).
// Hoisted out so every branch of handleBluetoothTap can call it without
// copy-pasting the submit boilerplate. `on` is the state the row offered when
// it was drawn. Submit failure is a no-op.
static void bluetoothToggleAutoStart(bool on) {
  const char* arg = on ? "on" : "off";
  char line[32];
  snprintf(line, sizeof(line), "bleautostart %s", arg);
  G2CmdCookie cookie{};
  cookie.targetPage   = g2GetHijackPage();
  cookie.targetNetSub = (uint8_t)gNetSub;
  if (g2SubmitHijackCommand(line, cookie, onBluetoothMenuRefreshDone, nullptr)) {
    BROADCAST_PRINTF("[G2] Bluetooth: Auto Start toggle %s→%s submitted via cmd_exec",
                     on ? "OFF" : "ON", on ? "ON" : "OFF");
  } else {
    DEBUG_G2F("[G2] Bluetooth: Auto Start submit FAILED — no inline mutate");
  }
}

static void handleBluetoothG2Tap(uint32_t idx) {
  // Rows as showBluetoothG2Menu bound them (after 2026-05-09 Mode-row promotion
  // to parent menu):
  //   active:    0=Back 1=Conn(info) 2=AutoReconnect 3=Reconnect 4=Disconnect
  //   inactive:  0=Back 1=BLE-stopped(info) 2=AutoReconnect
  NetRowBinding row;
  if (!netRowAt(NET_SUB_BLUETOOTH_G2, idx, &row)) return;
  const G2RowAction action = (G2RowAction)row.action;
  if (action == G2_ROW_BACK) {
    showBluetoothMenu();
    return;
  }
  if (action == G2_ROW_AUTO_RECONNECT) {
    // Route through cmd_exec → `bleautoreconnect g2-glasses on|off`. This
    // is intentional, not just for consistency: cmd_bleautoreconnect calls
    // bleStampPairedByIfBlank when flipping ON, capturing whoever owns
    // the BT subsystem. With auth.user = pairedByUser, that means the
    // glasses become "owned by themselves" the first time you flip this
    // ON from a freshly-paired set — which is fine, since the very first
    // flip would have come from a real admin via the web UI or CLI to
    // pair them in the first place.
    const char* arg = row.on ? "on" : "off";
    char line[64];
    snprintf(line, sizeof(line), "bleautoreconnect g2-glasses %s", arg);
    G2CmdCookie cookie{};
    cookie.targetPage   = g2GetHijackPage();
    cookie.targetNetSub = (uint8_t)gNetSub;
    if (g2SubmitHijackCommand(line, cookie, onBluetoothG2MenuRefreshDone, nullptr)) {
      BROADCAST_PRINTF("[G2] G2 AutoReconnect %s→%s submitted via cmd_exec",
                       row.on ? "OFF" : "ON", row.on ? "ON" : "OFF");
    } else {
      DEBUG_G2F("[G2] G2 AutoReconnect submit FAILED — no inline mutate");
    }
    return;
  }
  if (action != G2_ROW_RECONNECT && action != G2_ROW_DISCONNECT) {
    DEBUG_G2F("[G2] Bluetooth → G2: info row %u (no action)", (unsigned)idx);
    return;
  }
  if (!isG2ClientInitialized()) {
    DEBUG_G2F("[G2] Bluetooth → G2: idx=%u — start BLE first", (unsigned)idx);
    return;
  }
  if (action == G2_ROW_RECONNECT) {
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
  // G2_ROW_DISCONNECT: closeg2 drops the links and releases the glasses
  // runtime (GATT cache included); the ring, the phone and the stack stay up.
  G2CmdCookie cookie{};
  cookie.targetPage   = g2GetHijackPage();
  cookie.targetNetSub = (uint8_t)gNetSub;
  DEBUG_G2F("[G2] Disconnect G2 via cmd_exec (closeg2)");
  if (!g2SubmitHijackCommand("closeg2", cookie,
                             onBluetoothG2MenuRefreshDone, nullptr)) {
    DEBUG_G2F("[G2] Disconnect G2 submit FAILED — no inline mutate");
  }
}

// Forward decl — the R1 menu refresh callback lives in the same
// callbacks block as onBluetoothG2MenuRefreshDone. Declared here so
// handleBluetoothR1Tap below can pass it as the cmd_exec completion.
static void onBluetoothR1MenuRefreshDone(bool ok,
                                         const char* result,
                                         const G2CmdCookie& cookie,
                                         void* userData);

static void handleBluetoothR1Tap(uint32_t idx) {
  // Rows as showBluetoothR1Menu bound them (mirror of handleBluetoothG2Tap):
  //   active+ringUp:    0=Back 1=Conn 2=AutoReconnect 3=Reconnect 4=Disconnect
  //   active+ringDown:  0=Back 1=Conn 2=AutoReconnect 3=Connect
  //   inactive:         0=Back 1=BLE-stopped 2=AutoReconnect
  NetRowBinding row;
  if (!netRowAt(NET_SUB_BLUETOOTH_R1, idx, &row)) return;
  const R1RowAction action = (R1RowAction)row.action;
  if (action == R1_ROW_BACK) {
    showBluetoothMenu();
    return;
  }
  if (action == R1_ROW_AUTO_RECONNECT) {
    // R1 AutoReconnect — same cmd_exec pattern as G2 AutoReconnect, just
    // targeting the r1-ring peer instead of g2-glasses.
    const char* arg = row.on ? "on" : "off";
    char line[64];
    snprintf(line, sizeof(line), "bleautoreconnect r1-ring %s", arg);
    G2CmdCookie cookie{};
    cookie.targetPage   = g2GetHijackPage();
    cookie.targetNetSub = (uint8_t)gNetSub;
    if (g2SubmitHijackCommand(line, cookie, onBluetoothR1MenuRefreshDone, nullptr)) {
      BROADCAST_PRINTF("[G2] R1 AutoReconnect %s→%s submitted via cmd_exec",
                       row.on ? "OFF" : "ON", row.on ? "ON" : "OFF");
    } else {
      DEBUG_G2F("[G2] R1 AutoReconnect submit FAILED — no inline mutate");
    }
    return;
  }
  if (action != R1_ROW_CONNECT && action != R1_ROW_RECONNECT &&
      action != R1_ROW_DISCONNECT) {
    DEBUG_G2F("[G2] Bluetooth → R1: info row %u (no action)", (unsigned)idx);
    return;
  }
  if (!isG2ClientInitialized()) {
    DEBUG_G2F("[G2] Bluetooth → R1: idx=%u — start BLE first", (unsigned)idx);
    return;
  }
  if (action != R1_ROW_DISCONNECT) {
    // "Reconnect Ring" (drawn while the ring was up) → drop + settle + connect
    // on cmd_exec; "Connect Ring" → a fresh connect, even if the ring came up
    // since. Heap guard + 500 ms settle live inside `openring reconnect`.
    const bool fresh = (action == R1_ROW_CONNECT);
    const char* line = fresh ? "openring" : "openring reconnect";
    G2CmdCookie cookie{};
    cookie.targetPage   = g2GetHijackPage();
    cookie.targetNetSub = (uint8_t)gNetSub;
    BROADCAST_PRINTF("[G2] Bluetooth: %s Ring via cmd_exec",
                     fresh ? "Connect" : "Reconnect");
    // Fresh connect: `openring` returns OK on kick-off, but scan+connect+auth
    // finishes ~5-8 s later with no completion event. Arm the "connecting..."
    // overlay + watchdog so the menu flips to "connected" when it actually is,
    // instead of staying stuck on "disconnected". (Reconnect keeps ringUp true,
    // so its menu already reads right — no overlay needed.) Mirrors WiFi connect.
    // While the ring gives way to the phone pipe the command refuses at once,
    // so there is nothing to wait for; the menu already reads "paused".
    const bool armOverlay = fresh && !bleCentralRingShouldYield();
    if (armOverlay) {
      gRingPendingDeadlineMs = millis() + 12000;
      showBluetoothR1Menu();            // immediate "Ring: connecting..." render
      vTaskDelay(pdMS_TO_TICKS(150));   // let the REBUILD hit the air before the scan storms BLE
    }
    if (!g2SubmitHijackCommand(line, cookie,
                               onBluetoothR1MenuRefreshDone, nullptr)) {
      DEBUG_G2F("[G2] R1 %s submit FAILED — no inline mutate",
                fresh ? "connect" : "reconnect");
      gRingPendingDeadlineMs = 0;
      if (fresh) showBluetoothR1Menu();
    } else if (armOverlay && !gRingPendingTaskActive) {
      gRingPendingTaskActive = true;
      taskStackRecord("g2_ring_pending", 4096);
      if (xTaskCreatePinnedToCore(ringPendingWatchdogTask, "g2_ring_pending",
                      4096, nullptr, /*prio*/ 5, nullptr, APP_CORE) != pdPASS) {
        DEBUG_G2F("[G2] Ring pending: xTaskCreate failed — menu won't auto-refresh on connect");
        gRingPendingTaskActive = false;
        gRingPendingDeadlineMs = 0;
      }
    }
    return;
  }
  // R1_ROW_DISCONNECT ("Disconnect Ring", drawn while the ring was up).
  G2CmdCookie cookie{};
  cookie.targetPage   = g2GetHijackPage();
  cookie.targetNetSub = (uint8_t)gNetSub;
  BROADCAST_PRINTF("[G2] Bluetooth: Disconnect Ring via cmd_exec");
  if (!g2SubmitHijackCommand("closering", cookie,
                             onBluetoothR1MenuRefreshDone, nullptr)) {
    DEBUG_G2F("[G2] R1 disconnect submit FAILED — no inline mutate");
  }
}

#if ENABLE_BLE_PHONE_PIPE
static void handleBluetoothPhoneTap(uint32_t idx) {
  // Rows as showBluetoothPhoneMenu bound them:
  //   0=Back 1=Pipe 2=Paired 3=Console 4=AutoReconnect 5=Open/Close Phone.
  NetRowBinding row;
  if (!netRowAt(NET_SUB_BLUETOOTH_PHONE, idx, &row)) return;
  const char* line = nullptr;
  switch ((PhoneRowAction)row.action) {
    case PHONE_ROW_BACK:           showBluetoothMenu(); return;
    case PHONE_ROW_AUTO_RECONNECT: line = row.on ? "bleautoreconnect phone on"
                                                 : "bleautoreconnect phone off"; break;
    case PHONE_ROW_PIPE:           line = row.on ? "openphone" : "closephone"; break;
    default:
      DEBUG_G2F("[G2] Bluetooth → Phone: info row %u (no action)", (unsigned)idx);
      return;
  }
  // cmd_exec as the glasses' owner (admin-gated like the G2/R1 rows).
  G2CmdCookie cookie{};
  cookie.targetPage   = g2GetHijackPage();
  cookie.targetNetSub = (uint8_t)gNetSub;
  if (!g2SubmitHijackCommand(line, cookie, onBluetoothPhoneMenuRefreshDone, nullptr)) {
    DEBUG_G2F("[G2] Phone '%s' submit FAILED — no inline mutate", line);
  }
}
#endif

static void handleBluetoothTap(uint32_t idx) {
  // Rows as showBluetoothMenu bound them — restructured 2026-05-09
  // to promote Mode (now the Role row) out of the G2 submenu and consolidate
  // ring controls under a new R1 >> drill-in:
  //   active + server:           0=Back 1=BLE 2=Role 3=Conn 4=ToggleAdv 5=Disconnect 6=Auto
  //   active + client:           0=Back 1=BLE 2=Role 3=G2>> 4=R1>> 5=Auto
  //                              (phone pipe builds: 5=Phone>> 6=Auto)
  //   inactive + server:         0=Back 1=BLE 2=Role 3=Auto
  //   inactive + client:         0=Back 1=BLE 2=Role 3=G2>> 4=R1>> 5=Auto
  //   A "Saved: ..." info row follows Role when the saved mode differs (always
  //   while inactive), shifting the rest down by one.
  NetRowBinding row;
  if (!netRowAt(NET_SUB_BLUETOOTH, idx, &row)) return;
  const BluetoothRowAction action = (BluetoothRowAction)row.action;
  if (action == BT_ROW_BACK) { g2ShowNetworkMenu(); return; }

  if (action == BT_ROW_CLIENT_POWER || action == BT_ROW_SERVER_POWER) {
    // BLE row — starts or stops the role it showed, via cmd_exec:
    //   client + on  → openg2      client + off → closeg2 (yanks the lens out from under the user)
    //   server + on  → openconsole server + off → closeconsole
    // Client+off is destructive to the lens redraw — the connection
    // we'd repaint on goes down. The completion callback will find the
    // lens disconnected and the redraw will skip silently in
    // g2EnqueueLensJob, which is fine.
    const bool client = (action == BT_ROW_CLIENT_POWER);
    const char* line;
    if (row.on) line = client ? "openg2"  : "openconsole";
    else        line = client ? "closeg2" : "closeconsole";

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

  if (action == BT_ROW_MODE) {  // Role row: boot role toggle (server ↔ client) — via cmd_exec.
    // `blemode <other>` stores the boot role only; nothing running changes
    // (see cmd_blemode in Bluetooth.cpp), so the lens hijack survives it. The
    // server switch itself is `openconsole` on the server power row, which
    // does drop the link we render through. "Other" is the role the row
    // showed (on = to client).
    const char* arg = row.on ? "client" : "server";
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

  switch (action) {
    case BT_ROW_G2:  showBluetoothG2Menu(); break;
    case BT_ROW_R1:  showBluetoothR1Menu(); break;
#if ENABLE_BLE_PHONE_PIPE
    case BT_ROW_PHONE: showBluetoothPhoneMenu(); break;
#endif

    case BT_ROW_INFO:  // Console (server layout) or Saved info row
      DEBUG_G2F("[G2] Bluetooth: info row %u (no action)", (unsigned)idx);
      break;

    case BT_ROW_TOGGLE_ADV: {  // Toggle Adv — routed via cmd_exec (`bleadv toggle` was
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

    case BT_ROW_DISCONNECT: {  // Disconnect — routed via cmd_exec (`bledisconnect`).
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

    case BT_ROW_AUTO_START:  // every layout's last row
      bluetoothToggleAutoStart(row.on);
      break;

    default:
      DEBUG_G2F("[G2] Bluetooth: unknown action=%u at idx=%u",
                (unsigned)action, (unsigned)idx);
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
  EXT_RAM_BSS_ATTR static char serverLine[40];  // PSRAM: deep-copied by g2ShowListPage
  EXT_RAM_BSS_ATTR static char httpsLine[32];  // PSRAM: deep-copied by g2ShowListPage
  EXT_RAM_BSS_ATTR static char autoLine[32];  // PSRAM: deep-copied by g2ShowListPage
  EXT_RAM_BSS_ATTR static char ipLine[40];  // PSRAM: deep-copied by g2ShowListPage

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
  // handleHttpTap dispatches from the bindings made here.
  NetRowList rows;
  rows.add("<- WiFi", HTTP_ROW_BACK);  // HTTP(S) is nested under WiFi
#if ENABLE_HTTP_SERVER
  rows.add(serverLine, HTTP_ROW_SERVER, /*on=*/!running);  // start/stop as labelled
  rows.add(httpsLine, HTTP_ROW_HTTPS, /*on=*/!gSettings.httpsEnabled);
  rows.add(autoLine, HTTP_ROW_AUTO_START, /*on=*/!gSettings.httpAutoStart);
  if (ipLine[0]) {
    rows.add(ipLine, HTTP_ROW_INFO);  // URL (when running)
  }
#else
  rows.add(serverLine, HTTP_ROW_INFO);
#endif

  showNetRows(NET_SUB_HTTP, rows);
  DEBUG_G2F("[G2] HTTP submenu shown (running=%d, https=%d, auto=%d)",
            isHttpServerRunning() ? 1 : 0,
            gSettings.httpsEnabled ? 1 : 0,
            gSettings.httpAutoStart ? 1 : 0);
}

// Completion callback for any cmd_exec submission inside the HTTP submenu.
// Re-renders the same submenu via the lens-applier Redraw path so the new
// state (running flag, label change) shows up as soon as cmd_exec finishes.
static void onHttpMenuRefreshDone(bool /*ok*/, const char* /*result*/,
                                  const G2CmdCookie& cookie, void* /*userData*/) {
  (void)g2EnqueueRedraw(cookie, &showHttpMenu, "HTTP-menu refresh");
}

static void handleHttpTap(uint32_t idx) {
  NetRowBinding row;
  if (!netRowAt(NET_SUB_HTTP, idx, &row)) return;
  // Back goes to the WiFi submenu now — HTTP(S) is nested under WiFi
  // since the server can only run with a WiFi connection.
  if (row.action == HTTP_ROW_BACK) { showWiFiMenu(); return; }

#if ENABLE_HTTP_SERVER
  G2CmdCookie cookie{};
  cookie.targetPage   = g2GetHijackPage();
  cookie.targetNetSub = (uint8_t)gNetSub;

  switch ((HttpRowAction)row.action) {
    case HTTP_ROW_SERVER: {  // Start/Stop server, as the row was labelled
      const char* line = row.on ? "openhttp" : "closehttp";
      DEBUG_G2F("[G2] HTTP toggle: %s via cmd_exec", line);
      if (!g2SubmitHijackCommand(line, cookie, onHttpMenuRefreshDone, nullptr)) {
        DEBUG_G2F("[G2] HTTP toggle: %s submit FAILED — no inline mutate", line);
      }
      break;
    }

    case HTTP_ROW_HTTPS: {  // HTTPS mode toggle (reboot required to take effect)
      char line[40];
      snprintf(line, sizeof(line), "httpsEnabled %d", row.on ? 1 : 0);
      if (g2SubmitHijackCommand(line, cookie, onHttpMenuRefreshDone, nullptr)) {
        BROADCAST_PRINTF("[G2] HTTPS mode toggle %s→%s submitted (reboot to apply)",
                         row.on ? "OFF" : "ON", row.on ? "ON" : "OFF");
      } else {
        DEBUG_G2F("[G2] HTTPS mode submit FAILED — no inline mutate");
      }
      break;
    }

    case HTTP_ROW_AUTO_START: {  // HTTP Auto Start toggle (boot-time start)
      char line[40];
      snprintf(line, sizeof(line), "httpAutoStart %d", row.on ? 1 : 0);
      if (g2SubmitHijackCommand(line, cookie, onHttpMenuRefreshDone, nullptr)) {
        BROADCAST_PRINTF("[G2] HTTP Auto Start toggle %s→%s submitted",
                         row.on ? "OFF" : "ON", row.on ? "ON" : "OFF");
      } else {
        DEBUG_G2F("[G2] HTTP Auto Start submit FAILED — no inline mutate");
      }
      break;
    }

    case HTTP_ROW_INFO:  // URL row (info only)
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
#if ENABLE_BLE_PHONE_PIPE
    case NET_SUB_BLUETOOTH_PHONE: handleBluetoothPhoneTap(idx); break;
#else
    case NET_SUB_BLUETOOTH_PHONE: break;  // no Phone menu without the pipe
#endif
    case NET_SUB_HTTP:        handleHttpTap(idx);        break;
  }
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
