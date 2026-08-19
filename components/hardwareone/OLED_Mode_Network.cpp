// OLED_Mode_Network.cpp — Network / Mesh / Web / ESP-NOW / Remote-Sensors modes
//
// This file registers the following OLED modes:
//   OLED_NETWORK_INFO       — Network main menu (scrollable, Power-style)
//   OLED_NETWORK_STATUS     — WiFi status detail (pushed sub-mode, read-only)
//   OLED_NETWORK_WIFI_MENU  — WiFi management (pushed sub-mode, owns Add-WiFi keyboard flow)
//   OLED_MESH_STATUS        — Mesh role + peer count (read-only)
//   OLED_WEB_STATS          — HTTP server stats (read-only)
//   OLED_ESPNOW             — ESP-NOW peer/state UI (delegates to OLED_ESPNow)
//   OLED_REMOTE_SENSORS     — Bonded device sensor data (scrollable)
//
// Standards followed (matches OLED_Mode_Power.cpp / OLED_Mode_SetPattern.cpp):
//   • Every scrollable list uses OLEDScrollState — no hand-rolled selection ints,
//     no "---" placeholder hacks, no render-time state mutation.
//   • Sub-screens are real OLED enum values entered via requestOLEDMode(); B back
//     pops via the global mode stack. No gShowing* flag soup.
//   • Keyboard flow follows the codebase-wide overlay pattern: input is intercepted
//     centrally by processOLEDInput() when oledKeyboardIsActive(); each frame the
//     mode polls oledKeyboardIsCompleted()/IsCancelled() to advance its state
//     machine. The active mode's display func short-circuits to oledKeyboardDisplay
//     while the keyboard is up (the "defensive curtain").

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include "OLED_Utils.h"
#include "HAL_Input.h"
#include "System_MemUtil.h"
#include "System_Settings.h"
#include "System_Utils.h"
#include "System_User.h"
#include "i2csensor_seesaw.h"      // For JOYSTICK_DEADZONE
#include "i2csensor_ano_encoder.h" // ANO_BTN_*/ANO_AXIS_* bit defines (live outside the
                                   // ENABLE_ANO_ENCODER guard) — the master decodes a
                                   // remote peer's ANO input regardless of its own input HW
#include <math.h>                  // cosf/sinf for the remote ANO rotary dial
#include "System_I2C.h"            // OLED_TRANSACTION (i2cDeviceTransactionVoid)
#include "System_WiFi.h"           // gWifiNetworks / gWifiNetworkCount / WifiNetwork /
                                   // ensureWiFiInitialized

#if ENABLE_WIFI
#include <WiFi.h>
#endif

#if ENABLE_ESPNOW
#include "System_ESPNow.h"
#include <esp_wifi.h>
#endif

// External references
#if ENABLE_HTTP_SERVER
extern bool gServerIsHttps;  // Defined in WebServer_Server.cpp
#endif

#if ENABLE_ESPNOW
// gMeshPeers, gMeshPeerSlots declared in System_ESPNow.h (pointer, not array)
extern String macToHexString(const uint8_t* mac);
extern void macFromHexString(const String& s, uint8_t out[6]);
extern bool isSelfMac(const uint8_t* mac);
extern bool isMeshPeerAlive(const MeshPeerHealth* peer);
extern bool meshEnabled();
extern String getEspNowDeviceName(const uint8_t* mac);
#endif

// ============================================================================
// Two-phase network render data
// ============================================================================
// Gathered in prepareNetworkData() OUTSIDE the I2C transaction (so WiFi API
// calls don't block the gamepad), then read by the OLED_NETWORK_INFO main
// menu and the OLED_NETWORK_STATUS detail screen.

struct NetworkRenderData {
  bool wifiConnected;  // CONNECTION axis: associated to an AP
  bool radioOn;        // RADIO axis: powered up at all (WiFi or ESP-NOW)
  char ssid[16];
  char ip[16];
  int rssi;
  bool valid;
};
static NetworkRenderData networkRenderData = {};

void prepareNetworkData() {
#if ENABLE_WIFI
  networkRenderData.wifiConnected = WiFi.isConnected();
  networkRenderData.radioOn = wifiRadioOn();

  if (networkRenderData.wifiConnected) {
    String ssid = WiFi.SSID();
    if (ssid.length() > 15) ssid = ssid.substring(0, 15);
    strncpy(networkRenderData.ssid, ssid.c_str(), 15);
    networkRenderData.ssid[15] = '\0';

    String ip = WiFi.localIP().toString();
    strncpy(networkRenderData.ip, ip.c_str(), 15);
    networkRenderData.ip[15] = '\0';

    networkRenderData.rssi = WiFi.RSSI();
  }
  networkRenderData.valid = true;
#else
  networkRenderData.wifiConnected = false;
  networkRenderData.radioOn = false;
  networkRenderData.valid = true;
#endif
}

// ============================================================================
// OLED_NETWORK_INFO — Network main menu (Power-style OLEDScrollState)
// ============================================================================
// One static OLEDScrollState owns selection + items + scroll. Items rebuild
// every frame from current WiFi/HTTP state. Items that don't apply are simply
// not added — no "---" placeholders, no skip-disabled cursor dance. Each item
// carries a NetworkMainAction in a parallel array so the input handler can
// dispatch without re-parsing strings.

enum NetworkMainAction {
  NET_ACT_VIEW_STATUS,
  NET_ACT_CONNECT_BEST,
  NET_ACT_WIFI_MENU,
  NET_ACT_DISCONNECT,
  NET_ACT_TOGGLE_HTTP,
};

EXT_RAM_BSS_ATTR static OLEDScrollState sNetworkMainScroll;
EXT_RAM_BSS_ATTR static NetworkMainAction sNetworkMainActions[OLED_SCROLL_MAX_ITEMS];
static bool sNetworkMainInitialized = false;

static void initNetworkMainScroll() {
  if (sNetworkMainInitialized) return;
  oledScrollInit(&sNetworkMainScroll, nullptr, 4);
  sNetworkMainInitialized = true;
}

static void populateNetworkMainMenu() {
  initNetworkMainScroll();
  oledScrollClearKeepSelection(&sNetworkMainScroll);

  bool wifiConnected = networkRenderData.wifiConnected;

  auto addAction = [&](const char* label, NetworkMainAction act) {
    if (oledScrollAddItem(&sNetworkMainScroll, label)) {
      int idx = sNetworkMainScroll.itemCount - 1;
      if (idx >= 0 && idx < (int)(sizeof(sNetworkMainActions) / sizeof(sNetworkMainActions[0]))) {
        sNetworkMainActions[idx] = act;
      }
    }
  };

  addAction("View Status", NET_ACT_VIEW_STATUS);
  if (!wifiConnected) {
    addAction("Connect", NET_ACT_CONNECT_BEST);
  }
  addAction("WiFi Management", NET_ACT_WIFI_MENU);
  if (wifiConnected) {
    addAction("Disconnect", NET_ACT_DISCONNECT);
  }
#if ENABLE_HTTP_SERVER
  {
    bool httpRunning = (server != nullptr);
    const char* httpLabel;
    if (httpRunning) {
      httpLabel = gServerIsHttps ? "Close HTTPS" : "Close HTTP";
    } else {
      httpLabel = "Open HTTP";
    }
    addAction(httpLabel, NET_ACT_TOGGLE_HTTP);
  }
#endif

  // Cursor preserved across the rebuild; clamp in case an item disappeared
  // (e.g. "Connect" vanishing once we're connected).
  oledScrollClampSelection(&sNetworkMainScroll);
}

// Render network main menu from pre-gathered data (called INSIDE I2C transaction)
void displayNetworkInfoRendered() {
  if (!oledDisplay || !oledConnected) return;

  if (!networkRenderData.valid) {
    oledDisplay->setTextSize(1);
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
    oledDisplay->println("Network data");
    oledDisplay->println("unavailable");
    return;
  }

  populateNetworkMainMenu();

  oledDisplay->setTextSize(1);
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);

#if ENABLE_WIFI
  // Status banner row — distinguishes radio power from network connection
  // (the old "(off)" meant merely disconnected, which read as radio-off).
  if (networkRenderData.wifiConnected) {
    oledDisplay->print(networkRenderData.rssi);
    oledDisplay->println("dBm");
  } else if (networkRenderData.radioOn) {
    oledDisplay->println("No WiFi");   // radio up (e.g. ESP-NOW), no network
  } else {
    oledDisplay->println("Radio off");
  }

  // Power-style decorated rendering: read items from the scroll state.
  // OLEDScrollState owns selection; we only paint.
  for (int i = 0; i < sNetworkMainScroll.itemCount; i++) {
    oledDisplay->print(i == sNetworkMainScroll.selectedIndex ? "> " : "  ");
    oledDisplay->print(sNetworkMainScroll.items[i].line1);
#if ENABLE_HTTP_SERVER
    // Tiny suffix on the HTTP row showing protocol while running.
    if (sNetworkMainActions[i] == NET_ACT_TOGGLE_HTTP) {
      if (server != nullptr) {
        oledDisplay->print(gServerIsHttps ? " [S]" : " *");
      }
    }
#endif
    oledDisplay->println();
  }
#else
  oledDisplay->println("WiFi: Disabled");
  oledDisplay->println();
  oledDisplay->println("Compile with");
  oledDisplay->println("ENABLE_WIFI=1");
#endif
}

// Confirmation callbacks for HTTP Start/Stop
static void httpStartConfirmedNetwork(void* userData) {
  (void)userData;
  executeOLEDCommand("openhttp");
}

static void httpStopConfirmedNetwork(void* userData) {
  (void)userData;
  executeOLEDCommand("closehttp");
}

static bool networkMainInputHandler(int /*deltaX*/, int /*deltaY*/, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;
  initNetworkMainScroll();
  if (oledScrollHandleNav(&sNetworkMainScroll)) return true;

  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    int idx = sNetworkMainScroll.selectedIndex;
    if (idx < 0 || idx >= sNetworkMainScroll.itemCount) return true;
    switch (sNetworkMainActions[idx]) {
      case NET_ACT_VIEW_STATUS:
        requestOLEDMode(OLED_NETWORK_STATUS, "network.status");
        break;
      case NET_ACT_CONNECT_BEST:
        executeOLEDCommand("openwifi --best");
        break;
      case NET_ACT_WIFI_MENU:
        requestOLEDMode(OLED_NETWORK_WIFI_MENU, "network.wifi.menu");
        break;
      case NET_ACT_DISCONNECT:
        // Drop the AP but keep the radio (and HTTP) up — HTTP has its own
        // toggle in this menu, so Disconnect should only leave the network.
        executeOLEDCommand("wifidisconnect");
        break;
      case NET_ACT_TOGGLE_HTTP:
#if ENABLE_HTTP_SERVER
        {
          if (server != nullptr) {
            oledConfirmRequest("Stop HTTP?", nullptr, httpStopConfirmedNetwork, nullptr, false);
          } else {
            oledConfirmRequest("Start HTTP?", nullptr, httpStartConfirmedNetwork, nullptr);
          }
        }
#endif
        break;
    }
    return true;
  }

  // B: return false so the global handler calls oledMenuBack() (Power pattern).
  return false;
}

// ============================================================================
// OLED_NETWORK_STATUS — WiFi status detail (pushed sub-mode, read-only)
// ============================================================================

static void displayNetworkStatusRendered() {
  if (!oledDisplay || !oledConnected) return;

  oledDisplay->setTextSize(1);
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);

  if (!networkRenderData.valid) {
    oledDisplay->println("Status data");
    oledDisplay->println("unavailable");
    return;
  }

#if ENABLE_WIFI
  // Two separate axes: RADIO power, then WiFi CONNECTION.
  oledDisplay->print("Radio: ");
  oledDisplay->println(networkRenderData.radioOn ? "ON" : "OFF");
  if (networkRenderData.wifiConnected) {
    oledDisplay->print("SSID: ");
    oledDisplay->println(networkRenderData.ssid);
    oledDisplay->print("IP: ");
    oledDisplay->println(networkRenderData.ip);
    oledDisplay->print("RSSI: ");
    oledDisplay->print(networkRenderData.rssi);
    oledDisplay->println(" dBm");
  } else {
    oledDisplay->println("WiFi: Disconnected");
  }
#else
  oledDisplay->println("WiFi: Disabled");
#endif
}

// No input handler — B falls through to global oledMenuBack() pop.

// ============================================================================
// OLED_NETWORK_WIFI_MENU — WiFi management (pushed sub-mode, scrollable)
// ============================================================================
// "Add Network" launches a multi-step keyboard flow lived inside this same
// mode, mirroring OLED_Mode_SetPattern.cpp: one step enum + one sKeyboardActive
// bool. The central input pump owns input while the keyboard is up; we just
// poll completion per frame to advance SSID → PASS → submit.

enum WifiAddStep {
  WIFI_ADD_NONE,
  WIFI_ADD_SSID,
  WIFI_ADD_PASS,
};

EXT_RAM_BSS_ATTR static OLEDScrollState sWifiMenuScroll;
static bool sWifiMenuInitialized = false;
static WifiAddStep sWifiAddStep = WIFI_ADD_NONE;
static bool sWifiKeyboardActive = false;
static bool sWifiAddConnectAfter = false;   // true when password flow began from a scan pick
static String sWifiAddSSID = "";
static String sWifiAddPass = "";
static TransportSessionEpoch sWifiAddEpoch = kNoTransportSessionEpoch;

// Saved-networks picker (shared by the List + Remove sub-modes) and scan-results
// picker. Both are plain OLEDScrollState lists rendered the SAME way as this menu
// and the Power submenus (oledScrollRenderSimple) — no separate popup mechanism.
EXT_RAM_BSS_ATTR static OLEDScrollState sSavedNetScroll;
static bool sSavedNetInitialized = false;
EXT_RAM_BSS_ATTR static OLEDScrollState sWifiScanScroll;
static bool sWifiScanInitialized = false;

// Scan results: bare SSID (for connect/add) + display label with signal bars.
// Kept in stable storage because OLEDScrollState items store pointers, not copies.
EXT_RAM_BSS_ATTR static char sWifiScanSSIDs[16][33];
EXT_RAM_BSS_ATTR static char sWifiScanLabels[16][40];
EXT_RAM_BSS_ATTR static char sWifiScanFailure[40];
static int  sWifiScanCount = 0;

// Pending removal SSID + confirm-prompt buffer. oledConfirmRequest stores the
// prompt as a const char* pointer (it does NOT copy), so the buffer must persist.
static String sPendingRemoveSSID = "";
static char   sRemovePromptBuf[40];

static void populateWifiMenu() {
  if (!sWifiMenuInitialized) {
    // Single-line list: ~8px per item, so the whole content area's worth of
    // items shows at once (the global header already labels this "WiFi", so no
    // in-list title). visibleLines drives scroll math only — all 5 items fit.
    oledScrollInit(&sWifiMenuScroll, nullptr, OLED_CONTENT_HEIGHT / 8);
    sWifiMenuInitialized = true;
  }
  oledScrollClearKeepSelection(&sWifiMenuScroll);
  oledScrollAddItem(&sWifiMenuScroll, "List Networks");
  oledScrollAddItem(&sWifiMenuScroll, "Add Network");
  oledScrollAddItem(&sWifiMenuScroll, "Remove Network");
  oledScrollAddItem(&sWifiMenuScroll, "Connect Best");
  oledScrollAddItem(&sWifiMenuScroll, "Scan Networks");
  oledScrollClampSelection(&sWifiMenuScroll);
}

static void displayNetworkWifiMenuRendered() {
  if (!oledDisplay || !oledConnected) return;

  // Defensive curtain: while keyboard is active, draw it ourselves.
  // (Matches OLED_Mode_Auth.cpp:50, OLED_Mode_SetPattern.cpp:88, OLED_ESPNow.cpp:506.)
  if (oledKeyboardDrawIfActive(oledDisplay)) return;

  populateWifiMenu();
  // Single-line list renderer — shows every option at once, matching the
  // Network main menu (NOT oledScrollRender, which is the 16px two-line/
  // split-pane renderer that only fit one item here).
  oledScrollRenderSimple(oledDisplay, &sWifiMenuScroll);
}

static void startWifiAddSSID() {
  String sessionUser;
  bool sessionAuthed = false;
  sWifiAddEpoch = localDisplayTransportSessionSnapshot(sessionUser, sessionAuthed);
  secureClearString(sessionUser);
  (void)sessionAuthed;  // AuthBypass also owns a real policy-scoped epoch.
  sWifiAddStep = WIFI_ADD_SSID;
  sWifiAddConnectAfter = false;  // manual Add = save only (unchanged behavior)
  secureClearString(sWifiAddSSID);
  secureClearString(sWifiAddPass);
  oledKeyboardInit("Enter SSID:", "");
  sWifiKeyboardActive = true;
}

static void startWifiAddPass() {
  sWifiAddStep = WIFI_ADD_PASS;
  oledKeyboardInit("Enter Password:", "");
  sWifiKeyboardActive = true;
}

static void wifiAddReset() {
  sWifiAddStep = WIFI_ADD_NONE;
  sWifiKeyboardActive = false;
  sWifiAddConnectAfter = false;
  secureClearString(sWifiAddSSID);
  secureClearString(sWifiAddPass);
  sWifiAddEpoch = kNoTransportSessionEpoch;
  oledKeyboardReset();
}

void oledNetworkModeResetSessionState() {
  wifiAddReset();
  secureClearString(sPendingRemoveSSID);
  memset(sRemovePromptBuf, 0, sizeof(sRemovePromptBuf));
}

// Pre-fill the SSID from a scan pick and jump straight to password entry.
static void startWifiAddFromScan(const char* ssid) {
  String sessionUser;
  bool sessionAuthed = false;
  sWifiAddEpoch = localDisplayTransportSessionSnapshot(sessionUser, sessionAuthed);
  secureClearString(sessionUser);
  (void)sessionAuthed;  // AuthBypass also owns a real policy-scoped epoch.
  sWifiAddStep = WIFI_ADD_PASS;
  sWifiAddConnectAfter = true;   // scan pick = save AND connect
  sWifiAddSSID = String(ssid);
  secureClearString(sWifiAddPass);
  oledKeyboardInit("Enter Password:", "");
  sWifiKeyboardActive = true;
}

// Drive the Add-WiFi keyboard state machine. Shared by manual "Add Network" and
// the scan-pick flow. Returns true if the keyboard flow is active (caller should
// `return *handled`); false if no flow is active (caller continues normally).
static bool wifiAddKeyboardTick(bool* handled) {
  if (!sWifiKeyboardActive) return false;
  *handled = false;
  if (oledKeyboardIsCompleted()) {
    const TransportSessionEpoch sessionEpoch = sWifiAddEpoch;
    if (!transportSessionEpochIsLive(
            SOURCE_LOCAL_DISPLAY, sessionEpoch)) {
      wifiAddReset();
      *handled = true;
      return true;
    }
    String text = String(oledKeyboardGetText());
    oledKeyboardReset();
    sWifiKeyboardActive = false;
    if (sWifiAddStep == WIFI_ADD_SSID) {
      sWifiAddSSID = text;
      startWifiAddPass();
    } else if (sWifiAddStep == WIFI_ADD_PASS) {
      sWifiAddPass = text;
      String command = "wifiadd \"" + sWifiAddSSID + "\" \"" + sWifiAddPass + "\"";
      const bool connectAfter = sWifiAddConnectAfter;
      // Expected-epoch admission/result fencing below fails closed if a
      // replacement arrives during FS/network I/O.
      char result[128] = {};
      const bool added = executeOLEDCommandWithResultForSession(
          command, sessionEpoch, result, sizeof(result));
      secureClearString(command);
      if (added && connectAfter) {
        (void)executeOLEDCommandWithResultForSession(
            "openwifi --best", sessionEpoch, result, sizeof(result));
      }
      volatile char* wipe = reinterpret_cast<volatile char*>(result);
      for (size_t i = 0; i < sizeof(result); ++i) wipe[i] = '\0';
      secureClearString(text);
      if (transportSessionEpochIsLive(
              SOURCE_LOCAL_DISPLAY, sessionEpoch)) {
        wifiAddReset();
      }
    } else {
      secureClearString(text);
      wifiAddReset();
    }
    *handled = true;
  } else if (oledKeyboardIsCancelled()) {
    wifiAddReset();
    *handled = true;
  }
  return true;
}

// ============================================================================
// OLED_NETWORK_WIFI_LIST / OLED_NETWORK_WIFI_REMOVE — saved-network pickers
// ============================================================================
// Plain OLEDScrollState lists, pushed via requestOLEDMode and popped by the
// global B handler — identical mechanism to the WiFi menu and Power submenus.
// Both render the same saved-network list (shared display); they differ only in
// the A action: List connects, Remove confirms+deletes via oledConfirmRequest.

static void populateSavedNetScroll() {
  if (!sSavedNetInitialized) {
    oledScrollInit(&sSavedNetScroll, nullptr, OLED_CONTENT_HEIGHT / 8);
    sSavedNetInitialized = true;
  }
  oledScrollClearKeepSelection(&sSavedNetScroll);
  for (int i = 0; i < gWifiNetworkCount && i < OLED_SCROLL_MAX_ITEMS; i++) {
    oledScrollAddItem(&sSavedNetScroll, gWifiNetworks[i].ssid.c_str());
  }
  oledScrollClampSelection(&sSavedNetScroll);
}

// Shared display for List + Remove (the header names which one).
static void displaySavedNetworksRendered() {
  if (!oledDisplay || !oledConnected) return;
  populateSavedNetScroll();
  if (sSavedNetScroll.itemCount == 0) {
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
    oledDisplay->println("No saved networks");
    return;
  }
  oledScrollRenderSimple(oledDisplay, &sSavedNetScroll);
}

static bool networkWifiListInputHandler(int /*dx*/, int /*dy*/, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;
  if (oledScrollHandleNav(&sSavedNetScroll)) return true;
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    int sel = sSavedNetScroll.selectedIndex;
    if (sel >= 0 && sel < gWifiNetworkCount) {
      // Registered command is `openwifi`; `wificonnect` is only the handler's
      // C function name (cmd_wificonnect) and never resolved here.
      executeOLEDCommand("openwifi --index " + String(sel + 1));
      oledMenuBack();  // pop back to the WiFi menu after kicking off the connect
    }
    return true;
  }
  return false;  // B → global pop
}

static void onWifiRemoveConfirmed(void* /*ud*/) {
  if (sPendingRemoveSSID.length() > 0) {
    executeOLEDCommand("wifirm \"" + sPendingRemoveSSID + "\"");
    sPendingRemoveSSID = "";
  }
}

static bool networkWifiRemoveInputHandler(int /*dx*/, int /*dy*/, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;
  if (oledScrollHandleNav(&sSavedNetScroll)) return true;
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    int sel = sSavedNetScroll.selectedIndex;
    if (sel >= 0 && sel < gWifiNetworkCount) {
      sPendingRemoveSSID = gWifiNetworks[sel].ssid;
      snprintf(sRemovePromptBuf, sizeof(sRemovePromptBuf), "Delete %s?", sPendingRemoveSSID.c_str());
      oledConfirmRequest(sRemovePromptBuf, nullptr, onWifiRemoveConfirmed, nullptr, /*defaultYes=*/false);
    }
    return true;
  }
  return false;  // B → global pop
}

// ============================================================================
// OLED_NETWORK_WIFI_SCAN — nearby-AP picker (pushed sub-mode)
// ============================================================================

static void populateWifiScanScroll() {
  if (!sWifiScanInitialized) {
    oledScrollInit(&sWifiScanScroll, nullptr, OLED_CONTENT_HEIGHT / 8);
    sWifiScanInitialized = true;
  }
  oledScrollClearKeepSelection(&sWifiScanScroll);
  for (int i = 0; i < sWifiScanCount; i++) {
    oledScrollAddItem(&sWifiScanScroll, sWifiScanLabels[i]);  // "SSID +++"
  }
  oledScrollClampSelection(&sWifiScanScroll);
}

static void displayNetworkWifiScanRendered() {
  if (!oledDisplay || !oledConnected) return;
  // Defensive curtain for the password keyboard (after picking a network).
  if (oledKeyboardDrawIfActive(oledDisplay)) return;
  populateWifiScanScroll();
  if (sWifiScanScroll.itemCount == 0) {
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
    oledDisplay->println(sWifiScanFailure[0]
                             ? sWifiScanFailure
                             : "No networks found");
    return;
  }
  oledScrollRenderSimple(oledDisplay, &sWifiScanScroll);
}

static bool networkWifiScanInputHandler(int /*dx*/, int /*dy*/, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;
  // Password keyboard flow (shared with manual Add) runs first.
  bool kbHandled;
  if (wifiAddKeyboardTick(&kbHandled)) return kbHandled;

  if (oledScrollHandleNav(&sWifiScanScroll)) return true;
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    int sel = sWifiScanScroll.selectedIndex;
    if (sel >= 0 && sel < sWifiScanCount) {
      startWifiAddFromScan(sWifiScanSSIDs[sel]);  // pre-fill SSID → password
    }
    return true;
  }
  return false;  // B → global pop
}

#if ENABLE_WIFI
static bool cacheOledWifiScanRecord(const WifiScanRecord& record,
                                    uint16_t /*index*/, uint16_t /*total*/,
                                    void* /*context*/) {
  if (record.ssid[0] == '\0') return true;  // named picker skips hidden APs
  if (sWifiScanCount >= 16) return false;

  memcpy(sWifiScanSSIDs[sWifiScanCount], record.ssid,
         sizeof(sWifiScanSSIDs[sWifiScanCount]));
  sWifiScanSSIDs[sWifiScanCount][32] = '\0';
  const char* bars = (record.rssi > -50) ? " +++"
                    : (record.rssi > -70) ? " ++" : " +";
  snprintf(sWifiScanLabels[sWifiScanCount],
           sizeof(sWifiScanLabels[sWifiScanCount]), "%.32s%s",
           sWifiScanSSIDs[sWifiScanCount], bars);
  ++sWifiScanCount;
  return sWifiScanCount < 16;
}
#endif

// Blocking WiFi scan, then push the scan sub-mode showing the results.
static void startWifiScan() {
  // Immediate "Scanning..." feedback before the blocking scan (mirrors the
  // first-time-setup wizard's getOLEDWiFiSelection()).
  if (oledDisplay) {
    OLED_TRANSACTION(
      oledDisplay->fillRect(0, 0, oledDisplay->width(), oledDisplay->height(), DISPLAY_COLOR_BLACK);
      oledDisplay->setTextSize(1);
      oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
      oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
      oledDisplay->print("Scanning WiFi...");
      oledDisplay->display();
    );
  }

  sWifiScanCount = 0;
  sWifiScanFailure[0] = '\0';
#if ENABLE_WIFI
  if (!ensureWiFiInitialized()) {
    snprintf(sWifiScanFailure, sizeof(sWifiScanFailure),
             "WiFi unavailable");
  } else {
    const WifiScanResult result = wifiScanForEach(
        /*includeHidden=*/true, cacheOledWifiScanRecord, nullptr,
        /*acquireTimeoutMs=*/0);
    if (!result.ok()) {
      // Do not expose a prefix of a failed driver walk as a selectable list.
      sWifiScanCount = 0;
      snprintf(sWifiScanFailure, sizeof(sWifiScanFailure),
               "Scan %s; try again", wifiScanStatusText(result.status));
      DEBUG_WIFIF("OLED WiFi scan failed (%s, driver=%ld)",
                  wifiScanStatusText(result.status),
                  (long)result.driverError);
    }
  }
#endif

  sWifiScanScroll.selectedIndex = 0;
  sWifiScanScroll.scrollOffset = 0;
  requestOLEDMode(OLED_NETWORK_WIFI_SCAN, "network.wifi.scan");
}

static bool networkWifiMenuInputHandler(int /*deltaX*/, int /*deltaY*/, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;
  // --- Multi-step Add-WiFi keyboard flow (shared with scan-pick) ---
  bool kbHandled;
  if (wifiAddKeyboardTick(&kbHandled)) return kbHandled;

  // --- Menu navigation ---
  if (oledScrollHandleNav(&sWifiMenuScroll)) return true;

  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    switch (sWifiMenuScroll.selectedIndex) {
      case 0: // List Networks — push the saved-network picker (A connects)
        sSavedNetScroll.selectedIndex = 0;
        sSavedNetScroll.scrollOffset = 0;
        requestOLEDMode(OLED_NETWORK_WIFI_LIST, "network.wifi.list");
        break;
      case 1: // Add Network — manual SSID → PASS keyboard flow
        startWifiAddSSID();
        break;
      case 2: // Remove Network — push the saved-network picker (A confirms+deletes)
        sSavedNetScroll.selectedIndex = 0;
        sSavedNetScroll.scrollOffset = 0;
        requestOLEDMode(OLED_NETWORK_WIFI_REMOVE, "network.wifi.remove");
        break;
      case 3: // Connect Best
        executeOLEDCommand("openwifi --best");
        oledMenuBack();  // close this submenu after kicking off the connect
        break;
      case 4: // Scan Networks — scan, then push the scan picker (A adds + connects)
        startWifiScan();
        break;
    }
    return true;
  }

  // B: return false so the global handler pops the mode stack.
  return false;
}

// ============================================================================
// OLED_ESPNOW — delegates to OLED_ESPNow module
// ============================================================================

extern void enterUnavailablePage(const String& title, const String& reason);
extern void oledEspNowDisplay(Adafruit_SSD1306* display);
extern void oledEspNowShowInitPrompt();
extern void oledEspNowInit();

// Entry hook for OLED_ESPNOW. Previously duplicated in cmd_oledmode AND the
// menu-select path (and skipped entirely on back-nav). Now owned here: on a
// fresh visit, show the init prompt if ESP-NOW isn't up yet, else (re)build the
// device-list view. Gated on isForward so backing into the mode preserves state.
static void espnowOnEnter(bool isForward) {
#if ENABLE_ESPNOW
  if (!isForward) return;
  if (!gEspNow || !gEspNow->initialized) {
    oledEspNowShowInitPrompt();
  } else {
    oledEspNowInit();
  }
#endif
}

void displayEspNow() {
#if !ENABLE_ESPNOW
  enterUnavailablePage("ESP-NOW", "Disabled at\ncompile time");
  return;
#else
  // Defensive curtain — keyboard is used for device-name entry on first setup.
  if (oledKeyboardDrawIfActive(oledDisplay)) return;

  if (!gSettings.espnowEnabled && (!gEspNow || !gEspNow->initialized)) {
    enterUnavailablePage("ESP-NOW", "Disabled\nRun: espnowenabled 1\nReboot required");
    return;
  }

  if (!gEspNow || !gEspNow->initialized) {
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
    oledDisplay->println("ESP-NOW not");
    oledDisplay->println("initialized");
    oledDisplay->println();
    // Match what Y actually does (see the OLED_ESPNOW Y handler in
    // processOLEDInput): if a device name already exists, Y runs "openespnow"
    // directly; only a first-time, unnamed device opens the name keyboard.
    if (gSettings.espnowDeviceName.length() > 0) {
      oledDisplay->println("Press Y to");
      oledDisplay->println("initialize ESP-NOW");
    } else {
      oledDisplay->println("Press Y to enter");
      oledDisplay->println("device name");
    }
    return;
  }

  oledEspNowDisplay(oledDisplay);
#endif // ENABLE_ESPNOW
}

// ============================================================================
// OLED_WEB_STATS — HTTP server stats (read-only, two-phase rendering)
// ============================================================================

#if ENABLE_HTTP_SERVER
#include <esp_http_server.h>
#include "WebServer_Server.h"  // getTotalFailedLoginCount()
#endif

extern SessionEntry* gSessions;

struct WebStatsRenderData {
  int activeSessions;
  int totalSessions;
  unsigned long uptimeSeconds;
  int failedLoginAttempts;
  bool httpServerRunning;
  bool valid;
};
static WebStatsRenderData webStatsRenderData = {};

void prepareWebStatsData() {
  webStatsRenderData.activeSessions = 0;
  webStatsRenderData.totalSessions = 0;

  if (gSessions) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
      if (gSessions[i].sid.length() > 0) {
        webStatsRenderData.totalSessions++;
        unsigned long now = millis();
        if (!(gSessions[i].expiresAt > 0 && (long)(now - gSessions[i].expiresAt) >= 0)) {
          webStatsRenderData.activeSessions++;
        }
      }
    }
  }

  webStatsRenderData.uptimeSeconds = millis() / 1000;

#if ENABLE_HTTP_SERVER
  webStatsRenderData.httpServerRunning = (server != nullptr);
  // Surfaced from WebServer_Server's cumulative since-boot counter. Distinct
  // from per-IP failCount (which is windowed + clears on success) — this is
  // the "did anyone hammer my login?" audit number.
  webStatsRenderData.failedLoginAttempts = (int)getTotalFailedLoginCount();
#else
  webStatsRenderData.httpServerRunning = false;
  webStatsRenderData.failedLoginAttempts = 0;
#endif

  webStatsRenderData.valid = true;
}

void displayWebStatsRendered() {
  if (!oledDisplay || !oledConnected) return;

  if (!webStatsRenderData.valid) {
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
    oledDisplay->println("Web Stats Error");
    return;
  }

  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  oledDisplay->println();

  oledDisplay->print("Status: ");
  if (webStatsRenderData.httpServerRunning) {
    oledDisplay->println(gServerIsHttps ? "HTTPS" : "HTTP");
  } else {
    oledDisplay->println("Stopped");
  }

  oledDisplay->print("Active: ");
  oledDisplay->print(webStatsRenderData.activeSessions);
  oledDisplay->print("/");
  oledDisplay->print(MAX_SESSIONS);
  oledDisplay->println(" users");

  oledDisplay->print("Total: ");
  oledDisplay->print(webStatsRenderData.totalSessions);
  oledDisplay->println(" sessions");

  oledDisplay->print("Uptime: ");
  unsigned long hours = webStatsRenderData.uptimeSeconds / 3600;
  unsigned long minutes = (webStatsRenderData.uptimeSeconds % 3600) / 60;
  oledDisplay->print(hours);
  oledDisplay->print("h ");
  oledDisplay->print(minutes);
  oledDisplay->println("m");

  if (webStatsRenderData.failedLoginAttempts > 0) {
    oledDisplay->print("Failed: ");
    oledDisplay->println(webStatsRenderData.failedLoginAttempts);
  }
}

// ============================================================================
// OLED_MESH_STATUS — mesh role + peer count (read-only, two-phase rendering)
// ============================================================================

struct MeshStatusRenderData {
  bool espNowEnabled;
  bool meshEnabled;
  char myName[12];
  char masterName[12];
  int meshRole;
  bool isWorker;
  int activePeers;
  bool valid;
};
static MeshStatusRenderData meshStatusRenderData = {};

void prepareMeshStatusData() {
#if ENABLE_ESPNOW
  meshStatusRenderData.espNowEnabled = (gEspNow && gEspNow->initialized);
  meshStatusRenderData.meshEnabled = meshEnabled();

  if (meshStatusRenderData.espNowEnabled && meshStatusRenderData.meshEnabled) {
    uint8_t myMac[6];
    esp_wifi_get_mac(WIFI_IF_STA, myMac);
    String myName = getEspNowDeviceName(myMac);
    if (myName.length() == 0) {
      myName = macToHexString(myMac).substring(8);
    }
    if (myName.length() > 11) {
      myName = myName.substring(0, 11);
    }
    strncpy(meshStatusRenderData.myName, myName.c_str(), 11);
    meshStatusRenderData.myName[11] = '\0';

    meshStatusRenderData.meshRole = gSettings.meshRole;
    meshStatusRenderData.isWorker = (gSettings.meshRole == MESH_ROLE_WORKER && gSettings.meshMasterMAC.length() > 0);

    if (meshStatusRenderData.isWorker) {
      uint8_t masterMac[6];
      macFromHexString(gSettings.meshMasterMAC, masterMac);
      String masterName = getEspNowDeviceName(masterMac);
      if (masterName.length() == 0) {
        masterName = gSettings.meshMasterMAC.substring(8);
      }
      if (masterName.length() > 11) {
        masterName = masterName.substring(0, 11);
      }
      strncpy(meshStatusRenderData.masterName, masterName.c_str(), 11);
      meshStatusRenderData.masterName[11] = '\0';
    }

    int activePeers = 0;
    for (int i = 0; i < gMeshPeerSlots; i++) {
      if (gMeshPeers[i].isActive && !isSelfMac(gMeshPeers[i].mac) && isMeshPeerAlive(&gMeshPeers[i])) {
        activePeers++;
      }
    }
    meshStatusRenderData.activePeers = activePeers;
  }

  meshStatusRenderData.valid = true;
#else
  meshStatusRenderData.espNowEnabled = false;
  meshStatusRenderData.valid = true;
#endif
}

void displayMeshStatusRendered() {
  if (!oledDisplay || !oledConnected) return;

  if (!meshStatusRenderData.valid) {
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
    oledDisplay->println("Mesh Error");
    return;
  }

  oledDisplay->setTextSize(1);
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);

#if ENABLE_ESPNOW
  if (!meshStatusRenderData.espNowEnabled) {
    oledDisplay->println("ESP-NOW not init");
    return;
  }

  if (!meshStatusRenderData.meshEnabled) {
    oledDisplay->println("Mesh disabled");
    oledDisplay->println();
    oledDisplay->println("Use 'espnowmode'");
    oledDisplay->println("to enable mesh");
    return;
  }

  if (meshStatusRenderData.isWorker) {
    oledDisplay->print(meshStatusRenderData.masterName);
    oledDisplay->println(" [M]");
    oledDisplay->print("  ");
    oledDisplay->print(meshStatusRenderData.myName);
    oledDisplay->println(" [W]");
  } else {
    oledDisplay->print(meshStatusRenderData.myName);
    if (meshStatusRenderData.meshRole == MESH_ROLE_MASTER) {
      oledDisplay->println(" [M]");
    } else if (meshStatusRenderData.meshRole == MESH_ROLE_BACKUP_MASTER) {
      oledDisplay->println(" [B]");
    } else {
      oledDisplay->println(" [W]");
    }
  }

  if (meshStatusRenderData.activePeers == 0) {
    oledDisplay->println("  No peers");
  } else {
    String indent = meshStatusRenderData.isWorker ? "    " : "  ";
    oledDisplay->print(indent);
    oledDisplay->print(meshStatusRenderData.activePeers);
    oledDisplay->println(" peer(s)");
  }
#else
  oledDisplay->println("ESP-NOW disabled");
#endif
}

// ============================================================================
// OLED_REMOTE_SENSORS — bonded device sensor data (scrollable detail screens)
// ============================================================================
// Each "row" of the OLEDScrollState is one cached sensor entry; we use the
// scroll state purely as a navigation model (selectedIndex + wrap-around +
// oledScrollHandleNav) and render the *current* entry's bespoke detail view
// ourselves (sensors have wildly different layouts — joystick, dial, lat/lon,
// distance bar, etc.). This keeps navigation idiomatic without forcing a
// list-renderer onto detail screens.

#if ENABLE_ESPNOW
#include "System_ESPNow_Sensors.h"
#include <ArduinoJson.h>

EXT_RAM_BSS_ATTR static OLEDScrollState sRemoteSensorScroll;
static bool sRemoteSensorScrollInitialized = false;

static int collectConnectedRemoteSensors(int* outValidIndices, int maxOut) {
  if (!gRemoteSensorCache || !outValidIndices || maxOut <= 0) return 0;
  int validCount = 0;
  unsigned long now = millis();
  for (int i = 0; i < MAX_REMOTE_DEVICES * MAX_SENSORS_PER_DEVICE && validCount < maxOut; i++) {
    // Full mirror: list every CONNECTED (present) remote sensor — including
    // disabled ones — not just the actively-streaming ones, matching the web.
    RemoteSensorData& e = gRemoteSensorCache[i];
    if (!e.connected) continue;
    // Presence aging from the display path: a device we haven't heard from in a
    // while is gone — free its slot so it drops off (keeps the cache healthy even
    // when no web client is around to age it).
    if (now - e.lastSeen > REMOTE_SENSOR_PRESENCE_TTL_MS) { e.connected = false; continue; }
    outValidIndices[validCount++] = i;
  }
  return validCount;
}

static void populateRemoteSensorScroll(int validCount) {
  if (!sRemoteSensorScrollInitialized) {
    oledScrollInit(&sRemoteSensorScroll, nullptr, /*visibleLines=*/1);
    sRemoteSensorScrollInitialized = true;
  }
  // Only rebuild the (placeholder) item list if the count changed — we don't
  // render this list, we just need .itemCount/.selectedIndex right for nav.
  if (sRemoteSensorScroll.itemCount != validCount) {
    oledScrollClearKeepSelection(&sRemoteSensorScroll);
    for (int i = 0; i < validCount; i++) {
      oledScrollAddItem(&sRemoteSensorScroll, "");  // navigation-only placeholders
    }
    oledScrollClampSelection(&sRemoteSensorScroll);
  }
}

void displayRemoteSensors() {
  if (!oledDisplay) return;

  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);

  // Check if ESP-NOW is configured for receiving sensor data
  bool meshOn = meshEnabled();
  bool isMeshMaster = (gSettings.meshRole == MESH_ROLE_MASTER);
#if ENABLE_BONDED_MODE
  bool isPairedMaster = (gSettings.bondModeEnabled && isBondMaster());
#else
  bool isPairedMaster = false;
#endif
  bool canReceiveSensors = (meshOn && isMeshMaster) || isPairedMaster;

  if (!canReceiveSensors) {
    oledDisplay->setCursor(0, 14);
#if ENABLE_BONDED_MODE
    if (gSettings.bondModeEnabled) {
      oledDisplay->println("Bonded as worker.");
      oledDisplay->println("");
      oledDisplay->println("Workers send data,");
      oledDisplay->println("masters receive.");
      oledDisplay->println("");
      oledDisplay->println("Use 'bondstream' to");
      oledDisplay->println("send sensor data.");
    } else
#endif // ENABLE_BONDED_MODE
    if (meshOn && !isMeshMaster) {
      oledDisplay->println("Not a master device!");
      oledDisplay->println("");
      oledDisplay->println("To set as master:");
      oledDisplay->println(" espnow meshrole master");
    } else {
      oledDisplay->println("No remote source.");
      oledDisplay->println("");
      oledDisplay->println("Enable mesh mode:");
      oledDisplay->println(" espnowmode mesh");
#if ENABLE_BONDED_MODE
      oledDisplay->println("Or bond with device:");
      oledDisplay->println(" bondconnect <dev>");
#endif
    }
    return;
  }

  int validIndices[MAX_REMOTE_DEVICES * MAX_SENSORS_PER_DEVICE];
  int validCount = collectConnectedRemoteSensors(validIndices,
                     (int)(sizeof(validIndices) / sizeof(validIndices[0])));

  if (validCount == 0) {
    oledDisplay->setCursor(0, 14);
    oledDisplay->println("No remote sensors");
    oledDisplay->println("connected yet.");
    oledDisplay->println("");
    oledDisplay->println("Waiting for workers");
    oledDisplay->println("to send sensor data...");
    return;
  }

  populateRemoteSensorScroll(validCount);

  // Display current sensor — selection lives in the scroll state.
  int sel = sRemoteSensorScroll.selectedIndex;
  if (sel < 0 || sel >= validCount) sel = 0;
  int idx = validIndices[sel];
  RemoteSensorData* entry = &gRemoteSensorCache[idx];

  // Device name and sensor type
  oledDisplay->setCursor(0, 12);
  oledDisplay->print(entry->deviceName);
  oledDisplay->print(" - ");
  oledDisplay->println(sensorTypeToString(entry->sensorType));

  // Streaming state (mirrors the web dot): only render live data when fresh.
  // A connected sensor that isn't streaming shows "Disabled" so present-but-off
  // is clearly distinct from active — rather than rendering a stale frame.
  bool freshData = entry->valid && (millis() - entry->lastUpdate <= REMOTE_SENSOR_TTL_MS);
  if (!freshData) {
    oledDisplay->setCursor(0, 28);
    oledDisplay->println(entry->enabled ? "Streaming starting..." : "Disabled (not streaming)");
    return;
  }

  // Parse and display sensor data based on type
  if (entry->jsonLength > 0) {
    PSRAM_JSON_DOC(doc);
    if (deserializeJson(doc, entry->jsonData) == DeserializationError::Ok) {
      switch (entry->sensorType) {
        case REMOTE_SENSOR_INPUT: {
          // Two distinct input devices both stream as REMOTE_SENSOR_INPUT:
          //   gamepad : {"x":N,"y":N,"buttons":B}        (buttons active-LOW)
          //   ANO enc : {"pos":N,"axis":0|1,"buttons":B} (buttons active-HIGH)
          // Detect the ANO shape by the presence of "pos" (gamepad never sends it)
          // and render the matching widget. Before this split, ANO data fell into
          // the gamepad joystick layout below: x/y defaulted to 512 (dead-center
          // stick) and the gamepad button-bit mapping mislabeled the ANO buttons.
          if (!doc["pos"].isNull()) {
            // ---- ANO rotary encoder widget ----
            long     pos  = doc["pos"]     | 0L;
            int      axis = doc["axis"]    | 0;
            uint32_t b    = doc["buttons"] | 0u;   // active-HIGH: set bit = pressed

            // Left column: position + active axis.
            oledDisplay->setCursor(0, 24);
            oledDisplay->print("Pos: ");
            oledDisplay->print(pos);
            oledDisplay->setCursor(0, 36);
            oledDisplay->print("Axis: ");
            oledDisplay->print(axis == ANO_AXIS_HORIZONTAL ? "Horiz" : "Vert");

            // Right side: a rotary dial whose needle tracks pos folded into one
            // revolution. encoderPosition is a monotonic absolute count, so we
            // mod it into the ANO's 24 detents/rev to get a needle that visibly
            // sweeps as the knob turns. Divisor is purely cosmetic.
            const int cx = 108, cy = 32, r = 11;
            oledDisplay->drawCircle(cx, cy, r, DISPLAY_COLOR_WHITE);
            long detent = ((pos % 24) + 24) % 24;  // 0..23, correct for negative pos
            float ang = (float)detent * (2.0f * (float)M_PI / 24.0f) - (float)M_PI / 2.0f;  // 0 at 12 o'clock
            int nx = cx + (int)(cosf(ang) * (float)(r - 2));
            int ny = cy + (int)(sinf(ang) * (float)(r - 2));
            oledDisplay->drawLine(cx, cy, nx, ny, DISPLAY_COLOR_WHITE);
            oledDisplay->fillCircle(nx, ny, 1, DISPLAY_COLOR_WHITE);

            // Bottom row: button labels, BOXED (inverted) while held. ANO is
            // active-HIGH so a set bit means pressed (opposite of the gamepad).
            struct BtnCell { const char* label; uint32_t bit; };
            static const BtnCell kAnoBtns[] = {
              { "IN", ANO_BTN_IN },   { "UP", ANO_BTN_UP },   { "DN", ANO_BTN_DOWN },
              { "L",  ANO_BTN_LEFT }, { "R",  ANO_BTN_RIGHT }, { "St", ANO_VIRT_START },
            };
            int bx = 0;
            const int by = 48;
            for (const BtnCell& cell : kAnoBtns) {
              int w = (int)strlen(cell.label) * 6 + 2;  // 6px/char (size-1 font) + pad
              bool pressed = (b & cell.bit) != 0;
              if (pressed) {
                oledDisplay->fillRect(bx, by - 1, w, 10, DISPLAY_COLOR_WHITE);
                oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
              }
              oledDisplay->setCursor(bx + 1, by);
              oledDisplay->print(cell.label);
              if (pressed) oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);  // restore
              bx += w + 2;
            }
            break;
          }

          // ---- gamepad joystick widget (buttons active-LOW) ----
          int x = doc["x"] | 512;
          int y = doc["y"] | 512;
          uint32_t buttons = doc["buttons"] | 0xFFFFFFFF;

          // Draw joystick position (small box)
          int joyX = 10 + ((x * 20) / 1023);
          int joyY = 35 + ((y * 15) / 1023);
          oledDisplay->drawRect(10, 35, 22, 17, DISPLAY_COLOR_WHITE);
          oledDisplay->fillCircle(joyX, joyY, 2, DISPLAY_COLOR_WHITE);

          oledDisplay->setCursor(40, 28);
          oledDisplay->print("X:");
          oledDisplay->print(x);
          oledDisplay->setCursor(40, 38);
          oledDisplay->print("Y:");
          oledDisplay->print(y);

          // Button indicators
          oledDisplay->setCursor(85, 28);
          oledDisplay->print((buttons & (1<<6)) ? " " : "X");
          oledDisplay->print((buttons & (1<<2)) ? " " : "Y");
          oledDisplay->setCursor(85, 38);
          oledDisplay->print((buttons & (1<<5)) ? " " : "A");
          oledDisplay->print((buttons & (1<<1)) ? " " : "B");
          oledDisplay->setCursor(85, 48);
          oledDisplay->print((buttons & (1<<0)) ? " " : "Sel");
          oledDisplay->print((buttons & (1<<16)) ? " " : "St");
          break;
        }

        case REMOTE_SENSOR_IMU: {
          // IMU JSON: {"ori":{"yaw":..,"pitch":..,"roll":..},...}
          JsonObject ori = doc["ori"];
          float roll = ori["roll"] | 0.0f;
          float pitch = ori["pitch"] | 0.0f;
          float yaw = ori["yaw"] | 0.0f;

          oledDisplay->setCursor(0, 24);
          oledDisplay->print("Roll:  ");
          oledDisplay->print(roll, 1);
          oledDisplay->println(" deg");
          oledDisplay->print("Pitch: ");
          oledDisplay->print(pitch, 1);
          oledDisplay->println(" deg");
          oledDisplay->print("Yaw:   ");
          oledDisplay->print(yaw, 1);
          oledDisplay->println(" deg");
          break;
        }

        case REMOTE_SENSOR_GPS: {
          // GPS JSON: {"fix":1,"sats":8,"lat":..,"lon":..,...}
          float lat = doc["lat"] | 0.0f;
          float lon = doc["lon"] | 0.0f;
          int sats = doc["sats"] | 0;
          int fix = doc["fix"] | 0;

          oledDisplay->setCursor(0, 24);
          oledDisplay->print("Lat: ");
          oledDisplay->println(lat, 5);
          oledDisplay->print("Lon: ");
          oledDisplay->println(lon, 5);
          oledDisplay->print("Sats: ");
          oledDisplay->print(sats);
          oledDisplay->print(fix ? " (Fix)" : " (No fix)");
          break;
        }

        case REMOTE_SENSOR_TOF: {
          // ToF JSON: {"objects":[{"distance_mm":123,"status":0},...]}
          JsonArray objects = doc["objects"];
          if (objects.size() > 0) {
            // Objects are now detected-only, so objects[0] existing IS a detection
            // (the per-object "detected" flag was dropped).
            JsonObject obj = objects[0];
            int dist = obj["distance_mm"] | 0;
            int status = obj["status"] | -1;

            oledDisplay->setCursor(0, 28);
            oledDisplay->print("Distance: ");
            oledDisplay->print(dist);
            oledDisplay->println(" mm");
            oledDisplay->print("Status: ");
            oledDisplay->println(status == 0 ? "OK" : "Error");
          } else {
            oledDisplay->setCursor(0, 28);
            oledDisplay->println("No object detected");
          }
          break;
        }

        case REMOTE_SENSOR_FMRADIO: {
          // FM JSON: {"frequency":101.5,"rssi":45,"station":"..."}
          float freq = doc["frequency"] | 0.0f;
          int rssi = doc["rssi"] | 0;
          const char* station = doc["station"] | "";

          oledDisplay->setCursor(0, 24);
          oledDisplay->print("Freq: ");
          oledDisplay->print(freq, 1);
          oledDisplay->println(" MHz");
          oledDisplay->print("RSSI: ");
          oledDisplay->println(rssi);
          if (strlen(station) > 0) {
            oledDisplay->print("Stn: ");
            oledDisplay->println(station);
          }
          break;
        }

        default: {
          // Generic readable key:value rendering — covers RTC, APDS, Presence,
          // Thermal, and any future sensor without a hard-coded per-type case.
          // Walks the cached JSON via the shared formatter so it stays current
          // with whatever the producer emits. (Gamepad/IMU/GPS/ToF/FM keep their
          // bespoke layouts above.)
          char lines[128];
          formatRemoteSensorReadable(entry->jsonData, lines, sizeof(lines), 4);
          oledDisplay->setCursor(0, 24);
          oledDisplay->print(lines);  // newline-separated; GFX advances on '\n'
          break;
        }
      }
    }
  } else {
    oledDisplay->setCursor(0, 24);
    oledDisplay->println("No data");
  }

  // Navigation hint
  oledDisplay->setCursor(0, OLED_CONTENT_HEIGHT - 8);
  oledDisplay->print(sel + 1);
  oledDisplay->print("/");
  oledDisplay->print(validCount);
}

static bool remoteSensorsAvailable(String* outReason) {
  // Always available — display function shows setup instructions if not configured.
  if (!meshEnabled()) {
    if (outReason) *outReason = "Mesh off";
    return true;
  }
  if (gSettings.meshRole != MESH_ROLE_MASTER) {
    if (outReason) *outReason = "Not master";
    return true;
  }
  return true;
}

static bool remoteSensorsInputHandler(int /*deltaX*/, int /*deltaY*/, uint32_t /*newlyPressed*/) {
  // Count valid entries; only enable nav if there are >=2 entries to switch between.
  int dummy[MAX_REMOTE_DEVICES * MAX_SENSORS_PER_DEVICE];
  int validCount = collectConnectedRemoteSensors(dummy, (int)(sizeof(dummy) / sizeof(dummy[0])));
  if (validCount == 0) return false;

  populateRemoteSensorScroll(validCount);
  return oledScrollHandleNav(&sRemoteSensorScroll);
}

// Remote Sensors OLED mode entry (bond mode: shows bonded device sensor data)
static const OLEDModeEntry remoteSensorsOLEDModes[] = {
  {
    OLED_REMOTE_SENSORS,       // mode enum
    "Bond",                    // menu name (shows bonded device capabilities)
    "notify_sensor",           // icon name
    displayRemoteSensors,      // displayFunc
    remoteSensorsAvailable,    // availFunc
    remoteSensorsInputHandler, // inputFunc
    true,                      // showInMenu
    30,                        // menuOrder
    nullptr                    // dynamic hints
  }
};

REGISTER_OLED_MODE_MODULE(remoteSensorsOLEDModes,
                          sizeof(remoteSensorsOLEDModes) / sizeof(remoteSensorsOLEDModes[0]),
                          "RemoteSensors");

#endif // ENABLE_ESPNOW

// ============================================================================
// Mode Registration — every screen is its own enum entry
// ============================================================================
// Columns: mode, name, iconName, displayFunc, availFunc, inputFunc,
//          showInMenu, menuOrder, hints

static const OLEDModeEntry sNetworkModes[] = {
  { OLED_NETWORK_INFO,      "Network", "wifi",          displayNetworkInfoRendered,
    nullptr, networkMainInputHandler,     false, -1, "A:Select B:Back" },
  { OLED_NETWORK_STATUS,    "Status",  "wifi",          displayNetworkStatusRendered,
    nullptr, nullptr,                     false, -1, "B:Back" },
  { OLED_NETWORK_WIFI_MENU, "WiFi",    "wifi",          displayNetworkWifiMenuRendered,
    nullptr, networkWifiMenuInputHandler, false, -1, "A:Select B:Back" },
  { OLED_NETWORK_WIFI_LIST, "Saved",   "wifi",          displaySavedNetworksRendered,
    nullptr, networkWifiListInputHandler,   false, -1, "A:Connect B:Back" },
  { OLED_NETWORK_WIFI_REMOVE,"Remove", "wifi",          displaySavedNetworksRendered,
    nullptr, networkWifiRemoveInputHandler, false, -1, "A:Delete B:Back" },
  { OLED_NETWORK_WIFI_SCAN, "Scan",    "wifi",          displayNetworkWifiScanRendered,
    nullptr, networkWifiScanInputHandler,   false, -1, "A:Add B:Back" },
  { OLED_MESH_STATUS,       "Mesh",    "wifi",          displayMeshStatusRendered,
    nullptr, nullptr,                     false, -1, "B:Back" },
  { OLED_WEB_STATS,         "Web",     "web",           displayWebStatsRendered,
    nullptr, nullptr,                     false, -1, nullptr  },  // dynamic hints in OLED_Utils.cpp
  { OLED_ESPNOW,            "ESP-NOW", "notify_espnow", displayEspNow,
    nullptr, nullptr,                     false, -1, nullptr, espnowOnEnter },  // dynamic hints in OLED_Utils.cpp
};

REGISTER_OLED_MODE_MODULE(sNetworkModes,
                          sizeof(sNetworkModes) / sizeof(sNetworkModes[0]),
                          "Network");

#endif // ENABLE_OLED_DISPLAY
